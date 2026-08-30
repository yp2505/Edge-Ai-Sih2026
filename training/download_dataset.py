#!/usr/bin/env python3
"""
Download Google Speech Commands v2 dataset for negative/background samples.
This provides ~105,000 audio clips of 35 words — used as "NOT Hey Vaani" training data.

Usage:
    python download_dataset.py              # Download + extract
    python download_dataset.py --small      # Download small subset only (faster)
"""

import os
import sys
import tarfile
import urllib.request
import shutil
import random
import argparse
from tqdm import tqdm

# ─── Configuration ───────────────────────────────────────────────────────────
DATASET_URL = "https://storage.googleapis.com/download.tensorflow.org/data/speech_commands_v0.02.tar.gz"
DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
DOWNLOAD_PATH = os.path.join(DATA_DIR, "speech_commands_v0.02.tar.gz")
EXTRACT_DIR = os.path.join(DATA_DIR, "speech_commands")
BACKGROUND_DIR = os.path.join(DATA_DIR, "background")
NOISE_DIR = os.path.join(DATA_DIR, "noise")

# Words to use as negative samples (NOT your keyword)
NEGATIVE_WORDS = [
    "yes", "no", "up", "down", "left", "right", "on", "off",
    "stop", "go", "zero", "one", "two", "three", "four", "five",
    "six", "seven", "eight", "nine", "cat", "dog", "bird", "tree",
    "house", "bed", "happy", "wow", "follow", "learn", "visual"
]


class DownloadProgressBar(tqdm):
    """Progress bar for urllib downloads."""
    def update_to(self, block_num=1, block_size=1, total_size=None):
        if total_size is not None:
            self.total = total_size
        self.update(block_num * block_size - self.n)


def download_dataset():
    """Download Google Speech Commands v2 dataset."""
    os.makedirs(DATA_DIR, exist_ok=True)
    
    if os.path.exists(DOWNLOAD_PATH):
        print(f"  ✅ Dataset archive already exists: {DOWNLOAD_PATH}")
        return
    
    print("  📥 Downloading Google Speech Commands v2 (2.4 GB)...")
    print(f"  URL: {DATASET_URL}")
    print("  This will take a few minutes depending on your internet speed.\n")
    
    with DownloadProgressBar(unit='B', unit_scale=True, miniters=1, desc="  Downloading") as t:
        urllib.request.urlretrieve(DATASET_URL, DOWNLOAD_PATH, reporthook=t.update_to)
    
    print("\n  ✅ Download complete!")


def extract_dataset():
    """Extract the tar.gz archive."""
    if os.path.exists(EXTRACT_DIR) and len(os.listdir(EXTRACT_DIR)) > 10:
        print(f"  ✅ Dataset already extracted: {EXTRACT_DIR}")
        return
    
    print("  📦 Extracting dataset...")
    os.makedirs(EXTRACT_DIR, exist_ok=True)
    
    with tarfile.open(DOWNLOAD_PATH, 'r:gz') as tar:
        tar.extractall(path=EXTRACT_DIR)
    
    print("  ✅ Extraction complete!")


def prepare_negative_samples(max_per_word=300):
    """
    Copy negative word samples to background/ directory.
    These are words that are NOT "Hey Vaani" — model learns to ignore them.
    Automatically scans all word directories available in the dataset.
    """
    os.makedirs(BACKGROUND_DIR, exist_ok=True)
    
    # Automatically discover all word directories (excluding _background_noise_)
    all_dirs = [
        d for d in os.listdir(EXTRACT_DIR) 
        if os.path.isdir(os.path.join(EXTRACT_DIR, d)) and not d.startswith('_')
    ]
    
    total_copied = 0
    print(f"\n  📂 Preparing negative samples ({max_per_word} per word across {len(all_dirs)} word categories)...")
    
    for word in sorted(all_dirs):
        word_dir = os.path.join(EXTRACT_DIR, word)
        wav_files = [f for f in os.listdir(word_dir) if f.endswith('.wav')]
        if not wav_files:
            continue
        
        selected = random.sample(wav_files, min(max_per_word, len(wav_files)))
        
        for wav_file in selected:
            src = os.path.join(word_dir, wav_file)
            dst = os.path.join(BACKGROUND_DIR, f"{word}_{wav_file}")
            shutil.copy2(src, dst)
            total_copied += 1
        
        print(f"  ✅ {word:12s}: {len(selected)} samples")
    
    print(f"\n  📊 Total negative samples: {total_copied}")
    return total_copied


def prepare_noise_samples():
    """
    Copy background noise files for data augmentation.
    Speech Commands dataset includes _background_noise_ directory.
    """
    os.makedirs(NOISE_DIR, exist_ok=True)
    
    noise_src = os.path.join(EXTRACT_DIR, "_background_noise_")
    if not os.path.isdir(noise_src):
        print("  ⚠️  No background noise directory found in dataset")
        return 0
    
    copied = 0
    for f in os.listdir(noise_src):
        if f.endswith('.wav'):
            shutil.copy2(os.path.join(noise_src, f), os.path.join(NOISE_DIR, f))
            copied += 1
    
    print(f"  🔊 Noise samples copied: {copied}")
    return copied


def main():
    parser = argparse.ArgumentParser(description="Download Google Speech Commands dataset")
    parser.add_argument("--small", action="store_true", help="Use smaller subset (100 per word)")
    parser.add_argument("--skip-download", action="store_true", help="Skip download, use existing archive")
    args = parser.parse_args()
    
    max_per_word = 100 if args.small else 200
    
    print("=" * 60)
    print("  📦 Google Speech Commands v2 — Dataset Setup")
    print("=" * 60)
    
    # Step 1: Download
    if not args.skip_download:
        download_dataset()
    
    # Step 2: Extract
    extract_dataset()
    
    # Step 3: Prepare negative samples
    neg_count = prepare_negative_samples(max_per_word)
    
    # Step 4: Prepare noise samples
    noise_count = prepare_noise_samples()
    
    # Summary
    keyword_count = 0
    if os.path.exists(os.path.join(DATA_DIR, "keyword")):
        keyword_count = len([f for f in os.listdir(os.path.join(DATA_DIR, "keyword")) if f.endswith('.wav')])
    
    print("\n" + "=" * 60)
    print("  📊 Dataset Summary")
    print("=" * 60)
    print(f"  Keyword samples ('Hey Vaani'): {keyword_count}")
    print(f"  Negative samples (other words): {neg_count}")
    print(f"  Noise samples (for augmentation): {noise_count}")
    print("=" * 60)
    
    if keyword_count < 50:
        print("\n  ⚠️  You need to record 'Hey Vaani' samples first!")
        print("  Run: python record_keyword.py --count 50 --speaker YOUR_NAME")
    
    print("\n  Next step: python augment_data.py")


if __name__ == "__main__":
    main()
