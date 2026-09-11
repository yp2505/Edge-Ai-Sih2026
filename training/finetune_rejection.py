#!/usr/bin/env python3
"""
finetune_rejection.py — Fine-tune "Hey Vaani" KWS model to reject confusable wake words.

TARGETED PASS: Teaches the model to reject "Hey Google", "Alexa", "OK Google",
"Hey Siri" and similar wake-word-like phrases. NOT a general-purpose retrain.

Datasets (LOCAL ONLY, no external downloads):
  Positives: voice sample/ folder (Divy, Khush, Nil, Yug, Vaani — "Hey Vaani" clips)
  Negatives: Downloads/samples + Downloads/Divy_s Voice (hard wake-word confusables)

RATIO FIX: ~158 raw negatives vs ~548 raw positives is ~1:3.5 (negatives underrepresented).
Fixed via aggressive oversampling: each negative gets 6-10 augmented copies.

Usage:
    python finetune_rejection.py
    python finetune_rejection.py --epochs 20 --lr 5e-5
"""

import os
import sys
import io

# Force UTF-8 output on Windows console
if sys.platform == "win32":
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")
import glob
import random
import warnings
import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers
from sklearn.model_selection import train_test_split
from sklearn.metrics import classification_report, confusion_matrix
import soundfile as sf

warnings.filterwarnings("ignore")

# ─── Configuration ───────────────────────────────────────────────────────────
SAMPLE_RATE = 16000
DURATION    = 1.0
N_MFCC      = 13
N_FFT       = 512
HOP_LENGTH  = 320
N_FRAMES    = 49

# Paths
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOICE_SAMPLE_DIR = os.path.join(PROJECT_ROOT, "voice sample")
NEGATIVE_DIR_1 = os.path.join(os.path.expanduser("~"), "Downloads", "samples")
NEGATIVE_DIR_2 = os.path.join(os.path.expanduser("~"), "Downloads", "Divy_s Voice")
OUTPUT_DIR     = os.path.join(PROJECT_ROOT, "training", "finetuned_output")
MODEL_DIR      = os.path.join(PROJECT_ROOT, "training", "models")
FIRMWARE_DIR   = os.path.join(PROJECT_ROOT, "esp32_firmware", "main")

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(MODEL_DIR, exist_ok=True)

# ─── Feature Extraction (MUST match ESP32 firmware exactly) ─────────────────
def compute_mfcc(audio, sr=SAMPLE_RATE):
    target_len = int(DURATION * sr)
    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        start = (len(audio) - target_len) // 2
        audio = audio[start:start + target_len]

    stft = tf.signal.stft(
        audio.astype(np.float32),
        frame_length=N_FFT, frame_step=HOP_LENGTH, fft_length=N_FFT
    )
    spectrogram = tf.abs(stft)
    num_bins = spectrogram.shape[-1]
    mel_w = tf.signal.linear_to_mel_weight_matrix(40, num_bins, sr, 20.0, sr / 2)
    mel = tf.matmul(spectrogram, mel_w)
    log_mel = tf.math.log(mel + 1e-6)
    mfccs = tf.signal.mfccs_from_log_mel_spectrograms(log_mel)[..., :N_MFCC].numpy()

    if mfccs.shape[0] < N_FRAMES:
        mfccs = np.pad(mfccs, ((0, N_FRAMES - mfccs.shape[0]), (0, 0)))
    elif mfccs.shape[0] > N_FRAMES:
        mfccs = mfccs[:N_FRAMES, :]
    return mfccs


def load_audio(path):
    """Load audio file, converting to 16kHz mono float32."""
    try:
        # Use soundfile for WAV, or subprocess for m4a/mpeg
        ext = os.path.splitext(path)[1].lower()
        if ext in ('.wav', '.flac', '.ogg'):
            audio, sr = sf.read(path, dtype='float32')
        else:
            # For m4a/mpeg, use pydub or ffmpeg via subprocess
            audio, sr = _load_with_ffmpeg(path)
        if audio.ndim > 1:
            audio = audio.mean(axis=1)
        if sr != SAMPLE_RATE:
            new_len = int(len(audio) * SAMPLE_RATE / sr)
            audio = np.interp(
                np.linspace(0, len(audio) - 1, new_len),
                np.arange(len(audio)), audio
            )
        return audio.astype(np.float32)
    except Exception as e:
        print(f"    ⚠️  Failed to load {os.path.basename(path)}: {e}")
        return None


def _load_with_ffmpeg(path):
    """Load audio via ffmpeg subprocess (handles m4a, mpeg, mp3, etc.)."""
    import subprocess
    import io
    cmd = [
        "ffmpeg", "-i", path, "-f", "wav", "-acodec", "pcm_s16le",
        "-ar", str(SAMPLE_RATE), "-ac", "1", "-v", "error", "-",
    ]
    result = subprocess.run(cmd, capture_output=True, timeout=10)
    if result.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {result.stderr.decode()[:200]}")
    audio, sr = sf.read(io.BytesIO(result.stdout), dtype='float32')
    return audio, sr


# ─── Augmentation Functions ─────────────────────────────────────────────────
def augment_audio(audio, sr=SAMPLE_RATE, n_augmentations=3):
    """Apply moderate augmentation to audio. Returns list of augmented clips."""
    augmented = [audio.copy()]  # Always include clean original
    for _ in range(n_augmentations - 1):
        aug = audio.copy()
        method = random.choice(["gain", "noise", "shift", "roll", "pitch"])

        if method == "gain":
            # Random gain: -12dB to +6dB
            gain_db = random.uniform(-12, 6)
            aug = aug * (10 ** (gain_db / 20))
        elif method == "noise":
            # Light background noise
            noise_level = random.uniform(0.001, 0.01)
            aug = aug + np.random.normal(0, noise_level, len(aug)).astype(np.float32)
        elif method == "shift":
            # Small time shift
            shift_samples = random.randint(-int(sr * 0.1), int(sr * 0.1))
            aug = np.roll(aug, shift_samples)
        elif method == "roll":
            # Circular roll
            roll_samples = random.randint(-int(sr * 0.15), int(sr * 0.15))
            aug = np.roll(aug, roll_samples)
        elif method == "pitch":
            # Slight pitch variation via resampling
            factor = random.uniform(0.9, 1.1)
            new_len = int(len(aug) * factor)
            aug = np.interp(
                np.linspace(0, 1, len(aug)),
                np.linspace(0, 1, new_len),
                aug[:new_len] if new_len <= len(aug) else np.pad(aug, (0, new_len - len(aug)))
            )[:len(aug)]

        # Clip to [-1, 1]
        aug = np.clip(aug, -1.0, 1.0)
        augmented.append(aug)
    return augmented


def augment_negatives(audio_list, min_augmentations=6, max_augmentations=10):
    """Aggressively augment hard negatives to fix ratio imbalance."""
    augmented = []
    for audio in audio_list:
        n_aug = random.randint(min_augmentations, max_augmentations)
        augmented.extend(augment_audio(audio, n_augmentations=n_aug))
    return augmented


# ─── DS-CNN Architecture (same as train_model.py) ──────────────────────────
def build_ds_cnn(input_shape=(N_FRAMES, N_MFCC, 1)):
    """
    Build DS-CNN (Depthwise Separable CNN) for keyword spotting.
    Same architecture as all prior successful training runs.
    Output: 1 sigmoid neuron (keyword probability).
    """
    model = keras.Sequential([
        keras.Input(shape=input_shape, name="mfcc_input"),

        # First conv layer
        layers.Conv2D(64, (10, 4), strides=(2, 2), padding='same', use_bias=False,
                      name="conv_initial"),
        layers.BatchNormalization(name="bn_initial"),
        layers.ReLU(name="relu_initial"),
        layers.Dropout(0.2),

        # DS-Conv Block 1
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, name="dw_conv_1"),
        layers.BatchNormalization(name="bn_dw_1"),
        layers.ReLU(name="relu_dw_1"),
        layers.Conv2D(64, (1, 1), use_bias=False, name="pw_conv_1"),
        layers.BatchNormalization(name="bn_pw_1"),
        layers.ReLU(name="relu_pw_1"),

        # DS-Conv Block 2
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, name="dw_conv_2"),
        layers.BatchNormalization(name="bn_dw_2"),
        layers.ReLU(name="relu_dw_2"),
        layers.Conv2D(64, (1, 1), use_bias=False, name="pw_conv_2"),
        layers.BatchNormalization(name="bn_pw_2"),
        layers.ReLU(name="relu_pw_2"),

        # DS-Conv Block 3
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, name="dw_conv_3"),
        layers.BatchNormalization(name="bn_dw_3"),
        layers.ReLU(name="relu_dw_3"),
        layers.Conv2D(64, (1, 1), use_bias=False, name="pw_conv_3"),
        layers.BatchNormalization(name="bn_pw_3"),
        layers.ReLU(name="relu_pw_3"),

        # DS-Conv Block 4
        layers.DepthwiseConv2D((3, 3), padding='same', use_bias=False, name="dw_conv_4"),
        layers.BatchNormalization(name="bn_dw_4"),
        layers.ReLU(name="relu_dw_4"),
        layers.Conv2D(64, (1, 1), use_bias=False, name="pw_conv_4"),
        layers.BatchNormalization(name="bn_pw_4"),
        layers.ReLU(name="relu_pw_4"),

        # Global pooling
        layers.GlobalAveragePooling2D(name="global_pool"),
        layers.Dropout(0.3),

        # Classification head
        layers.Dense(1, activation='sigmoid', name="keyword_probability"),
    ], name="DS_CNN_KeywordSpotting")

    return model


# ─── Main Pipeline ──────────────────────────────────────────────────────────
def main():
    print("\n" + "=" * 70)
    print("  Hey Vaani — Targeted Wake-Word Rejection Fine-Tune")
    print("=" * 70)
    print("  Purpose: Teach model to REJECT 'Hey Google', 'Alexa', 'OK Google',")
    print("           'Hey Siri' and similar confusable wake-word phrases.")
    print("  Data: LOCAL ONLY — no external downloads.")
    print("=" * 70)

    # ── 1. Load positive samples ("Hey Vaani" from voice sample/ folder) ──────
    print("\n📁 Loading POSITIVE samples (Hey Vaani)...")
    pos_files = []
    for subfolder in os.listdir(VOICE_SAMPLE_DIR):
        sub_path = os.path.join(VOICE_SAMPLE_DIR, subfolder)
        if os.path.isdir(sub_path):
            # Recursively find all audio files
            for ext in ('*.wav', '*.m4a', '*.mpeg', '*.mp3', '*.MP3'):
                pos_files.extend(glob.glob(os.path.join(sub_path, "**", ext), recursive=True))
    pos_files = list(set(pos_files))  # deduplicate
    print(f"  Found {len(pos_files)} positive audio files")

    # Load and augment positives (3x each: 1 clean + 2 augmented)
    pos_audios = []
    for f in pos_files:
        audio = load_audio(f)
        if audio is not None and len(audio) > 0:
            pos_audios.append(audio)
    print(f"  Loaded {len(pos_audios)} positive clips successfully")

    X_pos = []
    y_pos = []
    for audio in pos_audios:
        augmented = augment_audio(audio, n_augmentations=3)
        for aug in augmented:
            mfcc = compute_mfcc(aug)
            X_pos.append(mfcc)
            y_pos.append(1)

    print(f"  Positive after augmentation: {len(X_pos)} samples")

    # ── 2. Load hard-negative samples (wake-word confusables) ────────────────
    print("\n📁 Loading HARD-NEGATIVE samples (Hey Google, Alexa, etc.)...")
    neg_files = []
    for neg_dir in [NEGATIVE_DIR_1, NEGATIVE_DIR_2]:
        if os.path.exists(neg_dir):
            for ext in ('*.wav', '*.m4a', '*.mpeg', '*.mp3', '*.MP3'):
                neg_files.extend(glob.glob(os.path.join(neg_dir, "**", ext), recursive=True))
            print(f"  Found {len(glob.glob(os.path.join(neg_dir, '**/*'), recursive=True))} files in {os.path.basename(neg_dir)}")
        else:
            print(f"  ⚠️  Directory not found: {neg_dir}")
    neg_files = list(set(neg_files))
    print(f"  Total negative files: {len(neg_files)}")

    # Load negatives
    neg_audios = []
    for f in neg_files:
        audio = load_audio(f)
        if audio is not None and len(audio) > 0:
            neg_audios.append(audio)
    print(f"  Loaded {len(neg_audios)} negative clips successfully")

    # Aggressively augment negatives (6-10 copies each)
    X_neg = []
    y_neg = []
    augmented_negs = augment_negatives(neg_audios, min_augmentations=6, max_augmentations=10)
    for aug in augmented_negs:
        mfcc = compute_mfcc(aug)
        X_neg.append(mfcc)
        y_neg.append(0)

    print(f"  Negative after aggressive augmentation: {len(X_neg)} samples")

    # ── 3. Print ratio summary ───────────────────────────────────────────────
    print("\n" + "─" * 50)
    print("  📊 DATASET RATIO SUMMARY")
    print("─" * 50)
    print(f"  Raw positives:       {len(pos_audios)}")
    print(f"  Raw negatives:       {len(neg_audios)}")
    print(f"  Raw ratio (neg:pos): 1:{len(pos_audios)/max(len(neg_audios),1):.1f}")
    print(f"  Augmented positives: {len(X_pos)}")
    print(f"  Augmented negatives: {len(X_neg)}")
    final_ratio = len(X_neg) / max(len(X_pos), 1)
    print(f"  Final ratio (neg:pos): {final_ratio:.1f}:1")
    if final_ratio < 3.0:
        print(f"  ⚠️  Ratio below 3:1 — acceptable for this TARGETED rejection pass.")
        print(f"     This is NOT a general-purpose retrain. The goal is teaching")
        print(f"     wake-word confusion rejection, not replacing the full model.")
    else:
        print(f"  ✅ Good ratio for targeted fine-tune.")
    print("─" * 50)

    # ── 4. Combine and split ─────────────────────────────────────────────────
    X = np.array(X_pos + X_neg, dtype=np.float32)[..., np.newaxis]
    y = np.array(y_pos + y_neg, dtype=np.float32)

    # Hold out 10% for sanity test (MUST NOT be used in training)
    X_trainval, X_test, y_trainval, y_test = train_test_split(
        X, y, test_size=0.1, random_state=42, stratify=y
    )
    X_train, X_val, y_train, y_val = train_test_split(
        X_trainval, y_trainval, test_size=0.2, random_state=42, stratify=y_trainval
    )

    print(f"\n  Train: {len(X_train)} | Val: {len(X_val)} | Test (held-out): {len(X_test)}")

    # ── 5. Build model ───────────────────────────────────────────────────────
    print("\n🔨 Building DS-CNN model...")
    model = build_ds_cnn()
    model.summary()

    # ── 6. Train ALL layers (no pre-existing .keras model available) ───────────
    # Since no .keras weights file exists, this is effectively training from
    # scratch with the local dataset. All layers must be trainable.
    print("\n🔓 All layers trainable (no pre-existing weights found)...")

    total_params = sum(np.prod(w.shape) for w in model.trainable_weights)
    total_all = sum(np.prod(w.shape) for w in model.weights)
    print(f"\n  Trainable: {total_params:,} / {total_all:,} params "
          f"({100*total_params/total_all:.1f}%)")

    # ── 7. Compute class weights ─────────────────────────────────────────────
    n_pos = int(np.sum(y_train))
    n_neg = len(y_train) - n_pos
    weight_pos = len(y_train) / (2.0 * max(n_pos, 1))
    weight_neg = len(y_train) / (2.0 * max(n_neg, 1))
    class_weights = {0: weight_neg, 1: weight_pos}
    print(f"\n  Class weights: pos={weight_pos:.3f}, neg={weight_neg:.3f}")

    # ── 8. Compile and train ─────────────────────────────────────────────────
    model.compile(
        optimizer=keras.optimizers.Adam(learning_rate=1e-3),
        loss="binary_crossentropy",
        metrics=["accuracy", keras.metrics.AUC(name="auc")]
    )

    callbacks = [
        keras.callbacks.EarlyStopping(
            monitor="val_auc", patience=7, restore_best_weights=True, verbose=1,
            mode="max"
        ),
        keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss", factor=0.5, patience=3, min_lr=1e-6, verbose=1
        ),
        keras.callbacks.ModelCheckpoint(
            os.path.join(OUTPUT_DIR, "best_rejection.keras"),
            monitor="val_auc", save_best_only=True, verbose=1, mode="max"
        ),
    ]

    print(f"\nTraining for max 50 epochs @ lr=1e-3 (all layers)...")
    history = model.fit(
        X_train, y_train,
        validation_data=(X_val, y_val),
        epochs=50,
        batch_size=32,
        class_weight=class_weights,
        callbacks=callbacks,
        verbose=1
    )

    # ── 9. Evaluate on validation set ────────────────────────────────────────
    print("\n📊 Validation Set Evaluation:")
    y_pred_val = (model.predict(X_val, verbose=0) > 0.5).astype(int).flatten()
    print(classification_report(
        y_val.astype(int), y_pred_val,
        target_names=["not_keyword", "hey_vaani"]
    ))

    # ── 10. SANITY TEST: Feed held-out hard negatives ────────────────────────
    print("\n" + "=" * 70)
    print("  🔬 SANITY TEST — Held-Out Hard Negatives")
    print("=" * 70)

    # Get held-out negatives (should be 0=not_keyword)
    test_neg_mask = (y_test == 0)
    test_pos_mask = (y_test == 1)

    if np.any(test_neg_mask):
        X_test_neg = X_test[test_neg_mask]
        neg_scores = model.predict(X_test_neg, verbose=0).flatten()
        print(f"\n  Hard negative clips (held-out): {len(X_test_neg)}")
        print(f"  Confidence scores: min={neg_scores.min():.4f}  max={neg_scores.max():.4f}  "
              f"mean={neg_scores.mean():.4f}")
        low_scores = neg_scores[neg_scores < 0.5]
        print(f"  Correctly rejected (<0.5): {len(low_scores)}/{len(neg_scores)} "
              f"({100*len(low_scores)/len(neg_scores):.1f}%)")

        for i, score in enumerate(neg_scores):
            status = "✅ REJECTED" if score < 0.5 else "❌ TRIGGERED"
            print(f"    [{i+1}] score={score:.4f}  {status}")

    if np.any(test_pos_mask):
        X_test_pos = X_test[test_pos_mask]
        pos_scores = model.predict(X_test_pos, verbose=0).flatten()
        print(f"\n  Positive clips (held-out): {len(X_test_pos)}")
        print(f"  Confidence scores: min={pos_scores.min():.4f}  max={pos_scores.max():.4f}  "
              f"mean={pos_scores.mean():.4f}")

    # ── 11. Full Threshold Sweep ─────────────────────────────────────────────
    print("\n" + "=" * 70)
    print("  📈 THRESHOLD SWEEP")
    print("=" * 70)

    # Get predictions on full validation set
    y_val_scores = model.predict(X_val, verbose=0).flatten()
    y_val_true = y_val.astype(int)

    best_threshold = 0.5
    best_f1 = 0.0
    thresholds_to_test = np.arange(0.1, 0.99, 0.01)

    print(f"\n  {'Threshold':>10}  {'TPR':>6}  {'FPR':>6}  {'F1':>6}  {'Acc':>6}")
    print(f"  {'─'*10}  {'─'*6}  {'─'*6}  {'─'*6}  {'─'*6}")

    for thresh in thresholds_to_test:
        y_pred = (y_val_scores >= thresh).astype(int)
        tp = np.sum((y_pred == 1) & (y_val_true == 1))
        fp = np.sum((y_pred == 1) & (y_val_true == 0))
        tn = np.sum((y_pred == 0) & (y_val_true == 0))
        fn = np.sum((y_pred == 0) & (y_val_true == 1))

        tpr = tp / max(tp + fn, 1)  # true positive rate (recall for positives)
        fpr = fp / max(fp + tn, 1)  # false positive rate (how often negatives trigger)
        precision = tp / max(tp + fp, 1)
        f1 = 2 * precision * tpr / max(precision + tpr, 1e-6)
        acc = (tp + tn) / len(y_val_true)

        if f1 > best_f1:
            best_f1 = f1
            best_threshold = thresh

        if abs(thresh - 0.5) < 0.01 or abs(thresh - best_threshold) < 0.01:
            marker = " ◀ BEST" if abs(thresh - best_threshold) < 0.01 else ""
            print(f"  {thresh:>10.2f}  {tpr:>6.3f}  {fpr:>6.3f}  {f1:>6.3f}  {acc:>6.3f}{marker}")

    print(f"\n  🎯 Optimal threshold: {best_threshold:.2f} (F1={best_f1:.4f})")

    # ── 12. Export to INT8 TFLite + model_data.h ─────────────────────────────
    print("\n" + "=" * 70)
    print("  📦 EXPORTING INT8 TFLite + model_data.h")
    print("=" * 70)

    def representative_dataset():
        idx = np.random.choice(len(X_val), min(200, len(X_val)), replace=False)
        for i in idx:
            yield [X_val[i:i+1]]

    # For Keras 3.x / TF 2.16: save as .keras then reload via tf.keras
    # then convert to ensure TFLite compatibility
    import tempfile
    tmp_model_path = os.path.join(OUTPUT_DIR, "temp_export.keras")
    model.export(tmp_model_path) if hasattr(model, 'export') else model.save(tmp_model_path)

    # Reload with tf.keras for TFLite conversion
    reloaded = tf.keras.models.load_model(tmp_model_path)
    converter = tf.lite.TFLiteConverter.from_keras_model(reloaded)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type  = tf.int8
    converter.inference_output_type = tf.int8

    tflite_model = converter.convert()
    tflite_path = os.path.join(OUTPUT_DIR, "hey_vaani_rejection.tflite")
    with open(tflite_path, "wb") as f:
        f.write(tflite_model)
    model_size_kb = len(tflite_model) / 1024
    print(f"\n  ✅ TFLite saved: {tflite_path}")
    print(f"  📏 Model size: {len(tflite_model):,} bytes ({model_size_kb:.1f} KB)")

    # Compare with prior baseline
    prior_size = 45616  # bytes from existing model_data.h
    size_change = len(tflite_model) - prior_size
    if abs(size_change) > 2048:
        print(f"  ⚠️  Size changed by {size_change:+,} bytes from prior {prior_size:,} byte baseline")
    else:
        print(f"  ✅ Size stable (~{prior_size:,} byte baseline)")

    # Generate model_data.h
    header_path = os.path.join(FIRMWARE_DIR, "model_data.h")
    print(f"\n  📝 Generating: {header_path}")

    with open(header_path, "w") as f:
        f.write("// Auto-generated by finetune_rejection.py\n")
        f.write("// Hey Vaani KWS model — wake-word rejection fine-tune\n")
        f.write(f"// Model size: {len(tflite_model)} bytes ({model_size_kb:.1f} KB)\n")
        f.write(f"// Threshold: {best_threshold:.4f}\n")
        f.write(f"// Quantization: INT8\n")
        f.write(f"// Negatives: {len(neg_audios)} raw clips, {len(X_neg)} augmented\n")
        f.write("// Input: MFCC [49 x 13 x 1] (int8)\n")
        f.write("// Output: keyword_probability (int8)\n\n")
        f.write("#ifndef MODEL_DATA_H\n")
        f.write("#define MODEL_DATA_H\n\n")
        f.write(f"static const uint32_t g_model_data_len = {len(tflite_model)};\n\n")
        f.write("alignas(16) static const uint8_t g_model_data[] = {\n")

        for i, byte in enumerate(tflite_model):
            if i % 12 == 0:
                f.write("    ")
            f.write(f"0x{byte:02x}")
            if i < len(tflite_model) - 1:
                f.write(", ")
            if i % 12 == 11:
                f.write("\n")

        f.write("\n};\n\n")
        f.write("#endif // MODEL_DATA_H\n")

    print(f"  ✅ model_data.h exported: {header_path}")

    # Also save threshold separately for ESP32 firmware reference
    threshold_path = os.path.join(OUTPUT_DIR, "threshold.txt")
    with open(threshold_path, "w") as f:
        f.write(f"{best_threshold:.4f}\n")
    print(f"  📏 Threshold saved: {threshold_path}")

    # ── 13. Final Summary ────────────────────────────────────────────────────
    print("\n" + "=" * 70)
    print("  ✅ FINE-TUNE COMPLETE")
    print("=" * 70)
    print(f"  Model:          {header_path}")
    print(f"  TFLite:         {tflite_path} ({model_size_kb:.1f} KB)")
    print(f"  Threshold:      {best_threshold:.4f}")
    print(f"  Val accuracy:   {history.history['val_accuracy'][-1]:.4f}")
    print(f"  Val AUC:        {history.history['val_auc'][-1]:.4f}")
    print(f"  Hard neg reject: {100*len(low_scores)/len(neg_scores):.1f}%")
    print(f"\n  Next: Copy model_data.h to esp32_firmware/main/ and rebuild.")
    print("=" * 70)


if __name__ == "__main__":
    main()
