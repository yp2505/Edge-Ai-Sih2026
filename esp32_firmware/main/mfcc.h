// mfcc.h — MFCC Feature Extraction for Hey Vaani KWS
//
// CRITICAL: This MUST match train_model.py's compute_mfcc() exactly.
// Parameters verified against training/train_model.py:
//   SAMPLE_RATE  = 16000 Hz
//   N_FFT        = 512  (32ms window at 16kHz)
//   HOP_LENGTH   = 320  (20ms hop = 50% overlap)
//   N_MEL        = 40   (mel filterbank bins)
//   N_MFCC       = 13   (DCT-II output coefficients kept)
//   N_FRAMES     = 49   (frames per 1-second window)
//   Mel range    = 20 Hz – 8000 Hz
//   DCT type     = Type-II, normalized (matches tf.signal.mfccs_from_log_mel_spectrograms)
//
// Any deviation from the above will silently degrade accuracy on real hardware.

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include "esp_log.h"

// ─── Constants (must match train_model.py) ───────────────────────────────────
#define MFCC_SAMPLE_RATE   16000
#define MFCC_N_FFT         512
#define MFCC_HOP_LENGTH    320
#define MFCC_N_MEL         40
#define MFCC_N_MFCC        13
#define MFCC_N_FRAMES      49
#define MFCC_MEL_LOW_HZ    20.0f
#define MFCC_MEL_HIGH_HZ   8000.0f
#define MFCC_EPSILON       1e-6f

// Input buffer: 1 second of 16kHz mono audio = 16000 int16 samples
#define MFCC_AUDIO_SAMPLES 16000
// The widest triangular Mel band uses 32 non-zero FFT bins at these fixed settings.
#define MFCC_MAX_MEL_BINS 32

// Output shape: [N_FRAMES, N_MFCC] = [49, 13] = 637 floats
#define MFCC_OUTPUT_SIZE   (MFCC_N_FRAMES * MFCC_N_MFCC)

// ─── MFCC Processor Class ────────────────────────────────────────────────────
class MFCCProcessor {
public:
    // Sparse triangular Mel filterbank. Only non-zero weights are stored.
    // This is mathematically identical to the former 40 x 257 dense matrix.
    float    (*mel_filters)[MFCC_MAX_MEL_BINS];
    uint16_t mel_start[MFCC_N_MEL];
    uint8_t  mel_count[MFCC_N_MEL];

    // Working buffers (allocated once to avoid stack overflow)
    float  window[MFCC_N_FFT];       // Hann window coefficients
    float  fft_real[MFCC_N_FFT];     // FFT real part
    float  fft_imag[MFCC_N_FFT];     // FFT imaginary part
    float  power_spec[MFCC_N_FFT / 2 + 1]; // |STFT|^2
    float  mel_energy[MFCC_N_MEL];   // mel filterbank output
    float  log_mel[MFCC_N_MEL];      // log(mel + epsilon)
    float  dct_out[MFCC_N_MFCC];     // DCT-II output

    // ── Initialise: precompute Hann window + mel filterbank ───────────────
    void init() {
        // Hann window: matches tf.signal.stft default
        for (int i = 0; i < MFCC_N_FFT; i++) {
            window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / MFCC_N_FFT));
        }
        mel_filters = (float (*)[MFCC_MAX_MEL_BINS])malloc(MFCC_N_MEL * MFCC_MAX_MEL_BINS * sizeof(float));
        if (!mel_filters) { printf("[MFCC] FATAL: sparse Mel allocation failed\n"); abort(); }
        _build_mel_filters();
    }

    // ── Main entry point ─────────────────────────────────────────────────
    // audio_int16: 16000 int16 samples (1 second, 16kHz, mono)
    // output:      float[MFCC_N_FRAMES * MFCC_N_MFCC] = float[637]
    //              Layout: row-major [frame][coeff] matching TF tensor [49,13]
    void compute(const int16_t* audio_int16, float* output) {
        // Convert int16 → float and apply Hann window per-frame (no 64KB buffer needed)
        int frame_idx = 0;
        for (int start = 0; start + MFCC_N_FFT <= MFCC_AUDIO_SAMPLES && frame_idx < MFCC_N_FRAMES;
             start += MFCC_HOP_LENGTH, frame_idx++) {

            // Convert + window in one pass (replaces old 64KB audio_f32 buffer)
            for (int i = 0; i < MFCC_N_FFT; i++) {
                fft_real[i] = ((float)audio_int16[start + i] / 32768.0f) * window[i];
                fft_imag[i] = 0.0f;
            }

            // In-place FFT (Cooley-Tukey, radix-2)
            _fft(fft_real, fft_imag, MFCC_N_FFT);

            // Power spectrum: |STFT|  (NOT squared — matches tf.abs(stft))
            for (int k = 0; k <= MFCC_N_FFT / 2; k++) {
                power_spec[k] = sqrtf(fft_real[k] * fft_real[k] +
                                      fft_imag[k] * fft_imag[k]);
            }

            // Sparse Mel dot product: identical non-zero weights, no zero multiplications.
            for (int m = 0; m < MFCC_N_MEL; m++) {
                float sum = 0.0f;
                for (int i = 0; i < mel_count[m]; i++) {
                    sum += power_spec[mel_start[m] + i] * mel_filters[m][i];
                }
                mel_energy[m] = sum;
            }

            // Log mel: matches tf.math.log(mel + 1e-6)
            for (int m = 0; m < MFCC_N_MEL; m++) {
                log_mel[m] = logf(mel_energy[m] + MFCC_EPSILON);
            }

            // DCT-II: matches tf.signal.mfccs_from_log_mel_spectrograms.
            // TensorFlow's MFCC helper uses a non-orthonormal DCT-II and then
            // applies sqrt(1 / (2 * N)).  With the sum-only form below that is
            // sqrt(2 / N) for every coefficient, including c0.
            _dct_ii_normalized(log_mel, dct_out, MFCC_N_MEL, MFCC_N_MFCC);

            // Write to output [frame_idx * N_MFCC .. frame_idx * N_MFCC + N_MFCC]
            float* row = output + frame_idx * MFCC_N_MFCC;
            for (int c = 0; c < MFCC_N_MFCC; c++) {
                row[c] = dct_out[c];
            }
        }

        // Pad remaining frames with zeros if audio shorter than expected
        for (; frame_idx < MFCC_N_FRAMES; frame_idx++) {
            float* row = output + frame_idx * MFCC_N_MFCC;
            for (int c = 0; c < MFCC_N_MFCC; c++) row[c] = 0.0f;
        }

    }

private:
    // ── Build triangular mel filterbank ──────────────────────────────────
    // Matches tf.signal.linear_to_mel_weight_matrix exactly.
    void _build_mel_filters() {
        const float mel_low  = _hz_to_mel(MFCC_MEL_LOW_HZ);
        const float mel_high = _hz_to_mel(MFCC_MEL_HIGH_HZ);
        float mel_pts[MFCC_N_MEL + 2];
        float hz_pts[MFCC_N_MEL + 2];
        for (int i = 0; i < MFCC_N_MEL + 2; i++) {
            mel_pts[i] = mel_low + (mel_high - mel_low) * i / (MFCC_N_MEL + 1);
            hz_pts[i] = _mel_to_hz(mel_pts[i]);
        }
        for (int m = 0; m < MFCC_N_MEL; m++) {
            int count = 0;
            mel_start[m] = 0;
            for (int k = 0; k <= MFCC_N_FFT / 2; k++) {
                float freq = (float)k * MFCC_SAMPLE_RATE / MFCC_N_FFT;
                float weight = 0.0f;
                if (freq > hz_pts[m] && freq < hz_pts[m + 2]) {
                    weight = freq < hz_pts[m + 1]
                        ? (freq - hz_pts[m]) / (hz_pts[m + 1] - hz_pts[m])
                        : (hz_pts[m + 2] - freq) / (hz_pts[m + 2] - hz_pts[m + 1]);
                }
                if (weight != 0.0f) {
                    if (count == 0) mel_start[m] = k;
                    if (count >= MFCC_MAX_MEL_BINS) { printf("[MFCC] FATAL: sparse Mel band overflow\n"); abort(); }
                    mel_filters[m][count++] = weight;
                }
            }
            mel_count[m] = count;
        }
    }
    void _dct_ii_normalized(const float* x, float* out, int N_in, int N_out) {
        for (int n = 0; n < N_out; n++) {
            float sum = 0.0f;
            for (int m = 0; m < N_in; m++) {
                sum += x[m] * cosf(M_PI * n * (m + 0.5f) / N_in);
            }
            out[n] = sum * sqrtf(2.0f / N_in);
        }
    }

    // ── In-place radix-2 DIT FFT ─────────────────────────────────────────
    void _fft(float* real, float* imag, int n) {
        // Bit-reversal permutation
        for (int i = 1, j = 0; i < n; i++) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                float tr = real[i]; real[i] = real[j]; real[j] = tr;
                float ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
            }
        }
        // Cooley-Tukey butterfly
        for (int len = 2; len <= n; len <<= 1) {
            float ang = -2.0f * M_PI / len;
            float wRe = cosf(ang), wIm = sinf(ang);
            for (int i = 0; i < n; i += len) {
                float uRe = 1.0f, uIm = 0.0f;
                for (int j = 0; j < len / 2; j++) {
                    float tRe = real[i+j+len/2]*uRe - imag[i+j+len/2]*uIm;
                    float tIm = real[i+j+len/2]*uIm + imag[i+j+len/2]*uRe;
                    real[i+j+len/2] = real[i+j] - tRe;
                    imag[i+j+len/2] = imag[i+j] - tIm;
                    real[i+j] += tRe;
                    imag[i+j] += tIm;
                    float newURe = uRe*wRe - uIm*wIm;
                    uIm = uRe*wIm + uIm*wRe;
                    uRe = newURe;
                }
            }
        }
    }

    static float _hz_to_mel(float hz) {
        return 2595.0f * log10f(1.0f + hz / 700.0f);
    }
    static float _mel_to_hz(float mel) {
        return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
    }
};
