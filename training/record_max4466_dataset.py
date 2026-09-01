#!/usr/bin/env python3
"""Record real deployment-microphone data for Hey Vaani training.

This records from a sounddevice input and writes WAV files directly into the
folders consumed by train_model.py:
  positive -> training/data/keyword/
  negative -> training/data/background/

Important: MAX4466 connected directly to an ESP32 ADC is *not* a computer audio
device. Use this script only when the MAX4466 signal reaches the computer via a
USB audio interface/capture device, or after adding an ESP32 PCM export path.

Examples:
  python record_max4466_dataset.py --list-devices
  python record_max4466_dataset.py --mode positive --count 100 --speaker yug --device 2
  python record_max4466_dataset.py --mode negative --count 200 --speaker yug --device 2
"""

import argparse
import os
import struct
import sys
import time
from datetime import datetime

import numpy as np
import sounddevice as sd
import soundfile as sf


SAMPLE_RATE = 16000
CHANNELS = 1
DURATION_SECONDS = 1.5
DATA_DIR = os.path.join(os.path.dirname(__file__), "data")

# These are deliberately close to the wake word.  They are much more valuable
# for preventing false activations than arbitrary words alone.
NEGATIVE_PROMPTS = (
    "say hello",
    "say hey",
    "say hi vaani",
    "say vaani",
    "say hey bonnie",
    "say a normal sentence",
    "stay silent",
)


def list_devices() -> None:
    print("\nAvailable input devices:")
    for index, device in enumerate(sd.query_devices()):
        if device["max_input_channels"] > 0:
            default = " (default)" if index == sd.default.device[0] else ""
            print(f"  [{index}] {device['name']} — {device['max_input_channels']} input channel(s){default}")


def rms(audio: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(audio, dtype=np.float64))))


def record_from_sounddevice(device: int | None, duration: float) -> np.ndarray:
    audio = sd.rec(
        frames=round(duration * SAMPLE_RATE),
        samplerate=SAMPLE_RATE,
        channels=CHANNELS,
        dtype="float32",
        device=device,
    )
    sd.wait()
    return audio[:, 0]


def read_exact(port, count: int, timeout_seconds: float) -> bytes:
    data = bytearray()
    deadline = time.monotonic() + timeout_seconds
    while len(data) < count and time.monotonic() < deadline:
        chunk = port.read(count - len(data))
        if chunk:
            data.extend(chunk)
    if len(data) != count:
        raise TimeoutError(f"expected {count} bytes but received {len(data)}")
    return bytes(data)


def record_from_esp32(port, timeout_seconds: float = 12.0) -> np.ndarray:
    """Request one clip from dataset_recorder firmware and decode HVR1 PCM."""
    port.reset_input_buffer()
    port.write(b"R\n")
    port.flush()

    header = bytearray()
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        header.extend(port.read(128))
        marker = header.find(b"HVR1")
        if marker >= 0 and len(header) - marker >= 12:
            packet_header = bytes(header[marker:marker + 12])
            sample_rate, sample_count = struct.unpack("<II", packet_header[4:])
            if sample_rate != SAMPLE_RATE or not 1 <= sample_count <= 64000:
                raise ValueError(f"unexpected ESP32 header: {sample_rate=} {sample_count=}")
            leftover = bytes(header[marker + 12:])
            needed = sample_count * 2
            pcm = leftover + read_exact(port, max(0, needed - len(leftover)), timeout_seconds)
            if len(pcm) != needed:
                raise TimeoutError("incomplete PCM clip")
            return np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        # Keep enough bytes to detect a header split across reads.
        if len(header) > 4096:
            del header[:-3]
    raise TimeoutError("ESP32 recorder did not return an HVR1 packet")


def output_path(mode: str, speaker: str, sample_number: int, prompt: str) -> str:
    directory = os.path.join(DATA_DIR, "keyword" if mode == "positive" else "background")
    os.makedirs(directory, exist_ok=True)
    safe_prompt = "".join(c if c.isalnum() else "_" for c in prompt).strip("_")[:24]
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    label = "hey_vaani" if mode == "positive" else "not_keyword"
    return os.path.join(
        directory,
        f"{label}_max4466_{speaker}_{safe_prompt}_{sample_number:04d}_{timestamp}.wav",
    )


def prompt_for(mode: str, index: int) -> str:
    if mode == "positive":
        return "say: Hey Vaani"
    return NEGATIVE_PROMPTS[index % len(NEGATIVE_PROMPTS)]


def main() -> None:
    parser = argparse.ArgumentParser(description="Record positive and negative KWS samples")
    parser.add_argument("--list-devices", action="store_true", help="Print input devices and exit")
    parser.add_argument("--mode", choices=("positive", "negative"), help="Class to record")
    parser.add_argument("--count", type=int, default=0, help="Number of clips to record")
    parser.add_argument("--speaker", default="speaker", help="Speaker/source name used in filenames")
    parser.add_argument("--source", choices=("sounddevice", "esp32"), default="sounddevice",
                        help="Record from a PC input device or the ESP32 MAX4466 recorder firmware")
    parser.add_argument("--device", type=int, default=None, help="sounddevice input device ID")
    parser.add_argument("--port", help="ESP32 serial port, e.g. /dev/ttyUSB0 or COM3")
    parser.add_argument("--baud", type=int, default=115200, help="ESP32 serial baud rate")
    parser.add_argument("--duration", type=float, default=DURATION_SECONDS, help="Clip length in seconds")
    args = parser.parse_args()

    if args.list_devices:
        list_devices()
        return
    if not args.mode or args.count < 1:
        parser.error("--mode and a positive --count are required")
    if args.duration < 1.0 or args.duration > 4.0:
        parser.error("--duration must be between 1.0 and 4.0 seconds")
    if args.source == "esp32" and not args.port:
        parser.error("--source esp32 requires --port")
    if args.source == "esp32" and args.duration != DURATION_SECONDS:
        parser.error("ESP32 recorder captures a fixed 1.5-second clip; omit --duration")

    serial_port = None
    if args.source == "esp32":
        try:
            import serial  # pyserial
            serial_port = serial.Serial(args.port, args.baud, timeout=0.25)
        except ImportError:
            print("pyserial is required for --source esp32. Run: pip install pyserial")
            sys.exit(2)
        except Exception as exc:
            print(f"Cannot open ESP32 serial port {args.port}: {exc}")
            sys.exit(2)

    print("\nHey Vaani deployment-microphone recorder")
    print(f"Mode: {args.mode}; clips: {args.count}; sample rate: {SAMPLE_RATE} Hz")
    print("Use the same microphone, gain setting, cable, and distance as the ESP32 deployment setup.\n")

    saved = 0
    for index in range(args.count):
        instruction = prompt_for(args.mode, index)
        print(f"[{index + 1}/{args.count}] {instruction}")
        time.sleep(1.0)
        print("Recording...", end=" ", flush=True)
        try:
            audio = (record_from_esp32(serial_port) if serial_port is not None
                     else record_from_sounddevice(args.device, args.duration))
        except Exception as exc:
            print(f"failed: {exc}")
            print("Run with --list-devices and choose a valid --device.")
            sys.exit(2)

        level = rms(audio)
        peak = float(np.max(np.abs(audio)))
        # A positive wake word must be audible. Silence is intentional only for
        # the negative class, so it is retained there.
        if args.mode == "positive" and level < 0.005:
            print(f"too quiet (RMS={level:.4f}); not saved. Retry this sample.")
            continue
        if peak >= 0.99:
            print(f"clipped (peak={peak:.3f}); not saved. Lower microphone gain.")
            continue

        path = output_path(args.mode, args.speaker, index + 1, instruction)
        sf.write(path, audio, SAMPLE_RATE, subtype="PCM_16")
        saved += 1
        print(f"saved (RMS={level:.4f}, peak={peak:.3f}): {os.path.basename(path)}")
        time.sleep(0.5)

    print(f"\nSaved {saved}/{args.count} {args.mode} clips.")
    if serial_port is not None:
        serial_port.close()
    if args.mode == "positive":
        print("Record several speakers, then run augment_data.py and retrain.")
    else:
        print("Now retrain; these files are automatically loaded as not_keyword samples.")


if __name__ == "__main__":
    main()
