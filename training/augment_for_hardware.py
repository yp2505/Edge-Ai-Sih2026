#!/usr/bin/env python3
"""
augment_for_hardware.py — MAX4466 ADC Domain Adaptation
========================================================
Transforms existing clean mic recordings to sound like
what the ESP32 MAX4466 ADC chain produces.

This bridges the "training/deployment domain gap" without
needing to re-record all samples.

MAX4466 ADC chain simulation:
  1. Upsample to 20kHz (match ADC hardware rate)
  2. 12-bit quantization (4096 levels, vs 65536 for 16-bit)
  3. DC offset injection + EMA DC removal (firmware simulation)
  4. Power supply interference (50Hz hum from ESP32 regulators)
  5. ADC thermal noise floor (inherent in analog electronics)
  6. MAX4466 frequency response (rolls off below ~80Hz)
  7. Downsample back to 16kHz (match firmware 4:5 drop)

Usage:
    python3 augment_for_hardware.py

Output:
    training/data/keyword_hw/   ← hardware-augmented samples
"""

import os
import sys
import numpy as np
from pathlib import Path
from scipy.io import wavfile
from scipy.signal import resample_poly, butter, sosfilt

# ─── Config ────────────────────────────────────────────────────────────────────
TRAINING_DIR    = Path(__file__).parent
KEYWORD_DIR     = TRAINING_DIR / "data" / "keyword"
OUTPUT_DIR      = TRAINING_DIR / "data" / "keyword_hw"
TARGET_SR       = 16000    # Final sample rate (must match MFCC_SAMPLE_RATE)
HW_SR           = 20000    # ESP32 ADC hardware rate
ADC_BITS        = 12       # MAX4466 → ESP32 ADC resolution
AUGMENT_PER_FILE = 3       # Generate N variations per original file

# ─── MAX4466 simulation functions ──────────────────────────────────────────────

def to_float(audio_int16: np.ndarray) -> np.ndarray:
    """Convert int16 PCM [-32768, 32767] → float32 [-1.0, 1.0]"""
    return audio_int16.astype(np.float32) / 32768.0

def to_int16(audio_f32: np.ndarray) -> np.ndarray:
    """Convert float32 [-1.0, 1.0] → int16 PCM"""
    clipped = np.clip(audio_f32, -1.0, 1.0)
    return (clipped * 32767).astype(np.int16)

def resample_to(audio: np.ndarray, src_sr: int, dst_sr: int) -> np.ndarray:
    """High-quality polyphase resample."""
    from math import gcd
    g = gcd(dst_sr, src_sr)
    return resample_poly(audio, dst_sr // g, src_sr // g)

def simulate_max4466_adc(audio_f32: np.ndarray, sr: int, seed: int = 0) -> np.ndarray:
    """
    Apply the full MAX4466 → ESP32 ADC → firmware pipeline to clean audio.
    """
    rng = np.random.default_rng(seed)
    n_samples = len(audio_f32)

    # ── Step 1: MAX4466 gain variation ───────────────────────────────────────
    # MAX4466 has a trimmable gain pot. Simulate ±6dB random gain variation.
    gain = rng.uniform(0.5, 1.8)
    audio = audio_f32 * gain

    # ── Step 2: MAX4466 highpass (rolls off below ~80Hz) ─────────────────────
    # The MAX4466 output cap creates a ~80Hz first-order highpass filter.
    sos = butter(1, 80.0 / (sr / 2), btype='high', output='sos')
    audio = sosfilt(sos, audio)

    # ── Step 3: Upsample to hardware ADC rate (20kHz) ────────────────────────
    if sr != HW_SR:
        audio = resample_to(audio, sr, HW_SR)
    hw_n = len(audio)

    # ── Step 4: Power supply noise (50Hz hum from ESP32 buck regulator) ──────
    t = np.arange(hw_n) / HW_SR
    hum_amp = rng.uniform(0.001, 0.008)  # Very low but present
    audio += hum_amp * np.sin(2 * np.pi * 50 * t)
    audio += hum_amp * 0.5 * np.sin(2 * np.pi * 100 * t)  # 2nd harmonic

    # ── Step 5: ADC DC offset injection (ESP32 ADC biased around VDD/2) ──────
    # Typical ESP32 ADC has 100-300mV DC offset due to analog bias
    dc_offset_raw = rng.uniform(0.04, 0.10)  # ~130-300 mV out of 3.3V
    audio += dc_offset_raw

    # ── Step 6: 12-bit quantisation (ADC_BITS = 12) ──────────────────────────
    # Map [-1.0, 1.0] float to [0, 4095] int, then back
    adc_levels = 2 ** ADC_BITS  # 4096
    adc_int = np.clip(((audio + 1.0) / 2.0 * adc_levels), 0, adc_levels - 1).astype(np.int32)
    audio = (adc_int.astype(np.float32) / adc_levels) * 2.0 - 1.0  # back to float

    # ── Step 7: ADC thermal + quantisation noise floor ───────────────────────
    # Real ADC adds thermal noise equal to ~1 LSB RMS
    lsb = 2.0 / adc_levels
    noise = rng.normal(0, lsb * 0.8, hw_n)
    audio += noise

    # ── Step 8: EMA DC offset removal (mirrors firmware logic exactly) ────────
    # firmware: adc_dc_offset = 0.005f * raw + 0.995f * adc_dc_offset
    alpha = 0.005
    dc_ema = 0.5   # init value (2048/4096 normalised)
    centered = np.zeros(hw_n, dtype=np.float32)
    for i in range(hw_n):
        dc_ema = alpha * audio[i] + (1 - alpha) * dc_ema
        centered[i] = audio[i] - dc_ema
    audio = centered

    # ── Step 9: Downsample 20kHz → 16kHz (drop every 5th sample) ────────────
    # Mirrors firmware: if i % 5 == 4: continue
    keep_mask = np.arange(hw_n) % 5 != 4
    audio = audio[keep_mask]

    # ── Step 10: PCM left-shift (ADC_TO_PCM_SHIFT = 4) ───────────────────────
    # firmware: hop_buf[i] = (int16_t)(centered << 4)
    # Amplify by 2^4 = 16 to match firmware scaling
    audio = audio * 16.0

    # ── Step 11: Final clip & normalise to [-1.0, 1.0] ───────────────────────
    audio = np.clip(audio, -1.0, 1.0)

    # Pad or trim to exact target length
    target_len = int(TARGET_SR)  # 1 second
    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        audio = audio[:target_len]

    return audio


# ─── Main ──────────────────────────────────────────────────────────────────────
def main():
    print("=" * 60)
    print("  MAX4466 ADC Hardware Augmentation")
    print("=" * 60)

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    wav_files = sorted(KEYWORD_DIR.glob("*.wav"))

    if not wav_files:
        print(f"❌ No WAV files found in {KEYWORD_DIR}")
        sys.exit(1)

    print(f"📂 Found {len(wav_files)} original samples in {KEYWORD_DIR}")
    print(f"📂 Output → {OUTPUT_DIR}")
    print(f"🔄 Generating {AUGMENT_PER_FILE} variations per file = "
          f"{len(wav_files) * AUGMENT_PER_FILE} total hw-augmented samples\n")

    generated = 0
    errors = 0

    for wav_path in wav_files:
        try:
            sr, data = wavfile.read(str(wav_path))

            # Handle stereo → mono
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
                audio_f32 = resample_to(audio_f32, sr, TARGET_SR)

            # Generate N augmented versions
            stem = wav_path.stem
            for i in range(AUGMENT_PER_FILE):
                aug = simulate_max4466_adc(audio_f32, TARGET_SR, seed=i * 1000 + generated)
                out_name = OUTPUT_DIR / f"{stem}_hw{i:02d}.wav"
                wavfile.write(str(out_name), TARGET_SR, to_int16(aug))
                generated += 1

        except Exception as e:
            print(f"  ⚠ Error processing {wav_path.name}: {e}")
            errors += 1

    print(f"\n✅ Generated {generated} hardware-augmented samples")
    if errors:
        print(f"⚠  {errors} files had errors (skipped)")

    print("\n📋 Next steps:")
    print("   1. Verify samples sound correct:")
    print(f"      aplay {OUTPUT_DIR}/hey_vaani_yug_0001_hw00.wav")
    print("   2. Retrain the model including hw samples:")
    print("      python3 train_model.py --include-hw")
    print("   3. Convert to TFLite and update ESP32:")
    print("      python3 convert_tflite.py")
    print("      python3 update_model_header.py")


if __name__ == "__main__":
    main()
