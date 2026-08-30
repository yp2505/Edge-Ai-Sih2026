#!/usr/bin/env python3
"""
Record custom keyword samples for "Hey Vaani" KWS model training.
Each recording is 1.5 seconds, saved as 16kHz mono WAV.

Usage:
    python record_keyword.py                    # Interactive recording mode
    python record_keyword.py --count 50         # Record 50 samples
    python record_keyword.py --speaker "yug"    # Tag speaker name
"""

import os
import sys
import time
import argparse
import numpy as np
import sounddevice as sd
import soundfile as sf
from datetime import datetime

# ─── Configuration ───────────────────────────────────────────────────────────
SAMPLE_RATE = 16000       # 16kHz — standard for KWS
DURATION = 1.5            # 1.5 seconds per clip (keyword is ~0.8s, padding helps)
CHANNELS = 1              # Mono
KEYWORD = "hey_vaani"
OUTPUT_DIR = os.path.join(os.path.dirname(__file__), "data", "keyword")


def list_audio_devices():
    """Show available microphones."""
    print("\n🎤 Available Audio Devices:")
    print("─" * 60)
    devices = sd.query_devices()
    for i, dev in enumerate(devices):
        if dev['max_input_channels'] > 0:
            marker = " ← DEFAULT" if i == sd.default.device[0] else ""
            print(f"  [{i}] {dev['name']} (inputs: {dev['max_input_channels']}){marker}")
    print("─" * 60)


def record_one_sample(device_id=None):
    """Record a single audio sample."""
    print("\n  🔴 Recording...", end="", flush=True)
    audio = sd.rec(
        int(DURATION * SAMPLE_RATE),
        samplerate=SAMPLE_RATE,
        channels=CHANNELS,
        dtype='float32',
        device=device_id
    )
    sd.wait()  # Wait until recording is finished
    print(" ✅ Done!")
    return audio.flatten()


def save_sample(audio, sample_num, speaker="default"):
    """Save recorded audio as WAV file."""
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"{KEYWORD}_{speaker}_{sample_num:04d}_{timestamp}.wav"
    filepath = os.path.join(OUTPUT_DIR, filename)
    sf.write(filepath, audio, SAMPLE_RATE)
    return filepath


def check_audio_level(audio):
    """Check if recorded audio has sufficient volume."""
    rms = np.sqrt(np.mean(audio ** 2))
    peak = np.max(np.abs(audio))
    
    if rms < 0.005:
        print("  ⚠️  WARNING: Audio very quiet. Speak louder or move closer to mic.")
        return False
    if peak > 0.95:
        print("  ⚠️  WARNING: Audio clipping! Move away from mic slightly.")
        return False
    
    # Visual level meter
    level = int(rms * 100)
    bar = "█" * level + "░" * (20 - level)
    print(f"  📊 Level: [{bar}] RMS={rms:.4f}")
    return True


def playback_sample(audio):
    """Play back the recorded sample."""
    sd.play(audio, SAMPLE_RATE)
    sd.wait()


def main():
    parser = argparse.ArgumentParser(description="Record 'Hey Vaani' keyword samples")
    parser.add_argument("--count", type=int, default=0, help="Number of samples to record (0 = interactive)")
    parser.add_argument("--speaker", type=str, default="default", help="Speaker name tag")
    parser.add_argument("--device", type=int, default=None, help="Audio device ID")
    parser.add_argument("--list-devices", action="store_true", help="List audio devices")
    args = parser.parse_args()
    
    if args.list_devices:
        list_audio_devices()
        return
    
    print("=" * 60)
    print("  🎙️  HEY VAANI — Keyword Recording Tool")
    print("=" * 60)
    print(f"  Keyword:     'Hey Vaani'")
    print(f"  Duration:    {DURATION}s per clip")
    print(f"  Sample rate: {SAMPLE_RATE} Hz")
    print(f"  Speaker:     {args.speaker}")
    print(f"  Output:      {OUTPUT_DIR}")
    print("=" * 60)
    
    list_audio_devices()
    
    sample_num = len([f for f in os.listdir(OUTPUT_DIR) if f.endswith('.wav')]) if os.path.exists(OUTPUT_DIR) else 0
    print(f"\n  📁 Existing samples: {sample_num}")
    
    if args.count > 0:
        # Batch mode — record N samples with 2-second gaps
        print(f"\n  Recording {args.count} samples. Say 'Hey Vaani' after each beep.\n")
        for i in range(args.count):
            sample_num += 1
            print(f"\n  --- Sample {sample_num} of {sample_num + args.count - i - 1} ---")
            print("  🔔 Say 'Hey Vaani' NOW!")
            time.sleep(0.3)
            
            audio = record_one_sample(args.device)
            good = check_audio_level(audio)
            
            if good:
                path = save_sample(audio, sample_num, args.speaker)
                print(f"  💾 Saved: {os.path.basename(path)}")
            else:
                print("  ❌ Bad recording, skipping. Will re-record.")
                sample_num -= 1
            
            time.sleep(1.0)  # Gap between recordings
    else:
        # Interactive mode
        print("\n  Press ENTER to record, 'p' to playback last, 'q' to quit.\n")
        last_audio = None
        
        while True:
            user_input = input(f"  [{sample_num + 1}] Press ENTER to record (q=quit, p=play last): ").strip().lower()
            
            if user_input == 'q':
                break
            elif user_input == 'p' and last_audio is not None:
                print("  🔊 Playing back...")
                playback_sample(last_audio)
                continue
            
            sample_num += 1
            print("  🔔 Say 'Hey Vaani' NOW!")
            time.sleep(0.3)
            
            audio = record_one_sample(args.device)
            last_audio = audio
            check_audio_level(audio)
            
            path = save_sample(audio, sample_num, args.speaker)
            print(f"  💾 Saved: {os.path.basename(path)}")
    
    total = len([f for f in os.listdir(OUTPUT_DIR) if f.endswith('.wav')]) if os.path.exists(OUTPUT_DIR) else 0
    print(f"\n  ✅ Total keyword samples: {total}")
    print(f"  📁 Location: {OUTPUT_DIR}")
    
    if total < 100:
        print(f"  ⚠️  Need at least 500 samples. Currently: {total}. Keep recording!")
    elif total < 500:
        print(f"  🟡 Getting there! Target: 500+. Currently: {total}")
    else:
        print(f"  🟢 Excellent! {total} samples is enough to start training.")
    
    print("\n  Next step: python augment_data.py (to multiply your samples)")


if __name__ == "__main__":
    main()
