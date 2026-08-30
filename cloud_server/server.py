#!/usr/bin/env python3
"""
Cloud ASR Server — Receives audio from ESP32, transcribes with Whisper.

This is the "cloud" side of the edge-cloud pipeline:
  ESP32 detects "Hey Vaani" → streams audio here → Whisper transcribes → result

Runs on your laptop (or deploy to GCP/AWS later).

Usage:
    python server.py                     # Start server on port 5000
    python server.py --port 8080         # Custom port
    python server.py --whisper-model base # Use larger Whisper model
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

# ─── Configuration ───────────────────────────────────────────────────────────
DEFAULT_PORT = 5000
SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2  # 16-bit = 2 bytes
AUDIO_DIR = os.path.join(os.path.dirname(__file__), "received_audio")
LOG_FILE = os.path.join(os.path.dirname(__file__), "server_log.json")

# ─── Protocol Header ────────────────────────────────────────────────────────
# ESP32 sends a 16-byte header before audio data:
#   bytes 0-3:   magic number (0x48565031 = "HVP1" = Hey Vaani Protocol v1)
#   bytes 4-7:   sample rate (uint32, e.g., 16000)
#   bytes 8-9:   channels (uint16, e.g., 1)
#   bytes 10-11: bits per sample (uint16, e.g., 16)
#   bytes 12-15: expected audio length in bytes (uint32, 0 = stream until close)

MAGIC_NUMBER = 0x48565031  # "HVP1"
HEADER_SIZE = 16


class ASRServer:
    """TCP server that receives audio streams from ESP32 and transcribes them."""
    
    def __init__(self, port=DEFAULT_PORT, whisper_model="base"):
        self.port = port
        self.whisper_model = whisper_model
        self.transcriber = None
        self.session_count = 0
        self.log_entries = []
        
    def start(self):
        """Start the TCP server."""
        self._load_whisper()
        
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(("0.0.0.0", self.port))
        server_socket.listen(5)
        
        # Get local IP for ESP32 to connect to
        local_ip = self._get_local_ip()
        
        print("=" * 60)
        print("  ☁️  Hey Vaani — Cloud ASR Server")
        print("=" * 60)
        print(f"  Status:    🟢 RUNNING")
        print(f"  Address:   {local_ip}:{self.port}")
        print(f"  Whisper:   {self.whisper_model}")
        print(f"  Audio dir: {AUDIO_DIR}")
        print("=" * 60)
        print(f"\n  Configure ESP32 with: SERVER_IP = \"{local_ip}\"")
        print(f"                        SERVER_PORT = {self.port}")
        print(f"\n  Waiting for ESP32 connections...\n")
        
        try:
            while True:
                client_socket, client_addr = server_socket.accept()
                self.session_count += 1
                print(f"  📡 [{self.session_count}] ESP32 connected from {client_addr[0]}:{client_addr[1]}")
                
                # Handle each connection in a thread
                thread = threading.Thread(
                    target=self._handle_client,
                    args=(client_socket, client_addr),
                    daemon=True
                )
                thread.start()
        except KeyboardInterrupt:
            print("\n\n  🛑 Server shutting down...")
            self._save_log()
            server_socket.close()
    
    def _load_whisper(self):
        """Load Whisper model for transcription."""
        print("  Loading Whisper model...")
        try:
            from faster_whisper import WhisperModel
            self.transcriber = WhisperModel(
                self.whisper_model,
                device="cpu",
                compute_type="int8"
            )
            print(f"  ✅ Whisper '{self.whisper_model}' loaded (faster-whisper, INT8)")
        except ImportError:
            print("  ⚠️  faster-whisper not installed. Falling back to mock transcription.")
            print("  Install with: pip install faster-whisper")
            self.transcriber = None
    
    def _handle_client(self, client_socket, client_addr):
        """Handle a single ESP32 connection."""
        session_id = self.session_count
        receive_start = time.time()
        
        try:
            # Step 1: Receive header
            header_data = self._recv_exact(client_socket, HEADER_SIZE)
            if not header_data:
                print(f"  ❌ [{session_id}] No header received")
                return
            
            magic, sample_rate, channels, bits, expected_len = struct.unpack('<IIHHI', header_data)
            
            if magic != MAGIC_NUMBER:
                print(f"  ⚠️  [{session_id}] Invalid magic number: 0x{magic:08X}")
                print(f"  ⚠️  [{session_id}] Treating all data as raw PCM audio")
                # Treat header as audio data too
                audio_data = header_data
                sample_rate = SAMPLE_RATE
                channels = CHANNELS
                bits = 16
            else:
                print(f"  📋 [{session_id}] Header: {sample_rate}Hz, {channels}ch, {bits}bit")
                audio_data = b""
            
            # Step 2: Receive audio stream
            print(f"  🎤 [{session_id}] Receiving audio stream...")
            
            while True:
                chunk = client_socket.recv(4096)
                if not chunk:
                    break
                audio_data += chunk
            
            receive_end = time.time()
            receive_time_ms = (receive_end - receive_start) * 1000
            
            audio_duration = len(audio_data) / (sample_rate * channels * (bits // 8))
            print(f"  📊 [{session_id}] Received: {len(audio_data)} bytes ({audio_duration:.2f}s audio)")
            print(f"  ⏱️  [{session_id}] Receive latency: {receive_time_ms:.0f}ms")
            
            # Step 3: Save WAV file
            wav_path = self._save_wav(audio_data, session_id, sample_rate, channels, bits)
            
            # Step 4: Transcribe
            transcribe_start = time.time()
            transcript = self._transcribe(wav_path)
            transcribe_end = time.time()
            transcribe_time_ms = (transcribe_end - transcribe_start) * 1000
            
            total_latency_ms = (transcribe_end - receive_start) * 1000
            
            # Step 5: Display results
            print(f"\n  {'─' * 50}")
            print(f"  📝 [{session_id}] TRANSCRIPT: \"{transcript}\"")
            print(f"  ⏱️  [{session_id}] Receive:    {receive_time_ms:.0f}ms")
            print(f"  ⏱️  [{session_id}] Transcribe: {transcribe_time_ms:.0f}ms")
            print(f"  ⏱️  [{session_id}] TOTAL:      {total_latency_ms:.0f}ms")
            print(f"  {'─' * 50}\n")
            
            # Step 6: Send response back to ESP32
            response = json.dumps({
                "transcript": transcript,
                "latency_ms": round(total_latency_ms),
                "session_id": session_id
            }).encode('utf-8')
            
            try:
                client_socket.sendall(response)
            except Exception:
                pass  # ESP32 may have already closed connection
            
            # Step 7: Log metrics
            self.log_entries.append({
                "session_id": session_id,
                "timestamp": datetime.now().isoformat(),
                "client_ip": client_addr[0],
                "audio_bytes": len(audio_data),
                "audio_duration_sec": round(audio_duration, 2),
                "transcript": transcript,
                "receive_latency_ms": round(receive_time_ms),
                "transcribe_latency_ms": round(transcribe_time_ms),
                "total_latency_ms": round(total_latency_ms),
            })
            self._save_log()
            
        except Exception as e:
            print(f"  ❌ [{session_id}] Error: {e}")
        finally:
            client_socket.close()
    
    def _recv_exact(self, sock, n):
        """Receive exactly n bytes from socket."""
        data = b""
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data
    
    def _save_wav(self, audio_data, session_id, sr, ch, bits):
        """Save raw PCM audio as WAV file."""
        os.makedirs(AUDIO_DIR, exist_ok=True)
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        wav_path = os.path.join(AUDIO_DIR, f"session_{session_id}_{timestamp}.wav")
        
        with wave.open(wav_path, 'wb') as wav:
            wav.setnchannels(ch)
            wav.setsampwidth(bits // 8)
            wav.setframerate(sr)
            wav.writeframes(audio_data)
        
        return wav_path
    
    def _transcribe(self, wav_path):
        """Transcribe audio using Whisper."""
        if self.transcriber is None:
            # Mock transcription for testing without Whisper
            return "[whisper not installed — mock transcript]"
        
        try:
            segments, info = self.transcriber.transcribe(
                wav_path,
                beam_size=5,
                language="en",
                vad_filter=True
            )
            transcript = " ".join([seg.text.strip() for seg in segments])
            return transcript if transcript else "[silence]"
        except Exception as e:
            return f"[transcription error: {e}]"
    
    def _save_log(self):
        """Save session log to JSON file."""
        with open(LOG_FILE, 'w') as f:
            json.dump(self.log_entries, f, indent=2)
    
    def _get_local_ip(self):
        """Get local WiFi IP address."""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"


# ─── Test Mode ───────────────────────────────────────────────────────────────

def test_with_file(filepath, port=DEFAULT_PORT):
    """Test server by sending a WAV file as if it came from ESP32."""
    print(f"\n  🧪 Testing with file: {filepath}")
    
    import soundfile as sf
    audio, sr = sf.read(filepath, dtype='int16')
    if len(audio.shape) > 1:
        audio = audio[:, 0]
    
    audio_bytes = audio.tobytes()
    
    # Create header
    header = struct.pack('<IIHHI',
        MAGIC_NUMBER,           # magic
        sr,                     # sample rate
        1,                      # channels
        16,                     # bits per sample
        len(audio_bytes)        # audio length
    )
    
    # Connect and send
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", port))
    sock.sendall(header + audio_bytes)
    sock.shutdown(socket.SHUT_WR)
    
    # Receive response
    response = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        response += chunk
    sock.close()
    
    result = json.loads(response.decode('utf-8'))
    print(f"  📝 Transcript: {result['transcript']}")
    print(f"  ⏱️  Latency: {result['latency_ms']}ms")


# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Hey Vaani Cloud ASR Server")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Server port")
    parser.add_argument("--whisper-model", type=str, default="base", 
                       choices=["tiny", "base", "small", "medium"],
                       help="Whisper model size")
    parser.add_argument("--test", type=str, help="Test with a WAV file")
    args = parser.parse_args()
    
    if args.test:
        test_with_file(args.test, args.port)
    else:
        server = ASRServer(port=args.port, whisper_model=args.whisper_model)
        server.start()


if __name__ == "__main__":
    main()
