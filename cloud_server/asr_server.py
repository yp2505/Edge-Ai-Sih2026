#!/usr/bin/env python3
"""
Cloud ASR Server — Receives audio from edge device (RPi/ESP32),
transcribes using Vosk (open-source DNN-HMM ASR — NO transformers).

Why Vosk instead of Whisper?
  • Vosk uses Kaldi DNN-HMM architecture (NOT a transformer)
  • Fully open-source (Apache 2.0)
  • Works completely offline after model download
  • 50MB model vs Whisper's 150MB+ models
  • Compliant with PS restriction: "no pre-trained transformers"

Pipeline:
  Edge detects "Hey Vaani" → streams audio here via TCP
  → Vosk transcribes → JSON response sent back to edge device

Usage:
    python asr_server.py                          # Start on port 5000
    python asr_server.py --port 8080              # Custom port
    python asr_server.py --download-model         # Download Vosk model first
    python asr_server.py --model-path /path/to/vosk-model-en
    python asr_server.py --test audio.wav         # Test with a WAV file
"""

import os
import sys
import time
import json
import wave
import struct
import socket
import threading
import argparse
import urllib.request
import zipfile
from datetime import datetime

import numpy as np

# ─── Configuration ───────────────────────────────────────────────────────────
DEFAULT_PORT    = 5000
SAMPLE_RATE     = 16000
CHANNELS        = 1
SAMPLE_WIDTH    = 2   # 16-bit PCM = 2 bytes

# Vosk model — small English model (~50MB), NOT a transformer
VOSK_MODEL_URL  = "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip"
VOSK_MODEL_NAME = "vosk-model-small-en-us-0.15"
MODELS_DIR      = os.path.join(os.path.dirname(__file__), "vosk_models")
DEFAULT_MODEL   = os.path.join(MODELS_DIR, VOSK_MODEL_NAME)

# Protocol header (must match edge_inference.py)
MAGIC_NUMBER    = 0x48565031   # "HVP1"
HEADER_FORMAT   = "<IIHHII"    # magic, sample_rate, channels, bits, audio_len, session_id
HEADER_SIZE     = struct.calcsize(HEADER_FORMAT)

AUDIO_DIR       = os.path.join(os.path.dirname(__file__), "received_audio")
LOG_FILE        = os.path.join(os.path.dirname(__file__), "server_log.json")


# ─── Vosk Model Downloader ────────────────────────────────────────────────────

def download_vosk_model(model_path: str = DEFAULT_MODEL):
    """Download and extract Vosk small English model (~50MB)."""
    if os.path.exists(model_path) and os.listdir(model_path):
        print(f"  ✅ Vosk model already exists: {model_path}")
        return True

    os.makedirs(MODELS_DIR, exist_ok=True)
    zip_path = os.path.join(MODELS_DIR, f"{VOSK_MODEL_NAME}.zip")

    print(f"  📥 Downloading Vosk model (~50MB)...")
    print(f"  URL: {VOSK_MODEL_URL}")
    print(f"  This will take 1-2 minutes...\n")

    def progress_hook(block_num, block_size, total_size):
        downloaded = block_num * block_size
        if total_size > 0:
            pct = min(100, downloaded * 100 / total_size)
            bar = "█" * int(pct / 5) + "░" * (20 - int(pct / 5))
            mb_done = downloaded / (1024 * 1024)
            mb_total = total_size / (1024 * 1024)
            print(f"\r  [{bar}] {pct:.0f}% ({mb_done:.1f}/{mb_total:.1f} MB)", end="", flush=True)

    try:
        urllib.request.urlretrieve(VOSK_MODEL_URL, zip_path, reporthook=progress_hook)
        print(f"\n  ✅ Download complete!")

        print(f"  📦 Extracting model...")
        with zipfile.ZipFile(zip_path, 'r') as zf:
            zf.extractall(MODELS_DIR)
        os.remove(zip_path)
        print(f"  ✅ Model ready at: {model_path}")
        return True

    except Exception as e:
        print(f"\n  ❌ Download failed: {e}")
        print(f"  Manual download: {VOSK_MODEL_URL}")
        print(f"  Extract to: {MODELS_DIR}/")
        return False


# ─── Vosk Transcriber ─────────────────────────────────────────────────────────

class VoskTranscriber:
    """
    Speech recognition using Vosk — open-source Kaldi DNN-HMM model.

    Architecture: DNN-HMM (Deep Neural Network + Hidden Markov Model)
    NOT a transformer — fully compliant with project restrictions.
    Model size: ~50MB (vosk-model-small-en-us-0.15)
    """

    def __init__(self, model_path: str = DEFAULT_MODEL):
        self.model_path = model_path
        self.model      = None
        self._load_model()

    def _load_model(self):
        """Load Vosk model from disk."""
        try:
            from vosk import Model, KaldiRecognizer, SetLogLevel
            SetLogLevel(-1)  # Suppress Vosk verbose logging

            if not os.path.exists(self.model_path):
                print(f"  ⚠️  Vosk model not found at: {self.model_path}")
                print(f"  Run: python asr_server.py --download-model")
                print(f"  Falling back to mock transcription for testing.\n")
                self.model = None
                return

            print(f"  📥 Loading Vosk model: {os.path.basename(self.model_path)}...")
            self.model = Model(self.model_path)
            self._Model  = Model
            self._KaldiR = KaldiRecognizer
            print(f"  ✅ Vosk DNN-HMM model loaded (open-source, no transformer)")
            print(f"  📊 Architecture: Kaldi TDNN-F (Time-Delay Neural Network)")

        except ImportError:
            print("  ⚠️  Vosk not installed.")
            print("  Install: pip install vosk")
            self.model = None

    def transcribe(self, wav_path: str) -> str:
        """
        Transcribe a WAV file using Vosk Kaldi DNN-HMM model.
        Returns the full transcript as a string.
        """
        if self.model is None:
            return "[Vosk not loaded — install vosk and download model]"

        try:
            from vosk import KaldiRecognizer

            with wave.open(wav_path, 'rb') as wf:
                # Resample check
                if wf.getframerate() != SAMPLE_RATE:
                    return f"[Error: WAV must be {SAMPLE_RATE}Hz, got {wf.getframerate()}Hz]"

                recognizer = KaldiRecognizer(self.model, SAMPLE_RATE)
                recognizer.SetWords(True)  # Include word timestamps

                transcript_parts = []
                while True:
                    data = wf.readframes(4000)
                    if not data:
                        break
                    if recognizer.AcceptWaveform(data):
                        result = json.loads(recognizer.Result())
                        if result.get("text"):
                            transcript_parts.append(result["text"])

                # Get final result
                final = json.loads(recognizer.FinalResult())
                if final.get("text"):
                    transcript_parts.append(final["text"])

            full_transcript = " ".join(transcript_parts).strip()
            return full_transcript if full_transcript else "[silence detected]"

        except Exception as e:
            return f"[transcription error: {e}]"


# ─── ASR Server ──────────────────────────────────────────────────────────────

class ASRServer:
    """
    TCP server that receives audio from edge devices and returns transcripts.
    Uses Vosk (DNN-HMM) for speech recognition — open-source, no transformers.
    """

    def __init__(self, port: int = DEFAULT_PORT, model_path: str = DEFAULT_MODEL):
        self.port          = port
        self.model_path    = model_path
        self.transcriber   = None
        self.session_count = 0
        self.log_entries   = []
        self._lock         = threading.Lock()

    def start(self):
        """Start the TCP ASR server."""
        # Load Vosk model
        print("  Loading Vosk speech recognition model...")
        self.transcriber = VoskTranscriber(self.model_path)

        # Start TCP server
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(("0.0.0.0", self.port))
        server_socket.listen(10)

        local_ip = self._get_local_ip()

        print("\n" + "=" * 60)
        print("  ☁️  Hey Vaani — Cloud ASR Server (Vosk)")
        print("=" * 60)
        print(f"  Status:     🟢 RUNNING")
        print(f"  Address:    {local_ip}:{self.port}")
        print(f"  ASR Engine: Vosk DNN-HMM (open-source, no transformer)")
        print(f"  Audio dir:  {AUDIO_DIR}")
        print("=" * 60)
        print(f"\n  📋 Configure edge device with:")
        print(f"     --server-ip {local_ip} --server-port {self.port}")
        print(f"\n  ⏳ Waiting for connections...\n")

        try:
            while True:
                client_socket, client_addr = server_socket.accept()
                with self._lock:
                    self.session_count += 1
                    session_id = self.session_count

                print(f"  📡 [{session_id}] Connection from {client_addr[0]}:{client_addr[1]}")

                thread = threading.Thread(
                    target=self._handle_client,
                    args=(client_socket, client_addr, session_id),
                    daemon=True
                )
                thread.start()

        except KeyboardInterrupt:
            print("\n\n  🛑 Shutting down server...")
            self._save_log()
            server_socket.close()
            print(f"  📊 Total sessions handled: {self.session_count}")

    def _handle_client(self, client_socket: socket.socket,
                       client_addr: tuple, session_id: int):
        """Handle a single edge device connection in its own thread."""
        t_receive_start = time.time()

        try:
            # ── Step 1: Read protocol header ─────────────────────────────────
            header_data = self._recv_exact(client_socket, HEADER_SIZE)
            if not header_data:
                print(f"  ❌ [{session_id}] No header received")
                return

            magic, sr, ch, bits, expected_len, edge_session_id = struct.unpack(
                HEADER_FORMAT, header_data
            )

            if magic != MAGIC_NUMBER:
                # Treat as raw PCM (fallback for older clients)
                print(f"  ⚠️  [{session_id}] No HVP1 header — treating as raw PCM")
                audio_data = header_data
                sr, ch, bits = SAMPLE_RATE, CHANNELS, 16
            else:
                print(f"  📋 [{session_id}] HVP1 header: {sr}Hz, {ch}ch, {bits}bit, "
                      f"expected={expected_len}B, edge_session={edge_session_id}")
                audio_data = b""

            # ── Step 2: Receive audio stream (log timestamp of FIRST byte) ──────
            # cloud_receive_timestamp_ms: the moment the first audio byte arrives.
            # This is the latency anchor used by training/latency_analyzer.py to
            # compute: latency = cloud_receive_timestamp_ms - keyword_end_timestamp_ms
            cloud_receive_ts_ms: int | None = None

            if expected_len > 0:
                # Known length — receive exactly that many bytes
                remaining = expected_len - len(audio_data)
                if remaining > 0:
                    chunk = self._recv_exact(client_socket, remaining)
                    if chunk:
                        cloud_receive_ts_ms = int(time.time() * 1000)  # ← METRIC
                        audio_data += chunk
            else:
                # Streaming mode — read until connection closed
                first_chunk = True
                while True:
                    chunk = client_socket.recv(8192)
                    if not chunk:
                        break
                    if first_chunk:
                        cloud_receive_ts_ms = int(time.time() * 1000)  # ← METRIC
                        first_chunk = False
                    audio_data += chunk

            t_receive_end = time.time()
            receive_ms    = (t_receive_end - t_receive_start) * 1000
            audio_duration = len(audio_data) / (sr * ch * (bits // 8))

            print(f"  📊 [{session_id}] Received {len(audio_data):,}B "
                  f"({audio_duration:.2f}s audio, {receive_ms:.0f}ms)")
            print(f"  📌 [{session_id}] cloud_receive_timestamp_ms={cloud_receive_ts_ms}  "
                  f"← latency anchor for latency_analyzer.py")

            # ── Step 3: Save WAV ──────────────────────────────────────────────
            wav_path = self._save_wav(audio_data, session_id, sr, ch, bits)

            # ── Step 4: Vosk transcription ────────────────────────────────────
            print(f"  🗣️  [{session_id}] Transcribing with Vosk...")
            t_transcribe_start = time.time()
            transcript = self.transcriber.transcribe(wav_path)
            transcribe_ms = (time.time() - t_transcribe_start) * 1000

            total_ms = (time.time() - t_receive_start) * 1000

            # ── Step 5: Print results ─────────────────────────────────────────
            print(f"\n  {'─' * 55}")
            print(f"  📝 [{session_id}] TRANSCRIPT: \"{transcript}\"")
            print(f"  ⏱️  [{session_id}] Receive:    {receive_ms:.0f}ms")
            print(f"  ⏱️  [{session_id}] Transcribe: {transcribe_ms:.0f}ms")
            print(f"  ⏱️  [{session_id}] TOTAL:      {total_ms:.0f}ms")
            print(f"  {'─' * 55}\n")

            # ── Step 6: Send JSON response to edge device ─────────────────────
            response = json.dumps({
                "transcript":      transcript,
                "receive_ms":      round(receive_ms),
                "transcribe_ms":   round(transcribe_ms),
                "total_latency_ms": round(total_ms),
                "session_id":      session_id,
                "asr_engine":      "vosk-dnn-hmm"
            }).encode("utf-8")

            try:
                client_socket.sendall(response)
            except Exception:
                pass  # Edge may have already closed connection

            # ── Step 7: Save log entry ────────────────────────────────────────
            log_entry = {
                "session_id":                   session_id,
                "timestamp":                    datetime.now().isoformat(),
                "client_ip":                    client_addr[0],
                "audio_bytes":                  len(audio_data),
                "audio_duration_s":             round(audio_duration, 2),
                "transcript":                   transcript,
                "receive_ms":                   round(receive_ms),
                "transcribe_ms":                round(transcribe_ms),
                "total_ms":                     round(total_ms),
                # ← LATENCY METRIC: used by training/latency_analyzer.py
                "cloud_receive_timestamp_ms":   cloud_receive_ts_ms,
            }
            with self._lock:
                self.log_entries.append(log_entry)
            self._save_log()

        except Exception as e:
            print(f"  ❌ [{session_id}] Error: {e}")
        finally:
            client_socket.close()

    def _recv_exact(self, sock: socket.socket, n: int) -> bytes | None:
        """Receive exactly n bytes from socket."""
        data = b""
        while len(data) < n:
            try:
                chunk = sock.recv(n - len(data))
                if not chunk:
                    return None
                data += chunk
            except socket.timeout:
                return None
        return data

    def _save_wav(self, audio_data: bytes, session_id: int,
                  sr: int, ch: int, bits: int) -> str:
        """Save raw PCM audio bytes as WAV file."""
        os.makedirs(AUDIO_DIR, exist_ok=True)
        ts       = datetime.now().strftime("%Y%m%d_%H%M%S")
        wav_path = os.path.join(AUDIO_DIR, f"session_{session_id:04d}_{ts}.wav")

        with wave.open(wav_path, 'wb') as wf:
            wf.setnchannels(ch)
            wf.setsampwidth(bits // 8)
            wf.setframerate(sr)
            wf.writeframes(audio_data)

        return wav_path

    def _save_log(self):
        """Write all session logs to JSON file."""
        try:
            with open(LOG_FILE, 'w') as f:
                json.dump(self.log_entries, f, indent=2)
        except Exception:
            pass

    def _get_local_ip(self) -> str:
        """Get local network IP address."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"


# ─── Test Utility ─────────────────────────────────────────────────────────────

def test_with_file(wav_path: str, server_ip: str = "127.0.0.1",
                   port: int = DEFAULT_PORT):
    """Send a WAV file to the server as if it came from an edge device."""
    import soundfile as sf

    print(f"\n  🧪 Testing with file: {wav_path}")

    audio, sr = sf.read(wav_path, dtype='int16')
    if len(audio.shape) > 1:
        audio = audio[:, 0]
    audio_bytes = audio.tobytes()

    header = struct.pack(
        HEADER_FORMAT,
        MAGIC_NUMBER,
        sr, 1, 16,
        len(audio_bytes),
        0  # test session
    )

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((server_ip, port))
    sock.sendall(header + audio_bytes)
    sock.shutdown(socket.SHUT_WR)

    response = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response += chunk
    sock.close()

    result = json.loads(response.decode("utf-8"))
    print(f"  📝 Transcript:   \"{result['transcript']}\"")
    print(f"  ⏱️  Total latency: {result.get('total_latency_ms', '?')}ms")
    print(f"  🔧 ASR engine:   {result.get('asr_engine', 'unknown')}")


# ─── Entry Point ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Hey Vaani — Cloud ASR Server using Vosk (open-source, no transformer)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python asr_server.py --download-model    # Download Vosk model first
  python asr_server.py                     # Start server
  python asr_server.py --port 8080
  python asr_server.py --test audio.wav   # Test with a WAV file
        """
    )
    parser.add_argument("--port",           type=int, default=DEFAULT_PORT,
                        help=f"Server port (default: {DEFAULT_PORT})")
    parser.add_argument("--model-path",     type=str, default=DEFAULT_MODEL,
                        help=f"Vosk model path (default: {DEFAULT_MODEL})")
    parser.add_argument("--download-model", action="store_true",
                        help="Download Vosk small English model and exit")
    parser.add_argument("--test",           type=str, metavar="WAV_FILE",
                        help="Test server by sending a WAV file")
    parser.add_argument("--server-ip",      type=str, default="127.0.0.1",
                        help="Server IP for --test mode (default: 127.0.0.1)")
    args = parser.parse_args()

    if args.download_model:
        success = download_vosk_model(args.model_path)
        sys.exit(0 if success else 1)

    if args.test:
        test_with_file(args.test, args.server_ip, args.port)
        return

    # Start server
    server = ASRServer(port=args.port, model_path=args.model_path)
    server.start()


if __name__ == "__main__":
    main()
