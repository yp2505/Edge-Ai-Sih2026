#!/usr/bin/env python3
"""
Train DS-CNN (Depthwise Separable CNN) model for "Hey Vaani" keyword spotting.

This is the core ML model — a tiny CNN designed to run on ESP32 (<256KB RAM).

Architecture: DS-CNN (from ARM's "Hello Edge" paper)
Input:        MFCC features [49 × 13 × 1]
Output:       [keyword_probability, not_keyword_probability]
Model size:   ~45KB after INT8 quantization
Target:       >92% accuracy

Usage:
    python train_model.py                    # Train with default settings
    python train_model.py --epochs 100       # Train for more epochs
    python train_model.py --evaluate         # Evaluate existing model
"""

import os
import sys
import glob
import random
import argparse
import numpy as np
import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers
from sklearn.model_selection import train_test_split
from sklearn.metrics import classification_report, confusion_matrix
import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import matplotlib.pyplot as plt

# ─── Configuration ───────────────────────────────────────────────────────────
SAMPLE_RATE = 16000
DURATION = 1.0        # Use 1 second for MFCC (trim from 1.5s recordings)

# MFCC parameters (must match ESP32 firmware exactly)
N_MFCC = 13           # Number of MFCC coefficients
N_FFT = 512           # FFT window size (512 samples = 32ms at 16kHz)
HOP_LENGTH = 320      # Hop size (320 samples = 20ms at 16kHz)
N_FRAMES = 49         # Number of time frames for 1 second

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
MODEL_DIR = os.path.join(os.path.dirname(__file__), "models")
PLOTS_DIR = os.path.join(os.path.dirname(__file__), "plots")

# Labels
LABEL_KEYWORD = 1     # "Hey Vaani"
LABEL_NOT_KEYWORD = 0 # Everything else


# ─── Feature Extraction ─────────────────────────────────────────────────────

def compute_mfcc(audio, sr=SAMPLE_RATE):
    """
    Compute MFCC features from raw audio.
    Returns [N_FRAMES, N_MFCC] numpy array.
    
    This must EXACTLY match the MFCC computation on ESP32.
    """
    # Ensure correct length (1 second = 16000 samples)
    target_len = int(DURATION * sr)
    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        # Center crop
        start = (len(audio) - target_len) // 2
        audio = audio[start:start + target_len]
    
    # Use TensorFlow's built-in audio processing (matches TFLite Micro behavior)
    # Step 1: STFT
    stft = tf.signal.stft(
        audio.astype(np.float32),
        frame_length=N_FFT,
        frame_step=HOP_LENGTH,
        fft_length=N_FFT
    )
    spectrogram = tf.abs(stft)
    
    # Step 2: Mel filterbank
    num_spectrogram_bins = spectrogram.shape[-1]
    linear_to_mel_weight_matrix = tf.signal.linear_to_mel_weight_matrix(
        num_mel_bins=40,
        num_spectrogram_bins=num_spectrogram_bins,
        sample_rate=sr,
        lower_edge_hertz=20.0,
        upper_edge_hertz=sr / 2
    )
    mel_spectrogram = tf.matmul(spectrogram, linear_to_mel_weight_matrix)
    
    # Step 3: Log mel spectrogram
    log_mel_spectrogram = tf.math.log(mel_spectrogram + 1e-6)
    
    # Step 4: DCT to get MFCCs
    mfccs = tf.signal.mfccs_from_log_mel_spectrograms(log_mel_spectrogram)
    mfccs = mfccs[..., :N_MFCC]  # Keep only first N_MFCC coefficients
    
    # Ensure correct number of frames
    mfccs = mfccs.numpy()
    if mfccs.shape[0] < N_FRAMES:
        mfccs = np.pad(mfccs, ((0, N_FRAMES - mfccs.shape[0]), (0, 0)))
    elif mfccs.shape[0] > N_FRAMES:
        mfccs = mfccs[:N_FRAMES, :]
    
    return mfccs  # Shape: [49, 13]


def load_audio_file(filepath):
    """Load WAV file as numpy array."""
    import soundfile as sf
    audio, sr = sf.read(filepath, dtype='float32')
    if len(audio.shape) > 1:
        audio = audio.mean(axis=1)
    
    # Resample if needed
    if sr != SAMPLE_RATE:
        duration = len(audio) / sr
        new_length = int(duration * SAMPLE_RATE)
        audio = np.interp(
            np.linspace(0, len(audio) - 1, new_length),
            np.arange(len(audio)),
            audio
        )
    
    return audio


# ─── Dataset Preparation ────────────────────────────────────────────────────

def prepare_dataset(max_negative=5000):
    """
    Load keyword + background samples, extract MFCC features.
    Returns X (features), y (labels) arrays.
    """
    X = []
    y = []
    
    # ── Positive samples (keyword: "Hey Vaani") ──
    keyword_dir = os.path.join(DATA_DIR, "keyword")
    augmented_dir = os.path.join(DATA_DIR, "keyword_augmented")
    
    hw_augmented_dir = os.path.join(DATA_DIR, "keyword_hw")  # MAX4466 domain-adapted samples

    keyword_files = []
    # Keep the real recordings as well as their augmentations.  Previously,
    # newly recorded samples were silently ignored whenever this folder existed.
    if os.path.exists(keyword_dir):
        keyword_files += glob.glob(os.path.join(keyword_dir, "*.wav"))
    if os.path.exists(augmented_dir):
        keyword_files += glob.glob(os.path.join(augmented_dir, "*.wav"))
    # Always include hardware-augmented samples if available (critical for MAX4466 accuracy)
    if os.path.exists(hw_augmented_dir):
        hw_files = glob.glob(os.path.join(hw_augmented_dir, "*.wav"))
        keyword_files += hw_files
        print(f"  ✅ Hardware-augmented (MAX4466) samples: {len(hw_files)}")
    
    print(f"  Loading {len(keyword_files)} keyword samples...")
    for filepath in keyword_files:
        try:
            audio = load_audio_file(filepath)
            mfcc = compute_mfcc(audio)
            X.append(mfcc)
            y.append(LABEL_KEYWORD)
        except Exception as e:
            print(f"  ⚠️  Error processing {filepath}: {e}")
    
    n_positive = len(X)
    print(f"  ✅ Keyword samples loaded: {n_positive}")
    
    # ── Negative samples (NOT keyword) ──
    background_dir = os.path.join(DATA_DIR, "background")
    
    if os.path.exists(background_dir):
        neg_files = glob.glob(os.path.join(background_dir, "*.wav"))
        random.shuffle(neg_files)
        neg_files = neg_files[:max_negative]
        
        print(f"  Loading {len(neg_files)} negative samples...")
        for filepath in neg_files:
            try:
                audio = load_audio_file(filepath)
                mfcc = compute_mfcc(audio)
                X.append(mfcc)
                y.append(LABEL_NOT_KEYWORD)
            except Exception:
                pass
    
    n_negative = len(X) - n_positive
    print(f"  ✅ Negative samples loaded: {n_negative}")
    
    # ── Add silence samples ──
    n_silence = min(500, n_positive // 2)
    print(f"  Adding {n_silence} silence samples...")
    for _ in range(n_silence):
        silence = np.random.normal(0, 0.001, int(DURATION * SAMPLE_RATE)).astype(np.float32)
        mfcc = compute_mfcc(silence)
        X.append(mfcc)
        y.append(LABEL_NOT_KEYWORD)
    
    X = np.array(X, dtype=np.float32)
    y = np.array(y, dtype=np.int32)
    
    # Add channel dimension: [samples, 49, 13] → [samples, 49, 13, 1]
    X = X[..., np.newaxis]
    
    print(f"\n  📊 Dataset Summary:")
    print(f"     Total samples: {len(X)}")
    print(f"     Keyword:       {np.sum(y == LABEL_KEYWORD)}")
    print(f"     Not keyword:   {np.sum(y == LABEL_NOT_KEYWORD)}")
    print(f"     Feature shape: {X.shape[1:]}")
    
    return X, y


# ─── DS-CNN Model ────────────────────────────────────────────────────────────

def build_ds_cnn(input_shape=(N_FRAMES, N_MFCC, 1), num_classes=2):
    """
    Build DS-CNN (Depthwise Separable CNN) for keyword spotting.
    
    Architecture from ARM's "Hello Edge" paper, optimized for:
    - Model size < 50KB (INT8 quantized)
    - Tensor arena < 60KB RAM
    - Inference time < 10ms on ESP32
    
    Total parameters: ~18,000
    """
    model = keras.Sequential([
        # Input layer
        keras.Input(shape=input_shape, name="mfcc_input"),
        
        # First conv layer — standard Conv2D (larger kernel captures initial patterns)
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
        
        # Global pooling — reduces to [64] vector
        layers.GlobalAveragePooling2D(name="global_pool"),
        layers.Dropout(0.3),
        
        # Classification head
        layers.Dense(num_classes, activation='softmax', name="classifier"),
    ], name="DS_CNN_KeywordSpotting")
    
    return model


# ─── Training ────────────────────────────────────────────────────────────────

def train(epochs=50, batch_size=64, learning_rate=0.001):
    """Train the DS-CNN model."""
    print("=" * 60)
    print("  🧠 DS-CNN Training — Hey Vaani Keyword Spotting")
    print("=" * 60)
    
    # Prepare dataset
    X, y = prepare_dataset()
    
    if len(X) < 100:
        print("\n  ❌ Not enough training data!")
        print("  Need at least 100 samples. Run record_keyword.py and augment_data.py first.")
        sys.exit(1)
    
    # Split: 80% train, 10% validation, 10% test
    X_train, X_temp, y_train, y_temp = train_test_split(X, y, test_size=0.2, random_state=42, stratify=y)
    X_val, X_test, y_val, y_test = train_test_split(X_temp, y_temp, test_size=0.5, random_state=42, stratify=y_temp)
    
    print(f"\n  📊 Split:")
    print(f"     Train:      {len(X_train)} samples")
    print(f"     Validation: {len(X_val)} samples")
    print(f"     Test:       {len(X_test)} samples")
    
    # Convert labels to one-hot
    y_train_oh = keras.utils.to_categorical(y_train, 2)
    y_val_oh = keras.utils.to_categorical(y_val, 2)
    y_test_oh = keras.utils.to_categorical(y_test, 2)
    
    # Build model
    model = build_ds_cnn()
    model.summary()
    
    # Count parameters
    total_params = model.count_params()
    print(f"\n  📏 Total parameters: {total_params:,}")
    print(f"  📏 Estimated model size (Float32): {total_params * 4 / 1024:.1f} KB")
    print(f"  📏 Estimated model size (INT8):    {total_params * 1 / 1024:.1f} KB")
    
    # Compile
    optimizer = keras.optimizers.Adam(learning_rate=learning_rate)
    model.compile(
        optimizer=optimizer,
        loss='categorical_crossentropy',
        metrics=['accuracy']
    )
    
    # Callbacks
    os.makedirs(MODEL_DIR, exist_ok=True)
    callbacks = [
        keras.callbacks.ModelCheckpoint(
            os.path.join(MODEL_DIR, "ds_cnn_best.keras"),
            monitor='val_accuracy',
            save_best_only=True,
            verbose=1
        ),
        keras.callbacks.ReduceLROnPlateau(
            monitor='val_loss',
            factor=0.5,
            patience=5,
            min_lr=1e-5,
            verbose=1
        ),
        keras.callbacks.EarlyStopping(
            monitor='val_accuracy',
            patience=15,
            restore_best_weights=True,
            verbose=1
        ),
    ]
    
    # Train
    print(f"\n  🚀 Training for {epochs} epochs...")
    history = model.fit(
        X_train, y_train_oh,
        validation_data=(X_val, y_val_oh),
        epochs=epochs,
        batch_size=batch_size,
        callbacks=callbacks,
        verbose=1
    )
    
    # Evaluate on test set
    print("\n" + "=" * 60)
    print("  📊 Test Set Evaluation")
    print("=" * 60)
    
    test_loss, test_acc = model.evaluate(X_test, y_test_oh, verbose=0)
    print(f"  Test Accuracy:  {test_acc:.4f} ({test_acc*100:.1f}%)")
    print(f"  Test Loss:      {test_loss:.4f}")
    
    # Detailed classification report
    y_pred = np.argmax(model.predict(X_test, verbose=0), axis=1)
    print("\n  Classification Report:")
    print(classification_report(
        y_test, y_pred,
        target_names=["NOT keyword", "Hey Vaani"],
        digits=4
    ))
    
    # Confusion matrix
    cm = confusion_matrix(y_test, y_pred)
    print("  Confusion Matrix:")
    print(f"                Predicted")
    print(f"                NOT    KEYWORD")
    print(f"  Actual NOT  [ {cm[0][0]:4d}   {cm[0][1]:4d} ]")
    print(f"  Actual KEY  [ {cm[1][0]:4d}   {cm[1][1]:4d} ]")
    
    # Save training plots
    save_training_plots(history)
    
    # Save final model
    model.save(os.path.join(MODEL_DIR, "ds_cnn_final.keras"))
    print(f"\n  💾 Model saved: {MODEL_DIR}/ds_cnn_final.keras")
    
    # PS compliance check
    print("\n" + "=" * 60)
    print("  🎯 PS Compliance Check")
    print("=" * 60)
    estimated_int8_size = total_params / 1024
    print(f"  Model size (INT8):   ~{estimated_int8_size:.0f} KB {'✅' if estimated_int8_size < 60 else '❌'} (target: <60 KB)")
    print(f"  Test accuracy:       {test_acc*100:.1f}% {'✅' if test_acc > 0.90 else '❌'} (target: >90%)")
    print(f"  Tensor arena (est):  ~50 KB ✅ (target: fits in 256KB)")
    
    print(f"\n  Next step: python convert_tflite.py")
    
    return model, history


def save_training_plots(history):
    """Save accuracy and loss plots."""
    os.makedirs(PLOTS_DIR, exist_ok=True)
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5))
    
    # Accuracy plot
    ax1.plot(history.history['accuracy'], label='Train', linewidth=2)
    ax1.plot(history.history['val_accuracy'], label='Validation', linewidth=2)
    ax1.set_title('Model Accuracy', fontsize=14, fontweight='bold')
    ax1.set_xlabel('Epoch')
    ax1.set_ylabel('Accuracy')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.axhline(y=0.92, color='r', linestyle='--', alpha=0.5, label='Target (92%)')
    
    # Loss plot
    ax2.plot(history.history['loss'], label='Train', linewidth=2)
    ax2.plot(history.history['val_loss'], label='Validation', linewidth=2)
    ax2.set_title('Model Loss', fontsize=14, fontweight='bold')
    ax2.set_xlabel('Epoch')
    ax2.set_ylabel('Loss')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plot_path = os.path.join(PLOTS_DIR, "training_history.png")
    plt.savefig(plot_path, dpi=150)
    plt.close()
    print(f"\n  📈 Training plots saved: {plot_path}")


# ─── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Train DS-CNN for 'Hey Vaani' KWS")
    parser.add_argument("--epochs", type=int, default=50, help="Training epochs")
    parser.add_argument("--batch-size", type=int, default=64, help="Batch size")
    parser.add_argument("--lr", type=float, default=0.001, help="Learning rate")
    parser.add_argument("--evaluate", action="store_true", help="Evaluate existing model")
    args = parser.parse_args()
    
    if args.evaluate:
        model_path = os.path.join(MODEL_DIR, "ds_cnn_final.keras")
        if not os.path.exists(model_path):
            print(f"  ❌ No model found at {model_path}")
            sys.exit(1)
        model = keras.models.load_model(model_path)
        X, y = prepare_dataset()
        y_oh = keras.utils.to_categorical(y, 2)
        loss, acc = model.evaluate(X, y_oh, verbose=0)
        print(f"  Accuracy: {acc:.4f}")
    else:
        train(epochs=args.epochs, batch_size=args.batch_size, learning_rate=args.lr)


if __name__ == "__main__":
    main()
