#!/usr/bin/env python3
"""
augment_for_hardware.py — INMP441 I2S Light Augmentation
=========================================================
Replaces the old MAX4466 ADC simulation. The INMP441 is a digital MEMS
microphone with clean 24-bit I2S output — no DC offset, no 12-bit
quantization, no 50Hz hum artifacts needed.

This script applies LIGHT augmentation only:
  1. Random gain ±3 dB (realistic volume variation)
  2. Time shift ±50ms (mic position variation)
  3. Additive Gaussian noise at SNR 30–50 dB (realistic background)

For best results: record training data directly through the ESP32+INMP441
pipeline using record_max4466_dataset.py (rename for clarity). This script
is a supplement, not a replacement for real hardware recordings.

Usage:
    python3 augment_for_hardware.py

Output:
    training/data/keyword_hw/   ← light-augmented samples
"""

import os
import sys
import numpy as np
from pathlib import Path
from scipy.io import wavfile

# ─── Config ────────────────────────────────────────────────────────────────────
TRAINING_DIR    = Path(__file__).parent
KEYWORD_DIR     = TRAINING_DIR / "data" / "keyword"
OUTPUT_DIR      = TRAINING_DIR / "data" / "keyword_hw"
TARGET_SR       = 16000    # Must match MFCC_SAMPLE_RATE
AUGMENT_PER_FILE = 3       # Generate N variations per original file

# ─── Light augmentation for INMP441 digital mic ───────────────────────────────

def to_float(audio_int16: np.ndarray) -> np.ndarray:
    """Convert int16 PCM [-32768, 32767] -> float32 [-1.0, 1.0]"""
    return audio_int16.astype(np.float32) / 32768.0

def to_int16(audio_f32: np.ndarray) -> np.ndarray:
    """Convert float32 [-1.0, 1.0] -> int16 PCM"""
    clipped = np.clip(audio_f32, -1.0, 1.0)
    return (clipped * 32767).astype(np.int16)

def augment_inmp441(audio_f32: np.ndarray, sr: int, seed: int = 0) -> np.ndarray:
    """
    Apply light augmentation realistic for INMP441 I2S digital mic.
    No ADC simulation — the mic output is already clean 24-bit digital.
    """
    rng = np.random.default_rng(seed)
    audio = audio_f32.copy()

    # ── Step 1: Random gain ±3 dB ────────────────────────────────────────────
    # Realistic: user moves relative to mic, different mic placements
    gain_db = rng.uniform(-3.0, 3.0)
    audio = audio * (10.0 ** (gain_db / 20.0))

    # ── Step 2: Time shift ±50ms ─────────────────────────────────────────────
    # Realistic: speech start time varies relative to 1-second window
    max_shift = int(sr * 0.050)  # 50ms = 800 samples at 16kHz
    shift = rng.integers(-max_shift, max_shift + 1)
    if shift > 0:
        audio = np.concatenate([np.zeros(shift, dtype=np.float32), audio[:-shift]])
    elif shift < 0:
        audio = np.concatenate([audio[-shift:], np.zeros(-shift, dtype=np.float32)])

    # ── Step 3: Additive Gaussian noise at SNR 30–50 dB ─────────────────────
    # Realistic: INMP441 has ~60dB SNR; background noise adds ~30–50dB SNR
    signal_power = np.mean(audio ** 2)
    if signal_power > 1e-10:
        snr_db = rng.uniform(30.0, 50.0)
        noise_power = signal_power / (10.0 ** (snr_db / 10.0))
        noise = rng.normal(0, np.sqrt(noise_power), len(audio))
        audio = audio + noise

    # ── Step 4: Final clip & normalise ────────────────────────────────────────
    audio = np.clip(audio, -1.0, 1.0)

    # Pad or trim to exact 1 second
    target_len = int(sr)
    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        audio = audio[:target_len]

    return audio


# ─── Main ──────────────────────────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("  INMP441 I2S Light Augmentation")
    print("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    wav_files = sorted(KEYWORD_DIR.glob("*.wav"))

    if not wav_files:
        print(f"  No WAV files found in {KEYWORD_DIR}")
        sys.exit(1)

    print(f"  Found {len(wav_files)} original samples in {KEYWORD_DIR}")
    print(f"  Output -> {OUTPUT_DIR}")
    print(f"  Generating {AUGMENT_PER_FILE} variations per file = "
          f"{len(wav_files) * AUGMENT_PER_FILE} total augmented samples\n")

    generated = 0
    errors = 0

    for wav_path in wav_files:
        try:
            sr, data = wavfile.read(str(wav_path))

            # Handle stereo -> mono
            if data.ndim > 1:
                data = data[:, 0]

            # Convert to float
            if data.dtype == np.int16:
                audio_f32 = to_float(data)
            elif data.dtype == np.int32:
                audio_f32 = data.astype(np.float32) / 2147483648.0
            else:
                audio_f32 = data.astype(np.float32)

            # Resample to 16kHz if needed
            if sr != TARGET_SR:
                from scipy.signal import resample_poly
                from math import gcd
                g = gcd(TARGET_SR, sr)
                audio_f32 = resample_poly(audio_f32, TARGET_SR // g, sr // g)

            # Generate N augmented versions
            stem = wav_path.stem
            for i in range(AUGMENT_PER_FILE):
                aug = augment_inmp441(audio_f32, TARGET_SR, seed=i * 1000 + generated)
                out_name = OUTPUT_DIR / f"{stem}_hw{i:02d}.wav"
                wavfile.write(str(out_name), TARGET_SR, to_int16(aug))
                generated += 1

        except Exception as e:
            print(f"  Warning: error processing {wav_path.name}: {e}")
            errors += 1

    print(f"\n  Generated {generated} light-augmented samples")
    if errors:
        print(f"  {errors} files had errors (skipped)")

    print("\n  Next steps:")
    print(f"    1. Listen to samples: aplay {OUTPUT_DIR}/<file>.wav")
    print("    2. Retrain model including hw samples:")
    print("       python finetune_rejection.py")
    print("    3. Export TFLite and flash to ESP32")


if __name__ == "__main__":
    main()
