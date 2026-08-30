#!/usr/bin/env python3
"""
Augment "Hey Vaani" keyword recordings to generate more training data.
Takes ~100 real recordings and produces ~3000+ augmented variants.

Augmentations applied:
  - Add background noise (from Speech Commands dataset)
  - Add white/pink noise at various SNR levels
  - Time shift (±100ms)
  - Speed change (0.85x - 1.15x)
  - Pitch shift (±2 semitones)
  - Room reverb simulation
  - Volume variation

Usage:
    python augment_data.py                     # Default augmentation
    python augment_data.py --multiplier 10     # 10x augmentation per sample
"""

import os
import sys
import glob
import random
import argparse
import numpy as np
import soundfile as sf
from tqdm import tqdm

# ─── Configuration ───────────────────────────────────────────────────────────
SAMPLE_RATE = 16000
TARGET_LENGTH = int(1.5 * SAMPLE_RATE)  # 1.5 seconds = 24000 samples

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
KEYWORD_DIR = os.path.join(DATA_DIR, "keyword")
NOISE_DIR = os.path.join(DATA_DIR, "noise")
AUGMENTED_DIR = os.path.join(DATA_DIR, "keyword_augmented")


def load_audio(filepath):
    """Load audio file, resample to 16kHz mono, pad/trim to target length."""
    audio, sr = sf.read(filepath, dtype='float32')
    
    # Convert to mono if stereo
    if len(audio.shape) > 1:
        audio = audio.mean(axis=1)
    
    # Resample if needed (simple linear interpolation)
    if sr != SAMPLE_RATE:
        duration = len(audio) / sr
        new_length = int(duration * SAMPLE_RATE)
        audio = np.interp(
            np.linspace(0, len(audio) - 1, new_length),
            np.arange(len(audio)),
            audio
        )
    
    # Pad or trim to target length
    if len(audio) < TARGET_LENGTH:
        # Pad with silence (centered)
        pad_total = TARGET_LENGTH - len(audio)
        pad_left = pad_total // 2
        pad_right = pad_total - pad_left
        audio = np.pad(audio, (pad_left, pad_right), mode='constant')
    elif len(audio) > TARGET_LENGTH:
        # Trim from center
        start = (len(audio) - TARGET_LENGTH) // 2
        audio = audio[start:start + TARGET_LENGTH]
    
    return audio


def load_noise_samples():
    """Load all noise files for mixing."""
    noise_files = glob.glob(os.path.join(NOISE_DIR, "*.wav"))
    noises = []
    for f in noise_files:
        try:
            audio, sr = sf.read(f, dtype='float32')
            if len(audio.shape) > 1:
                audio = audio.mean(axis=1)
            noises.append(audio)
        except Exception:
            pass
    return noises


# ─── Augmentation Functions ──────────────────────────────────────────────────

def add_white_noise(audio, snr_db=20):
    """Add white Gaussian noise at specified SNR."""
    signal_power = np.mean(audio ** 2)
    noise_power = signal_power / (10 ** (snr_db / 10))
    noise = np.random.normal(0, np.sqrt(noise_power), len(audio))
    return audio + noise.astype(np.float32)


def add_background_noise(audio, noise_samples, snr_db=15):
    """Mix with real background noise from dataset."""
    if not noise_samples:
        return audio
    
    noise = random.choice(noise_samples)
    
    # Random segment from noise
    if len(noise) > len(audio):
        start = random.randint(0, len(noise) - len(audio))
        noise_segment = noise[start:start + len(audio)]
    else:
        # Loop noise if shorter
        repeats = (len(audio) // len(noise)) + 1
        noise_segment = np.tile(noise, repeats)[:len(audio)]
    
    # Mix at specified SNR
    signal_power = np.mean(audio ** 2) + 1e-10
    noise_power = np.mean(noise_segment ** 2) + 1e-10
    scale = np.sqrt(signal_power / (noise_power * (10 ** (snr_db / 10))))
    
    return audio + scale * noise_segment


def time_shift(audio, max_shift_ms=100):
    """Shift audio left or right by random amount."""
    max_shift = int(max_shift_ms * SAMPLE_RATE / 1000)
    shift = random.randint(-max_shift, max_shift)
    return np.roll(audio, shift)


def change_speed(audio, speed_factor):
    """Change playback speed (resampling)."""
    indices = np.round(np.arange(0, len(audio), speed_factor)).astype(int)
    indices = indices[indices < len(audio)]
    stretched = audio[indices]
    
    # Pad or trim to original length
    if len(stretched) < len(audio):
        stretched = np.pad(stretched, (0, len(audio) - len(stretched)))
    else:
        stretched = stretched[:len(audio)]
    
    return stretched


def change_volume(audio, gain_db):
    """Change volume by specified dB."""
    gain = 10 ** (gain_db / 20)
    return np.clip(audio * gain, -1.0, 1.0)


def pitch_shift_simple(audio, semitones):
    """Simple pitch shift by resampling (changes duration slightly)."""
    factor = 2 ** (semitones / 12.0)
    indices = np.round(np.arange(0, len(audio), factor)).astype(int)
    indices = indices[indices < len(audio)]
    shifted = audio[indices]
    
    if len(shifted) < len(audio):
        shifted = np.pad(shifted, (0, len(audio) - len(shifted)))
    else:
        shifted = shifted[:len(audio)]
    
    return shifted


def add_reverb_simple(audio, decay=0.3, delay_ms=30):
    """Simple reverb effect using delayed copies."""
    delay_samples = int(delay_ms * SAMPLE_RATE / 1000)
    reverbed = audio.copy()
    
    for i in range(1, 4):  # 3 reflections
        delayed = np.zeros_like(audio)
        offset = delay_samples * i
        if offset < len(audio):
            delayed[offset:] = audio[:-offset] * (decay ** i)
            reverbed += delayed
    
    # Normalize
    max_val = np.max(np.abs(reverbed))
    if max_val > 0:
        reverbed = reverbed / max_val * np.max(np.abs(audio))
    
    return reverbed


def augment_one_sample(audio, noise_samples, augment_id):
    """Apply a random combination of augmentations."""
    augmented = audio.copy()
    
    # Each augmentation has a probability of being applied
    # Different augment_id creates different combinations
    
    if augment_id % 7 == 0:
        # White noise only
        snr = random.uniform(10, 30)
        augmented = add_white_noise(augmented, snr_db=snr)
    
    elif augment_id % 7 == 1:
        # Background noise
        snr = random.uniform(5, 25)
        augmented = add_background_noise(augmented, noise_samples, snr_db=snr)
    
    elif augment_id % 7 == 2:
        # Speed change + noise
        speed = random.uniform(0.85, 1.15)
        augmented = change_speed(augmented, speed)
        augmented = add_white_noise(augmented, snr_db=random.uniform(15, 30))
    
    elif augment_id % 7 == 3:
        # Pitch shift
        semitones = random.uniform(-2, 2)
        augmented = pitch_shift_simple(augmented, semitones)
    
    elif augment_id % 7 == 4:
        # Time shift + volume change
        augmented = time_shift(augmented, max_shift_ms=100)
        augmented = change_volume(augmented, random.uniform(-6, 6))
    
    elif augment_id % 7 == 5:
        # Reverb + background noise
        augmented = add_reverb_simple(augmented, decay=random.uniform(0.1, 0.4))
        augmented = add_background_noise(augmented, noise_samples, snr_db=random.uniform(10, 20))
    
    else:
        # Full combo: speed + pitch + noise + shift
        augmented = change_speed(augmented, random.uniform(0.9, 1.1))
        augmented = pitch_shift_simple(augmented, random.uniform(-1, 1))
        augmented = add_white_noise(augmented, snr_db=random.uniform(15, 25))
        augmented = time_shift(augmented, max_shift_ms=50)
    
    # Normalize to prevent clipping
    max_val = np.max(np.abs(augmented))
    if max_val > 1.0:
        augmented = augmented / max_val * 0.95
    
    return augmented.astype(np.float32)


def main():
    parser = argparse.ArgumentParser(description="Augment 'Hey Vaani' keyword samples")
    parser.add_argument("--multiplier", type=int, default=10, help="Augmentation multiplier per sample")
    args = parser.parse_args()
    
    print("=" * 60)
    print("  🔄 Data Augmentation — Hey Vaani")
    print("=" * 60)
    
    # Load original keyword samples
    keyword_files = glob.glob(os.path.join(KEYWORD_DIR, "*.wav"))
    if not keyword_files:
        print(f"\n  ❌ No keyword samples found in {KEYWORD_DIR}")
        print("  Run first: python record_keyword.py --count 50")
        sys.exit(1)
    
    print(f"  📂 Original samples: {len(keyword_files)}")
    print(f"  🔄 Multiplier: {args.multiplier}x")
    print(f"  📊 Will produce: ~{len(keyword_files) * args.multiplier} augmented samples")
    
    # Load noise samples
    print("\n  Loading noise samples...")
    noise_samples = load_noise_samples()
    print(f"  🔊 Loaded {len(noise_samples)} noise files")
    
    # Create output directory
    os.makedirs(AUGMENTED_DIR, exist_ok=True)
    
    # Also copy originals to augmented dir
    total_created = 0
    
    print(f"\n  Augmenting...")
    for filepath in tqdm(keyword_files, desc="  Processing"):
        audio = load_audio(filepath)
        basename = os.path.splitext(os.path.basename(filepath))[0]
        
        # Save original (normalized)
        out_path = os.path.join(AUGMENTED_DIR, f"{basename}_orig.wav")
        sf.write(out_path, audio, SAMPLE_RATE)
        total_created += 1
        
        # Create augmented versions
        for i in range(args.multiplier):
            augmented = augment_one_sample(audio, noise_samples, i)
            out_path = os.path.join(AUGMENTED_DIR, f"{basename}_aug{i:03d}.wav")
            sf.write(out_path, augmented, SAMPLE_RATE)
            total_created += 1
    
    print(f"\n  ✅ Total augmented samples: {total_created}")
    print(f"  📁 Location: {AUGMENTED_DIR}")
    print(f"\n  Next step: python train_model.py")


if __name__ == "__main__":
    main()
