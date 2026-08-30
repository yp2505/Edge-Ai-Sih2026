#!/usr/bin/env python3
"""
Convert phone recordings to training-ready format.

Takes any audio files (m4a, mp3, wav, ogg, flac) from a folder
and converts them to 16kHz mono WAV files for Hey Vaani training.

Usage:
    python convert_phone_recordings.py --input ~/Downloads/phone_recordings/
    python convert_phone_recordings.py --input recordings.zip
    python convert_phone_recordings.py --input ~/Downloads/ --speaker "yug"
"""

import os
import sys
import glob
import shutil
import zipfile
import argparse
import numpy as np

SAMPLE_RATE = 16000
OUTPUT_DIR  = os.path.join(os.path.dirname(__file__), "data", "keyword")
SUPPORTED   = [".wav", ".m4a", ".mp3", ".ogg", ".flac", ".aac", ".opus", ".webm"]


def convert_file(input_path: str, output_path: str) -> bool:
    """Convert any audio file to 16kHz mono WAV."""

    # Try soundfile first (fast, handles WAV/FLAC/OGG)
    try:
        import soundfile as sf
        audio, sr = sf.read(input_path, dtype='float32', always_2d=True)
        audio = audio.mean(axis=1)  # stereo → mono

        # Resample to 16kHz if needed
        if sr != SAMPLE_RATE:
            audio = _resample(audio, sr, SAMPLE_RATE)

        # Pad/trim to 1.5 seconds
        target_len = int(1.5 * SAMPLE_RATE)
        if len(audio) < target_len:
            audio = np.pad(audio, (0, target_len - len(audio)))
        else:
            audio = audio[:target_len]

        sf.write(output_path, audio, SAMPLE_RATE, subtype='PCM_16')
        return True
    except Exception:
        pass

    # Fallback: try pydub (handles m4a, mp3, etc.)
    try:
        from pydub import AudioSegment
        audio_seg = AudioSegment.from_file(input_path)
        audio_seg = audio_seg.set_channels(1)           # mono
        audio_seg = audio_seg.set_frame_rate(SAMPLE_RATE)  # 16kHz
        audio_seg = audio_seg.set_sample_width(2)       # 16-bit

        # Trim/pad to 1.5 seconds
        target_ms = 1500
        if len(audio_seg) < target_ms:
            silence = AudioSegment.silent(duration=target_ms - len(audio_seg))
            audio_seg = audio_seg + silence
        else:
            audio_seg = audio_seg[:target_ms]

        audio_seg.export(output_path, format="wav")
        return True
    except Exception as e:
        print(f"    ❌ Failed: {e}")
        return False


def _resample(audio: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    """Simple linear resampling."""
    duration    = len(audio) / orig_sr
    new_length  = int(duration * target_sr)
    return np.interp(
        np.linspace(0, len(audio) - 1, new_length),
        np.arange(len(audio)),
        audio
    ).astype(np.float32)


def extract_zip(zip_path: str) -> str:
    """Extract zip file to a temp folder. Returns extraction path."""
    extract_dir = zip_path.replace(".zip", "_extracted")
    os.makedirs(extract_dir, exist_ok=True)
    print(f"  📦 Extracting {os.path.basename(zip_path)}...")
    with zipfile.ZipFile(zip_path, 'r') as zf:
        zf.extractall(extract_dir)
    print(f"  ✅ Extracted to: {extract_dir}")
    return extract_dir


def find_audio_files(folder: str) -> list:
    """Recursively find all audio files in folder."""
    files = []
    for ext in SUPPORTED:
        files += glob.glob(os.path.join(folder, "**", f"*{ext}"), recursive=True)
        files += glob.glob(os.path.join(folder, "**", f"*{ext.upper()}"), recursive=True)
    return sorted(set(files))


def check_dependencies():
    """Check if required packages are installed."""
    has_soundfile = False
    has_pydub     = False

    try:
        import soundfile
        has_soundfile = True
    except ImportError:
        pass

    try:
        from pydub import AudioSegment
        has_pydub = True
    except ImportError:
        pass

    if not has_soundfile and not has_pydub:
        print("  ❌ No audio converter installed!")
        print("  Install one of:")
        print("    pip install soundfile          ← for WAV/FLAC/OGG")
        print("    pip install pydub              ← for m4a/mp3 (needs ffmpeg)")
        print("\n  For m4a (iPhone/Android recordings):")
        print("    pip install pydub")
        print("    sudo apt install ffmpeg        ← required by pydub")
        sys.exit(1)

    if has_pydub and not has_soundfile:
        print("  ⚠️  pydub found but soundfile missing — install soundfile for faster conversion:")
        print("      pip install soundfile")

    return has_soundfile, has_pydub


def main():
    parser = argparse.ArgumentParser(
        description="Convert phone recordings → 16kHz mono WAV for Hey Vaani training"
    )
    parser.add_argument("--input",   type=str, required=True,
                        help="Path to folder OR .zip file containing phone recordings")
    parser.add_argument("--speaker", type=str, default="phone",
                        help="Speaker name tag (default: phone)")
    parser.add_argument("--dry-run", action="store_true",
                        help="Show what would be converted without converting")
    args = parser.parse_args()

    print("=" * 60)
    print("  📱 Phone Recording Converter → Hey Vaani Training Data")
    print("=" * 60)

    # Check dependencies
    has_sf, has_pydub = check_dependencies()
    print(f"  🔧 soundfile: {'✅' if has_sf else '❌'} | pydub: {'✅' if has_pydub else '❌'}")

    # Handle zip input
    input_path = args.input
    if input_path.endswith(".zip"):
        if not os.path.exists(input_path):
            print(f"  ❌ Zip not found: {input_path}")
            sys.exit(1)
        input_path = extract_zip(input_path)

    if not os.path.isdir(input_path):
        print(f"  ❌ Folder not found: {input_path}")
        sys.exit(1)

    # Find audio files
    audio_files = find_audio_files(input_path)

    if not audio_files:
        print(f"  ❌ No audio files found in: {input_path}")
        print(f"  Supported formats: {', '.join(SUPPORTED)}")
        sys.exit(1)

    print(f"\n  📁 Found {len(audio_files)} audio files in: {input_path}")

    # Get existing count
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    existing = len(glob.glob(os.path.join(OUTPUT_DIR, "*.wav")))
    print(f"  📊 Existing training samples: {existing}")
    print(f"  📊 Will add: {len(audio_files)} new samples")
    print(f"  📊 Total after: {existing + len(audio_files)}")
    print(f"  📁 Output: {OUTPUT_DIR}\n")

    if args.dry_run:
        print("  🔍 DRY RUN — files that would be converted:")
        for f in audio_files:
            print(f"    {os.path.basename(f)}")
        return

    # Convert each file
    success = 0
    failed  = 0

    for i, filepath in enumerate(audio_files, 1):
        sample_num  = existing + i
        filename    = f"hey_vaani_{args.speaker}_{sample_num:04d}_phone.wav"
        output_path = os.path.join(OUTPUT_DIR, filename)

        ext = os.path.splitext(filepath)[1].lower()
        print(f"  [{i:3d}/{len(audio_files)}] {os.path.basename(filepath)} ({ext}) → {filename}", end="")

        if convert_file(filepath, output_path):
            print(" ✅")
            success += 1
        else:
            print(" ❌")
            failed += 1

    # Final summary
    total = len(glob.glob(os.path.join(OUTPUT_DIR, "*.wav")))
    print(f"\n{'=' * 60}")
    print(f"  ✅ Converted: {success} files")
    print(f"  ❌ Failed:    {failed} files")
    print(f"  📊 Total training samples now: {total}")
    print("=" * 60)

    if total < 100:
        print(f"  ⚠️  Need at least 500 samples. Currently: {total}. Keep recording!")
    elif total < 500:
        remaining = 500 - total
        print(f"  🟡 Getting there! Need {remaining} more samples to reach 500.")
    else:
        print(f"  🟢 {total} samples — enough to start training!")
        print(f"\n  Next step: python augment_data.py")

    if failed > 0:
        print(f"\n  💡 For m4a/mp3 files, install:")
        print(f"     pip install pydub && sudo apt install ffmpeg")


if __name__ == "__main__":
    main()
