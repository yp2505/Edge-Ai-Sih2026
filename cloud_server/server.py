#!/usr/bin/env python3
"""
cloud_server/server.py — CANONICAL Cloud ASR Server for Hey Vaani

This is the ONE active server wired to the ESP32 firmware:
  ESP32 (main.cpp) → TCP socket → HVP1 header → raw PCM → THIS SERVER → Whisper → transcript

Protocol: 20-byte HVP1 header (magic 0x48565031) followed by raw PCM audio.
ASR: faster-whisper (Whisper INT8, runs fully offline on CPU).

Legacy prototype: cloud_server/legacy_prototypes/asr_server_vosk_websocket.py
(NOT connected to firmware — do not use for demos)

Usage:
    python server.py                        # Start on port 5000, Whisper 'tiny' (recommended)
    python server.py --whisper-model base   # Larger/slower model
    python server.py --port 8080
    python server.py --test audio.wav       # Test by sending a WAV as if from ESP32

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

import numpy as np

# ─── Startup faster-whisper check (FAIL LOUDLY if missing) ───────────────────
# This must happen at import time, NOT lazily, to prevent silent mock transcription
# during a live demo. If this fails, fix it before running the server.
try:
    from faster_whisper import WhisperModel  # type: ignore
    _WHISPER_AVAILABLE = True
except ImportError:
    _WHISPER_AVAILABLE = False
    _WHISPER_IMPORT_ERROR = (
        "\n"
        "╔══════════════════════════════════════════════════════════════╗\n"
        "║   ❌  FATAL: faster-whisper NOT INSTALLED                    ║\n"
        "║                                                              ║\n"
        "║   The server CANNOT transcribe audio without it.            ║\n"
        "║   Running anyway would give '[mock transcription]' output   ║\n"
        "║   with no warning — NEVER acceptable in a live demo.        ║\n"
        "║                                                              ║\n"
        "║   FIX:  pip install faster-whisper                          ║\n"
        "║         (or: uv pip install faster-whisper)                 ║\n"
        "╚══════════════════════════════════════════════════════════════╝\n"
    )
    print(_WHISPER_IMPORT_ERROR, file=sys.stderr)
    sys.exit(1)   # Hard exit — do not start with silent mock mode


# ─── Configuration ─────────────────────────────────────────────────────────
DEFAULT_PORT    = 5000
SAMPLE_RATE     = 16000
CHANNELS        = 1
SAMPLE_WIDTH    = 2   # 16-bit PCM

AUDIO_DIR = os.path.join(os.path.dirname(__file__), "received_audio")
LOG_FILE  = os.path.join(os.path.dirname(__file__), "server_log.json")

# ─── HVP1 Protocol Header ──────────────────────────────────────────────────
# 20-byte header sent by ESP32 before every audio stream:
#   bytes  0-3:  magic         uint32  0x48565031 ("HVP1")
#   bytes  4-7:  sample_rate   uint32  e.g. 16000
#   bytes  8-9:  channels      uint16  1
#   bytes 10-11: bits          uint16  16
#   bytes 12-15: audio_len     uint32  total PCM bytes (0 = stream-until-close)
#   bytes 16-19: keyword_end_to_connect_ms  uint32  ← latency field (see below)
#
# keyword_end_to_connect_ms: milliseconds elapsed on the ESP32's own monotonic
# clock (esp_timer_get_time) between "keyword confirmed" and "TCP connect()
# returned". This avoids any need for NTP/synced clocks for the latency metric.
# Total end-to-end latency = keyword_end_to_connect_ms + server-measured receive gap.

MAGIC_NUMBER  = 0x48565031   # "HVP1"
HEADER_FORMAT = "<IIHHII"    # magic, sr, ch, bits, audio_len, kw_to_connect_ms
HEADER_SIZE   = struct.calcsize(HEADER_FORMAT)   # 20 bytes

# Max sane audio length (10 seconds × 16kHz × 2 bytes = 320KB)
MAX_AUDIO_BYTES = 320_000
# Min magic number sanity
VALID_SAMPLE_RATES = {8000, 16000, 22050, 44100, 48000}


class ASRServer:
    """TCP server that receives HVP1 audio streams from ESP32 and transcribes."""

    def __init__(self, port: int = DEFAULT_PORT, whisper_model: str = "tiny"):
        self.port           = port
        self.whisper_model  = whisper_model
        self.transcriber    = None
        self.session_count  = 0
        self._lock          = threading.Lock()
        self.log_entries: list = []

    def start(self):
        """Load Whisper, open TCP socket, serve forever."""
        self._load_whisper()
        os.makedirs(AUDIO_DIR, exist_ok=True)

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
        print(f"  ASR engine:      faster-whisper '{self.whisper_model}' (INT8)")
        print(f"  Protocol:        HVP1 v1 — raw TCP, 20-byte header")
        print(f"  Log:             {LOG_FILE}")
        print("=" * 62)
        print(f"\n  ⚙️  Configure ESP32 with:")
        print(f"      CONFIG_SERVER_IP   = \"{local_ip}\"")
        print(f"      CONFIG_SERVER_PORT = {self.port}")
        print(f"\n  ⚠️  NOTE: asr_server.py (Vosk/WebSocket) is a DEPRECATED prototype.")
        print(f"      The firmware does NOT connect to it. Use THIS server only.\n")
        print(f"  Waiting for ESP32 connections...\n")

        try:
            while True:
                client_socket, client_addr = server_socket.accept()
                with self._lock:
                    self.session_count += 1
                    sid = self.session_count
                print(f"  📡 [{sid}] Connection from {client_addr[0]}:{client_addr[1]}")
                thread = threading.Thread(
                    target=self._handle_client,
                    args=(client_socket, client_addr, sid),
                    daemon=True
                )
                thread.start()
        except KeyboardInterrupt:
            print("\n\n  🛑 Server shutting down...")
            self._save_log()
            server_socket.close()

    # ── Whisper Loading ────────────────────────────────────────────────────
    def _load_whisper(self):
        print(f"  Loading faster-whisper model '{self.whisper_model}'...")
        t0 = time.time()
        self.transcriber = WhisperModel(
            self.whisper_model,
            device="cpu",
            compute_type="int8"
        )
        load_ms = (time.time() - t0) * 1000
        print(f"  ✅ Whisper '{self.whisper_model}' loaded in {load_ms:.0f}ms (INT8, CPU)")

    # ── Client Handler ─────────────────────────────────────────────────────
    def _handle_client(self, client_socket: socket.socket,
                        client_addr: tuple, session_id: int):
        """Handle one ESP32 connection end-to-end."""
        # Server-side latency anchor — time connection was accepted
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

            # Hard validation — reject corrupted streams before they crash the server
            if magic != MAGIC_NUMBER:
                print(f"  ❌ [{session_id}] Bad magic: 0x{magic:08X} (expected 0x{MAGIC_NUMBER:08X}). "
                      f"Closing — corrupted or wrong client.")
                return

            if sr not in VALID_SAMPLE_RATES:
                print(f"  ❌ [{session_id}] Implausible sample rate: {sr}Hz. Closing.")
                return

            if audio_len > MAX_AUDIO_BYTES:
                print(f"  ❌ [{session_id}] Claimed audio_len {audio_len} exceeds "
                      f"safety limit {MAX_AUDIO_BYTES}. Closing.")
                return

            print(f"  📋 [{session_id}] HVP1: {sr}Hz/{ch}ch/{bits}bit "
                  f"len={audio_len} kw_to_connect={kw_to_connect_ms}ms")

            # ── Step 2: Receive audio (log first-byte timestamp) ──────────
            audio_data = b""
            first_chunk = True
            if audio_len > 0:
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
                # Streaming mode — read until peer closes
                while True:
                    chunk = client_socket.recv(8192)
                    if not chunk:
                        break
                    if first_chunk:
                        first_audio_byte_ms = int(time.time() * 1000)
                        first_chunk = False
                    audio_data += chunk

            receive_done_ms = int(time.time() * 1000)
            audio_duration  = len(audio_data) / (sr * ch * (bits // 8))
            receive_gap_ms  = (first_audio_byte_ms or receive_done_ms) - connection_accepted_ms

            print(f"  📊 [{session_id}] {len(audio_data):,}B ({audio_duration:.2f}s) "
                  f"| connection→first_byte: {receive_gap_ms}ms")

            # ── Step 3: Save WAV ───────────────────────────────────────────
            wav_path = self._save_wav(audio_data, session_id, sr, ch, bits)

            # ── Step 4: Transcribe ─────────────────────────────────────────
            print(f"  🗣️  [{session_id}] Transcribing...")
            t_t0 = time.time()
            transcript, avg_log_prob = self._transcribe(wav_path)
            transcribe_ms = int((time.time() - t_t0) * 1000)

            total_server_ms = int(time.time() * 1000) - connection_accepted_ms

            # Total end-to-end latency using single-clock method:
            # = ESP32's own pre-connection delay + server-measured gap
            # (no NTP, no synced clocks needed)
            end_to_end_ms = kw_to_connect_ms + receive_gap_ms + transcribe_ms

            print(f"\n  {'─' * 55}")
            print(f"  📝 [{session_id}] TRANSCRIPT: \"{transcript}\"")
            print(f"  ⏱️  [{session_id}] kw→socket_open (ESP32): {kw_to_connect_ms}ms")
            print(f"  ⏱️  [{session_id}] connect→first_byte (server): {receive_gap_ms}ms")
            print(f"  ⏱️  [{session_id}] Transcribe: {transcribe_ms}ms")
            print(f"  ⏱️  [{session_id}] END-TO-END: {end_to_end_ms}ms")
            print(f"  {'─' * 55}\n")

            # ── Step 5: Send JSON response to ESP32 ────────────────────────
            response = json.dumps({
                "transcript":      transcript,
                "end_to_end_ms":   end_to_end_ms,
                "transcribe_ms":   transcribe_ms,
                "session_id":      session_id,
                "asr_engine":      f"faster-whisper-{self.whisper_model}"
            }).encode("utf-8")
            try:
                client_socket.sendall(response)
            except Exception:
                pass  # ESP32 may have already closed connection

            # ── Step 6: Append to log (format consumed by latency_analyzer.py) ─
            log_entry = {
                "session_id":                   session_id,
                "timestamp":                    datetime.now().isoformat(),
                "client_ip":                    client_addr[0],
                "audio_bytes":                  len(audio_data),
                "audio_duration_s":             round(audio_duration, 3),
                "sample_rate":                  sr,
                "transcript":                   transcript,
                "avg_log_prob":                 round(avg_log_prob, 4) if avg_log_prob else None,
                # Single-clock latency components (no NTP needed)
                "kw_to_connect_ms":             kw_to_connect_ms,    # ESP32's own clock
                "connection_accepted_ms":       connection_accepted_ms,
                "first_audio_byte_ms":          first_audio_byte_ms,
                "receive_gap_ms":               receive_gap_ms,
                "transcribe_ms":                transcribe_ms,
                "total_server_ms":              total_server_ms,
                "end_to_end_ms":                end_to_end_ms,
                # Kept for latency_analyzer.py compatibility
                "cloud_receive_timestamp_ms":   first_audio_byte_ms,
            }
            with self._lock:
                self.log_entries.append(log_entry)
            self._save_log()

        except Exception as e:
            print(f"  ❌ [{session_id}] Unhandled error: {e}")
        finally:
            client_socket.close()

    # ── Transcription ──────────────────────────────────────────────────────
    def _transcribe(self, wav_path: str) -> tuple[str, float | None]:
        """Run faster-whisper on wav_path. Returns (transcript, avg_log_prob)."""
        try:
            segments, info = self.transcriber.transcribe(
                wav_path,
                beam_size=5,
                language="en",
                vad_filter=True,
            )
            parts = []
            avg_lp = []
            for seg in segments:
                if seg.text.strip():
                    parts.append(seg.text.strip())
                if hasattr(seg, "avg_logprob"):
                    avg_lp.append(seg.avg_logprob)
            transcript = " ".join(parts) if parts else "[silence]"
            avg_log_prob = sum(avg_lp) / len(avg_lp) if avg_lp else None
            return transcript, avg_log_prob
        except Exception as e:
            return f"[transcription error: {e}]", None

    # ── Helpers ────────────────────────────────────────────────────────────
    def _recv_exact(self, sock: socket.socket, n: int) -> bytes | None:
        data = b""
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def _save_wav(self, audio_data: bytes, session_id: int,
                  sr: int, ch: int, bits: int) -> str:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
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
def test_with_file(filepath: str, port: int = DEFAULT_PORT, model: str = "tiny"):
    """Send a WAV file to the server as if it came from ESP32. For local testing."""
    import soundfile as sf  # type: ignore
    audio, sr = sf.read(filepath, dtype="int16")
    if audio.ndim > 1:
        audio = audio[:, 0]
    audio_bytes = audio.tobytes()

    # Build HVP1 header (kw_to_connect_ms=0 for test mode)
    header = struct.pack(HEADER_FORMAT,
        MAGIC_NUMBER, sr, 1, 16,
        len(audio_bytes),
        0   # kw_to_connect_ms not measured in test mode
    )
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", port))
    sock.sendall(header + audio_bytes)
    sock.shutdown(socket.SHUT_WR)

    resp = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        resp += chunk
    sock.close()

    result = json.loads(resp.decode("utf-8"))
    print(f"\n  📝 Transcript:    {result['transcript']}")
    print(f"  ⏱️  End-to-end:   {result['end_to_end_ms']}ms")
    print(f"  ⏱️  Transcribe:   {result['transcribe_ms']}ms")


# ─── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Hey Vaani — Cloud ASR Server (canonical, firmware-compatible)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Whisper model tradeoffs (measured on i5-7th Gen laptop, 2s audio):
  tiny   ~200ms transcription — recommended for low-latency demos
  base   ~450ms transcription — better accuracy, noticeably slower on CPU
  small  ~1200ms — too slow for live demo without GPU

Default changed to 'tiny'. Use --whisper-model base only if you see bad accuracy.
        """
    )
    parser.add_argument("--port",           type=int, default=DEFAULT_PORT)
    parser.add_argument("--whisper-model",  type=str, default="tiny",
                        choices=["tiny", "base", "small", "medium"])
    parser.add_argument("--test",           type=str, metavar="WAV_FILE",
                        help="Test by sending a WAV file as if from ESP32")
    args = parser.parse_args()

    if args.test:
        test_with_file(args.test, args.port, args.whisper_model)
    else:
        server = ASRServer(port=args.port, whisper_model=args.whisper_model)
        server.start()


if __name__ == "__main__":
    main()
