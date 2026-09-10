#!/usr/bin/env python3
"""
cloud_server/server.py — CANONICAL Cloud ASR Server for Hey Vaani

Pipeline:
  ESP32 → TCP socket → HVP1 header → raw PCM (live-streamed) → THIS SERVER
  → ASR (Vosk offline OR faster-whisper) → transcript
  → Intent Engine (Ollama SLM, optional) → JSON response → ESP32

Protocol: 20-byte HVP1 header (magic 0x48565031).
  audio_len = 0  →  live-streaming mode (ESP32 sends 30ms chunks until TCP close)
  audio_len > 0  →  batch mode (legacy / --test mode)

ASR Engines (all open-source, fully offline):
  --asr whisper  (default) — faster-whisper INT8, no account needed.
  --asr vosk               — Vosk offline ASR, lighter weight.

Intent Engine (optional, requires Ollama running locally):
  --intent                 — Enable Ollama SLM intent extraction after ASR.

Usage:
    python server.py                          # Whisper, no intent engine
    python server.py --asr vosk               # Vosk ASR
    python server.py --asr vosk --intent      # Vosk + SLM intent
    python server.py --whisper-model base     # larger Whisper model
    python server.py --port 8080
    python server.py --test audio.wav

Install:
    pip install faster-whisper vosk numpy soundfile requests
    # For intent: install Ollama + pull qwen2.5:0.5b
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

# ─── AES-128-CTR Encryption ───────────────────────────────────────────────────
# All audio (ESP32 → server) and responses (server → ESP32) are encrypted
# with AES-128-CTR using a pre-shared key.  No licence, no API key.
# Key source (in order of priority):
#   1. HV_AES_KEY environment variable (hex string, 32 chars = 16 bytes)
#   2. Hardcoded demo key below   ← CHANGE THIS before production deployment
#
# Protocol:
#   [20-byte HVP1 header] [16-byte nonce] [encrypted audio...]
#   [encrypted JSON response]
# Nonces:
#   Audio    nonce: 16 random bytes sent by ESP32 right after HVP1 header
#   Response nonce: audio_nonce with last byte XOR 0xFF (both sides derive it)
try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # type: ignore
    from cryptography.hazmat.backends import default_backend as _crypto_backend  # type: ignore
    _CRYPTO_AVAILABLE = True
except ImportError:
    _CRYPTO_AVAILABLE = False

# 16-byte AES-128 key.  Must EXACTLY match AES_KEY[] in main.cpp.
_HV_AES_KEY_HEX = os.environ.get("HV_AES_KEY", "48657956616e6e69534948323032362a")
try:
    AES_KEY = bytes.fromhex(_HV_AES_KEY_HEX)
    assert len(AES_KEY) == 16, f"HV_AES_KEY must be 32 hex chars (16 bytes), got {len(AES_KEY)}"
except Exception as e:
    print(f"[AES] Key error: {e} — using default key")
    AES_KEY = b'HeyVaaniSIH2026*'  # fallback


def _aes_ctr_cipher(nonce: bytes):
    """Return a fresh AES-128-CTR Cipher object for the given 16-byte nonce."""
    if not _CRYPTO_AVAILABLE:
        raise RuntimeError("cryptography library not installed — run: pip install cryptography")
    return Cipher(algorithms.AES(AES_KEY), modes.CTR(nonce), backend=_crypto_backend())


def _response_nonce(audio_nonce: bytes) -> bytes:
    """Derive response nonce from audio nonce: flip last byte.
    Both ESP32 and server compute this independently — no extra transmission needed."""
    n = bytearray(audio_nonce)
    n[15] ^= 0xFF
    return bytes(n)

# ─── Server-side VAD (Silence Detection) ─────────────────────────────────────
# ⚡ LATENCY-OPTIMISED for <200ms total end-to-end (rev8)
#   VAD_SILENCE_MS dropped 350→100ms (matches ESP32 COMMAND_SILENCE_MS=100ms)
#   VAD_MIN_AUDIO_MS dropped 1500→300ms (no need to wait 1.5s ring buffer)
#
VAD_RMS_THRESHOLD = 0.008    # 0.8% of full scale
VAD_SILENCE_MS    = 100      # stop after 100ms consecutive silence (⚡ was 350)
VAD_MIN_AUDIO_MS  = 300      # accept stream after 300ms minimum (⚡ was 1500)

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

# ─── ASR backend imports ──────────────────────────────────────────────────────
# The server supports two open-source, fully-offline ASR backends:
#
#   --asr whisper  (default) — faster-whisper running locally.
#                              Free, offline, no account needed.
#                              pip install faster-whisper
#
#   --asr vosk               — Vosk offline ASR engine.
#                              Lighter weight, lower latency on CPU.
#                              pip install vosk
#                              Download model: https://alphacephei.com/vosk/models
#                              Place in: cloud_server/vosk_model/
#
# Intent Engine (--intent flag):
#   After ASR transcription, optionally call a local Ollama SLM to extract
#   structured intent (action, target, value) from the user's command.
#   Requires: ollama running on localhost:11434
#             ollama pull qwen2.5:0.5b
#
# All backends are open-source and run 100% offline — no cloud services needed.

try:
    from faster_whisper import WhisperModel  # type: ignore
    _WHISPER_AVAILABLE = True
except ImportError:
    _WHISPER_AVAILABLE = False

try:
    import vosk  # type: ignore
    _VOSK_AVAILABLE = True
except ImportError:
    _VOSK_AVAILABLE = False

try:
    import requests as _requests  # used for Ollama intent API
    _REQUESTS_AVAILABLE = True
except ImportError:
    _REQUESTS_AVAILABLE = False

try:
    from amazon_transcribe.client import TranscribeStreamingClient  # type: ignore
    _AWS_AVAILABLE = True
except ImportError:
    _AWS_AVAILABLE = False

try:
    import psutil as _psutil  # used for server CPU/RAM metrics
    _PSUTIL_AVAILABLE = True
except ImportError:
    _PSUTIL_AVAILABLE = False


# ─── Persistent state (in-memory + file) ─────────────────────────────────────
import json as _json_module

# WiFi provisioning config (persisted to wifi_config.json)
_WIFI_CONFIG: dict = {}
_WIFI_CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wifi_config.json")
try:
    with open(_WIFI_CONFIG_FILE) as _f:
        _WIFI_CONFIG = _json_module.load(_f)
except Exception:
    pass

# ESP32 wireless log ring buffer (last 500 entries)
_ESP_LOG_BUFFER: list = []
_ESP_LOG_LOCK = threading.Lock()
_ESP_LOG_MAX = 300

def _esp_log_append(level: str, tag: str, msg: str):
    """Append a log line to the ring buffer (thread-safe)."""
    entry = {
        "ts": datetime.now().isoformat(timespec="milliseconds"),
        "level": level,
        "tag": tag,
        "msg": msg,
    }
    with _ESP_LOG_LOCK:
        _ESP_LOG_BUFFER.append(entry)
        if len(_ESP_LOG_BUFFER) > _ESP_LOG_MAX:
            _ESP_LOG_BUFFER.pop(0)

# ─── ASR backend + intent selection ──────────────────────────────────────────
# Set by CLI flags in main().
ASR_ENGINE    = "whisper"   # overridden by --asr flag
INTENT_ENABLE = False       # overridden by --intent flag
OLLAMA_MODEL  = "qwen2.5:0.5b"   # small, fast, ~400MB RAM
OLLAMA_URL    = "http://localhost:11434/api/generate"


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
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
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
        path = self.path.split("?", 1)[0]
        if path not in {"/api/telemetry", "/api/wifi-config", "/api/esp-logs"}:
            self._send_json({"error": "not found"}, status=404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(data, dict): raise ValueError("expected object")
            if path == "/api/telemetry":
                data["received_at"] = datetime.now().isoformat()
                asr = self.server.asr_server
                with asr._lock: asr.telemetry = data
                self._send_json({"ok": True})
            elif path == "/api/wifi-config":
                # Save WiFi provisioning config to disk
                global _WIFI_CONFIG
                allowed = {"ssid", "password", "server_ip", "server_port"}
                next_config = {k: v for k, v in data.items() if k in allowed}
                if not next_config.get("password") and _WIFI_CONFIG.get("password"):
                    next_config["password"] = _WIFI_CONFIG["password"]
                if not next_config.get("ssid") or not next_config.get("server_ip"):
                    raise ValueError("ssid and server_ip are required")
                try:
                    next_config["server_port"] = int(next_config.get("server_port", 5000))
                    if not 1 <= next_config["server_port"] <= 65535:
                        raise ValueError
                except (TypeError, ValueError):
                    raise ValueError("server_port must be between 1 and 65535")
                _WIFI_CONFIG = next_config
                try:
                    with open(_WIFI_CONFIG_FILE, "w") as wf:
                        json.dump(_WIFI_CONFIG, wf, indent=2)
                    _esp_log_append("I", "WIFI-CFG", f"New WiFi config saved: SSID={_WIFI_CONFIG.get('ssid', '?')} IP={_WIFI_CONFIG.get('server_ip', '?')}:{_WIFI_CONFIG.get('server_port', '?')}")
                    self._send_json({"ok": True, "saved": True})
                except Exception as e:
                    self._send_json({"error": str(e)}, status=500)
            elif path == "/api/esp-logs":
                # Accept ESP32 log POST (for wireless logging firmware patch)
                entries = data.get("entries", [])
                if isinstance(entries, list) and entries:
                    for e in entries:
                        _esp_log_append(
                            e.get("level", "I"),
                            e.get("tag", "ESP32"),
                            e.get("msg", ""),
                        )
                elif "msg" in data:
                    _esp_log_append(data.get("level", "I"), data.get("tag", "ESP32"), data["msg"])
                self._send_json({"ok": True})
            else:
                self._send_json({"error": "not found"}, status=404)
        except Exception as e:
            self._send_json({"error": str(e)}, status=400)

    def do_GET(self):
        asr: "ASRServer" = self.server.asr_server
        path = self.path.split("?")[0]
        if path == "/api/health":
            with asr._lock:
                count = len(asr.log_entries)
            resp = {
                "status":         "running",
                "uptime_seconds": int(time.time() - asr.start_time),
                "session_count":  count,
                "asr_engine":     f"faster-whisper-{asr.whisper_model}",
            }
            # Server CPU / RAM via psutil
            if _PSUTIL_AVAILABLE:
                try:
                    resp["server_cpu_pct"]  = _psutil.cpu_percent(interval=None)
                    vm = _psutil.virtual_memory()
                    resp["server_ram_pct"]  = vm.percent
                    resp["server_ram_mb"] = round(vm.used / 1024 / 1024, 1)
                    resp["server_ram_used_mb"] = round(vm.used / 1024 / 1024, 1)
                    resp["server_ram_total_mb"] = round(vm.total / 1024 / 1024, 1)
                except Exception:
                    pass
            self._send_json(resp)
        elif path == "/api/telemetry":
            with asr._lock:
                telemetry = dict(asr.telemetry)
            self._send_json(telemetry)
        elif path == "/api/events":
            with asr._lock:
                entries = list(asr.log_entries)
            self._send_json(entries)
        elif path == "/api/realtime":
            offset_ms = int(5.5 * 3600 * 1000)
            epoch_ms = int(time.time() * 1000) + offset_ms
            self._send_json({"ok": True, "epochMs": epoch_ms, "timeZone": "Asia/Kolkata", "offsetMs": offset_ms})
        elif path == "/api/wifi-config":
            # Return stored WiFi config (password masked), auto-filling server IP if unset
            safe = dict(_WIFI_CONFIG)
            if not safe.get("server_ip"):
                safe["server_ip"] = self.server.asr_server._get_local_ip()
            if not safe.get("server_port"):
                safe["server_port"] = 5000
            if "password" in safe and safe["password"]:
                safe["password"] = "*" * len(safe["password"])
            self._send_json(safe)
        elif path == "/api/esp-logs":
            # Return ESP32 / server log ring buffer
            since_idx = 0
            qs = self.path.split("?", 1)
            if len(qs) > 1:
                for part in qs[1].split("&"):
                    if part.startswith("since="):
                        try: since_idx = int(part[6:])
                        except ValueError: pass
            with _ESP_LOG_LOCK:
                total = len(_ESP_LOG_BUFFER)
                entries = list(_ESP_LOG_BUFFER[since_idx:])
            self._send_json({"total": total, "entries": entries})
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

    def __init__(self, port: int = DEFAULT_PORT,
                 whisper_model: str = "tiny",
                 dashboard_port: int = DASHBOARD_API_PORT,
                 asr_engine: str = "whisper",
                 intent_enable: bool = False):
        self.port          = port
        self.dashboard_port = dashboard_port
        self.whisper_model  = whisper_model
        self.asr_engine    = asr_engine
        self.intent_enable = intent_enable
        self.transcriber    = None
        self.session_count  = 0
        self.start_time     = time.time()
        self._lock          = threading.Lock()
        self.log_entries: list = []
        self.telemetry: dict = {}

    def start(self):
        self._load_asr()
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
        if self.asr_engine == "aws":
            print(f"  ASR engine:      ☁️  Amazon Transcribe Streaming (en-IN, ap-south-1)")
            print(f"  AWS free tier:   60 min/month — ~1,800 free detections/month")
        else:
            print(f"  ASR engine:      faster-whisper '{self.whisper_model}' (INT8, CPU)")
        print("  Wake word:       custom TFLite Micro model on ESP32 (edge-authoritative)")
        print(f"  Protocol:        HVP1 v1 — 20-byte header, live-stream mode")
        print(f"  Log:             {LOG_FILE}")
        print("=" * 62)
        print(f"\n  ⚙️  Configure ESP32 with:")
        print(f"      Server IP   = \"{local_ip}\"  (enter this in HeyVaani-Setup portal)")
        print(f"      Server Port = {self.port}\n")
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
                _esp_log_append("I", "SESSION", f"Wake word detected from {client_addr[0]} (session {sid})")
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

    # ── ASR backend loader ─────────────────────────────────────────────────
    def _load_asr(self):
        self._vosk_model = None   # ⚡ pre-load cache

        if self.asr_engine == "vosk":
            if not _VOSK_AVAILABLE:
                print("  ❌ vosk not installed — run: pip install vosk", file=sys.stderr)
                sys.exit(1)
            model_path = os.path.join(os.path.dirname(__file__), "vosk_model")
            if not os.path.isdir(model_path):
                print(
                    f"  ❌ Vosk model not found at {model_path}\n"
                    "  Download from https://alphacephei.com/vosk/models\n"
                    "  Use vosk-model-small-en-us-0.15 (40MB) for best latency\n"
                    "  Unzip into cloud_server/vosk_model/",
                    file=sys.stderr,
                )
                sys.exit(1)
            print("  ⏳ Pre-loading Vosk model at startup (⚡ eliminates per-request cold-start)...")
            t0 = time.time()
            self._vosk_model = vosk.Model(model_path)
            print(f"  ✅ Vosk model loaded in {(time.time()-t0)*1000:.0f}ms — cached for all sessions")
            return

        if self.asr_engine == "aws":
            if not _AWS_AVAILABLE:
                print(
                    "\n"
                    "╔══════════════════════════════════════════════════════════════╗\n"
                    "║   ❌  amazon-transcribe SDK NOT INSTALLED                    ║\n"
                    "║                                                              ║\n"
                    "║   FIX:  pip install \"amazon-transcribe[awscrt]\"             ║\n"
                    "║   Then: aws configure  (enter your AWS key + ap-south-1)   ║\n"
                    "╚══════════════════════════════════════════════════════════════╝\n",
                    file=sys.stderr,
                )
                sys.exit(1)
            print("  ☁️  ASR backend: Amazon Transcribe Streaming (en-IN, ap-south-1)")
            print("  ℹ️  AWS free tier: 60 min/month for 12 months.")
            self.transcriber = None   # no local model needed
            return

        # Default: faster-whisper
        if not _WHISPER_AVAILABLE:
            print(
                "\n"
                "╔══════════════════════════════════════════════════════════════╗\n"
                "║   ❌  FATAL: faster-whisper NOT INSTALLED                    ║\n"
                "║                                                              ║\n"
                "║   FIX:  pip install faster-whisper                          ║\n"
                "║   OR use --asr vosk to switch to Vosk (lower latency)       ║\n"
                "╚══════════════════════════════════════════════════════════════╝\n",
                file=sys.stderr,
            )
            sys.exit(1)
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

            # ── Step 1b: Read AES nonce (16 bytes, sent right after HVP1 header) ──
            aes_nonce: bytes | None = None
            aes_decryptor = None
            if _CRYPTO_AVAILABLE:
                aes_nonce = self._recv_exact(client_socket, 16)
                if not aes_nonce or len(aes_nonce) < 16:
                    print(f"  ⚠️  [{session_id}] Missing AES nonce — falling back to plaintext")
                    aes_nonce = None
                else:
                    aes_decryptor = _aes_ctr_cipher(aes_nonce).decryptor()
                    print(f"  🔐 [{session_id}] AES-128-CTR decryptor ready "
                          f"nonce={aes_nonce[:4].hex()}...")
            else:
                print(f"  ⚠️  [{session_id}] cryptography not installed — plaintext mode")

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
                    # Decrypt batch audio if AES active
                    if aes_decryptor:
                        chunk = aes_decryptor.update(chunk)
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
                    # Decrypt chunk BEFORE VAD/accumulation — VAD operates on plaintext
                    if aes_decryptor:
                        chunk = aes_decryptor.update(chunk)
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
                engine_label = f"Vosk" if self.asr_engine == "vosk" else f"faster-whisper '{self.whisper_model}'"
                print(f"  🗣️  [{session_id}] Transcribing with {engine_label}...")
                t_t0 = time.time()
                transcript, avg_log_prob = self._transcribe(audio_data, wav_path, session_id, sr)
                transcribe_ms = int((time.time() - t_t0) * 1000)

            # Step 3: Two-step Verification Gate evaluation
            is_confirmed, match_score, matched_var = self._verify_keyword_transcript(transcript)
            verification_status = "CONFIRMED" if is_confirmed else "REJECTED"

            # ── Step 3b: Intent extraction via Ollama SLM (optional) ──────
            intent_data = None
            intent_ms   = 0
            if self.intent_enable and not transcript.startswith("["):
                t_intent = time.time()
                intent_data = self._extract_intent(transcript, session_id)
                intent_ms = int((time.time() - t_intent) * 1000)

            total_server_ms = int(time.time() * 1000) - connection_accepted_ms
            end_to_end_ms   = kw_to_connect_ms + total_server_ms

            asr_label = "Vosk" if self.asr_engine == "vosk" else f"Whisper '{self.whisper_model}'"
            print(f"\n  {'─' * 58}")
            print(f"  📝 [{session_id}] RAW TRANSCRIPT:  \"{transcript}\"")
            _esp_log_append("I", "ASR", f"Session {session_id}: {transcript or '[empty transcript]'}")
            if intent_data:
                print(f"  🤖 [{session_id}] INTENT:          {json.dumps(intent_data)}")
            print(f"  🛡️  [{session_id}] VERIFICATION GATE: {verification_status} (score={match_score:.2f}, match='{matched_var}')")
            print(f"  ⏱️  [{session_id}] kw→connect   (ESP32):   {kw_to_connect_ms}ms")
            print(f"  ⏱️  [{session_id}] connect→1st byte:        {receive_gap_ms}ms")
            print(f"  ⏱️  [{session_id}] {asr_label}:            {transcribe_ms}ms")
            if intent_ms:
                print(f"  ⏱️  [{session_id}] Intent (Ollama):         {intent_ms}ms")
            print(f"  ⏱️  [{session_id}] END-TO-END:              {end_to_end_ms}ms")
            print(f"  {'─' * 58}\n")

            # ── Step 4: Encrypt + Send JSON response to ESP32 ─────────────
            response_dict = {
                "transcript":          transcript,
                "verification_status": verification_status,
                "verified":            is_confirmed,
                "match_score":         round(match_score, 2),
                "matched_variant":     matched_var,
                "end_to_end_ms":       end_to_end_ms,
                "transcribe_ms":       transcribe_ms,
                "session_id":          session_id,
                "asr_engine":          self.asr_engine,
                "wake_word_confirmed": is_confirmed,
            }
            if intent_data:
                response_dict["intent"] = intent_data
            response: bytes = json.dumps(response_dict).encode("utf-8")

            # Encrypt response with derived nonce (audio_nonce XOR 0xFF on last byte)
            if aes_nonce:
                try:
                    resp_nonce = _response_nonce(aes_nonce)
                    encryptor  = _aes_ctr_cipher(resp_nonce).encryptor()
                    response   = encryptor.update(response) + encryptor.finalize()
                    print(f"  \U0001f512 [{session_id}] Response encrypted "
                          f"({len(response)}B, nonce={resp_nonce[:4].hex()}...)")
                except Exception as enc_err:
                    print(f"  \u26a0\ufe0f  [{session_id}] Response encryption failed: {enc_err} "
                          f"— sending plaintext")
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
                "asr_engine":             self.asr_engine,
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
            _esp_log_append("E", "SERVER", f"Session {session_id} failed: {e}")
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
    def _transcribe(self, audio_data: bytes, wav_path: str,
                    session_id: int = 0, sample_rate: int = 16000) -> tuple:
        """
        Dispatcher: routes to _transcribe_vosk() or _transcribe_whisper()
        depending on self.asr_engine.  Returns (transcript, avg_log_prob).
        """
        if self.asr_engine == "vosk":
            return self._transcribe_vosk(audio_data, session_id, sample_rate)
        return self._transcribe_whisper(wav_path, session_id)

    # ── Backend A: Vosk (local, offline, open-source) ──────────────────────
    def _transcribe_vosk(self, audio_data: bytes, session_id: int,
                         sample_rate: int = 16000) -> tuple:
        """
        Transcribe raw 16-bit PCM using Vosk offline ASR.
        Vosk runs 100% locally — no cloud, no API key, no internet required.

        ⚡ LATENCY-OPTIMISED (rev8):
          - Vosk Model is pre-loaded ONCE at server startup (self._vosk_model)
            and reused across sessions. Eliminates ~300ms cold-start per call.
          - chunk_size 4000 bytes = 125ms chunks (was 8000 = 250ms)
        """
        try:
            if not _VOSK_AVAILABLE:
                raise RuntimeError("vosk not installed — run: pip install vosk")

            # Use pre-loaded model (set in __init__) or load on first call
            if not hasattr(self, '_vosk_model') or self._vosk_model is None:
                model_path = os.path.join(os.path.dirname(__file__), "vosk_model")
                if not os.path.isdir(model_path):
                    raise RuntimeError(
                        f"Vosk model not found at {model_path}\n"
                        "  Download from https://alphacephei.com/vosk/models\n"
                        "  Unzip into cloud_server/vosk_model/"
                    )
                print("  ⏳ [Vosk] Loading model (first call)...")
                self._vosk_model = vosk.Model(model_path)
                print("  ✅ [Vosk] Model ready (cached for future calls)")

            rec = vosk.KaldiRecognizer(self._vosk_model, sample_rate)
            rec.SetWords(False)   # ⚡ skip per-word timestamps — saves ~10ms

            # Feed audio in smaller 125ms chunks for faster processing
            chunk_size = 4000  # 125ms at 16kHz int16 (⚡ was 8000=250ms)
            for i in range(0, len(audio_data), chunk_size):
                rec.AcceptWaveform(audio_data[i:i + chunk_size])

            result = json.loads(rec.FinalResult())
            transcript = result.get("text", "").strip() or "[silence]"
            print(f"  🟢 [{session_id}] Vosk result: {repr(transcript)}")
            return transcript, None

        except Exception as e:
            print(f"  ❌ [{session_id}] Vosk error: {e}")
            return f"[vosk error: {e}]", None

    # ── Intent Extraction: Ollama SLM (local, fully offline) ───────────────
    def _extract_intent(self, transcript: str, session_id: int) -> dict | None:
        """
        Send transcribed text to a locally running Ollama SLM to extract
        structured intent. Ollama runs 100% offline — no internet required.

        Setup:
          curl -fsSL https://ollama.com/install.sh | sh
          ollama pull qwen2.5:0.5b   # ~400MB, fast on CPU
          ollama serve                # runs on localhost:11434

        The SLM maps natural language → structured JSON:
          "turn on the lights" → {"intent": "LIGHTS", "action": "ON", "target": "lights"}
          "it's getting dark" → {"intent": "LIGHTS", "action": "ON", "target": "ambient"}
          "oxygen level low" → {"intent": "OXYGEN", "action": "INCREASE", "target": "flow"}
        """
        if not _REQUESTS_AVAILABLE:
            print(f"  ⚠️  [{session_id}] requests not installed — skipping intent")
            return None

        prompt = (
            "You are an AI assistant embedded in a space habitat voice control system. "
            "Extract the intent from the astronaut's voice command below.\n"
            "Reply ONLY with a single JSON object — no explanation, no markdown.\n"
            "JSON keys: intent (string), action (string), target (string), value (string or null).\n"
            "Examples:\n"
            "  Command: 'turn on the lights' → {\"intent\": \"LIGHTS\", \"action\": \"ON\", \"target\": \"lights\", \"value\": null}\n"
            "  Command: 'increase oxygen to section 2' → {\"intent\": \"OXYGEN\", \"action\": \"INCREASE\", \"target\": \"section_2\", \"value\": null}\n"
            "  Command: 'set temperature to 22 degrees' → {\"intent\": \"TEMPERATURE\", \"action\": \"SET\", \"target\": \"habitat\", \"value\": \"22\"}\n"
            "  Command: 'send status report' → {\"intent\": \"REPORT\", \"action\": \"SEND\", \"target\": \"status\", \"value\": null}\n"
            f"\nCommand: '{transcript}'\nJSON:"
        )
        try:
            resp = _requests.post(
                OLLAMA_URL,
                json={"model": OLLAMA_MODEL, "prompt": prompt, "stream": False},
                timeout=15,
            )
            resp.raise_for_status()
            raw = resp.json().get("response", "").strip()
            # Strip markdown code blocks if model wrapped the JSON
            raw = raw.strip("` \n").removeprefix("json").strip()
            intent = json.loads(raw)
            print(f"  🤖 [{session_id}] Ollama intent: {intent}")
            return intent
        except _requests.exceptions.ConnectionError:
            print(f"  ⚠️  [{session_id}] Ollama not running — start with: ollama serve")
            return None
        except json.JSONDecodeError as e:
            print(f"  ⚠️  [{session_id}] Ollama returned non-JSON: {raw!r} — {e}")
            return None
        except Exception as e:
            print(f"  ❌ [{session_id}] Intent extraction error: {e}")
            return None

    # ── Backend B: faster-whisper (local, offline, open-source) ───────────
    def _transcribe_whisper(self, wav_path: str, session_id: int = 0) -> tuple:
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
        description="Hey Vaani — Offline Cloud ASR + Intent Server",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
All ASR backends are open-source and run fully offline — no cloud, no API key.

ASR backends:
  --asr whisper  (default) — faster-whisper INT8, runs locally.
                             pip install faster-whisper
  --asr vosk               — Vosk offline ASR, lighter weight.
                             pip install vosk
                             Download model: https://alphacephei.com/vosk/models
                             Unzip into: cloud_server/vosk_model/

Intent engine (--intent flag):
  Calls a local Ollama SLM to extract structured intent from the transcript.
  Setup:
    curl -fsSL https://ollama.com/install.sh | sh
    ollama pull qwen2.5:0.5b
    ollama serve
  Then run: python server.py --asr vosk --intent

Whisper model tradeoffs (i5 laptop CPU, 2s audio):
  tiny   ~200ms — recommended for live demos
  base   ~450ms — better accuracy on noisy audio
  small  ~1200ms — too slow without GPU
        """,
    )
    parser.add_argument("--port",          type=int, default=DEFAULT_PORT,
                        help=f"TCP port for ESP32 audio stream (default {DEFAULT_PORT})")
    parser.add_argument("--whisper-model", type=str, default="base",
                        choices=["tiny", "base", "small", "medium"],
                        help="Whisper model size (default: base, only used with --asr whisper)")
    parser.add_argument("--asr",           type=str, default="whisper",
                        choices=["whisper", "vosk"],
                        help="ASR backend: 'whisper' (default, local) or 'vosk' (offline, lighter)")
    parser.add_argument("--intent",        action="store_true",
                        help="Enable Ollama SLM intent extraction after ASR (requires ollama serve)")
    parser.add_argument("--ollama-model",  type=str, default="qwen2.5:0.5b",
                        help="Ollama model to use for intent (default: qwen2.5:0.5b)")
    parser.add_argument("--test",          type=str, metavar="WAV_FILE",
                        help="Test by streaming a WAV file as if from ESP32")
    args = parser.parse_args()

    global ASR_ENGINE, INTENT_ENABLE, OLLAMA_MODEL
    ASR_ENGINE    = args.asr
    INTENT_ENABLE = args.intent
    OLLAMA_MODEL  = args.ollama_model

    if args.test:
        test_with_file(args.test, args.port)
    else:
        server = ASRServer(
            port=args.port,
            whisper_model=args.whisper_model,
            dashboard_port=DASHBOARD_API_PORT,
            asr_engine=args.asr,
            intent_enable=args.intent,
        )
        server.start()


if __name__ == "__main__":
    main()
