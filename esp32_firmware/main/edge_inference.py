#!/usr/bin/env python3
"""
Edge Inference Script — Runs on Raspberry Pi 5 (or any Linux device)
"Hey Vaani" Keyword Spotting + Cloud ASR Streaming

Pipeline:
  Mic → Sliding Window MFCC → DS-CNN TFLite → Keyword Detected?
                                                    YES → Stream audio → Cloud Vosk ASR → Transcript

Usage:
    python edge_inference.py                          # Run with defaults
    python edge_inference.py --model path/to/model.tflite
    python edge_inference.py --server-ip 192.168.1.10 --server-port 5000
    python edge_inference.py --threshold 0.85         # Confidence threshold
    python edge_inference.py --list-devices           # Show audio devices
"""

import os
import sys
import time
import json
import struct
import socket
import argparse
import threading
import collections
import numpy as np
import sounddevice as sd

# ─── Configuration ───────────────────────────────────────────────────────────
SAMPLE_RATE     = 16000      # Must match training exactly
DURATION        = 1.0        # 1 second inference window
N_MFCC          = 13         # Must match training exactly
N_FFT           = 512        # Must match training exactly
HOP_LENGTH      = 320        # Must match training exactly
N_FRAMES        = 49         # Must match training exactly
CHANNELS        = 1          # Mono audio

KEYWORD         = "Hey Vaani"
DETECTION_THRESHOLD = 0.85   # Confidence above this = keyword detected
COMMAND_DURATION    = 4.0    # Seconds of command audio to capture after keyword
SLIDE_STEP          = 0.5    # Run inference every 0.5s (2Hz → ~1% CPU on RPi 5)

# Protocol (must match cloud server)
MAGIC_NUMBER    = 0x48565031  # "HVP1" = Hey Vaani Protocol v1
HEADER_FORMAT   = "<IIHHII"   # magic, sample_rate, channels, bits, audio_len, session_id
HEADER_SIZE     = struct.calcsize(HEADER_FORMAT)

# Default paths
DEFAULT_MODEL   = os.path.join(os.path.dirname(__file__), "..", "..", "training", "models", "ds_cnn_quantized.tflite")
DEFAULT_SERVER  = "127.0.0.1"
DEFAULT_PORT    = 5000

# VAD configuration
VAD_ENERGY_THRESHOLD  = 0.002   # RMS energy — below this = silence
VAD_ZCR_MAX           = 0.30    # Zero-crossing rate — very high = non-speech noise
VAD_SPEECH_FRAMES_MIN = 3       # Minimum consecutive speech frames to trigger inference


# ─── MFCC Feature Extraction ─────────────────────────────────────────────────
# CRITICAL: Must EXACTLY match compute_mfcc() in train_model.py

def compute_mfcc(audio: np.ndarray) -> np.ndarray:
    """
    Compute MFCC features from 1 second of 16kHz mono audio.
    Returns: numpy array of shape (N_FRAMES, N_MFCC) = (49, 13)

    This MUST match the MFCC computation in train_model.py exactly.
    Uses TensorFlow ops to guarantee identical results.
    """
    import tensorflow as tf

    # Ensure exactly 1 second = 16000 samples
    target_len = int(DURATION * SAMPLE_RATE)
    audio = audio.astype(np.float32)

    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        start = (len(audio) - target_len) // 2
        audio = audio[start:start + target_len]

    # Step 1: STFT
    stft = tf.signal.stft(
        audio,
        frame_length=N_FFT,
        frame_step=HOP_LENGTH,
        fft_length=N_FFT
    )
    spectrogram = tf.abs(stft)

    # Step 2: Mel filterbank
    num_spectrogram_bins = spectrogram.shape[-1]
    mel_matrix = tf.signal.linear_to_mel_weight_matrix(
        40, num_spectrogram_bins, SAMPLE_RATE, 20.0, SAMPLE_RATE / 2
    )
    mel = tf.matmul(spectrogram, mel_matrix)

    # Step 3: Log + MFCC
    log_mel = tf.math.log(mel + 1e-6)
    mfccs = tf.signal.mfccs_from_log_mel_spectrograms(log_mel)
    mfccs = mfccs[..., :N_MFCC].numpy()

    # Step 4: Pad/trim to N_FRAMES
    if mfccs.shape[0] < N_FRAMES:
        mfccs = np.pad(mfccs, ((0, N_FRAMES - mfccs.shape[0]), (0, 0)))
    elif mfccs.shape[0] > N_FRAMES:
        mfccs = mfccs[:N_FRAMES, :]

    return mfccs  # shape: (49, 13)


# ─── TFLite Inference Engine ──────────────────────────────────────────────────

class KeywordDetector:
    """Loads and runs the DS-CNN TFLite model for keyword detection."""

    def __init__(self, model_path: str):
        self.model_path = model_path
        self.interpreter = None
        self.input_details = None
        self.output_details = None
        self.input_scale = 1.0
        self.input_zero_point = 0
        self._load_model()

    def _load_model(self):
        """Load TFLite model — tries tflite-runtime first, falls back to TensorFlow."""
        try:
            from tflite_runtime.interpreter import Interpreter
            print(f"  ✅ Using tflite-runtime (lightweight)")
        except ImportError:
            from tensorflow.lite.python.interpreter import Interpreter
            print(f"  ✅ Using TensorFlow Lite (full TF)")

        if not os.path.exists(self.model_path):
            raise FileNotFoundError(
                f"\n  ❌ Model not found: {self.model_path}\n"
                f"  Run 'python training/convert_tflite.py' first to generate the model."
            )

        self.interpreter = Interpreter(model_path=self.model_path)
        self.interpreter.allocate_tensors()

        self.input_details  = self.interpreter.get_input_details()
        self.output_details = self.interpreter.get_output_details()

        # INT8 quantization parameters
        if self.input_details[0]['dtype'] == np.int8:
            q_params = self.input_details[0]['quantization']
            self.input_scale      = q_params[0]
            self.input_zero_point = q_params[1]
            print(f"  📊 Model: INT8 quantized (scale={self.input_scale:.4f}, zp={self.input_zero_point})")
        else:
            print(f"  📊 Model: Float32")

        print(f"  📁 Model loaded: {os.path.basename(self.model_path)}")
        print(f"  🔷 Input shape:  {self.input_details[0]['shape']}")
        print(f"  🔷 Output shape: {self.output_details[0]['shape']}")

    def predict(self, mfcc: np.ndarray) -> tuple[float, float]:
        """
        Run inference on a (49, 13) MFCC array.
        Returns: (keyword_prob, not_keyword_prob) as floats in [0, 1]
        """
        # Add batch + channel dims: (49, 13) → (1, 49, 13, 1)
        x = mfcc[np.newaxis, ..., np.newaxis].astype(np.float32)

        # Quantize if model is INT8
        if self.input_details[0]['dtype'] == np.int8:
            x = (x / self.input_scale + self.input_zero_point).astype(np.int8)

        self.interpreter.set_tensor(self.input_details[0]['index'], x)
        self.interpreter.invoke()

        output = self.interpreter.get_tensor(self.output_details[0]['index'])

        # Dequantize output if needed
        if self.output_details[0]['dtype'] == np.int8:
            out_scale, out_zp = self.output_details[0]['quantization']
            output = (output.astype(np.float32) - out_zp) * out_scale

        # output shape: (1, 2) → [not_keyword_prob, keyword_prob]
        probs = output[0]
        not_kw_prob = float(probs[0])
        kw_prob     = float(probs[1])
        return kw_prob, not_kw_prob


# ─── Audio Ring Buffer ────────────────────────────────────────────────────────

class AudioRingBuffer:
    """Thread-safe ring buffer for continuous audio capture."""

    def __init__(self, max_seconds: float = 2.0):
        self.max_samples = int(max_seconds * SAMPLE_RATE)
        self.buffer = collections.deque(maxlen=self.max_samples)
        self.lock = threading.Lock()

    def push(self, samples: np.ndarray):
        with self.lock:
            self.buffer.extend(samples.flatten())

    def get_last(self, seconds: float) -> np.ndarray:
        """Get the last N seconds of audio from the buffer."""
        n = int(seconds * SAMPLE_RATE)
        with self.lock:
            buf = list(self.buffer)
        if len(buf) < n:
            # Pad with silence if not enough data yet
            buf = [0.0] * (n - len(buf)) + buf
        return np.array(buf[-n:], dtype=np.float32)


# ─── Voice Activity Detection ───────────────────────────────────────────────

class VoiceActivityDetector:
    """
    Lightweight VAD using energy + zero-crossing rate.
    No extra dependencies — runs in microseconds.

    Purpose:
      - Skip MFCC + inference when audio is silence
      - Reduces CPU usage during quiet periods
      - Reduces false triggers from background noise

    How it works:
      1. Energy check: Is audio loud enough to be speech?
      2. Zero-crossing rate: Is it speech-like (not pure tone/noise)?
      3. Only run DS-CNN if both checks pass
    """

    def __init__(self,
                 energy_threshold: float = VAD_ENERGY_THRESHOLD,
                 zcr_max: float = VAD_ZCR_MAX,
                 min_speech_frames: int = VAD_SPEECH_FRAMES_MIN):
        self.energy_threshold   = energy_threshold
        self.zcr_max            = zcr_max
        self.min_speech_frames  = min_speech_frames
        self._speech_frame_count = 0

        # Stats
        self.total_frames   = 0
        self.skipped_frames = 0

    def is_speech(self, audio: np.ndarray) -> tuple[bool, dict]:
        """
        Decide if audio chunk contains speech.
        Returns (is_speech: bool, metrics: dict)
        """
        self.total_frames += 1

        # ── Metric 1: RMS Energy ────────────────────────────────────────────
        # Speech has higher energy than silence/very quiet noise
        rms = float(np.sqrt(np.mean(audio ** 2)))
        energy_ok = rms >= self.energy_threshold

        # ── Metric 2: Zero-Crossing Rate ────────────────────────────────────
        # Speech has moderate ZCR (~0.05-0.25)
        # Pure noise/hiss has very high ZCR (>0.4)
        # Silence has very low ZCR (<0.02)
        signs      = np.sign(audio)
        signs[signs == 0] = 1           # treat zero as positive
        crossings  = np.sum(np.abs(np.diff(signs))) / 2
        zcr        = float(crossings / len(audio))
        zcr_ok     = zcr <= self.zcr_max

        # ── Combined Decision ────────────────────────────────────────────────
        frame_is_speech = energy_ok and zcr_ok

        if frame_is_speech:
            self._speech_frame_count += 1
        else:
            self._speech_frame_count = max(0, self._speech_frame_count - 1)
            self.skipped_frames += 1

        # Require minimum consecutive speech frames before triggering
        confirmed = self._speech_frame_count >= self.min_speech_frames

        return confirmed, {
            "rms":      round(rms, 5),
            "zcr":      round(zcr, 3),
            "energy_ok": energy_ok,
            "zcr_ok":   zcr_ok,
            "speech_frames": self._speech_frame_count
        }

    @property
    def skip_rate(self) -> float:
        """Percentage of frames skipped (silence) — shows CPU savings."""
        if self.total_frames == 0:
            return 0.0
        return (self.skipped_frames / self.total_frames) * 100


# ─── Cloud Streaming ──────────────────────────────────────────────────────────

def stream_to_cloud(audio: np.ndarray, server_ip: str, server_port: int,
                    session_id: int) -> dict:
    """
    Stream audio to cloud ASR server via TCP.
    Protocol: 16-byte header + raw PCM int16 audio.
    Returns transcript dict from server.
    """
    # Convert float32 [-1, 1] → int16 PCM
    audio_int16 = (audio * 32767).clip(-32768, 32767).astype(np.int16)
    audio_bytes  = audio_int16.tobytes()

    # Build header
    header = struct.pack(
        HEADER_FORMAT,
        MAGIC_NUMBER,
        SAMPLE_RATE,
        CHANNELS,
        16,               # bits per sample
        len(audio_bytes),
        session_id
    )

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(15.0)
        sock.connect((server_ip, server_port))

        # Send header + audio
        sock.sendall(header + audio_bytes)
        sock.shutdown(socket.SHUT_WR)

        # Receive transcript response
        response_data = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            response_data += chunk
        sock.close()

        if response_data:
            return json.loads(response_data.decode("utf-8"))
        return {"transcript": "[no response]", "latency_ms": -1}

    except ConnectionRefusedError:
        return {"transcript": "[server not reachable]", "latency_ms": -1}
    except socket.timeout:
        return {"transcript": "[server timeout]", "latency_ms": -1}
    except Exception as e:
        return {"transcript": f"[error: {e}]", "latency_ms": -1}


# ─── Main Detection Loop ──────────────────────────────────────────────────────

class HeyVaaniEdge:
    """Main edge inference controller for Hey Vaani keyword spotting."""

    def __init__(self, model_path: str, server_ip: str, server_port: int,
                 threshold: float = DETECTION_THRESHOLD, device_id=None):
        self.server_ip   = server_ip
        self.server_port = server_port
        self.threshold   = threshold
        self.device_id   = device_id
        self.session_id  = 0

        self.ring_buffer = AudioRingBuffer(max_seconds=2.0)
        self.detector    = KeywordDetector(model_path)
        self.vad         = VoiceActivityDetector()   # ← VAD added here

        self._is_listening    = False
        self._is_capturing    = False   # True during command capture
        self._command_buffer  = []      # Raw audio for command

        # CPU monitoring
        self._inference_count = 0
        self._total_inference_ms = 0.0
        self._vad_skipped     = 0

    def _audio_callback(self, indata, frames, time_info, status):
        """Called by sounddevice for each audio chunk."""
        if status:
            print(f"  ⚠️  Audio status: {status}", flush=True)

        samples = indata[:, 0].copy()  # mono
        self.ring_buffer.push(samples)

        if self._is_capturing:
            self._command_buffer.extend(samples)

    def _run_inference_loop(self):
        """Inference loop — runs every SLIDE_STEP seconds (0.5s = 2Hz)."""
        print(f"\n  🟢 Listening for '{KEYWORD}'... (threshold={self.threshold})")
        print(f"  💡 Inference rate: every {SLIDE_STEP}s | Slide window: {DURATION}s\n")

        while self._is_listening:
            loop_start = time.time()

            # Get last 1 second of audio
            audio_window = self.ring_buffer.get_last(DURATION)

            # ── VAD Check: Skip inference if silence ───────────────────────
            speech_detected, vad_metrics = self.vad.is_speech(audio_window)

            if not speech_detected:
                # Silence — skip expensive MFCC + DS-CNN inference
                self._vad_skipped += 1
                rms_bar = int(min(20, vad_metrics['rms'] / VAD_ENERGY_THRESHOLD * 10))
                bar     = "░" * 20
                print(
                    f"\r  [{bar}] VAD:SILENT rms={vad_metrics['rms']:.4f} "
                    f"skip={self.vad.skip_rate:.0f}%",
                    end="", flush=True
                )
                # Sleep and continue — no MFCC, no inference
                elapsed    = time.time() - loop_start
                sleep_time = max(0.0, SLIDE_STEP - elapsed)
                time.sleep(sleep_time)
                continue   # ← skip to next iteration

            # ── Speech detected — run full MFCC + DS-CNN pipeline ──────────
            # Compute MFCC
            mfcc = compute_mfcc(audio_window)

            # Run inference
            t0 = time.time()
            kw_prob, not_kw_prob = self.detector.predict(mfcc)
            inference_ms = (time.time() - t0) * 1000

            self._inference_count += 1
            self._total_inference_ms += inference_ms

            # Display status bar (speech mode)
            bar_len  = 20
            filled   = int(kw_prob * bar_len)
            bar      = "█" * filled + "░" * (bar_len - filled)
            marker   = "🔔 DETECTED!" if kw_prob >= self.threshold else "  "
            print(
                f"\r  [{bar}] {kw_prob:.2f} | {inference_ms:.1f}ms "
                f"rms={vad_metrics['rms']:.4f} {marker}",
                end="", flush=True
            )

            # Keyword detected — start command capture
            if kw_prob >= self.threshold and not self._is_capturing:
                self._on_keyword_detected(kw_prob)

            # Adaptive sleep — maintain target inference rate
            elapsed = time.time() - loop_start
            sleep_time = max(0.0, SLIDE_STEP - elapsed)
            time.sleep(sleep_time)

    def _on_keyword_detected(self, confidence: float):
        """Handle keyword detection event."""
        self.session_id += 1
        print(f"\n\n  🔔 '{KEYWORD}' DETECTED! (confidence={confidence:.3f})")
        print(f"  🎤 Capturing command audio for {COMMAND_DURATION}s...")

        self._is_capturing  = True
        self._command_buffer = []

        # Capture command audio
        time.sleep(COMMAND_DURATION)

        command_audio = np.array(self._command_buffer, dtype=np.float32)
        self._is_capturing  = False
        self._command_buffer = []

        audio_duration = len(command_audio) / SAMPLE_RATE
        print(f"  📊 Captured {audio_duration:.1f}s of command audio ({len(command_audio)} samples)")

        # Stream to cloud
        print(f"  📡 Streaming to cloud ASR ({self.server_ip}:{self.server_port})...")
        stream_start = time.time()
        result = stream_to_cloud(command_audio, self.server_ip, self.server_port, self.session_id)
        round_trip_ms = (time.time() - stream_start) * 1000

        # Display result
        transcript = result.get("transcript", "[unknown]")
        server_latency = result.get("latency_ms", -1)

        print(f"\n  {'═' * 50}")
        print(f"  📝 TRANSCRIPT: \"{transcript}\"")
        print(f"  ⏱️  Round-trip: {round_trip_ms:.0f}ms | Server: {server_latency}ms")
        print(f"  {'═' * 50}\n")
        print(f"  🟢 Listening again...\n")

    def _print_cpu_stats(self):
        """Periodically print CPU/performance statistics."""
        while self._is_listening:
            time.sleep(30)  # Every 30 seconds
            if self._inference_count > 0:
                avg_inf_ms = self._total_inference_ms / self._inference_count
                duty_cycle = (avg_inf_ms / (SLIDE_STEP * 1000)) * 100
                total_frames = self.vad.total_frames
                skip_rate    = self.vad.skip_rate
                print(
                    f"\n  📊 Stats:"
                    f" {self._inference_count} inferences"
                    f" | avg {avg_inf_ms:.1f}ms"
                    f" | duty cycle ~{duty_cycle:.2f}% CPU"
                    f" | VAD skipped {skip_rate:.0f}% of frames (silence)"
                    f"\n"
                )

    def start(self, device_id=None):
        """Start the edge inference system."""
        print("=" * 60)
        print("  🎙️  HEY VAANI — Edge Inference")
        print("=" * 60)
        print(f"  Model:     {os.path.basename(self.detector.model_path)}")
        print(f"  Server:    {self.server_ip}:{self.server_port}")
        print(f"  Threshold: {self.threshold}")
        print(f"  Audio:     {SAMPLE_RATE}Hz mono, {DURATION}s window")
        print("=" * 60)

        self._is_listening = True

        # Stats thread
        stats_thread = threading.Thread(target=self._print_cpu_stats, daemon=True)
        stats_thread.start()

        # Inference thread
        inference_thread = threading.Thread(target=self._run_inference_loop, daemon=True)

        # Start audio stream
        try:
            with sd.InputStream(
                samplerate=SAMPLE_RATE,
                channels=CHANNELS,
                dtype='float32',
                blocksize=int(SAMPLE_RATE * 0.1),  # 100ms chunks
                callback=self._audio_callback,
                device=device_id
            ):
                inference_thread.start()
                print(f"  🎤 Microphone active. Press Ctrl+C to stop.\n")
                while True:
                    time.sleep(0.1)

        except KeyboardInterrupt:
            print("\n\n  🛑 Stopping edge inference...")
            self._is_listening = False
            avg_inf = self._total_inference_ms / max(1, self._inference_count)
            print(f"  📊 Final: {self._inference_count} inferences, avg {avg_inf:.1f}ms each")
            print("  Goodbye!")
        except Exception as e:
            print(f"\n  ❌ Error: {e}")
            raise


# ─── Entry Point ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Hey Vaani — Edge Keyword Spotting + Cloud ASR Streaming",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python edge_inference.py
  python edge_inference.py --server-ip 192.168.1.10
  python edge_inference.py --threshold 0.80 --model models/ds_cnn_quantized.tflite
  python edge_inference.py --list-devices
        """
    )
    parser.add_argument("--model",       type=str,   default=DEFAULT_MODEL,
                        help=f"Path to .tflite model (default: {DEFAULT_MODEL})")
    parser.add_argument("--server-ip",   type=str,   default=DEFAULT_SERVER,
                        help="Cloud ASR server IP (default: 127.0.0.1)")
    parser.add_argument("--server-port", type=int,   default=DEFAULT_PORT,
                        help="Cloud ASR server port (default: 5000)")
    parser.add_argument("--threshold",   type=float, default=DETECTION_THRESHOLD,
                        help=f"Keyword confidence threshold (default: {DETECTION_THRESHOLD})")
    parser.add_argument("--device",      type=int,   default=None,
                        help="Audio device ID (default: system default)")
    parser.add_argument("--list-devices", action="store_true",
                        help="List all audio input devices and exit")
    args = parser.parse_args()

    if args.list_devices:
        print("\n🎤 Available Audio Input Devices:")
        print("─" * 60)
        devices = sd.query_devices()
        for i, dev in enumerate(devices):
            if dev['max_input_channels'] > 0:
                marker = " ← DEFAULT" if i == sd.default.device[0] else ""
                print(f"  [{i}] {dev['name']} "
                      f"(inputs: {dev['max_input_channels']}){marker}")
        print("─" * 60)
        return

    edge = HeyVaaniEdge(
        model_path  = args.model,
        server_ip   = args.server_ip,
        server_port = args.server_port,
        threshold   = args.threshold,
    )
    edge.start(device_id=args.device)


if __name__ == "__main__":
    main()
