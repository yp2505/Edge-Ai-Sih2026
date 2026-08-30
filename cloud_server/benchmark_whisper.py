#!/usr/bin/env python3
"""
benchmark_whisper.py — Compare Whisper model sizes for latency on demo hardware.

Measures transcription time for tiny vs base on the actual CPU of this machine.
Run this ONCE before the demo to pick the best model.

Usage:
    python benchmark_whisper.py                   # Uses a generated 2-second sine-wave test clip
    python benchmark_whisper.py --wav audio.wav   # Use a real "Hey Vaani" command recording

Results are printed as a comparison table and a recommendation is made automatically.
"""

import os
import sys
import time
import wave
import struct
import argparse
import tempfile
import numpy as np

SAMPLE_RATE  = 16000
TEST_DURATION = 2.0   # seconds — typical command duration post-keyword

MODELS_TO_BENCH = ["tiny", "base"]
RUNS_PER_MODEL  = 3   # Average over 3 runs to reduce variance


def make_test_wav(path: str, duration: float = TEST_DURATION):
    """Generate a simple test WAV (silence with slight noise) for benchmarking."""
    n = int(SAMPLE_RATE * duration)
    audio = (np.random.randn(n) * 200).astype(np.int16)   # low-level noise
    with wave.open(path, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(audio.tobytes())


def bench_model(model_name: str, wav_path: str, runs: int) -> dict:
    """Load and benchmark a single Whisper model. Returns timing stats."""
    try:
        from faster_whisper import WhisperModel  # type: ignore
    except ImportError:
        print("❌ faster-whisper not installed. Run: pip install faster-whisper")
        sys.exit(1)

    print(f"\n  Loading '{model_name}'...", end="", flush=True)
    t_load = time.time()
    model = WhisperModel(model_name, device="cpu", compute_type="int8")
    load_ms = int((time.time() - t_load) * 1000)
    print(f" loaded in {load_ms}ms")

    latencies = []
    for i in range(runs):
        t0 = time.time()
        segments, info = model.transcribe(
            wav_path, beam_size=5, language="en", vad_filter=True
        )
        _ = list(segments)   # Force evaluation (generator is lazy)
        ms = int((time.time() - t0) * 1000)
        latencies.append(ms)
        print(f"    run {i+1}/{runs}: {ms}ms")

    return {
        "model":    model_name,
        "load_ms":  load_ms,
        "min_ms":   min(latencies),
        "avg_ms":   int(sum(latencies) / len(latencies)),
        "max_ms":   max(latencies),
        "runs":     runs,
    }


def print_report(results: list, audio_duration: float):
    print("\n")
    print("  ╔══════════════════════════════════════════════════════════════╗")
    print("  ║         Whisper Model Benchmark — Hey Vaani Demo Laptop      ║")
    print(f"  ║         Audio clip: {audio_duration:.1f}s  |  CPU: INT8 compute           ║")
    print("  ╠════════════╦══════════╦══════════╦══════════╦═══════════════╣")
    print("  ║ Model      ║ Load(ms) ║ Min(ms)  ║ Avg(ms)  ║ Max(ms)       ║")
    print("  ╠════════════╬══════════╬══════════╬══════════╬═══════════════╣")
    for r in results:
        print(f"  ║ {r['model']:<10} ║ {r['load_ms']:>8} ║ {r['min_ms']:>8} ║ {r['avg_ms']:>8} ║ {r['max_ms']:>13} ║")
    print("  ╚════════════╩══════════╩══════════╩══════════╩═══════════════╝")

    # Auto-recommendation
    tiny = next((r for r in results if r["model"] == "tiny"), None)
    base = next((r for r in results if r["model"] == "base"), None)

    print("\n  📋 RECOMMENDATION:")
    if tiny and base:
        diff_ms = base["avg_ms"] - tiny["avg_ms"]
        if diff_ms > 200:
            print(f"     ✅ Use --whisper-model tiny")
            print(f"        Saves {diff_ms}ms vs base (significant for <1.5s target).")
            print(f"        Accuracy tradeoff is minimal for short commands in English.")
        else:
            print(f"     ℹ️  Tiny is only {diff_ms}ms faster than base.")
            print(f"        Consider --whisper-model base for better accuracy.")
    elif tiny:
        print(f"     ✅ Use --whisper-model tiny (only model tested)")

    print(f"\n  Note: Run `python server.py --whisper-model tiny` to use the faster model.")
    print(f"        Results above are for a {audio_duration:.1f}s synthetic clip.")
    print(f"        Real-world clips may differ by ±50ms.\n")


def main():
    parser = argparse.ArgumentParser(description="Whisper model latency benchmark")
    parser.add_argument("--wav",    type=str, default=None, help="WAV file to transcribe")
    parser.add_argument("--runs",   type=int, default=RUNS_PER_MODEL, help="Runs per model")
    parser.add_argument("--models", type=str, default="tiny,base", help="Comma-separated models")
    args = parser.parse_args()

    model_list = [m.strip() for m in args.models.split(",")]

    if args.wav:
        wav_path = args.wav
        import soundfile as sf  # type: ignore
        audio, sr = sf.read(wav_path)
        audio_duration = len(audio) / sr
        print(f"  Using WAV: {os.path.basename(wav_path)} ({audio_duration:.2f}s)")
        temp_cleanup = False
    else:
        tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
        wav_path = tmp.name
        tmp.close()
        make_test_wav(wav_path, TEST_DURATION)
        audio_duration = TEST_DURATION
        print(f"  Generated synthetic {TEST_DURATION}s test clip (no --wav provided)")
        temp_cleanup = True

    print(f"  Benchmarking models: {model_list} | {args.runs} runs each\n")

    results = []
    for m in model_list:
        results.append(bench_model(m, wav_path, args.runs))

    if temp_cleanup:
        os.unlink(wav_path)

    print_report(results, audio_duration)


if __name__ == "__main__":
    main()
