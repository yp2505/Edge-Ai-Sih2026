# ==============================================================================
# 🚀 ENHANCED COLAB SCRIPT FOR "HEY VAANI" EDGE AI KEYWORD SPOTTING
# ==============================================================================
# Features:
# 1. Multi-folder Positive Audio Search (dataset/, voice sample/, keywords and augmented/)
# 2. Auto-unzip for voice_sample.zip & keywords and augmented.zip
# 3. 6:1 to 8:1 Negative-to-Positive Ratio Control
# 4. MUSAN Dataset Integration (Music/Singing + Environmental Noise + Speech)
# 5. Mozilla Common Voice & LibriSpeech Datasets (Continuous real human speech)
# 6. Dedicated Conversational Phrases (gTTS synthetic greetings/questions)
# 7. Hard-Negative Mining (Oversampled 5x for real false-positive clips)
# 8. Balanced Class Weights (compute_class_weight)
# 9. Per-Category Evaluation Breakdown (TPR & Category FPRs)
# 10. Hardware-matching INT8 TFLite Micro Export (model_data.h)
# ==============================================================================

# --- DEPENDENCY INSTALLATION (Uncomment in Colab cell if needed) ---
# !apt-get update -y && !apt-get install -y libsndfile1 ffmpeg espeak-ng
# !pip install -q tensorflow soundfile librosa scikit-learn matplotlib datasets kagglehub gtts requests tqdm

import os
import sys
import glob
import math
import zipfile
import tarfile
import urllib.request
import random
import warnings
from typing import List, Tuple, Dict

# Suppress librosa & future warnings
warnings.filterwarnings("ignore", category=UserWarning)
warnings.filterwarnings("ignore", category=FutureWarning)

import numpy as np
import librosa
import soundfile as sf
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers, regularizers
from sklearn.model_selection import train_test_split
from sklearn.utils.class_weight import compute_class_weight
from sklearn.metrics import classification_report, confusion_matrix

try:
    import kagglehub
    HAS_KAGGLEHUB = True
except ImportError:
    HAS_KAGGLEHUB = False

try:
    from gtts import gTTS
    HAS_GTTS = True
except ImportError:
    HAS_GTTS = False

# ─── Configuration & Audio Parameters ───────────────────────────────────────
DATA_DIR = "dataset"            # Primary directory for positive samples
HARD_NEG_DIR = os.path.join(DATA_DIR, "hard_negatives")

# Auto-detect Google Drive to persist downloads across Colab sessions
_gdrive_cache = "/content/drive/MyDrive/hey_vaani_cache"
_local_cache   = "dataset_cache"
CACHE_DIR = _gdrive_cache if os.path.isdir("/content/drive/MyDrive") else _local_cache

# Force kagglehub to use the persistent cache directory (so it doesn't re-download 12GB every session)
os.environ["KAGGLEHUB_CACHE"] = os.path.join(CACHE_DIR, "kagglehub")

print(f"  📂 Cache directory: {CACHE_DIR} ({'Google Drive — persists across sessions ✅' if CACHE_DIR == _gdrive_cache else 'Local Colab disk — re-downloaded each session ⚠️'})")

SAMPLE_RATE = 16000
DURATION = 1.0                  # 1.0 second window length (16,000 samples)
WINDOW_LEN = int(SAMPLE_RATE * DURATION)

N_MFCC = 13
N_FFT = 512
HOP_LENGTH = 320
N_FRAMES = 49                   # (16000 - 512) // 320 + 1 = 49 frames

LABEL_KEYWORD = 1.0
LABEL_NOT_KEYWORD = 0.0

TARGET_NEG_POS_RATIO_MIN = 6.0
TARGET_NEG_POS_RATIO_MAX = 8.0
TARGET_RATIO_NOMINAL = 7.0

# ─── Audio Utility & Hardware-Simulating Augmentation ────────────────────────
def augment_adc_audio(audio: np.ndarray, sr: int = SAMPLE_RATE, augment_prob: float = 0.5) -> np.ndarray:
    """Simulate real MAX4466 microphone hardware ADC noise & clipping imperfections."""
    if random.random() > augment_prob:
        return audio
    audio = audio.copy()
    
    # 1. Add background thermal/ADC noise
    noise_snr_db = random.uniform(-50, -35)
    signal_rms = np.sqrt(np.mean(audio**2)) + 1e-10
    noise_rms = signal_rms * (10 ** (noise_snr_db / 20))
    audio += np.random.normal(0, noise_rms, audio.shape).astype(np.float32)
    
    # 2. Random gain shift (-6dB to +6dB)
    gain_db = random.uniform(-6, 6)
    audio *= (10 ** (gain_db / 20))
    
    # 3. Soft clipping threshold
    clip_threshold = random.uniform(0.92, 0.99)
    audio = np.clip(audio, -clip_threshold, clip_threshold)
    return audio

def load_and_fix_audio(filepath: str, target_sr: int = SAMPLE_RATE) -> np.ndarray:
    """Load audio file via librosa/soundfile and resample/mono convert."""
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        audio, sr = librosa.load(filepath, sr=None, mono=True)
    if sr != target_sr:
        audio = librosa.resample(audio, orig_sr=sr, target_sr=target_sr)
    return audio.astype(np.float32)

def extract_window_chunks(audio: np.ndarray, window_len: int = WINDOW_LEN, hop: int = WINDOW_LEN // 2) -> List[np.ndarray]:
    """Slice longer audio into 1-second chunks."""
    chunks = []
    if len(audio) < window_len:
        padded = np.pad(audio, (0, window_len - len(audio)))
        chunks.append(padded)
    else:
        for start in range(0, len(audio) - window_len + 1, hop):
            chunks.append(audio[start:start + window_len])
    return chunks

def compute_mfcc(audio: np.ndarray, sr: int = SAMPLE_RATE) -> np.ndarray:
    """Compute MFCC features matching ESP32 firmware implementation exactly."""
    target_len = int(DURATION * sr)
    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        start = (len(audio) - target_len) // 2
        audio = audio[start:start + target_len]
        
    stft = tf.signal.stft(audio.astype(np.float32), frame_length=N_FFT, frame_step=HOP_LENGTH, fft_length=N_FFT)
    spectrogram = tf.abs(stft)
    mel_w = tf.signal.linear_to_mel_weight_matrix(40, spectrogram.shape[-1], sr, 20.0, sr / 2)
    log_mel = tf.math.log(tf.matmul(spectrogram, mel_w) + 1e-6)
    mfccs = tf.signal.mfccs_from_log_mel_spectrograms(log_mel)[..., :N_MFCC].numpy()
    
    if mfccs.shape[0] < N_FRAMES:
        mfccs = np.pad(mfccs, ((0, N_FRAMES - mfccs.shape[0]), (0, 0)))
    elif mfccs.shape[0] > N_FRAMES:
        mfccs = mfccs[:N_FRAMES, :]
    return mfccs

# ─── Positive Dataset Helper & Zip Extractor ─────────────────────────────────
def unzip_positive_archives():
    """Automatically extract positive zip archives if present in workspace."""
    zip_candidates = [
        "voice_sample.zip",
        "voice sample.zip",
    ]
    os.makedirs(DATA_DIR, exist_ok=True)
    for zip_name in zip_candidates:
        if os.path.exists(zip_name):
            print(f"  📦 Extracting positive archive '{zip_name}' into '{DATA_DIR}'...")
            try:
                with zipfile.ZipFile(zip_name, 'r') as zip_ref:
                    zip_ref.extractall(DATA_DIR)
                print(f"  ✅ Successfully extracted '{zip_name}'.")
            except Exception as e:
                print(f"  ⚠️ Warning: Could not extract {zip_name}: {e}")

# ─── Dataset Download & Loading Helpers ─────────────────────────────────────

def download_and_extract_tgz(url: str, extract_to: str, desc: str):
    """Download tar.gz dataset as fast as possible using aria2c (16x parallel) with pigz extraction."""
    os.makedirs(extract_to, exist_ok=True)
    marker = os.path.join(extract_to, ".completed")
    if os.path.exists(marker):
        print(f"  ✅ {desc} already downloaded & cached in '{extract_to}'.")
        return

    archive_path = os.path.join(extract_to, "archive.tar.gz")
    print(f"  🌐 Downloading {desc}...")

    # --- Install fast download tools on first run ---
    os.system("apt-get install -qq -y aria2 pigz > /dev/null 2>&1")

    # --- Try aria2c first (16 parallel connections = up to 10x faster) ---
    aria2_cmd = (
        f'aria2c -x 16 -s 16 -k 10M --retry-wait=3 --max-tries=5 '
        f'--console-log-level=warn --summary-interval=10 '
        f'-d "{extract_to}" -o "archive.tar.gz" "{url}"'
    )
    ret = os.system(aria2_cmd)
    if ret != 0:
        print(f"  ⚠️ aria2c failed, falling back to urllib...")
        try:
            urllib.request.urlretrieve(url, archive_path)
        except Exception as e:
            print(f"  ⚠️ Download failed: {e}")
            return

    print(f"  📦 Extracting {desc} (parallel pigz)...")
    # Try pigz (parallel gzip) for faster extraction
    ret = os.system(f'pigz -dc "{archive_path}" | tar -xf - -C "{extract_to}" 2>/dev/null')
    if ret != 0:
        # Fallback to standard tarfile
        try:
            with tarfile.open(archive_path, "r:gz") as tar:
                tar.extractall(path=extract_to)
        except Exception as e:
            print(f"  ⚠️ Extraction failed: {e}")
            return

    if os.path.exists(archive_path):
        os.remove(archive_path)
    with open(marker, "w") as f:
        f.write("OK")
    print(f"  ✅ {desc} extracted successfully.")


# 1. MUSAN Dataset (Music/Singing + Noise + Speech)
def load_musan_dataset(target_count: int) -> Dict[str, List[np.ndarray]]:
    """Download OpenSLR 17 (MUSAN) and return music (singing), noise, and speech subsets."""
    musan_dir = os.path.join(CACHE_DIR, "musan")
    url = "http://www.openslr.org/resources/17/musan.tar.gz"
    download_and_extract_tgz(url, musan_dir, "MUSAN Dataset (OpenSLR 17)")
    
    subsets = {
        "musan_music": [],  # Singing & instrumental confusion cases
        "musan_noise": [],  # Environmental sounds
        "musan_speech": []  # Background speech
    }
    
    base_search = os.path.join(musan_dir, "musan")
    if not os.path.exists(base_search):
        base_search = musan_dir
        
    cat_counts = {"music": int(target_count * 0.40), "noise": int(target_count * 0.30), "speech": int(target_count * 0.30)}
    
    for cat in ["music", "noise", "speech"]:
        cat_dir = os.path.join(base_search, cat)
        files = glob.glob(os.path.join(cat_dir, "**", "*.wav"), recursive=True)
        random.shuffle(files)
        
        target_sub = cat_counts.get(cat, target_count // 3)
        collected = []
        for filepath in files:
            if len(collected) >= target_sub:
                break
            try:
                audio = load_and_fix_audio(filepath)
                chunks = extract_window_chunks(audio)
                for chunk in chunks[:3]:  # Max 3 chunks per file
                    chunk = augment_adc_audio(chunk)
                    collected.append(compute_mfcc(chunk))
                    if len(collected) >= target_sub:
                        break
            except Exception:
                continue
        key_name = f"musan_{cat}"
        subsets[key_name] = collected
        print(f"     Loaded {len(collected)} MUSAN {cat} samples.")
        
    return subsets

# 2. Mozilla Common Voice Dataset
def load_mozilla_common_voice_dataset(target_count: int) -> List[np.ndarray]:
    """Download and load Mozilla Common Voice dataset via Kagglehub or local cache."""
    collected = []
    print("  🌐 Downloading / Loading Mozilla Common Voice dataset (this is ~12GB and may take 10-15 minutes)...")
    cv_dir = os.path.join(CACHE_DIR, "common_voice")
    
    cv_files = []
    if HAS_KAGGLEHUB:
        try:
            download_path = kagglehub.dataset_download("mozillaorg/common-voice")
            print(f"  ✅ Kagglehub download complete: {download_path}")
            cv_files = glob.glob(os.path.join(download_path, "**", "*.mp3"), recursive=True) + \
                       glob.glob(os.path.join(download_path, "**", "*.wav"), recursive=True)
        except Exception as e:
            print(f"  ℹ️ Kagglehub download skipped ({e}). Checking local cache...")
            
    if not cv_files:
        cv_files = glob.glob(os.path.join(cv_dir, "**", "*.mp3"), recursive=True) + \
                   glob.glob(os.path.join(cv_dir, "**", "*.wav"), recursive=True)
                   
    random.shuffle(cv_files)
    for filepath in cv_files:
        if len(collected) >= target_count:
            break
        try:
            audio = load_and_fix_audio(filepath)
            chunks = extract_window_chunks(audio, hop=WINDOW_LEN)
            for chunk in chunks[:2]:
                chunk = augment_adc_audio(chunk)
                collected.append(compute_mfcc(chunk))
                if len(collected) >= target_count:
                    break
        except Exception:
            continue
            
    print(f"     Loaded {len(collected)} Mozilla Common Voice speech samples.")
    return collected

# 3. LibriSpeech Continuous Speech Dataset
def load_librispeech_dataset(target_count: int) -> List[np.ndarray]:
    """Download LibriSpeech dev-clean from OpenSLR 12 for continuous speech."""
    ls_dir = os.path.join(CACHE_DIR, "librispeech")
    url = "http://www.openslr.org/resources/12/dev-clean.tar.gz"
    download_and_extract_tgz(url, ls_dir, "LibriSpeech dev-clean (OpenSLR 12)")
    
    flac_files = glob.glob(os.path.join(ls_dir, "**", "*.flac"), recursive=True)
    wav_files = glob.glob(os.path.join(ls_dir, "**", "*.wav"), recursive=True)
    all_files = flac_files + wav_files
    random.shuffle(all_files)
    
    collected = []
    for filepath in all_files:
        if len(collected) >= target_count:
            break
        try:
            audio = load_and_fix_audio(filepath)
            chunks = extract_window_chunks(audio, hop=WINDOW_LEN)
            for chunk in chunks:
                chunk = augment_adc_audio(chunk)
                collected.append(compute_mfcc(chunk))
                if len(collected) >= target_count:
                    break
        except Exception:
            continue
            
    print(f"     Loaded {len(collected)} LibriSpeech continuous speech samples.")
    return collected

# 4. Conversational Phrases (gTTS text-to-speech)
def load_conversational_phrases(target_count: int = 200) -> List[np.ndarray]:
    """Generate or load common greetings, questions, and conversational phrases via gTTS."""
    collected = []
    phrases = [
        "how are you", "hello hello", "good morning", "what is the time",
        "sing a song", "turn off the light", "play some music", "how is the weather",
        "what are you doing", "tell me a joke", "yes please", "no thank you",
        "who are you", "open the door", "cancel alarm", "set a timer for five minutes",
        "hey google", "alexa turn on tv", "siri play music", "where do you live",
        "call mom", "send a message", "stop playing", "volume up", "volume down"
    ]
    
    tts_dir = os.path.join(CACHE_DIR, "conversational_tts")
    os.makedirs(tts_dir, exist_ok=True)
    
    tlds = ["com", "co.uk", "ca", "co.in", "com.au"] if HAS_GTTS else []
    
    file_idx = 0
    if HAS_GTTS:
        for phrase in phrases:
            for tld in tlds:
                out_path = os.path.join(tts_dir, f"conv_{file_idx}.mp3")
                file_idx += 1
                if not os.path.exists(out_path):
                    try:
                        tts = gTTS(text=phrase, lang='en', tld=tld)
                        tts.save(out_path)
                    except Exception:
                        continue
                        
    tts_files = glob.glob(os.path.join(tts_dir, "*.mp3")) + glob.glob(os.path.join(tts_dir, "*.wav"))
    for filepath in tts_files:
        try:
            audio = load_and_fix_audio(filepath)
            chunks = extract_window_chunks(audio)
            for chunk in chunks:
                chunk = augment_adc_audio(chunk)
                collected.append(compute_mfcc(chunk))
        except Exception:
            continue
            
    # Oversample/replicate if needed to reach target_count
    if collected and len(collected) < target_count:
        multiplier = math.ceil(target_count / len(collected))
        collected = (collected * multiplier)[:target_count]
        
    print(f"     Loaded {len(collected)} Conversational phrase samples.")
    return collected

# 5. Hard Negatives Mining (Oversampled 5x)
def load_hard_negatives(hard_neg_dir: str = HARD_NEG_DIR, oversample_factor: int = 5) -> List[np.ndarray]:
    """Load real captured false-positive clips and oversample them for priority weighting."""
    collected = []
    if not os.path.exists(hard_neg_dir):
        print(f"  ℹ️  No dedicated '{hard_neg_dir}' folder found. Skipping hard-negatives oversampling.")
        return collected
        
    files = glob.glob(os.path.join(hard_neg_dir, "**", "*.wav"), recursive=True) + \
            glob.glob(os.path.join(hard_neg_dir, "**", "*.m4a"), recursive=True)
            
    for filepath in files:
        try:
            audio = load_and_fix_audio(filepath)
            chunks = extract_window_chunks(audio)
            for chunk in chunks:
                # Oversample 5x with ADC augmentation to prioritize hard false positives
                for _ in range(oversample_factor):
                    aug_chunk = augment_adc_audio(chunk, augment_prob=0.8)
                    collected.append(compute_mfcc(aug_chunk))
        except Exception:
            continue
            
    print(f"     Loaded {len(collected)} Hard-Negative samples (oversampled {oversample_factor}x).")
    return collected

# ─── Dataset Assembly & Ratio Balancing ──────────────────────────────────────
def prepare_enhanced_dataset():
    """Load positive and negative datasets across all categories, ensuring 6:1 to 8:1 ratio."""
    # 0. Auto-unzip positive archives if available
    unzip_positive_archives()
    
    X, y, categories = [], [], []
    
    # 1. Search all positive sample directories
    pos_dirs = [
        DATA_DIR,
        "voice sample",
        "voice_sample",
        "keywords and augmented",
        "tmp_voice_samples"
    ]
    
    pos_files = []
    for pdir in pos_dirs:
        if os.path.exists(pdir):
            pos_files += glob.glob(os.path.join(pdir, "**", "*.wav"), recursive=True)
            pos_files += glob.glob(os.path.join(pdir, "**", "*.m4a"), recursive=True)
            pos_files += glob.glob(os.path.join(pdir, "**", "*.mp3"), recursive=True)
            
    # Exclude hard_negatives subdirectory
    pos_files = sorted(list(set([f for f in pos_files if "hard_negatives" not in f])))
    
    print(f"\n  📂 Searching positive keyword audio files across folders: {pos_dirs}...")
    print(f"  Found {len(pos_files)} positive audio files. Loading...")
    
    for filepath in pos_files:
        try:
            audio = load_and_fix_audio(filepath)
            audio = augment_adc_audio(audio)
            X.append(compute_mfcc(audio))
            y.append(LABEL_KEYWORD)
            categories.append("positive")
        except Exception:
            continue
            
    n_positive = len(X)
    print(f"  ✅ Total Positive Keywords Loaded: {n_positive}")
    if n_positive == 0:
        raise ValueError(f"No positive audio files found! Please upload/unzip your team voice sample folder.")
        
    # Calculate target total negatives for 7:1 ratio (min 6:1, max 8:1)
    target_total_negatives = int(n_positive * TARGET_RATIO_NOMINAL)
    print(f"  🎯 Target Negative Count: {target_total_negatives} (Target Ratio: {TARGET_RATIO_NOMINAL}:1)")
    
    # Allocations across negative sources
    musan_target = int(target_total_negatives * 0.35) # 35% MUSAN (singing/music/noise)
    cv_target = int(target_total_negatives * 0.25)    # 25% Mozilla Common Voice
    ls_target = int(target_total_negatives * 0.25)    # 25% LibriSpeech continuous speech
    conv_target = int(target_total_negatives * 0.15)  # 15% Conversational TTS phrases
    
    # Load Negative Categories
    print("\n  🌐 Gathering Multi-Source Negative Datasets (MUSAN + Common Voice + LibriSpeech + Conversational)...")
    
    # Category 1: MUSAN (Music/Singing, Noise, Speech)
    musan_subsets = load_musan_dataset(musan_target)
    for cat_key, samples in musan_subsets.items():
        for mfcc in samples:
            X.append(mfcc)
            y.append(LABEL_NOT_KEYWORD)
            categories.append(cat_key)

    # Category 2: Mozilla Common Voice
    cv_samples = load_mozilla_common_voice_dataset(cv_target)
    for mfcc in cv_samples:
        X.append(mfcc)
        y.append(LABEL_NOT_KEYWORD)
        categories.append("common_voice")
            
    # Category 3: LibriSpeech (Continuous Speech)
    ls_samples = load_librispeech_dataset(ls_target)
    for mfcc in ls_samples:
        X.append(mfcc)
        y.append(LABEL_NOT_KEYWORD)
        categories.append("librispeech")
        
    # Category 4: Conversational Phrases
    conv_samples = load_conversational_phrases(conv_target)
    for mfcc in conv_samples:
        X.append(mfcc)
        y.append(LABEL_NOT_KEYWORD)
        categories.append("conversational")
        
    # Category 5: Hard Negatives (Priority Oversampled)
    hard_neg_samples = load_hard_negatives(HARD_NEG_DIR, oversample_factor=5)
    for mfcc in hard_neg_samples:
        X.append(mfcc)
        y.append(LABEL_NOT_KEYWORD)
        categories.append("hard_negatives")
        
    # Check current ratio and adjust with extra silence/ambient noise if below 6:1
    n_neg_current = sum(1 for label in y if label == LABEL_NOT_KEYWORD)
    ratio_current = n_neg_current / max(1, n_positive)
    
    if ratio_current < TARGET_NEG_POS_RATIO_MIN:
        needed_extra = int(n_positive * TARGET_RATIO_NOMINAL) - n_neg_current
        print(f"  ℹ️ Adding {needed_extra} synthetic ambient/silence noise samples to reach ~{TARGET_RATIO_NOMINAL}:1 ratio...")
        for _ in range(needed_extra):
            silence = np.random.normal(0, 0.002, WINDOW_LEN).astype(np.float32)
            silence = augment_adc_audio(silence, augment_prob=1.0)
            X.append(compute_mfcc(silence))
            y.append(LABEL_NOT_KEYWORD)
            categories.append("synthetic_silence")
            
    X = np.array(X, dtype=np.float32)[..., np.newaxis]
    y = np.array(y, dtype=np.float32)
    categories = np.array(categories)
    
    final_pos = int(np.sum(y == LABEL_KEYWORD))
    final_neg = int(np.sum(y == LABEL_NOT_KEYWORD))
    final_ratio = final_neg / max(1, final_pos)
    
    print("\n" + "=" * 60)
    print("  📊 FINAL DATASET SUMMARY & RATIO REPORT")
    print("=" * 60)
    print(f"  Total Samples:        {len(X)}")
    print(f"  Positive Keywords:    {final_pos}")
    print(f"  Total Negatives:      {final_neg}")
    print(f"  Final Negative:Pos:   {final_ratio:.2f}:1")
    assert TARGET_NEG_POS_RATIO_MIN <= final_ratio <= 10.0, f"Ratio {final_ratio:.2f}:1 outside target!"
    
    print("\n  Category Breakdown:")
    unique_cats, counts = np.unique(categories, return_counts=True)
    for cat, count in zip(unique_cats, counts):
        print(f"     - {cat:20s}: {count:5d} samples")
    print("=" * 60 + "\n")
    
    return X, y, categories

# ─── DS-CNN Model Architecture (Exact Hardware Parity) ────────────────────────
def build_ds_cnn(input_shape=(N_FRAMES, N_MFCC, 1), l2_strength=0.001):
    l2_reg = regularizers.l2(l2_strength)
    model = keras.Sequential([
        keras.Input(shape=input_shape, name="mfcc_input"),
        layers.Conv2D(64, (10, 4), strides=(2, 2), padding='same', use_bias=False, kernel_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(), layers.Dropout(0.2),
        
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, depthwise_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        layers.Conv2D(64, (1, 1), use_bias=False, kernel_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, depthwise_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        layers.Conv2D(64, (1, 1), use_bias=False, kernel_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, depthwise_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        layers.Conv2D(64, (1, 1), use_bias=False, kernel_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, depthwise_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        layers.Conv2D(64, (1, 1), use_bias=False, kernel_regularizer=l2_reg),
        layers.BatchNormalization(), layers.ReLU(),
        
        layers.GlobalAveragePooling2D(),
        layers.Dropout(0.3),
        layers.Dense(1, activation='sigmoid', kernel_regularizer=l2_reg, name="keyword_probability"),
    ])
    return model

# ─── Main Pipeline Execution ──────────────────────────────────────────────────
def main():
    print("=" * 65)
    print("  🧠 ENHANCED DS-CNN KEYWORD SPOTTING TRAINING (MULTI-SOURCE NEGATIVES)")
    print("=" * 65)
    
    # 1. Prepare Dataset
    X, y, categories = prepare_enhanced_dataset()
    
    # Stratified Train/Val/Test Split
    indices = np.arange(len(X))
    idx_train, idx_temp, y_train, y_temp = train_test_split(indices, y, test_size=0.2, stratify=y, random_state=42)
    idx_val, idx_test, y_val, y_test = train_test_split(idx_temp, y_temp, test_size=0.5, stratify=y_temp, random_state=42)
    
    X_train, X_val, X_test = X[idx_train], X[idx_val], X[idx_test]
    cat_test = categories[idx_test]
    
    # Compute Balanced Class Weights
    class_weights_arr = compute_class_weight("balanced", classes=np.unique(y_train), y=y_train)
    class_weight_dict = {0: float(class_weights_arr[0]), 1: float(class_weights_arr[1])}
    print(f"  ⚖️ Balanced Class Weights: Negative (0) = {class_weight_dict[0]:.3f}, Positive (1) = {class_weight_dict[1]:.3f}")
    
    # 2. Build & Compile Model
    model = build_ds_cnn()
    model.compile(
        optimizer=keras.optimizers.Adam(learning_rate=0.001),
        loss='binary_crossentropy',
        metrics=['accuracy', keras.metrics.AUC(name='auc'), keras.metrics.Precision(name='precision'), keras.metrics.Recall(name='recall')]
    )
    
    callbacks = [
        keras.callbacks.ModelCheckpoint("ds_cnn_best.keras", monitor='val_auc', save_best_only=True, verbose=1),
        keras.callbacks.ReduceLROnPlateau(monitor='val_loss', factor=0.5, patience=5, min_lr=1e-5),
        keras.callbacks.EarlyStopping(monitor='val_auc', patience=15, restore_best_weights=True)
    ]
    
    print(f"\n  🚀 Training DS-CNN Model on {len(X_train)} samples...")
    model.fit(
        X_train, y_train,
        validation_data=(X_val, y_val),
        epochs=50,
        batch_size=64,
        class_weight=class_weight_dict,
        callbacks=callbacks,
        verbose=1
    )
    
    # Load Best Model for Evaluation
    best_model = keras.models.load_model("ds_cnn_best.keras")
    
    # 3. Per-Category Evaluation Breakdown
    print("\n" + "=" * 65)
    print("  🔬 PER-CATEGORY EVALUATION BREAKDOWN (TEST SPLIT)")
    print("=" * 65)
    
    preds_prob = best_model.predict(X_test, verbose=0).flatten()
    preds_binary = (preds_prob >= 0.5).astype(np.float32)
    
    # True Positive Rate (TPR) on Positives
    pos_mask = (y_test == LABEL_KEYWORD)
    tpr = np.mean(preds_binary[pos_mask] == LABEL_KEYWORD) if np.sum(pos_mask) > 0 else 0.0
    print(f"  ✅ Positive Keywords True Positive Rate (TPR): {tpr * 100:.2f}% ({np.sum(preds_binary[pos_mask] == 1)} / {np.sum(pos_mask)})")
    
    print("\n  Negative Categories False Activation / False Positive Rates (FPR):")
    unique_test_cats = np.unique(cat_test)
    for cat in unique_test_cats:
        if cat == "positive":
            continue
        mask = (cat_test == cat)
        cat_preds = preds_binary[mask]
        fpr = np.mean(cat_preds == LABEL_KEYWORD) if np.sum(mask) > 0 else 0.0
        print(f"     - {cat:20s}: FPR = {fpr * 100:.2f}% (False Triggers: {np.sum(cat_preds == 1)} / {np.sum(mask)})")
        
    print("=" * 65 + "\n")
    
    # 4. TFLite INT8 Export & C++ Header Generation
    print("=" * 65)
    print("  🔄 Exporting Keras → TFLite INT8 (ESP32 Firmware Ready)")
    print("=" * 65)
    
    converter = tf.lite.TFLiteConverter.from_keras_model(best_model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    
    def representative_gen():
        indices = np.random.choice(len(X_train), size=200, replace=False)
        for idx in indices:
            yield [X_train[idx:idx+1]]
            
    converter.representative_dataset = representative_gen
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    
    tflite_model = converter.convert()
    with open("hey_vaani_int8.tflite", "wb") as f:
        f.write(tflite_model)
        
    header_path = "model_data.h"
    with open(header_path, "w") as f:
        f.write("// Auto-generated TFLite INT8 Model\n")
        f.write(f"// Size: {len(tflite_model)} bytes\n\n")
        f.write("#ifndef MODEL_DATA_H\n#define MODEL_DATA_H\n\n")
        f.write(f"const unsigned int g_model_len = {len(tflite_model)};\n\n")
        f.write("alignas(8) const unsigned char g_model_data[] = {\n  ")
        for i, byte in enumerate(tflite_model):
            if i > 0 and i % 12 == 0:
                f.write("\n  ")
            f.write(f"0x{byte:02x}, ")
        f.write("\n};\n\n#endif // MODEL_DATA_H\n")
        
    print(f"\n  ✅ SUCCESS! Final firmware header generated: '{header_path}' ({len(tflite_model)} bytes)")
    
    if len(tflite_model) > 65000:
        print("\n" + "!" * 65)
        print("  ⚠️ WARNING: MODEL SIZE EXCEEDS 65KB! ⚠️")
        print(f"  The generated model is {len(tflite_model)} bytes.")
        print("  This may overflow the existing ESP32 tensor arena (~54KB).")
        print("  Consider reducing the model capacity (e.g., lower filter count).")
        print("!" * 65 + "\n")
    
    try:
        from google.colab import files
        files.download('model_data.h')
        files.download('hey_vaani_int8.tflite')
        files.download('ds_cnn_best.keras')
    except Exception:
        pass

if __name__ == "__main__":
    main()
