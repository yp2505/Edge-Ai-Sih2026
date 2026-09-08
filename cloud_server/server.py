#!/usr/bin/env python3
"""
cloud_server/server.py — CANONICAL Cloud ASR Server for Hey Vaani

Pipeline:
  ESP32 (main.cpp) → TCP socket → HVP1 header → raw PCM (live-streamed) → THIS SERVER
  → faster-whisper (INT8, CPU) → transcript → JSON response → ESP32 display

Protocol: 20-byte HVP1 header (magic 0x48565031).
  audio_len = 0  →  live-streaming mode (ESP32 sends 30ms chunks until TCP close)
  audio_len > 0  →  batch mode (legacy / --test mode)

ASR Engine: faster-whisper only (open-source, fully offline, no API key).

Usage:
    python server.py                        # port 5000, Whisper tiny
    python server.py --whisper-model base   # larger model, better accuracy
    python server.py --port 8080
    python server.py --test audio.wav       # send a WAV as if from ESP32

Install:
    pip install faster-whisper numpy soundfile
"""

import os
import sys

# Force UTF-8 encoding on Windows to prevent UnicodeEncodeError with emojis
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

import time
import json
import struct
import socket
import wave
import argparse
import threading
from datetime import datetime
import re
from difflib import SequenceMatcher
from http.server import BaseHTTPRequestHandler, HTTPServer
import numpy as np

# ─── Server-side VAD (Silence Detection) ─────────────────────────────────────
# The ESP32 streams audio for COMMAND_DURATION_MS (2000ms) before closing.
# Rather than waiting the full 2s, the server monitors RMS energy per chunk and
# cuts off early when the user has clearly stopped speaking.
#
# Tuning:
#   VAD_RMS_THRESHOLD   — below this level, chunk is "silence"
#   VAD_SILENCE_MS      — how many consecutive ms of silence triggers cut-off
#   VAD_MIN_AUDIO_MS    — don't cut before this — lets the command word start
#
VAD_RMS_THRESHOLD = 0.008    # 0.8% of full scale — works well for most mics
VAD_SILENCE_MS    = 350      # stop after 350ms of consecutive silence
VAD_MIN_AUDIO_MS  = 1500     # wait at least 1.5s (ring buffer 1s + command start)

# ─── Pre-Transcription Gates ─────────────────────────────────────────────────
# These filters run BEFORE calling Whisper at all.
# They prevent the most common hallucination trigger: near-silent or very short
# audio clips reaching the model.
#
# ENERGY_RMS_THRESHOLD:
#   RMS energy of the full audio buffer (normalised 0.0–1.0).
#   Clips below this are near-silent and are skipped entirely.
#   Typical quiet room noise floor ≈ 0.001–0.003; human speech ≈ 0.015+.
#   Start at 0.005, raise if still getting hallucinations on silence.
ENERGY_RMS_THRESHOLD = 0.005

# MIN_AUDIO_DURATION_MS:
#   Clips shorter than this are skipped — too short to contain a real command.
#   Whisper is especially prone to hallucination on clips < 500ms.
MIN_AUDIO_DURATION_MS = 300    # milliseconds (tune: 300–500ms)

# ─── Whisper Hallucination-Suppression Parameters ────────────────────────────
# All passed directly to faster-whisper's transcribe() call.
# Each is tunable without touching the function body.

WHISPER_BEAM_SIZE = 5

# WHISPER_NO_SPEECH_THRESHOLD (0.0–1.0):
#   Segments whose no_speech_prob exceeds this are silently dropped.
#   Most effective single hallucination filter for short clips. Default: 0.6.
WHISPER_NO_SPEECH_THRESHOLD = 0.6

# WHISPER_LOG_PROB_THRESHOLD (negative float):
#   Segments below this average log-probability are discarded.
#   Raise toward 0.0 to be more aggressive (e.g. -0.5). Default: -1.0.
WHISPER_LOG_PROB_THRESHOLD = -1.0

# WHISPER_COMPRESSION_RATIO_THRESHOLD:
#   Segments with gzip compression ratio above this are repetition loops.
#   Default: 2.4.
WHISPER_COMPRESSION_RATIO_THRESHOLD = 2.4

# WHISPER_CONDITION_ON_PREVIOUS_TEXT:
#   When True (default), Whisper anchors on prior context — causing runaway
#   hallucinations across short disconnected clips. Set False for our use case.
WHISPER_CONDITION_ON_PREVIOUS_TEXT = False

# WHISPER_VAD_FILTER / WHISPER_VAD_PARAMETERS:
#   Use Silero VAD inside faster-whisper to trim silence before the model.
#   min_silence_duration_ms: shorten from the 2000ms default for short clips.
WHISPER_VAD_FILTER = True
WHISPER_VAD_PARAMETERS = {
    "min_silence_duration_ms": 500,
}

# WHISPER_REPETITION_PENALTY:
#   Decoder-level penalty for repeated n-grams. 1.0 = disabled. 1.1 recommended.
WHISPER_REPETITION_PENALTY = 1.1

# ─── Startup check — FAIL LOUDLY if faster-whisper missing ──────────────────
try:
    from faster_whisper import WhisperModel  # type: ignore
except ImportError:
    print(
        "\n"
        "╔══════════════════════════════════════════════════════════════╗\n"
        "║   ❌  FATAL: faster-whisper NOT INSTALLED                    ║\n"
        "║                                                              ║\n"
        "║   The server CANNOT transcribe audio without it.            ║\n"
        "║   Running without it gives silent mock output —             ║\n"
        "║   NEVER acceptable in a live demo.                          ║\n"
        "║                                                              ║\n"
        "║   FIX:  pip install faster-whisper                          ║\n"
        "║         (or: uv pip install faster-whisper)                 ║\n"
        "╚══════════════════════════════════════════════════════════════╝\n",
        file=sys.stderr,
    )
    sys.exit(1)


# ─── Configuration ─────────────────────────────────────────────────────────
DEFAULT_PORT       = 5000
DASHBOARD_API_PORT = 8080

AUDIO_DIR = os.path.join(os.path.dirname(__file__), "received_audio")
LOG_FILE  = os.path.join(os.path.dirname(__file__), "server_log.json")

# ─── HVP1 Protocol ───────────────────────────────────────────────────────────
# 20-byte header sent by ESP32 before every audio stream:
#   bytes  0-3:  magic             uint32  0x48565031 ("HVP1")
#   bytes  4-7:  sample_rate       uint32  e.g. 16000
#   bytes  8-9:  channels          uint16  1
#   bytes 10-11: bits              uint16  16
#   bytes 12-15: audio_len         uint32  0 = live-stream until TCP close
#   bytes 16-19: kw_to_connect_ms  uint32  ESP32 monotonic: keyword_end → connect
MAGIC_NUMBER  = 0x48565031   # "HVP1"
HEADER_FORMAT = "<IIHHII"
HEADER_SIZE   = struct.calcsize(HEADER_FORMAT)   # 20 bytes

MAX_AUDIO_BYTES    = 640_000   # 20s × 16kHz × 2B safety cap
VALID_SAMPLE_RATES = {8000, 16000, 22050, 44100, 48000}


# ─── Dashboard HTTP API ───────────────────────────────────────────────────────
class _DashboardHandler(BaseHTTPRequestHandler):
    """Minimal HTTP handler exposing /api/events and /api/health."""

    def log_message(self, format, *args): pass  # silence access log

    def _send_json(self, data, status: int = 200):
        try:
            body = json.dumps(data, indent=2).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "*")
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            # Browsers can cancel a poll/navigation while its JSON response is
            # being written. The request is already gone, so no response or
            # traceback is useful here.
            pass

    def do_OPTIONS(self): self._send_json({})

    def do_POST(self):
        if self.path.split("?")[0] != "/api/telemetry":
            self._send_json({"error": "not found"}, status=404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(data, dict): raise ValueError("expected object")
            data["received_at"] = datetime.now().isoformat()
            asr = self.server.asr_server
            with asr._lock: asr.telemetry = data
            self._send_json({"ok": True})
        except Exception as e:
            self._send_json({"error": str(e)}, status=400)

    def do_GET(self):
        asr: "ASRServer" = self.server.asr_server
        path = self.path.split("?")[0]
        if path == "/api/health":
            with asr._lock:
                count = len(asr.log_entries)
            self._send_json({
                "status":         "running",
                "uptime_seconds": int(time.time() - asr.start_time),
                "session_count":  count,
                "asr_engine":     f"faster-whisper-{asr.whisper_model}",
            })
        elif path == "/api/telemetry":
            with asr._lock:
                telemetry = dict(asr.telemetry)
            self._send_json(telemetry)
        elif path == "/api/events":
            with asr._lock:
                entries = list(asr.log_entries)
            self._send_json(entries)
        else:
            self._send_json({"error": "not found"}, status=404)


class DashboardHTTPServer:
    def __init__(self, asr_server: "ASRServer", port: int = DASHBOARD_API_PORT):
        self.asr_server = asr_server
        self.port = port

    def start_in_thread(self):
        httpd = HTTPServer(("0.0.0.0", self.port), _DashboardHandler)
        httpd.asr_server = self.asr_server
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        print(f"  🌐 Dashboard API:    http://localhost:{self.port}/api/health")
        print(f"                       http://localhost:{self.port}/api/events")


# ─── Main ASR Server ──────────────────────────────────────────────────────────
class ASRServer:
    """
    TCP server: receives HVP1 live-stream from ESP32, transcribes with faster-whisper.

    The ESP32 firmware connects immediately at keyword detection and streams
    30ms PCM chunks live. This server buffers all chunks, then runs Whisper
    on the complete audio once the stream closes.

    Latency breakdown:
      kw_to_connect_ms  (ESP32 clock)   = keyword_end → TCP connect done
      receive_gap_ms    (server clock)  = connection accepted → first byte
      transcribe_ms     (server clock)  = Whisper inference on complete audio
      ──────────────────────────────────────────────────────────────
      end_to_end_ms                     = sum of the three above
    """

    def __init__(self, port: int = DEFAULT_PORT, whisper_model: str = "tiny",
                 dashboard_port: int = DASHBOARD_API_PORT):
        self.port          = port
        self.dashboard_port = dashboard_port
        self.whisper_model  = whisper_model
        self.transcriber    = None
        self.session_count  = 0
        self.start_time     = time.time()
        self._lock          = threading.Lock()
        self.log_entries: list = []
        self.telemetry: dict = {}

    def start(self):
        self._load_whisper()
        os.makedirs(AUDIO_DIR, exist_ok=True)
        DashboardHTTPServer(self, port=self.dashboard_port).start_in_thread()

        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(("0.0.0.0", self.port))
        server_socket.listen(5)

        local_ip = self._get_local_ip()

        print("=" * 62)
        print("  ☁️   Hey Vaani — Cloud ASR Server (CANONICAL)")
        print("=" * 62)
        print(f"  Status:          🟢 RUNNING")
        print(f"  Address:         {local_ip}:{self.port}")
        print(f"  ASR engine:      faster-whisper '{self.whisper_model}' (INT8, CPU)")
        print("  Wake word:       custom TFLite Micro model on ESP32 (edge-authoritative)")
        print(f"  Protocol:        HVP1 v1 — 20-byte header, live-stream mode")
        print(f"  Log:             {LOG_FILE}")
        print("=" * 62)
        print(f"\n  ⚙️  Configure ESP32 with:")
        print(f"      CONFIG_SERVER_IP   = \"{local_ip}\"")
        print(f"      CONFIG_SERVER_PORT = {self.port}\n")
        print(f"  Waiting for ESP32 connections...\n")

        try:
            while True:
                client_socket, client_addr = server_socket.accept()
                with self._lock:
                    self.session_count += 1
                    sid = self.session_count
                    # Publish the on-device wake-word event before audio finishes streaming.
                    self.log_entries.append({
                        "session_id": sid,
                        "timestamp": datetime.now().isoformat(),
                        "client_ip": client_addr[0],
                        "status": "wake_word_detected",
                        "wake_word": "Hey Vaani",
                    })
                self._save_log()
                print(f"  📡 [{sid}] Connection from {client_addr[0]}:{client_addr[1]}")
                threading.Thread(
                    target=self._handle_client,
                    args=(client_socket, client_addr, sid),
                    daemon=True,
                ).start()
        except KeyboardInterrupt:
            print("\n\n  🛑 Server shutting down...")
            self._save_log()
            server_socket.close()

    # ── Whisper ────────────────────────────────────────────────────────────
    def _load_whisper(self):
        print(f"  Loading faster-whisper model '{self.whisper_model}' (INT8)...")
        t0 = time.time()
        self.transcriber = WhisperModel(
            self.whisper_model,
            device="cpu",
            compute_type="int8",
        )
        print(f"  ✅ Whisper '{self.whisper_model}' loaded in {(time.time()-t0)*1000:.0f}ms")

    # ── Client Handler ─────────────────────────────────────────────────────
    def _handle_client(self, client_socket: socket.socket,
                        client_addr: tuple, session_id: int):
        """
        Handle one ESP32 session end-to-end:
          1. Validate 20-byte HVP1 header
          2. Receive all audio (live-streaming chunks or single batch)
          3. Transcribe with faster-whisper
          4. Send JSON response back to ESP32
          5. Log to server_log.json (consumed by React dashboard & latency_analyzer.py)
        """
        connection_accepted_ms = int(time.time() * 1000)
        first_audio_byte_ms: int | None = None

        try:
            # ── Step 1: Validate HVP1 header ──────────────────────────────
            header_data = self._recv_exact(client_socket, HEADER_SIZE)
            if not header_data or len(header_data) < HEADER_SIZE:
                print(f"  ❌ [{session_id}] Short/missing header — closing")
                return

            magic, sr, ch, bits, audio_len, kw_to_connect_ms = struct.unpack(
                HEADER_FORMAT, header_data
            )

            if magic != MAGIC_NUMBER:
                print(f"  ❌ [{session_id}] Bad magic 0x{magic:08X} — closing")
                return
            if sr not in VALID_SAMPLE_RATES:
                print(f"  ❌ [{session_id}] Implausible sample rate {sr}Hz — closing")
                return
            if audio_len > MAX_AUDIO_BYTES:
                print(f"  ❌ [{session_id}] audio_len {audio_len} exceeds safety cap — closing")
                return

            mode = "streaming" if audio_len == 0 else f"batch ({audio_len}B)"
            print(f"  📋 [{session_id}] HVP1: {sr}Hz/{ch}ch/{bits}bit "
                  f"mode={mode} kw→connect={kw_to_connect_ms}ms")

            # ── Step 2: Receive audio ──────────────────────────────────────
            audio_data = b""
            first_chunk = True

            if audio_len > 0:
                # Batch mode (legacy / --test)
                remaining = audio_len
                while remaining > 0:
                    chunk = client_socket.recv(min(8192, remaining))
                    if not chunk:
                        break
                    if first_chunk:
                        first_audio_byte_ms = int(time.time() * 1000)
                        first_chunk = False
                    audio_data += chunk
                    remaining -= len(chunk)
            else:
                # The custom, open-source TFLite Micro KWS model runs on the
                # ESP32. After its ring buffer arrives, acknowledge the edge
                # detection immediately and keep receiving command audio for
                # ASR. Whisper is deliberately not used to judge the wake word.
                consecutive_silence_ms = 0
                total_audio_ms = 0
                stage = "ring_buffer"  # first1s = ring buffer containing keyword
                ring_buffer_audio = b""
                edge_wake_word_accepted = False

                while True:
                    chunk = client_socket.recv(960)   # 960B ≈ 30ms at 16kHz int16
                    if not chunk:
                        break
                    if first_chunk:
                        first_audio_byte_ms = int(time.time() * 1000)
                        first_chunk = False
                    audio_data += chunk
                    total_audio_ms += 30

                    # Collect first1s for wake-word verification
                    if stage == "ring_buffer":
                        ring_buffer_audio += chunk
                        if len(ring_buffer_audio) >= sr * 2:  # 1s = sr * 2 bytes
                            edge_wake_word_accepted = True
                            stage = "live_stream"
                            print(f"  ✅ [{session_id}] Edge wake-word accepted — receiving command")
                            continue
                    
                    # Stage 2: Live streaming (after wake-word confirmed)
                    if stage == "live_stream":
                        if total_audio_ms >= VAD_MIN_AUDIO_MS:
                            samples = np.frombuffer(chunk, dtype=np.int16).astype(np.float32)
                            rms = np.sqrt(np.mean(samples ** 2)) / 32768.0

                            if rms < VAD_RMS_THRESHOLD:
                                consecutive_silence_ms += 30
                                if consecutive_silence_ms >= VAD_SILENCE_MS:
                                    print(f"  🔇 [{session_id}] Silence detected "
                                          f"after {total_audio_ms}ms — cutting stream early")
                                    break
                            else:
                                consecutive_silence_ms = 0

            audio_duration_s  = len(audio_data) / (sr * ch * (bits // 8)) if audio_data else 0.0
            audio_duration_ms = int(audio_duration_s * 1000)
            receive_gap_ms    = (first_audio_byte_ms or int(time.time()*1000)) - connection_accepted_ms

            print(f"  📊 [{session_id}] {len(audio_data):,}B ({audio_duration_s:.2f}s) "
                  f"| gap={receive_gap_ms}ms")

            # ── Step 3: Save WAV (always — useful for debugging) ───────────
            wav_path = self._save_wav(audio_data, session_id, sr, ch, bits)

            # ── Step 3a: Pre-transcription gates ──────────────────────────
            # Energy + duration checks BEFORE calling Whisper.
            # Prevents the most common hallucination trigger: near-silent clips.
            skip_reason = self._check_audio_gates(
                audio_data, sr, ch, bits, audio_duration_ms, session_id
            )

            if skip_reason:
                # Audio failed a pre-transcription gate — skip Whisper entirely.
                transcript    = skip_reason   # e.g. "[skipped: below energy threshold]"
                avg_log_prob  = None
                transcribe_ms = 0
                print(f"  ⚠️  [{session_id}] SKIPPED transcription — {skip_reason}")
            else:
                print(f"  🗣️  [{session_id}] Transcribing with faster-whisper '{self.whisper_model}'...")
                t_t0 = time.time()
                transcript, avg_log_prob = self._transcribe(wav_path, session_id)
                transcribe_ms = int((time.time() - t_t0) * 1000)

            # Step 3: Two-step Verification Gate evaluation
            is_confirmed, match_score, matched_var = self._verify_keyword_transcript(transcript)
            verification_status = "CONFIRMED" if is_confirmed else "REJECTED"

            total_server_ms = int(time.time() * 1000) - connection_accepted_ms
            end_to_end_ms   = kw_to_connect_ms + total_server_ms

            print(f"\n  {'─' * 58}")
            print(f"  📝 [{session_id}] RAW TRANSCRIPT:  \"{transcript}\"")
            print(f"  🛡️  [{session_id}] VERIFICATION GATE: {verification_status} (score={match_score:.2f}, match='{matched_var}')")
            print(f"  ⏱️  [{session_id}] kw→connect   (ESP32):   {kw_to_connect_ms}ms")
            print(f"  ⏱️  [{session_id}] connect→1st byte:        {receive_gap_ms}ms")
            print(f"  ⏱️  [{session_id}] Whisper '{self.whisper_model}':   {transcribe_ms}ms")
            print(f"  ⏱️  [{session_id}] END-TO-END:              {end_to_end_ms}ms")
            print(f"  {'─' * 58}\n")

            # ── Step 4: Send JSON response to ESP32 ───────────────────────
            response = json.dumps({
                "transcript":          transcript,
                "verification_status": verification_status,
                "verified":            is_confirmed,
                "match_score":         round(match_score, 2),
                "matched_variant":     matched_var,
                "end_to_end_ms":       end_to_end_ms,
                "transcribe_ms":       transcribe_ms,
                "session_id":          session_id,
                "asr_engine":          f"faster-whisper-{self.whisper_model}",
                "wake_word_confirmed": is_confirmed,
            }).encode("utf-8")
            try:
                client_socket.sendall(response)
            except Exception:
                pass   # ESP32 may have already closed its read side

            # ── Step 5: Append to server_log.json ─────────────────────────
            log_entry = {
                "session_id":             session_id,
                "timestamp":              datetime.now().isoformat(),
                "client_ip":              client_addr[0],
                "audio_bytes":            len(audio_data),
                "audio_duration_s":       round(audio_duration_s, 3),
                "audio_duration_ms":      audio_duration_ms,
                "audio_capture_ms":       audio_duration_ms,
                "sample_rate":            sr,
                "raw_transcript":         transcript,
                "transcript":             transcript,
                "verification_status":    verification_status,
                "verified":                is_confirmed,
                "match_score":            round(match_score, 2),
                "matched_variant":        matched_var,
                "avg_log_prob":           round(avg_log_prob, 4) if avg_log_prob else None,
                "skipped":                skip_reason is not None,
                "skip_reason":            skip_reason,
                "asr_engine":             f"faster-whisper-{self.whisper_model}",
                # Single-clock latency (no NTP needed)
                "kw_to_connect_ms":       kw_to_connect_ms,
                "connection_accepted_ms": connection_accepted_ms,
                "first_audio_byte_ms":    first_audio_byte_ms,
                "receive_gap_ms":         receive_gap_ms,
                "transcribe_ms":          transcribe_ms,
                "total_server_ms":        total_server_ms,
                "end_to_end_ms":          end_to_end_ms,
                # latency_analyzer.py compatibility key
                "cloud_receive_timestamp_ms": first_audio_byte_ms,
                "status":                 "complete",
                "wake_word":              "Hey Vaani",
            }
            with self._lock:
                for index, entry in enumerate(self.log_entries):
                    if entry.get("session_id") == session_id:
                        self.log_entries[index] = log_entry
                        break
                else:
                    self.log_entries.append(log_entry)
            self._save_log()

        except Exception as e:
            print(f"  ❌ [{session_id}] Unhandled error: {e}")
        finally:
            client_socket.close()

    # ── Pre-Transcription Audio Quality Gates ──────────────────────────────
    def _check_audio_gates(
        self,
        audio_data: bytes,
        sr: int, ch: int, bits: int,
        audio_duration_ms: int,
        session_id: int,
    ) -> "str | None":
        """
        Run lightweight audio quality checks BEFORE calling Whisper.

        Returns a skip-reason string when Whisper should be bypassed,
        or None when the audio is acceptable.

        Gate order (cheapest checks first):
          0. Empty buffer
          1. Duration too short  (< MIN_AUDIO_DURATION_MS)
          2. Energy too low      (RMS < ENERGY_RMS_THRESHOLD)
        """
        if not audio_data:
            reason = "[skipped: empty audio buffer]"
            print(f"  🚫 [{session_id}] Gate 0 TRIGGERED — empty buffer")
            return reason

        if audio_duration_ms < MIN_AUDIO_DURATION_MS:
            reason = (
                f"[skipped: audio too short "
                f"({audio_duration_ms}ms < {MIN_AUDIO_DURATION_MS}ms minimum)]"
            )
            print(f"  🚫 [{session_id}] Gate 1 TRIGGERED — {reason}")
            return reason

        try:
            samples = np.frombuffer(audio_data, dtype=np.int16).astype(np.float32)
            rms = float(np.sqrt(np.mean(samples ** 2)) / 32768.0)
            print(
                f"  🔬 [{session_id}] Energy check: RMS={rms:.5f} "
                f"(threshold={ENERGY_RMS_THRESHOLD})"
            )
            if rms < ENERGY_RMS_THRESHOLD:
                reason = (
                    f"[skipped: audio below energy threshold "
                    f"(RMS={rms:.5f} < {ENERGY_RMS_THRESHOLD})]"
                )
                print(f"  🚫 [{session_id}] Gate 2 TRIGGERED — {reason}")
                return reason
        except Exception as e:
            print(f"  ⚠️  [{session_id}] Energy gate error (continuing anyway): {e}")

        print(f"  ✅ [{session_id}] Audio passed all pre-transcription gates — calling Whisper")
        return None

    # ── Step 3: Two-step Verification Gate ──────────────────────────────────
    def _verify_keyword_transcript(self, transcript: str) -> tuple:
        """
        Fuzzy-match the raw ASR transcript against 'hey vaani' and real-world variants.
        Returns: (is_confirmed: bool, best_score: float, matched_variant: str)
        """
        if not transcript or transcript.startswith("[skipped"):
            return False, 0.0, "none"

        clean_text = re.sub(r"[^\w\s]", "", transcript.lower()).strip()
        if not clean_text:
            return False, 0.0, "empty"

        target_variants = [
            "hey vaani", "hey vani", "hi vaani", "hay vaani",
            "hey wani", "hey banni", "hey vanni", "hey bhani",
            "vaani", "vani", "wani"
        ]

        # 1. Direct substring match check
        for var in target_variants:
            if var in clean_text:
                return True, 1.0, var

        # 2. Fuzzy sequence similarity check
        best_score = 0.0
        best_variant = "none"
        for var in target_variants:
            # Check against full text & sliding window tokens
            ratio = SequenceMatcher(None, clean_text, var).ratio()
            if ratio > best_score:
                best_score = ratio
                best_variant = var

            # Check sub-phrases
            words = clean_text.split()
            for i in range(len(words)):
                for j in range(i + 1, min(i + 4, len(words) + 1)):
                    sub_phrase = " ".join(words[i:j])
                    sub_ratio = SequenceMatcher(None, sub_phrase, var).ratio()
                    if sub_ratio > best_score:
                        best_score = sub_ratio
                        best_variant = var

        # Accept threshold: 0.65+ for fuzzy phoneme variants
        is_confirmed = best_score >= 0.65
        return is_confirmed, best_score, best_variant

    # ── Transcription ──────────────────────────────────────────────────────
    def _transcribe(self, wav_path: str, session_id: int = 0) -> tuple:
        """
        Run faster-whisper with hallucination-suppression parameters.
        Returns (transcript, avg_log_prob).

        All threshold values are defined as module-level constants (top of file)
        so they can be tuned without touching this function.
        """
        try:
            segments, info = self.transcriber.transcribe(
                wav_path,
                language="en",
                beam_size=WHISPER_BEAM_SIZE,
                # ── Hallucination suppression ──────────────────────────────
                condition_on_previous_text=WHISPER_CONDITION_ON_PREVIOUS_TEXT,
                no_speech_threshold=WHISPER_NO_SPEECH_THRESHOLD,
                log_prob_threshold=WHISPER_LOG_PROB_THRESHOLD,
                compression_ratio_threshold=WHISPER_COMPRESSION_RATIO_THRESHOLD,
                repetition_penalty=WHISPER_REPETITION_PENALTY,
                # ── VAD (silence stripping before model) ──────────────────
                vad_filter=WHISPER_VAD_FILTER,
                vad_parameters=WHISPER_VAD_PARAMETERS,
            )

            parts, log_probs = [], []
            for seg in segments:
                text      = seg.text.strip()
                no_speech = getattr(seg, "no_speech_prob", None)
                avg_lp    = getattr(seg, "avg_logprob", None)
                comp_r    = getattr(seg, "compression_ratio", None)
                # Per-segment diagnostic log — helps distinguish Whisper filters
                # from too-aggressive threshold settings during calibration.
                ns_str = f"{no_speech:.3f}" if no_speech is not None else "N/A"
                lp_str = f"{avg_lp:.3f}"    if avg_lp    is not None else "N/A"
                cr_str = f"{comp_r:.2f}"    if comp_r    is not None else "N/A"
                print(
                    f"  🔍 [{session_id}] seg [{seg.start:.2f}s–{seg.end:.2f}s] "
                    f"no_speech={ns_str} logprob={lp_str} "
                    f"compression={cr_str} text={repr(text)}"
                )
                if text:
                    parts.append(text)
                if avg_lp is not None:
                    log_probs.append(avg_lp)

            if not parts:
                print(
                    f"  🔕 [{session_id}] All segments filtered by Whisper thresholds "
                    f"(no_speech≥{WHISPER_NO_SPEECH_THRESHOLD} or "
                    f"logprob≤{WHISPER_LOG_PROB_THRESHOLD})"
                )
                transcript = "[silence]"
            else:
                transcript = " ".join(parts)

            avg_log_prob = sum(log_probs) / len(log_probs) if log_probs else None
            return transcript, avg_log_prob

        except Exception as e:
            print(f"  ❌ [{session_id}] Transcription error: {e}")
            return f"[transcription error: {e}]", None

    # ── Helpers ────────────────────────────────────────────────────────────
    def _recv_exact(self, sock: socket.socket, n: int):
        data = b""
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def _save_wav(self, audio_data: bytes, session_id: int,
                  sr: int, ch: int, bits: int) -> str:
        os.makedirs(AUDIO_DIR, exist_ok=True)
        ts   = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = os.path.join(AUDIO_DIR, f"session_{session_id:04d}_{ts}.wav")
        with wave.open(path, "wb") as wf:
            wf.setnchannels(ch)
            wf.setsampwidth(bits // 8)
            wf.setframerate(sr)
            wf.writeframes(audio_data)
        return path

    def _save_log(self):
        with open(LOG_FILE, "w") as f:
            json.dump(self.log_entries, f, indent=2)

    def _get_local_ip(self) -> str:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"


# ─── Test Mode ────────────────────────────────────────────────────────────────
def test_with_file(filepath: str, port: int = DEFAULT_PORT):
    """
    Send a WAV file to the server as if it came from the ESP32 in live-streaming mode.
    Sends audio in 30ms chunks to simulate real hardware pacing.
    """
    import soundfile as sf  # type: ignore
    audio, sr = sf.read(filepath, dtype="int16")
    if audio.ndim > 1:
        audio = audio[:, 0]
    audio_bytes = audio.tobytes()

    # audio_len=0 → streaming mode (server reads until we close write side)
    header = struct.pack(HEADER_FORMAT, MAGIC_NUMBER, sr, 1, 16, 0, 0)
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", port))
    sock.sendall(header)

    # Send in 30ms chunks to simulate live ESP32 pacing
    chunk_bytes = int(sr * 0.03) * 2   # 30ms of int16 samples
    for i in range(0, len(audio_bytes), chunk_bytes):
        sock.sendall(audio_bytes[i:i + chunk_bytes])
        time.sleep(0.03)

    sock.shutdown(socket.SHUT_WR)

    resp = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        resp += chunk
    sock.close()

    # Streaming mode sends an early confirmation event followed by final ASR
    # JSON.  Display the final response, not the acknowledgement event.
    messages = [json.loads(line) for line in resp.decode("utf-8").splitlines() if line.strip()]
    result = next(message for message in reversed(messages) if "transcript" in message)
    print(f"\n  📝 Transcript:   {result['transcript']}")
    print(f"  ⏱️  End-to-end:  {result['end_to_end_ms']}ms")
    print(f"  ⏱️  Transcribe:  {result['transcribe_ms']}ms")
    print(f"  🤖 Engine:      {result.get('asr_engine', '?')}")


# ─── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Hey Vaani — Cloud ASR Server (faster-whisper, firmware-compatible)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Whisper model tradeoffs (i5 laptop CPU, 2s audio):
  tiny   ~200ms — recommended for live demos
  base   ~450ms — better accuracy on noisy audio
  small  ~1200ms — too slow without GPU

Deploy to Oracle Cloud:
  1. SSH into your OCI instance
  2. pip install faster-whisper numpy soundfile
  3. python server.py --whisper-model tiny
  4. Set CONFIG_SERVER_IP in ESP32 firmware to the OCI public IP
        """,
    )
    parser.add_argument("--port",          type=int, default=DEFAULT_PORT,
                        help=f"TCP port for ESP32 audio stream (default {DEFAULT_PORT})")
    parser.add_argument("--whisper-model", type=str, default="base",
                        choices=["tiny", "base", "small", "medium"],
                        help="Whisper model size (default: base)")
    parser.add_argument("--test",          type=str, metavar="WAV_FILE",
                        help="Test by streaming a WAV file as if from ESP32")
    args = parser.parse_args()

    if args.test:
        test_with_file(args.test, args.port)
    else:
        server = ASRServer(
            port=args.port,
            whisper_model=args.whisper_model,
            dashboard_port=DASHBOARD_API_PORT,
        )
        server.start()


if __name__ == "__main__":
    main()
