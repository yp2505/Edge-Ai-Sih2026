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

// Output shape: [N_FRAMES, N_MFCC] = [49, 13] = 637 floats
#define MFCC_OUTPUT_SIZE   (MFCC_N_FRAMES * MFCC_N_MFCC)

// ─── MFCC Processor Class ────────────────────────────────────────────────────
class MFCCProcessor {
public:
    // Pre-computed mel filterbank [N_MEL][N_FFT/2+1] — computed once at init
    float mel_filters[MFCC_N_MEL][MFCC_N_FFT / 2 + 1];

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
        _build_mel_filters();
    }

    // ── Main entry point ─────────────────────────────────────────────────
    // audio_int16: 16000 int16 samples (1 second, 16kHz, mono)
    // output:      float[MFCC_N_FRAMES * MFCC_N_MFCC] = float[637]
    //              Layout: row-major [frame][coeff] matching TF tensor [49,13]
    void compute(const int16_t* audio_int16, float* output) {
        // Normalise int16 → float32 [-1.0, 1.0] (heap allocated to prevent 64KB stack overflow)
        float* audio_f32 = (float*)malloc(MFCC_AUDIO_SAMPLES * sizeof(float));
        if (!audio_f32) return;
        for (int i = 0; i < MFCC_AUDIO_SAMPLES; i++) {
            audio_f32[i] = (float)audio_int16[i] / 32768.0f;
        }

        // Sliding window STFT → mel → log → DCT
        int frame_idx = 0;
        for (int start = 0; start + MFCC_N_FFT <= MFCC_AUDIO_SAMPLES && frame_idx < MFCC_N_FRAMES;
             start += MFCC_HOP_LENGTH, frame_idx++) {

            // Apply Hann window
            for (int i = 0; i < MFCC_N_FFT; i++) {
                fft_real[i] = audio_f32[start + i] * window[i];
                fft_imag[i] = 0.0f;
            }

            // In-place FFT (Cooley-Tukey, radix-2)
            _fft(fft_real, fft_imag, MFCC_N_FFT);

            // Power spectrum: |STFT|  (NOT squared — matches tf.abs(stft))
            for (int k = 0; k <= MFCC_N_FFT / 2; k++) {
                power_spec[k] = sqrtf(fft_real[k] * fft_real[k] +
                                      fft_imag[k] * fft_imag[k]);
            }

            // Mel filterbank: dot(power_spec, mel_filters[m])
            for (int m = 0; m < MFCC_N_MEL; m++) {
                float sum = 0.0f;
                for (int k = 0; k <= MFCC_N_FFT / 2; k++) {
                    sum += power_spec[k] * mel_filters[m][k];
                }
                mel_energy[m] = sum;
            }

            // Log mel: matches tf.math.log(mel + 1e-6)
            for (int m = 0; m < MFCC_N_MEL; m++) {
                log_mel[m] = logf(mel_energy[m] + MFCC_EPSILON);
            }

            // DCT-II (normalised): matches tf.signal.mfccs_from_log_mel_spectrograms
            // x[n] = (2/N) * sum_{m=0}^{N-1} log_mel[m] * cos(pi*n*(m+0.5)/N)
            // with orthonormal scaling (n=0 uses 1/sqrt(4N), others 1/sqrt(2N))
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

        free(audio_f32);
    }

private:
    // ── Build triangular mel filterbank ──────────────────────────────────
    // Matches tf.signal.linear_to_mel_weight_matrix exactly.
    void _build_mel_filters() {
        const float mel_low  = _hz_to_mel(MFCC_MEL_LOW_HZ);
        const float mel_high = _hz_to_mel(MFCC_MEL_HIGH_HZ);
        const int   n_bins   = MFCC_N_FFT / 2 + 1;

        // N_MEL + 2 centre frequencies in mel scale
        float mel_pts[MFCC_N_MEL + 2];
        for (int i = 0; i < MFCC_N_MEL + 2; i++) {
            mel_pts[i] = mel_low + (mel_high - mel_low) * i / (MFCC_N_MEL + 1);
        }

        // Convert mel → Hz → FFT bin index
        float hz_pts[MFCC_N_MEL + 2];
        for (int i = 0; i < MFCC_N_MEL + 2; i++) {
            hz_pts[i] = _mel_to_hz(mel_pts[i]);
        }

        // Build triangular filters
        for (int m = 0; m < MFCC_N_MEL; m++) {
            for (int k = 0; k < n_bins; k++) {
                float freq = (float)k * MFCC_SAMPLE_RATE / MFCC_N_FFT;
                float left   = hz_pts[m];
                float center = hz_pts[m + 1];
                float right  = hz_pts[m + 2];

                if (freq <= left || freq >= right) {
                    mel_filters[m][k] = 0.0f;
                } else if (freq < center) {
                    mel_filters[m][k] = (freq - left) / (center - left);
                } else {
                    mel_filters[m][k] = (right - freq) / (right - center);
                }
            }
        }
    }

    // ── DCT-II (normalised), matching TensorFlow's implementation ────────
    void _dct_ii_normalized(const float* x, float* out, int N_in, int N_out) {
        for (int n = 0; n < N_out; n++) {
            float sum = 0.0f;
            for (int m = 0; m < N_in; m++) {
                sum += x[m] * cosf(M_PI * n * (m + 0.5f) / N_in);
            }
            // Orthonormal scaling: matches TF DCT-II with norm='ortho'
            if (n == 0) {
                out[n] = sum * sqrtf(1.0f / (4.0f * N_in));
            } else {
                out[n] = sum * sqrtf(1.0f / (2.0f * N_in));
            }
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
