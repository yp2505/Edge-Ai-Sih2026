#!/usr/bin/env python3
"""
evaluate_accuracy.py — Measure True-Positive Rate + False-Activation Rate

Runs the trained TFLite model against:
  (a) A held-out set of real "Hey Vaani" recordings  → True-Positive Rate
  (b) A negative set (background noise + other words) → False-Activation Rate

Both numbers are required by the PS compliance checklist.

Usage:
    python evaluate_accuracy.py                        # Auto-discovers test data
    python evaluate_accuracy.py --model path/to/model.tflite
    python evaluate_accuracy.py --threshold 0.85

Output (example):
    ┌─────────────────────────────────────────────────┐
    │  Hey Vaani — Model Accuracy Evaluation          │
    ├──────────────────────────┬──────────────────────┤
    │  True-Positive Rate      │  99.6%  (586/588)    │
    │  False-Activation Rate   │  0.5%   (3/550)      │
    │  Threshold               │  0.85                 │
    └──────────────────────────┴──────────────────────┘
"""

import os
import sys
import argparse
import numpy as np

# ─── Configuration (must match train_model.py) ─────────────────────────────
SAMPLE_RATE  = 16000
N_MFCC       = 13
N_FFT        = 512
HOP_LENGTH   = 320
N_FRAMES     = 49
N_MEL        = 40
MEL_LOW_HZ   = 20.0
MEL_HIGH_HZ  = 8000.0
DURATION     = 1.0

TRAINING_DIR = os.path.dirname(__file__)
DATA_DIR     = os.path.join(TRAINING_DIR, "data")
MODEL_DIR    = os.path.join(TRAINING_DIR, "models")
DEFAULT_MODEL = os.path.join(MODEL_DIR, "hey_vaani_kws_int8.tflite")

# ─── MFCC (identical pipeline to train_model.py) ───────────────────────────
def compute_mfcc(audio: np.ndarray) -> np.ndarray:
    """Compute MFCC features. MUST match train_model.py exactly."""
    import tensorflow as tf

    target_len = int(DURATION * SAMPLE_RATE)
    audio = audio.astype(np.float32)

    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        start = (len(audio) - target_len) // 2
        audio = audio[start:start + target_len]

    stft = tf.signal.stft(audio, frame_length=N_FFT, frame_step=HOP_LENGTH, fft_length=N_FFT)
    spectrogram = tf.abs(stft)

    mel_matrix = tf.signal.linear_to_mel_weight_matrix(
        N_MEL, spectrogram.shape[-1], SAMPLE_RATE, MEL_LOW_HZ, MEL_HIGH_HZ
    )
    mel = tf.matmul(spectrogram, mel_matrix)
    log_mel = tf.math.log(mel + 1e-6)
    mfccs = tf.signal.mfccs_from_log_mel_spectrograms(log_mel)[..., :N_MFCC].numpy()

    if mfccs.shape[0] < N_FRAMES:
        mfccs = np.pad(mfccs, ((0, N_FRAMES - mfccs.shape[0]), (0, 0)))
    elif mfccs.shape[0] > N_FRAMES:
        mfccs = mfccs[:N_FRAMES, :]

    return mfccs  # [49, 13]


def load_wav(path: str) -> np.ndarray:
    """Load WAV file as float32 numpy array at SAMPLE_RATE."""
    import soundfile as sf
    audio, sr = sf.read(path, dtype='float32')
    if audio.ndim > 1:
        audio = audio[:, 0]  # mono
    if sr != SAMPLE_RATE:
        import librosa
        audio = librosa.resample(audio, orig_sr=sr, target_sr=SAMPLE_RATE)
    return audio


def discover_files(data_dir: str):
    """
    Auto-discover positive + negative WAV files from the dataset structure.

    Expected layout (same as download_dataset.py / augment_data.py):
        data/
          hey_vaani/      ← positive keyword recordings
          negative/       ← background noise + other speech
    """
    pos_dir = os.path.join(data_dir, "hey_vaani")
    neg_dir = os.path.join(data_dir, "negative")

    positives = []
    negatives = []

    if os.path.isdir(pos_dir):
        for f in os.listdir(pos_dir):
            if f.lower().endswith('.wav'):
                positives.append(os.path.join(pos_dir, f))

    if os.path.isdir(neg_dir):
        for f in os.listdir(neg_dir):
            if f.lower().endswith('.wav'):
                negatives.append(os.path.join(neg_dir, f))

    return positives, negatives


def run_tflite_inference(interpreter, mfcc: np.ndarray) -> float:
    """Run TFLite model on a single MFCC frame. Returns keyword probability."""
    input_details  = interpreter.get_input_details()
    output_details = interpreter.get_output_details()

    x = mfcc[np.newaxis, ..., np.newaxis].astype(np.float32)

    # Quantise if INT8 model
    if input_details[0]['dtype'] == np.int8:
        scale, zp = input_details[0]['quantization']
        x = (x / scale + zp).clip(-128, 127).astype(np.int8)

    interpreter.set_tensor(input_details[0]['index'], x)
    interpreter.invoke()

    output = interpreter.get_tensor(output_details[0]['index'])

    if output_details[0]['dtype'] == np.int8:
        out_scale, out_zp = output_details[0]['quantization']
        output = (output.astype(np.float32) - out_zp) * out_scale

    # output[0] = [not_keyword_prob, keyword_prob]
    return float(output[0][1])


def evaluate(model_path: str, positives: list, negatives: list,
             threshold: float) -> dict:
    """Run full evaluation. Returns dict with TP, FP, TN, FN counts + rates."""
    try:
        from tflite_runtime.interpreter import Interpreter
    except ImportError:
        from tensorflow.lite.python.interpreter import Interpreter

    print(f"\n  Loading model: {os.path.basename(model_path)}")
    interp = Interpreter(model_path=model_path)
    interp.allocate_tensors()

    tp, fn = 0, 0
    fp, tn = 0, 0
    errors = []

    # ── Evaluate positives (should detect) ────────────────────────────────
    print(f"\n  Evaluating {len(positives)} positive samples ('Hey Vaani')...")
    for i, path in enumerate(positives):
        try:
            audio = load_wav(path)
            mfcc  = compute_mfcc(audio)
            prob  = run_tflite_inference(interp, mfcc)
            if prob >= threshold:
                tp += 1
            else:
                fn += 1
                errors.append(f"  FALSE NEGATIVE: {os.path.basename(path)} (prob={prob:.3f})")
        except Exception as e:
            errors.append(f"  ERROR: {os.path.basename(path)} — {e}")

        if (i + 1) % 50 == 0:
            print(f"    [{i+1}/{len(positives)}] TP so far: {tp}")

    # ── Evaluate negatives (should NOT detect) ─────────────────────────────
    print(f"\n  Evaluating {len(negatives)} negative samples (non-keyword audio)...")
    for i, path in enumerate(negatives):
        try:
            audio = load_wav(path)
            mfcc  = compute_mfcc(audio)
            prob  = run_tflite_inference(interp, mfcc)
            if prob < threshold:
                tn += 1
            else:
                fp += 1
                errors.append(f"  FALSE ACTIVATION: {os.path.basename(path)} (prob={prob:.3f})")
        except Exception as e:
            errors.append(f"  ERROR: {os.path.basename(path)} — {e}")

        if (i + 1) % 100 == 0:
            print(f"    [{i+1}/{len(negatives)}] FP so far: {fp}")

    tp_rate = tp / (tp + fn) * 100 if (tp + fn) > 0 else 0.0
    fa_rate = fp / (fp + tn) * 100 if (fp + tn) > 0 else 0.0

    return {
        "tp": tp, "fn": fn, "fp": fp, "tn": tn,
        "tp_rate": tp_rate, "fa_rate": fa_rate,
        "threshold": threshold,
        "errors": errors[:20]  # Show first 20 failures only
    }


def print_results(results: dict):
    """Pretty-print evaluation results."""
    print("\n")
    print("  ┌─────────────────────────────────────────────────────┐")
    print("  │         Hey Vaani — Model Accuracy Evaluation        │")
    print("  ├──────────────────────────────┬──────────────────────┤")
    print(f"  │  True-Positive Rate          │  {results['tp_rate']:5.1f}%  ({results['tp']}/{results['tp']+results['fn']})      │")
    print(f"  │  False-Activation Rate       │  {results['fa_rate']:5.1f}%  ({results['fp']}/{results['fp']+results['tn']})      │")
    print(f"  │  Detection Threshold         │  {results['threshold']}                   │")
    print("  ├──────────────────────────────┴──────────────────────┤")

    # PS compliance check
    tp_ok = results['tp_rate'] >= 90.0
    fa_ok = results['fa_rate'] <= 5.0
    print(f"  │  PS: TP ≥ 90%   {'✅ PASS' if tp_ok else '❌ FAIL'}  |  FA ≤ 5%   {'✅ PASS' if fa_ok else '❌ FAIL'}       │")
    print("  └─────────────────────────────────────────────────────┘")

    if results['errors']:
        print(f"\n  ⚠️  First {len(results['errors'])} failures:")
        for e in results['errors']:
            print(e)


def main():
    parser = argparse.ArgumentParser(description="Hey Vaani — Accuracy Evaluator")
    parser.add_argument("--model",      default=DEFAULT_MODEL, help="Path to .tflite model")
    parser.add_argument("--data-dir",   default=DATA_DIR,      help="Path to dataset root")
    parser.add_argument("--pos-dir",    default=None,          help="Override positive samples dir")
    parser.add_argument("--neg-dir",    default=None,          help="Override negative samples dir")
    parser.add_argument("--threshold",  type=float, default=0.85, help="Detection threshold")
    parser.add_argument("--limit",      type=int, default=None, help="Max samples per class (for speed)")
    args = parser.parse_args()

    print("=" * 60)
    print("  Hey Vaani — Accuracy & False-Activation Rate Evaluator")
    print("=" * 60)

    if not os.path.exists(args.model):
        print(f"  ❌ Model not found: {args.model}")
        print(f"  Run: python convert_tflite.py --verify")
        sys.exit(1)

    # Discover files
    if args.pos_dir and args.neg_dir:
        positives = [os.path.join(args.pos_dir, f) for f in os.listdir(args.pos_dir)
                     if f.lower().endswith('.wav')]
        negatives = [os.path.join(args.neg_dir, f) for f in os.listdir(args.neg_dir)
                     if f.lower().endswith('.wav')]
    else:
        positives, negatives = discover_files(args.data_dir)

    if args.limit:
        positives = positives[:args.limit]
        negatives = negatives[:args.limit]

    print(f"\n  Positives found: {len(positives)}")
    print(f"  Negatives found: {len(negatives)}")
    print(f"  Threshold:       {args.threshold}")

    if not positives and not negatives:
        print(f"\n  ⚠️  No WAV files found in {args.data_dir}")
        print(f"  Expected: data/hey_vaani/*.wav and data/negative/*.wav")
        sys.exit(1)

    results = evaluate(args.model, positives, negatives, args.threshold)
    print_results(results)


if __name__ == "__main__":
    main()
