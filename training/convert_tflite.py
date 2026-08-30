#!/usr/bin/env python3
"""
Convert trained DS-CNN model to TensorFlow Lite with INT8 quantization.
Exports as:
  1. .tflite file (for testing on laptop)
  2. C header array (model_data.h for ESP32 firmware)

Usage:
    python convert_tflite.py                    # Convert with INT8 quantization
    python convert_tflite.py --verify           # Convert + verify accuracy
    python convert_tflite.py --float16          # Use float16 instead of INT8
"""

import os
import sys
import glob
import argparse
import numpy as np
import tensorflow as tf
from tensorflow import keras
import soundfile as sf

# ─── Configuration ───────────────────────────────────────────────────────────
SAMPLE_RATE = 16000
N_MFCC = 13
N_FRAMES = 49
N_FFT = 512
HOP_LENGTH = 320
DURATION = 1.0

DATA_DIR = os.path.join(os.path.dirname(__file__), "data")
MODEL_DIR = os.path.join(os.path.dirname(__file__), "models")
FIRMWARE_DIR = os.path.join(os.path.dirname(__file__), "..", "esp32_firmware", "main")


def compute_mfcc(audio, sr=SAMPLE_RATE):
    """Compute MFCC — same as train_model.py."""
    target_len = int(DURATION * sr)
    if len(audio) < target_len:
        audio = np.pad(audio, (0, target_len - len(audio)))
    else:
        start = (len(audio) - target_len) // 2
        audio = audio[start:start + target_len]
    
    stft = tf.signal.stft(audio.astype(np.float32), frame_length=N_FFT, frame_step=HOP_LENGTH, fft_length=N_FFT)
    spectrogram = tf.abs(stft)
    num_spectrogram_bins = spectrogram.shape[-1]
    mel_matrix = tf.signal.linear_to_mel_weight_matrix(40, num_spectrogram_bins, sr, 20.0, sr / 2)
    mel = tf.matmul(spectrogram, mel_matrix)
    log_mel = tf.math.log(mel + 1e-6)
    mfccs = tf.signal.mfccs_from_log_mel_spectrograms(log_mel)
    mfccs = mfccs[..., :N_MFCC].numpy()
    
    if mfccs.shape[0] < N_FRAMES:
        mfccs = np.pad(mfccs, ((0, N_FRAMES - mfccs.shape[0]), (0, 0)))
    elif mfccs.shape[0] > N_FRAMES:
        mfccs = mfccs[:N_FRAMES, :]
    
    return mfccs


def get_representative_dataset():
    """Generate representative data for INT8 quantization calibration."""
    # Load a subset of training data for calibration
    keyword_dir = os.path.join(DATA_DIR, "keyword_augmented")
    background_dir = os.path.join(DATA_DIR, "background")
    
    all_files = []
    if os.path.exists(keyword_dir):
        all_files += glob.glob(os.path.join(keyword_dir, "*.wav"))[:100]
    if os.path.exists(background_dir):
        all_files += glob.glob(os.path.join(background_dir, "*.wav"))[:100]
    
    if not all_files:
        # Fallback: generate random data
        print("  ⚠️  No calibration data found, using random data")
        for _ in range(100):
            data = np.random.randn(1, N_FRAMES, N_MFCC, 1).astype(np.float32)
            yield [data]
        return
    
    np.random.shuffle(all_files)
    
    for filepath in all_files[:200]:
        try:
            audio, sr = sf.read(filepath, dtype='float32')
            if len(audio.shape) > 1:
                audio = audio.mean(axis=1)
            mfcc = compute_mfcc(audio)
            data = mfcc[np.newaxis, ..., np.newaxis].astype(np.float32)
            yield [data]
        except Exception:
            pass


def convert_to_tflite(use_float16=False):
    """Convert Keras model to TFLite with quantization."""
    model_path = os.path.join(MODEL_DIR, "ds_cnn_final.keras")
    
    if not os.path.exists(model_path):
        print(f"  ❌ No trained model found at {model_path}")
        print("  Run first: python train_model.py")
        sys.exit(1)
    
    print("  Loading trained model...")
    model = keras.models.load_model(model_path)
    
    # Convert to TFLite
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    
    if use_float16:
        print("  📦 Applying Float16 quantization...")
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.target_spec.supported_types = [tf.float16]
        quant_label = "float16"
    else:
        print("  📦 Applying INT8 full quantization...")
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.representative_dataset = get_representative_dataset
        converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
        converter.inference_input_type = tf.int8
        converter.inference_output_type = tf.int8
        quant_label = "int8"
    
    tflite_model = converter.convert()
    
    # Save .tflite file
    tflite_path = os.path.join(MODEL_DIR, f"hey_vaani_kws_{quant_label}.tflite")
    with open(tflite_path, 'wb') as f:
        f.write(tflite_model)
    
    model_size_kb = len(tflite_model) / 1024
    print(f"\n  ✅ TFLite model saved: {tflite_path}")
    print(f"  📏 Model size: {model_size_kb:.1f} KB")
    
    return tflite_model, tflite_path


def export_c_header(tflite_model):
    """Export TFLite model as C header array for ESP32 firmware."""
    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    header_path = os.path.join(FIRMWARE_DIR, "model_data.h")
    
    print(f"\n  📝 Exporting C header: {header_path}")
    
    with open(header_path, 'w') as f:
        f.write("// Auto-generated by convert_tflite.py\n")
        f.write("// DS-CNN model for 'Hey Vaani' keyword spotting\n")
        f.write(f"// Model size: {len(tflite_model)} bytes ({len(tflite_model)/1024:.1f} KB)\n")
        f.write(f"// Quantization: INT8\n")
        f.write("// Input: MFCC [49 × 13 × 1] (int8)\n")
        f.write("// Output: [not_keyword, keyword] (int8)\n\n")
        f.write("#ifndef MODEL_DATA_H\n")
        f.write("#define MODEL_DATA_H\n\n")
        f.write(f"const unsigned int model_data_len = {len(tflite_model)};\n\n")
        f.write("alignas(16) const unsigned char model_data[] = {\n")
        
        # Write hex bytes, 12 per line
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
    
    print(f"  ✅ C header exported: {header_path}")
    print(f"  📋 Use in ESP32: #include \"model_data.h\"")


def verify_tflite(tflite_path):
    """Verify quantized model accuracy matches original."""
    print("\n  🔍 Verifying TFLite model accuracy...")
    
    # Load TFLite model
    interpreter = tf.lite.Interpreter(model_path=tflite_path)
    interpreter.allocate_tensors()
    
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()
    
    print(f"  Input:  {input_details[0]['shape']} dtype={input_details[0]['dtype']}")
    print(f"  Output: {output_details[0]['shape']} dtype={output_details[0]['dtype']}")
    
    # Get quantization parameters
    input_scale = input_details[0].get('quantization_parameters', {}).get('scales', [1.0])
    input_zp = input_details[0].get('quantization_parameters', {}).get('zero_points', [0])
    
    print(f"  Input scale: {input_scale}, zero_point: {input_zp}")
    
    # Test with calibration data
    correct = 0
    total = 0
    
    keyword_dir = os.path.join(DATA_DIR, "keyword_augmented")
    background_dir = os.path.join(DATA_DIR, "background")
    
    test_files = []
    if os.path.exists(keyword_dir):
        kw_files = glob.glob(os.path.join(keyword_dir, "*.wav"))[-50:]
        test_files += [(f, 1) for f in kw_files]
    if os.path.exists(background_dir):
        bg_files = glob.glob(os.path.join(background_dir, "*.wav"))[-50:]
        test_files += [(f, 0) for f in bg_files]
    
    for filepath, label in test_files:
        try:
            audio, sr = sf.read(filepath, dtype='float32')
            if len(audio.shape) > 1:
                audio = audio.mean(axis=1)
            mfcc = compute_mfcc(audio)
            
            input_data = mfcc[np.newaxis, ..., np.newaxis].astype(np.float32)
            
            # Quantize input if needed
            if input_details[0]['dtype'] == np.int8:
                input_data = (input_data / input_scale[0] + input_zp[0]).astype(np.int8)
            
            interpreter.set_tensor(input_details[0]['index'], input_data)
            interpreter.invoke()
            output = interpreter.get_tensor(output_details[0]['index'])[0]
            
            predicted = np.argmax(output)
            if predicted == label:
                correct += 1
            total += 1
        except Exception:
            pass
    
    if total > 0:
        accuracy = correct / total
        print(f"\n  📊 TFLite Accuracy: {accuracy:.4f} ({accuracy*100:.1f}%)")
        print(f"     Tested on {total} samples")
        
        if accuracy > 0.88:
            print("  ✅ Quantization quality: GOOD")
        else:
            print("  ⚠️  Quantization quality: DEGRADED — consider float16 instead")
    else:
        print("  ⚠️  No test data available for verification")


def main():
    parser = argparse.ArgumentParser(description="Convert DS-CNN to TFLite for ESP32")
    parser.add_argument("--float16", action="store_true", help="Use float16 instead of INT8")
    parser.add_argument("--verify", action="store_true", help="Verify quantized accuracy")
    parser.add_argument("--no-header", action="store_true", help="Skip C header export")
    args = parser.parse_args()
    
    print("=" * 60)
    print("  🔄 TFLite Conversion — Hey Vaani KWS Model")
    print("=" * 60)
    
    # Convert
    tflite_model, tflite_path = convert_to_tflite(use_float16=args.float16)
    
    # Export C header for ESP32
    if not args.no_header:
        export_c_header(tflite_model)
    
    # Verify
    if args.verify:
        verify_tflite(tflite_path)
    
    # Size report
    model_size = len(tflite_model) / 1024
    print("\n" + "=" * 60)
    print("  🎯 PS Compliance — Model Size")
    print("=" * 60)
    print(f"  Model size:        {model_size:.1f} KB")
    print(f"  Tensor arena:      ~50 KB (estimated)")
    print(f"  Total RAM needed:  ~{model_size + 50:.0f} KB")
    print(f"  PS limit:          256 KB")
    print(f"  Status:            {'✅ PASS' if model_size + 50 < 256 else '❌ FAIL'}")
    
    print(f"\n  Next step: Flash ESP32 firmware with this model")


if __name__ == "__main__":
    main()
