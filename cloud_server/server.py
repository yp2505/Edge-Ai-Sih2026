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
import time
import json
import struct
import socket
import wave
import argparse
import threading
from datetime import datetime
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
VAD_MIN_AUDIO_MS  = 500      # wait at least 500ms (keyword echo + command start)

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
        body = json.dumps(data, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self): self._send_json({})

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
                # Live-streaming mode — read 30ms chunks with server-side VAD.
                # Stop early when the user has clearly stopped speaking instead
                # of waiting the full COMMAND_DURATION_MS (2000ms).
                consecutive_silence_ms = 0
                total_audio_ms         = 0

                while True:
                    chunk = client_socket.recv(960)   # 960B ≈ 30ms at 16kHz int16
                    if not chunk:
                        break
                    if first_chunk:
                        first_audio_byte_ms = int(time.time() * 1000)
                        first_chunk = False
                    audio_data += chunk
                    total_audio_ms += 30

                    # ── Server-side VAD ────────────────────────────────────
                    # Measure RMS of this 30ms chunk to decide if it's speech
                    # or silence. Only apply VAD after VAD_MIN_AUDIO_MS to
                    # avoid cutting off before the command word starts.
                    if total_audio_ms >= VAD_MIN_AUDIO_MS:
                        samples = np.frombuffer(chunk, dtype=np.int16).astype(np.float32)
                        rms = np.sqrt(np.mean(samples ** 2)) / 32768.0

                        if rms < VAD_RMS_THRESHOLD:
                            consecutive_silence_ms += 30
                            if consecutive_silence_ms >= VAD_SILENCE_MS:
                                # User stopped speaking — don't wait for the full
                                # COMMAND_DURATION_MS. Run Whisper immediately.
                                print(f"  🔇 [{session_id}] Silence detected "
                                      f"after {total_audio_ms}ms — cutting stream early")
                                break
                        else:
                            consecutive_silence_ms = 0  # reset on speech chunk

            audio_duration = len(audio_data) / (sr * ch * (bits // 8)) if audio_data else 0
            receive_gap_ms = (first_audio_byte_ms or int(time.time()*1000)) - connection_accepted_ms

            print(f"  📊 [{session_id}] {len(audio_data):,}B ({audio_duration:.2f}s) "
                  f"| gap={receive_gap_ms}ms")

            # ── Step 3: Save WAV & Transcribe ─────────────────────────────
            wav_path = self._save_wav(audio_data, session_id, sr, ch, bits)

            print(f"  🗣️  [{session_id}] Transcribing with faster-whisper '{self.whisper_model}'...")
            t_t0 = time.time()
            transcript, avg_log_prob = self._transcribe(wav_path)
            transcribe_ms = int((time.time() - t_t0) * 1000)

            end_to_end_ms  = kw_to_connect_ms + receive_gap_ms + transcribe_ms
            total_server_ms = int(time.time() * 1000) - connection_accepted_ms

            print(f"\n  {'─' * 58}")
            print(f"  📝 [{session_id}] TRANSCRIPT: \"{transcript}\"")
            print(f"  ⏱️  [{session_id}] kw→connect   (ESP32):   {kw_to_connect_ms}ms")
            print(f"  ⏱️  [{session_id}] connect→1st byte:        {receive_gap_ms}ms")
            print(f"  ⏱️  [{session_id}] Whisper '{self.whisper_model}':   {transcribe_ms}ms")
            print(f"  ⏱️  [{session_id}] END-TO-END:              {end_to_end_ms}ms")
            print(f"  {'─' * 58}\n")

            # ── Step 4: Send JSON response to ESP32 ───────────────────────
            response = json.dumps({
                "transcript":    transcript,
                "end_to_end_ms": end_to_end_ms,
                "transcribe_ms": transcribe_ms,
                "session_id":    session_id,
                "asr_engine":    f"faster-whisper-{self.whisper_model}",
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
                "audio_duration_s":       round(audio_duration, 3),
                "sample_rate":            sr,
                "transcript":             transcript,
                "avg_log_prob":           round(avg_log_prob, 4) if avg_log_prob else None,
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
            }
            with self._lock:
                self.log_entries.append(log_entry)
            self._save_log()

        except Exception as e:
            print(f"  ❌ [{session_id}] Unhandled error: {e}")
        finally:
            client_socket.close()

    # ── Transcription ──────────────────────────────────────────────────────
    def _transcribe(self, wav_path: str) -> tuple:
        """Run faster-whisper on a saved WAV file. Returns (transcript, avg_log_prob)."""
        try:
            segments, _ = self.transcriber.transcribe(
                wav_path,
                beam_size=5,
                language="en",
                vad_filter=True,
            )
            parts, log_probs = [], []
            for seg in segments:
                if seg.text.strip():
                    parts.append(seg.text.strip())
                if hasattr(seg, "avg_logprob"):
                    log_probs.append(seg.avg_logprob)
            transcript   = " ".join(parts) if parts else "[silence]"
            avg_log_prob = sum(log_probs) / len(log_probs) if log_probs else None
            return transcript, avg_log_prob
        except Exception as e:
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

    result = json.loads(resp.decode("utf-8"))
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
    parser.add_argument("--whisper-model", type=str, default="tiny",
                        choices=["tiny", "base", "small", "medium"],
                        help="Whisper model size (default: tiny)")
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
