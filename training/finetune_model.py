#!/usr/bin/env python3
"""
finetune_model.py — Fine-tune existing "Hey Vaani" KWS model on new samples.

Instead of retraining from scratch, this:
  1. Loads your existing trained model (new_model/)
  2. FREEZES all Conv/DepthwiseConv layers (keeps learned audio features)
  3. Retrains ONLY the Dense classifier head on new + existing data
  4. Re-exports to INT8 TFLite + model_data.h for flashing

Training time: ~5 minutes (vs 30 min from scratch)
Data needed:   Even 20-30 new samples make a big difference!

Usage:
    # Fine-tune on new samples only (fastest):
    python finetune_model.py --new-data path/to/new_keyword_wavs/

    # Fine-tune on new + all existing data (best accuracy):
    python finetune_model.py --new-data path/to/new_keyword_wavs/ --combine-existing

    # Unfreeze all layers for full fine-tune (use if accuracy is low):
    python finetune_model.py --new-data path/to/new/ --unfreeze-all

Run in Google Colab:
    Upload this file + your new WAV files, then run each section.
"""

import os
import sys
import glob
import random
import argparse
import warnings
import numpy as np
import tensorflow as tf
from tensorflow import keras
from sklearn.model_selection import train_test_split
from sklearn.metrics import classification_report
import soundfile as sf

warnings.filterwarnings("ignore")

# ─── CONFIGURE THESE PATHS ────────────────────────────────────────────────────
# Path to your existing trained Keras model
BASE_MODEL_PATH  = "../training/new_model/hey_vaani_model.keras"  # or .h5

# Where new WAV keyword files are (folder of .wav files saying "Hey Vaani")
NEW_KEYWORD_DIR  = "new_keyword_samples"

# Where existing keyword WAVs are (to combine with new)
EXISTING_KEYWORD_DIR = "../training/dataset/keyword"
EXISTING_AUGMENTED_DIR = "../training/dataset/keyword_augmented"
EXISTING_BG_DIR  = "../training/dataset/background"

# Output directory for fine-tuned model
OUTPUT_DIR = "finetuned_model"
os.makedirs(OUTPUT_DIR, exist_ok=True)

# ─── Audio Parameters (MUST match ESP32 firmware exactly) ────────────────────
SAMPLE_RATE = 16000
DURATION    = 1.0
N_MFCC      = 13
N_FFT       = 512
HOP_LENGTH  = 320
N_FRAMES    = 49

LABEL_KEYWORD     = 1
LABEL_NOT_KEYWORD = 0

# ─── Feature Extraction (same as firmware + colab_training.py) ───────────────
def compute_mfcc(audio: np.ndarray) -> np.ndarray:
    target_len = int(DURATION * SAMPLE_RATE)
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
    mel_w = tf.signal.linear_to_mel_weight_matrix(
        40, spectrogram.shape[-1], SAMPLE_RATE, 20.0, SAMPLE_RATE / 2
    )
    log_mel = tf.math.log(tf.matmul(spectrogram, mel_w) + 1e-6)
    mfccs = tf.signal.mfccs_from_log_mel_spectrograms(log_mel)[..., :N_MFCC].numpy()

    if mfccs.shape[0] < N_FRAMES:
        mfccs = np.pad(mfccs, ((0, N_FRAMES - mfccs.shape[0]), (0, 0)))
    elif mfccs.shape[0] > N_FRAMES:
        mfccs = mfccs[:N_FRAMES, :]
    return mfccs  # [49, 13]


def load_wav(path: str) -> np.ndarray:
    audio, sr = sf.read(path, dtype='float32')
    if audio.ndim > 1:
        audio = audio.mean(axis=1)
    if sr != SAMPLE_RATE:
        new_len = int(len(audio) * SAMPLE_RATE / sr)
        audio = np.interp(
            np.linspace(0, len(audio) - 1, new_len),
            np.arange(len(audio)), audio
        )
    return audio.astype(np.float32)


def load_dir(directory: str, label: int, max_files: int = 99999):
    X, y = [], []
    if not os.path.exists(directory):
        print(f"  ⚠️  Directory not found: {directory} — skipping")
        return X, y
    files = glob.glob(os.path.join(directory, "*.wav"))
    random.shuffle(files)
    files = files[:max_files]
    print(f"  Loading {len(files)} files from {os.path.basename(directory)}...")
    for f in files:
        try:
            audio = load_wav(f)
            mfcc  = compute_mfcc(audio)
            X.append(mfcc)
            y.append(label)
        except Exception as e:
            print(f"    ⚠️  {f}: {e}")
    return X, y


# ─── Main Fine-tuning Pipeline ────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Fine-tune Hey Vaani KWS model")
    parser.add_argument("--new-data",       default=NEW_KEYWORD_DIR,
                        help="Folder of new keyword WAV files")
    parser.add_argument("--base-model",     default=BASE_MODEL_PATH,
                        help="Path to existing .keras or .h5 model")
    parser.add_argument("--combine-existing", action="store_true",
                        help="Also include original training data (recommended)")
    parser.add_argument("--unfreeze-all",   action="store_true",
                        help="Unfreeze all layers (full fine-tune, needs more data)")
    parser.add_argument("--epochs",         type=int, default=30)
    parser.add_argument("--lr",             type=float, default=1e-4,
                        help="Learning rate (keep low for fine-tuning: 1e-4 to 1e-5)")
    args = parser.parse_args()

    print("\n" + "="*60)
    print("  Hey Vaani — Fine-tuning existing model")
    print("="*60)

    # ── 1. Load existing model ────────────────────────────────────────────────
    print(f"\nLoading base model: {args.base_model}")
    if not os.path.exists(args.base_model):
        print(f"  ERROR: Model not found at {args.base_model}")
        print("  Please set --base-model to the correct path of your .keras/.h5 file")
        sys.exit(1)
    model = keras.models.load_model(args.base_model)
    model.summary()

    # ── 2. Freeze layers ──────────────────────────────────────────────────────
    if args.unfreeze_all:
        print("\nUnfreezing ALL layers (full fine-tune)")
        for layer in model.layers:
            layer.trainable = True
    else:
        print("\nFreezing feature extractor layers, training only classifier head")
        # Freeze everything except the last 3 layers (Dense, BN, output Dense)
        for layer in model.layers[:-3]:
            layer.trainable = False
        for layer in model.layers[-3:]:
            layer.trainable = True
            print(f"  Trainable: {layer.name}")

    total_params     = sum(np.prod(w.shape) for w in model.trainable_weights)
    total_all_params = sum(np.prod(w.shape) for w in model.weights)
    print(f"\n  Trainable: {total_params:,} / {total_all_params:,} params "
          f"({100*total_params/total_all_params:.1f}% of model)")

    # ── 3. Build dataset ──────────────────────────────────────────────────────
    print("\nBuilding fine-tune dataset...")
    X_pos, y_pos = [], []
    X_neg, y_neg = [], []

    nx, ny = load_dir(args.new_data, LABEL_KEYWORD)
    X_pos += nx; y_pos += ny
    print(f"  New keyword samples: {len(nx)}")

    if args.combine_existing:
        for d in [EXISTING_KEYWORD_DIR, EXISTING_AUGMENTED_DIR]:
            nx, ny = load_dir(d, LABEL_KEYWORD)
            X_pos += nx; y_pos += ny
        max_neg = len(X_pos) * 7
        nx, ny = load_dir(EXISTING_BG_DIR, LABEL_NOT_KEYWORD, max_files=max_neg)
        X_neg += nx; y_neg += ny

    # Always add silence negatives
    n_silence = max(50, len(X_pos) // 3)
    print(f"  Adding {n_silence} synthetic silence negatives...")
    for _ in range(n_silence):
        silence = np.random.normal(0, 0.001, SAMPLE_RATE).astype(np.float32)
        X_neg.append(compute_mfcc(silence))
        y_neg.append(LABEL_NOT_KEYWORD)

    X = np.array(X_pos + X_neg, dtype=np.float32)[..., np.newaxis]
    y = np.array(y_pos + y_neg, dtype=np.float32)

    print(f"\n  Keyword (positive): {len(X_pos)}")
    print(f"  Not keyword (neg):  {len(X_neg)}")
    print(f"  Total:              {len(X)}")

    if len(X_pos) == 0:
        print(f"\n  ERROR: No keyword samples found in {args.new_data}")
        sys.exit(1)

    X_train, X_val, y_train, y_val = train_test_split(
        X, y, test_size=0.2, random_state=42, stratify=y
    )

    # ── 4. Compile with LOW learning rate ─────────────────────────────────────
    model.compile(
        optimizer=keras.optimizers.Adam(learning_rate=args.lr),
        loss="binary_crossentropy",
        metrics=["accuracy"]
    )

    # ── 5. Train ──────────────────────────────────────────────────────────────
    print(f"\nFine-tuning for {args.epochs} epochs @ lr={args.lr}...")
    callbacks = [
        keras.callbacks.EarlyStopping(
            monitor="val_accuracy", patience=8, restore_best_weights=True, verbose=1
        ),
        keras.callbacks.ReduceLROnPlateau(
            monitor="val_loss", factor=0.5, patience=4, min_lr=1e-6, verbose=1
        ),
        keras.callbacks.ModelCheckpoint(
            os.path.join(OUTPUT_DIR, "best_finetuned.keras"),
            monitor="val_accuracy", save_best_only=True, verbose=1
        )
    ]

    model.fit(
        X_train, y_train,
        validation_data=(X_val, y_val),
        epochs=args.epochs,
        batch_size=32,
        callbacks=callbacks,
        verbose=1
    )

    # ── 6. Evaluate ───────────────────────────────────────────────────────────
    print("\nEvaluation on validation set:")
    y_pred = (model.predict(X_val, verbose=0) > 0.5).astype(int).flatten()
    print(classification_report(y_val.astype(int), y_pred,
                                 target_names=["not_keyword", "hey_vaani"]))

    # ── 7. Save full model ────────────────────────────────────────────────────
    full_model_path = os.path.join(OUTPUT_DIR, "hey_vaani_finetuned.keras")
    model.save(full_model_path)
    print(f"\nFine-tuned model saved: {full_model_path}")

    # ── 8. Export INT8 TFLite ─────────────────────────────────────────────────
    print("\nExporting INT8 TFLite for ESP32...")

    def representative_dataset():
        idx = np.random.choice(len(X_val), min(200, len(X_val)), replace=False)
        for i in idx:
            yield [X_val[i:i+1]]

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = representative_dataset
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type  = tf.int8
    converter.inference_output_type = tf.int8

    tflite_model = converter.convert()
    tflite_path  = os.path.join(OUTPUT_DIR, "hey_vaani_finetuned.tflite")
    with open(tflite_path, "wb") as f:
        f.write(tflite_model)
    print(f"  TFLite saved: {tflite_path}  ({len(tflite_model)/1024:.1f} KB)")

    # ── 9. Generate model_data.h ──────────────────────────────────────────────
    header_path = os.path.join(OUTPUT_DIR, "model_data.h")
    with open(header_path, "w") as f:
        f.write("// Auto-generated by finetune_model.py\n")
        f.write("// Fine-tuned Hey Vaani KWS model\n\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"static const uint32_t g_model_data_len = {len(tflite_model)};\n\n")
        f.write("alignas(8) static const uint8_t g_model_data[] = {\n  ")
        hex_vals = [f"0x{b:02x}" for b in tflite_model]
        for i, h in enumerate(hex_vals):
            f.write(h)
            if i < len(hex_vals) - 1:
                f.write(", ")
                if (i + 1) % 16 == 0:
                    f.write("\n  ")
        f.write("\n};\n")

    print(f"  model_data.h saved: {header_path}")
    print(f"\nDone! Copy {header_path} to esp32_firmware/main/ and flash!")
    print("="*60)


if __name__ == "__main__":
    main()
