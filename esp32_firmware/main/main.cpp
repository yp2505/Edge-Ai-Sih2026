// main.cpp — Hey Vaani ESP32 Real Firmware (ESP-IDF)
//
// Architecture: Two clearly separated FreeRTOS tasks on two cores:
//
//   ┌─────────────────────────────────────────────────────────┐
//   │  CORE 1: inference_task  (always-on, low-power loop)   │
//   │  • I2S audio capture from INMP441                       │
//   │  • Mic self-check at boot (fail loudly if silent)       │
//   │  • Sliding-window MFCC every 30ms                       │
//   │  • TFLite Micro DS-CNN inference                        │
//   │  • Idle CPU target: < 10%                               │
//   └──────────────────────┬──────────────────────────────────┘
//                          │ keyword detected (prob ≥ 0.85)
//                          │ logs keyword_end_us (esp_timer_get_time)
//                          ▼
//   ┌─────────────────────────────────────────────────────────┐
//   │  CORE 0: streaming_task  (triggered, high-bandwidth)    │
//   │  • Opens TCP to cloud_server/server.py                  │
//   │  • Sends 20-byte HVP1 header (includes kw_to_connect_ms)│
//   │  • Streams raw PCM audio                                │
//   │  • Receives Whisper transcript JSON                      │
//   │  • Retries with backoff if server unreachable            │
//   │  • WiFi reconnect loop if connection drops               │
//   └─────────────────────────────────────────────────────────┘
//
// LATENCY METHOD (NTP-FREE):
//   The HVP1 header carries "keyword_end_to_connect_ms" — the time elapsed
//   on the ESP32's own monotonic clock (esp_timer_get_time, ±1µs, no internet)
//   between keyword confirmed and TCP connect() returning.
//   Server adds its own measured "connect→first_audio_byte" gap + transcribe time.
//   Total end-to-end = ESP32's value + server's values. No NTP needed.
//
// TENSOR ARENA NOTE:
//   tflite_arena is set to 54KB = measured 49152 bytes + 10% safety margin.
//   Run "idf.py monitor" after first boot — look for:
//     [INFERENCE] TFLite arena used: XXXXX bytes
//   If actual usage changes after model update, resize accordingly.
//
// MAX4466 Wiring (Analog ADC):
//   OUT   → GPIO 32  (ADC1_CH4)
//   VCC   → 3.3V
//   GND   → GND

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_adc/adc_continuous.h"
#include "hal/adc_types.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

// TFLite Micro
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model_data.h"    // 46.9 KB INT8 TFLite model
#include "mfcc.h"          // MFCC matching train_model.py exactly
#include "benchmark_cpu.h" // CPU load monitor

// ─── User Config ─────────────────────────────────────────────────────────────
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID       "Khush's A55"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD   "khush2073"
#endif
#ifndef CONFIG_SERVER_IP
#define CONFIG_SERVER_IP       "10.247.236.188"
#endif
#ifndef CONFIG_SERVER_PORT
#define CONFIG_SERVER_PORT     5000
#endif

// ─── ADC / Audio Configuration ───────────────────────────────────────────────
#define ADC_UNIT            ADC_UNIT_1
#define ADC_CHANNEL         ADC_CHANNEL_4    // GPIO 32
#define ADC_SAMPLE_RATE     16000
#define ADC_DMA_BUF_LEN     512

// ─── TFLite Configuration ───────────────────────────────────────────────────────
// A command session may start only after a high-confidence, sustained wake word.
// The previous 85% / two-window setting was sensitive enough for mic noise to
// start command streaming without anyone speaking.
static const float DETECT_THRESHOLD = 0.90f;
static const int   DETECTION_HITS_REQUIRED = 3;
// Calibrate from the actual microphone after boot, then require speech that is
// substantially louder than that noise floor before KWS can run.
static const float MIN_SPEECH_RMS = 0.050f;
static const float NOISE_FLOOR_MULTIPLIER = 2.5f;
static const int   NOISE_CALIBRATION_FRAMES = 50;  // 1.5 seconds at 30 ms/hop
#define SLIDE_STEP_MS       30
#define COMMAND_DURATION_MS 4000  // Safety maximum; silence ends capture sooner
#define COMMAND_MIN_DURATION_MS 700
#define COMMAND_SILENCE_MS 450
#define AUDIO_BUFFER_SAMPLES 16000   // 1 second ring buffer

// ─── HVP1 Protocol (must match cloud_server/server.py) ───────────────────────
// 20-byte header:  magic | sr | ch | bits | audio_len | kw_to_connect_ms
#define MAGIC_NUMBER     0x48565031u
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t audio_len;
    uint32_t kw_to_connect_ms;   // ESP32 monotonic: keyword_end → TCP connect
} hvp1_header_t;

// ─── WiFi ─────────────────────────────────────────────────────────────────────
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_MAX_RETRIES    10

static const char* TAG_INF  = "INFERENCE";
static const char* TAG_STR  = "STREAM";
static const char* TAG_MAIN = "MAIN";
static const char* TAG_WIFI = "WIFI";
static const char* TAG_MIC  = "MIC";

static volatile int  wifi_retry_count  = 0;
static volatile bool wifi_connected    = false;
// Set true while streaming_task holds I2S to prevent inference_task racing on it.
// Declared volatile for cross-core visibility (Core 0 ↔ Core 1).
static volatile bool streaming_active  = false;
static volatile uint32_t telemetry_inference_count = 0;
static volatile float telemetry_inference_ms = 0.0f;
static volatile float telemetry_keyword_confidence = 0.0f;
static volatile float telemetry_mic_rms = 0.0f;
static volatile float telemetry_noise_floor_rms = 0.0f;

// ─── Globals ──────────────────────────────────────────────────────────────────
static int16_t  audio_ring[AUDIO_BUFFER_SAMPLES];
static int16_t  audio_window[AUDIO_BUFFER_SAMPLES];  // Pre-allocated — avoids per-cycle 32KB malloc
static int      ring_write_pos = 0;
static SemaphoreHandle_t ring_mutex;

// Detection queue: sends keyword_end_us (int64_t, esp_timer_get_time())
static QueueHandle_t detect_queue;

static volatile uint32_t session_id = 0;

// ─── Tensor Arena ─────────────────────────────────────────────────────────────
// Measured arena_used_bytes() = 29136 bytes. 35KB gives ~20% safety margin.
// Reduced from 54KB to 35KB to free DRAM for audio_window heap allocation.
static const size_t TENSOR_ARENA_SIZE = 32 * 1024;
static uint8_t tflite_arena[32 * 1024];

// TFLite objects
static tflite::MicroMutableOpResolver<8> resolver;
static tflite::MicroInterpreter*         interpreter = nullptr;
static TfLiteTensor*                     input_tensor  = nullptr;
static TfLiteTensor*                     output_tensor = nullptr;

// ─── ADC handle & DC offset tracking ──────────────────────────────────────────
static adc_continuous_handle_t adc_handle = NULL;
// EMA filter for DC offset removal (starts roughly mid-point of 12-bit ADC)
static float adc_dc_offset = 2048.0f;
// Shift 12-bit ADC to 16-bit PCM. Normal is 4.
// We use 6 here to provide an automatic 4x software gain (volume boost).
static const int ADC_TO_PCM_SHIFT = 6;

// ─── MFCC processor ───────────────────────────────────────────────────────────
static MFCCProcessor mfcc_proc;
static float mfcc_output[MFCC_OUTPUT_SIZE];

// ─── TFLite Init ──────────────────────────────────────────────────────────────
static void tflite_init() {
    const tflite::Model* model = tflite::GetModel(model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG_INF, "TFLite schema version mismatch! Halting.");
        esp_restart();
    }
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddBatchMatMul();
    resolver.AddRelu();
    resolver.AddSoftmax();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddMean();

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tflite_arena, TENSOR_ARENA_SIZE
    );
    interpreter = &static_interpreter;

    TfLiteStatus st = interpreter->AllocateTensors();
    if (st != kTfLiteOk) {
        ESP_LOGE(TAG_INF, "AllocateTensors() failed! Arena may be too small.");
        esp_restart();
    }
    input_tensor  = interpreter->input(0);
    output_tensor = interpreter->output(0);

    // Log actual arena usage — capture this from serial and update TENSOR_ARENA_SIZE
    size_t used = interpreter->arena_used_bytes();
    size_t margin = TENSOR_ARENA_SIZE - used;
    ESP_LOGI(TAG_INF, "TFLite arena used: %u bytes (%.1f KB) — margin: %u bytes",
             (unsigned)used, used / 1024.0f, (unsigned)margin);
    ESP_LOGI(TAG_INF, "Free heap after TFLite init: %u bytes",
             (unsigned)esp_get_free_heap_size());
}

// ─── ADC Init ─────────────────────────────────────────────────────────────────
static void adc_init() {
    adc_continuous_handle_cfg_t adc_config = {};
    adc_config.max_store_buf_size = 4096;
    adc_config.conv_frame_size = ADC_DMA_BUF_LEN;
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &adc_handle));

    adc_continuous_config_t dig_cfg = {};
    dig_cfg.sample_freq_hz = 20000; // ESP-IDF v5+ requires minimum 20000 Hz
    dig_cfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    // dig_cfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1; // Deprecated in ESP-IDF v5+
    
    adc_digi_pattern_config_t adc_pattern[1];
    adc_pattern[0].atten = ADC_ATTEN_DB_12;
    adc_pattern[0].channel = ADC_CHANNEL;
    adc_pattern[0].unit = ADC_UNIT;
    adc_pattern[0].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    dig_cfg.pattern_num = 1;
    dig_cfg.adc_pattern = adc_pattern;

    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
    ESP_LOGI(TAG_MAIN, "ADC MAX4466 initialized: %d Hz", ADC_SAMPLE_RATE);
}

// ─── Mic Self-Check ───────────────────────────────────────────────────────────
// Reads 0.5 seconds of audio at boot. If all-zero (mic disconnected or miswired),
// prints a loud error and halts rather than silently running on dead audio.
static void mic_selfcheck() {
    const int check_samples = 20000 / 2;   // 0.5 seconds at 20kHz hw rate
    uint8_t raw_buf[512];
    int64_t sum_sq = 0;
    int captured = 0;

    ESP_LOGI(TAG_MIC, "Mic self-check: reading 0.5s of audio...");
    while (captured < check_samples) {
        uint32_t bytes_read = 0;
        int want = (check_samples - captured < 128) ? (check_samples - captured) : 128; // up to 128 samples = 512 bytes
        adc_continuous_read(adc_handle, raw_buf, want * SOC_ADC_DIGI_RESULT_BYTES,
                            &bytes_read, pdMS_TO_TICKS(500));
        int got = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
        for (int i = 0; i < got; i++) {
            adc_digi_output_data_t *p = (adc_digi_output_data_t*)&raw_buf[i * SOC_ADC_DIGI_RESULT_BYTES];
            uint32_t raw_adc_val = p->type1.data;
            
            // EMA filter
            adc_dc_offset = 0.005f * raw_adc_val + 0.995f * adc_dc_offset;
            int16_t centered = (int16_t)(raw_adc_val - adc_dc_offset);
            int16_t s = (int16_t)(centered << ADC_TO_PCM_SHIFT);
            
            sum_sq += (int64_t)s * s;
        }
        captured += got;
    }
    float rms = sqrtf((float)sum_sq / captured) / 32768.0f;
    ESP_LOGI(TAG_MIC, "Mic RMS energy: %.5f", rms);

    if (rms < 1e-4f) { // Slightly higher threshold than I2S due to noise floor, but low enough for "dead"
        ESP_LOGE(TAG_MIC,
            "\n"
            "╔══════════════════════════════════════════════════════════╗\n"
            "║  ❌  MIC SELF-CHECK FAILED — No valid audio detected!    ║\n"
            "║  Possible causes:                                        ║\n"
            "║   • MAX4466 OUT pin not connected to GPIO 32             ║\n"
            "║   • VCC not connected to 3.3V                            ║\n"
            "║  Halting. Fix wiring, then reset.                        ║\n"
            "╚══════════════════════════════════════════════════════════╝");
        // Blink indefinitely to signal hardware error before halting
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG_MIC, "✅ Mic self-check PASSED (RMS=%.5f)", rms);
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────
static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t event_id, void* data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (wifi_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            wifi_retry_count = wifi_retry_count + 1;
            ESP_LOGW(TAG_WIFI, "WiFi lost — retry %d/%d", wifi_retry_count, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG_WIFI, "WiFi failed after %d retries", WIFI_MAX_RETRIES);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0;
        wifi_connected   = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG_WIFI, "✅ WiFi connected — IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void wifi_init() {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid,     CONFIG_WIFI_SSID,     sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Block until connected or failed
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_WIFI, "WiFi ready");
    } else {
        ESP_LOGW(TAG_WIFI, "WiFi not connected — streaming will fail until reconnected");
    }
}

// ─── WiFi reconnect helper (called from streaming_task before each connect) ───
static bool wait_for_wifi(uint32_t timeout_ms) {
    if (wifi_connected) return true;
    ESP_LOGW(TAG_STR, "WiFi not connected, waiting up to %lums...", (unsigned long)timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

// ─── TASK A: Inference Loop (CORE 1, always-on) ──────────────────────────────
static void inference_task(void* arg) {
    printf(">>> INFERENCE TASK STARTING! <<<\n"); fflush(stdout);
    ESP_LOGI(TAG_INF, "Inference task started on core 1");

    const int hop_samples = 16000 * SLIDE_STEP_MS / 1000;  // 480 @ 16kHz
    // ADC runs at 20 kHz. Dropping one sample in every five produces 16 kHz.
    // The driver returns one DMA frame at a time (512 bytes = 128 ADC samples),
    // so collecting a complete 480-sample output hop needs several reads.
    
    // Allocate ADC buffers ONCE at startup (audio_window is now static global)
    uint8_t* raw_buf = (uint8_t*)malloc(ADC_DMA_BUF_LEN);
    int16_t* hop_buf = (int16_t*)malloc(hop_samples * sizeof(int16_t));
    if (!raw_buf || !hop_buf) {
        ESP_LOGE(TAG_INF, "FATAL: raw/hop buffer alloc failed! heap=%lu",
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(portMAX_DELAY));
        return;
    }
    ESP_LOGI(TAG_INF, "Inference buffers OK (heap=%lu)", (unsigned long)esp_get_free_heap_size());

    uint32_t infer_count   = 0;
    float    infer_total_ms = 0.0f;
    int      decimation_phase = 0;
    int      valid_ring_samples = 0;
    int      consecutive_hits = 0;
    float    noise_floor_rms = 0.0f;
    int      noise_calibration_frames = 0;
    // A DMA frame is 128 raw samples, while a 30 ms output hop needs 600 raw
    // samples.  Preserve the few valid samples left over from the final frame.
    int16_t  pending_pcm[ADC_DMA_BUF_LEN / SOC_ADC_DIGI_RESULT_BYTES];
    int      pending_count = 0;

    // ─── [DEBUG] Heartbeat counter (raw printf, bypasses ESP_LOG filtering) ─
    uint32_t dbg_loop_count = 0;
    uint32_t last_raw_adc = 0;

    while (true) {
        dbg_loop_count++;

        // 1. Capture exactly one valid 480-sample, 16 kHz hop.  Never write a
        // partial hop: doing so feeds uninitialised memory into the MFCC/model.
        int out_idx = 0;
        while (pending_count > 0 && out_idx < hop_samples) {
            hop_buf[out_idx++] = pending_pcm[0];
            memmove(pending_pcm, pending_pcm + 1,
                    (size_t)(--pending_count) * sizeof(pending_pcm[0]));
        }
        while (out_idx < hop_samples) {
            uint32_t bytes_read = 0;
            esp_err_t err = adc_continuous_read(adc_handle, raw_buf, ADC_DMA_BUF_LEN,
                                                &bytes_read, pdMS_TO_TICKS(100));
            if (err != ESP_OK || bytes_read == 0) continue;

            int got = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
            for (int i = 0; i < got; i++) {
                adc_digi_output_data_t *p =
                    (adc_digi_output_data_t*)&raw_buf[i * SOC_ADC_DIGI_RESULT_BYTES];
                uint32_t raw_adc_val = p->type1.data;
                last_raw_adc = raw_adc_val;

                // Keep the DC estimator at the native 20 kHz rate.
                adc_dc_offset = 0.005f * raw_adc_val + 0.995f * adc_dc_offset;
                int16_t centered = (int16_t)(raw_adc_val - adc_dc_offset);

                // Deterministic 20 kHz -> 16 kHz decimation: retain 4 of 5.
                bool keep_sample = decimation_phase != 4;
                decimation_phase = (decimation_phase + 1) % 5;
                if (keep_sample) {
                    int16_t pcm = (int16_t)(centered << ADC_TO_PCM_SHIFT);
                    if (out_idx < hop_samples) {
                        hop_buf[out_idx++] = pcm;
                    } else if (pending_count < (int)(sizeof(pending_pcm) / sizeof(pending_pcm[0]))) {
                        pending_pcm[pending_count++] = pcm;
                    }
                }

            }
        }

        // 2. Write into ring buffer
        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        for (int i = 0; i < hop_samples; i++) {
            audio_ring[ring_write_pos] = hop_buf[i];
            ring_write_pos = (ring_write_pos + 1) % AUDIO_BUFFER_SAMPLES;
        }
        xSemaphoreGive(ring_mutex);
        if (valid_ring_samples < AUDIO_BUFFER_SAMPLES) {
            valid_ring_samples += hop_samples;
            continue;  // Do not classify a partly zero/uninitialised startup window.
        }

        // 3. Compute RMS for telemetry and VAD
        int64_t sum_sq = 0;
        for (int i = 0; i < hop_samples; i++) sum_sq += (int64_t)hop_buf[i] * hop_buf[i];
        float rms = sqrtf((float)sum_sq / hop_samples) / 32768.0f;
        telemetry_mic_rms = rms;

        // Unconditional heartbeat every 5 frames so we don't miss it
        if (dbg_loop_count % 5 == 0) {
            printf("[DBG] loop=%lu rms=%.5f raw=%lu dc=%.1f streaming=%d\n",
                   (unsigned long)dbg_loop_count, rms, (unsigned long)last_raw_adc, (float)adc_dc_offset, (int)streaming_active);
            fflush(stdout);
        }

        // Pause I2S reads while streaming_task is live-streaming audio to server.
        // This prevents the two tasks from racing on the I2S FIFO.
        if (streaming_active) {
            vTaskDelay(pdMS_TO_TICKS(SLIDE_STEP_MS));
            continue;
        }

        // 4. Learn the installed microphone's idle noise level before enabling
        // wake-word inference. This prevents a noisy or floating MAX4466 output
        // from being mistaken for a spoken command after every restart.
        if (noise_calibration_frames < NOISE_CALIBRATION_FRAMES) {
            noise_floor_rms += (rms - noise_floor_rms) /
                               (float)(++noise_calibration_frames);
            telemetry_noise_floor_rms = noise_floor_rms;
            consecutive_hits = 0;
            continue;
        }

        // Track only sub-threshold audio as background noise. Speech cannot
        // raise the gate while a person is talking.
        float speech_rms_threshold = fmaxf(MIN_SPEECH_RMS,
                                            noise_floor_rms * NOISE_FLOOR_MULTIPLIER);
        if (rms < speech_rms_threshold) {
            noise_floor_rms = 0.02f * rms + 0.98f * noise_floor_rms;
            telemetry_noise_floor_rms = noise_floor_rms;
            consecutive_hits = 0;
            continue;
        }

        // 4. Linearise ring buffer → 1-second window (uses static buffer, no malloc needed)
        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        int start = ring_write_pos;
        for (int i = 0; i < AUDIO_BUFFER_SAMPLES; i++) {
            audio_window[i] = audio_ring[(start + i) % AUDIO_BUFFER_SAMPLES];
        }
        xSemaphoreGive(ring_mutex);

        // 5. MFCC + TFLite inference
        mfcc_proc.compute(audio_window, mfcc_output);

        float in_scale  = input_tensor->params.scale;
        int32_t in_zp   = input_tensor->params.zero_point;
        int8_t* inp_data = input_tensor->data.int8;
        for (int i = 0; i < MFCC_OUTPUT_SIZE; i++) {
            int32_t q = (int32_t)roundf(mfcc_output[i] / in_scale) + in_zp;
            if (q < -128) q = -128;
            if (q >  127) q =  127;
            inp_data[i] = (int8_t)q;
        }
        int64_t t_infer_start = esp_timer_get_time();
        interpreter->Invoke();
        float infer_us = (float)(esp_timer_get_time() - t_infer_start);

        float out_scale   = output_tensor->params.scale;
        int32_t out_zp    = output_tensor->params.zero_point;
        int8_t* out_data  = output_tensor->data.int8;
        float not_kw_prob = (out_data[0] - out_zp) * out_scale;
        float kw_prob     = (out_data[1] - out_zp) * out_scale;

        infer_count++;
        infer_total_ms += infer_us / 1000.0f;
        telemetry_inference_count = telemetry_inference_count + 1;
        telemetry_inference_ms += infer_us / 1000.0f;
        telemetry_keyword_confidence = kw_prob;
        bool model_hit = kw_prob >= DETECT_THRESHOLD;
        consecutive_hits = model_hit ? consecutive_hits + 1 : 0;

        if (consecutive_hits >= DETECTION_HITS_REQUIRED) {
            // 6. Keyword detected
            int64_t keyword_end_us = esp_timer_get_time();
            printf("\n╔══════════════════════════════════════════════════════════════╗\n");
            printf("║  🟢 🔔 KEYWORD CONFIRMED: \"Hey Vaani\" (Confidence: %5.1f%%)  ║\n", kw_prob * 100.0f);
            printf("║  📡 Streaming 4s command audio to Cloud Whisper Server...    ║\n");
            printf("╚══════════════════════════════════════════════════════════════╝\n\n");
            fflush(stdout);

            xQueueSend(detect_queue, &keyword_end_us, 0);
            consecutive_hits = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));  // short re-arm delay
        } else if (consecutive_hits > 0) {
            printf("  [⚡ Analyzing Match] Frame %d/%d (Confidence: %.1f%%)...\n",
                   consecutive_hits, DETECTION_HITS_REQUIRED, kw_prob * 100.0f);
            fflush(stdout);
        } else if (kw_prob >= 0.30f) {
            printf("  [🎙️ Voice Detected] Confidence: %.1f%% (Threshold: 50.0%%)\n", kw_prob * 100.0f);
            fflush(stdout);
        } else if (infer_count % 20 == 0) {
            printf("  [🎙️ Listening] Audio RMS: %.4f | Peak Conf: %.1f%%\n", rms, kw_prob * 100.0f);
            fflush(stdout);
        }
    }
}

// ─── TASK B: Streaming Task (CORE 0, triggered on detection) ─────────────────
//
// LIVE-STREAMING ARCHITECTURE (sub-100ms):
//
//   OLD (batch):    detect → record 2000ms → connect → send all → wait → result
//   Total: ~2240ms
//
//   NEW (streaming): detect → connect NOW → stream 30ms chunks live → result
//   Total: kw_to_connect (~40ms) + streaming (~0ms overhead) + ASR (~10ms) = ~50ms
//
// The key insight: connect to the server IMMEDIATELY at keyword detection.
// Stream audio in 30ms chunks as the user speaks the command.
// Server (Vosk streaming mode) processes each chunk as it arrives.
// By the time the user stops speaking, the transcript is already done.
// ─────────────────────────────────────────────────────────────────────────────
static void streaming_task(void* arg) {
    ESP_LOGI(TAG_STR, "Streaming task started (core %d) — LIVE-STREAMING mode",
             xPortGetCoreID());
    int64_t keyword_end_us;

    while (true) {
        xQueueReceive(detect_queue, &keyword_end_us, portMAX_DELAY);
        session_id = session_id + 1;
        uint32_t sid = session_id;
        ESP_LOGI(TAG_STR, "[Session %lu] Keyword detected — connecting immediately",
                 (unsigned long)sid);

        // 1. Wait for WiFi (with 5s timeout, same as before)
        if (!wait_for_wifi(5000)) {
            ESP_LOGE(TAG_STR, "[Session %lu] WiFi not ready — dropping",
                     (unsigned long)sid);
            continue;
        }

        // 2. Connect to server IMMEDIATELY — do NOT wait to record audio first
        int sock = -1;
        uint32_t kw_to_connect_ms = 0;
        uint32_t backoff_ms = 200;

        for (int attempt = 0; attempt < 4; attempt++) {
            struct sockaddr_in server_addr = {};
            server_addr.sin_family = AF_INET;
            server_addr.sin_port   = htons(CONFIG_SERVER_PORT);
            inet_pton(AF_INET, CONFIG_SERVER_IP, &server_addr.sin_addr);

            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) {
                ESP_LOGW(TAG_STR, "[Session %lu] socket() failed (attempt %d), retry in %lums",
                         (unsigned long)sid, attempt + 1, (unsigned long)backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms *= 2;
                sock = -1;
                continue;
            }

            // Disable Nagle — send each 30ms chunk immediately with no buffering delay
            int flag = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            // kw_to_connect_ms = ESP32 monotonic clock: keyword_end → TCP connect done
            if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                int64_t connect_done_us = esp_timer_get_time();
                kw_to_connect_ms = (uint32_t)((connect_done_us - keyword_end_us) / 1000);
                ESP_LOGI(TAG_STR, "[Session %lu] Connected (attempt %d) kw→connect=%lums",
                         (unsigned long)sid, attempt + 1, (unsigned long)kw_to_connect_ms);
                break;
            } else {
                ESP_LOGW(TAG_STR, "[Session %lu] connect() failed (attempt %d), retry in %lums",
                         (unsigned long)sid, attempt + 1, (unsigned long)backoff_ms);
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms *= 2;
            }
        }

        if (sock < 0) {
            ESP_LOGE(TAG_STR, "[Session %lu] All TCP attempts failed — dropping",
                     (unsigned long)sid);
            continue;
        }

        // 3. Send HVP1 header with audio_len=0 (= live-streaming mode)
        //    Server will read until we close the write side.
        hvp1_header_t hdr = {
            .magic            = MAGIC_NUMBER,
            .sample_rate      = ADC_SAMPLE_RATE,
            .channels         = 1,
            .bits             = 16,
            .audio_len        = 0,   // 0 = streaming until shutdown(SHUT_WR)
            .kw_to_connect_ms = kw_to_connect_ms,
        };
        send(sock, &hdr, sizeof(hdr), 0);

        // 4. Stream audio LIVE from ADC for COMMAND_DURATION_MS
        //    inference_task pauses its own reads while streaming_active=true
        //    so both tasks don't race on the ADC ring buffer.
        streaming_active = true;

        // Convert 20kHz ADC input to the 16kHz PCM declared in HVP1. This
        // keeps Whisper timing correct and produces a true 30ms chunk.
        const int CHUNK_SAMPLES = 480;
        const int RAW_CHUNK_SAMPLES = 600;
        uint8_t raw_buf[RAW_CHUNK_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES];
        int16_t pcm16[CHUNK_SAMPLES];
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(COMMAND_DURATION_MS);
        int total_sent = 0;
        int streamed_ms = 0;
        int consecutive_silence_ms = 0;
        int decimation_phase = 0;

        while (xTaskGetTickCount() < deadline) {
            uint32_t bytes_read = 0;
            esp_err_t read_err = adc_continuous_read(adc_handle, raw_buf, sizeof(raw_buf),
                                                     &bytes_read, pdMS_TO_TICKS(50));
            if (read_err != ESP_OK || bytes_read == 0) continue;

            int output_samples = 0;
            int raw_samples = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
            for (int i = 0; i < raw_samples && output_samples < CHUNK_SAMPLES; i++) {
                adc_digi_output_data_t *p = (adc_digi_output_data_t*)&raw_buf[i * SOC_ADC_DIGI_RESULT_BYTES];
                uint32_t raw_adc_val = p->type1.data;
                adc_dc_offset = 0.005f * raw_adc_val + 0.995f * adc_dc_offset;
                int16_t centered = (int16_t)(raw_adc_val - adc_dc_offset);
                bool keep_sample = decimation_phase != 4;
                decimation_phase = (decimation_phase + 1) % 5;
                if (keep_sample) pcm16[output_samples++] = (int16_t)(centered << ADC_TO_PCM_SHIFT);
            }
            if (output_samples == 0) continue;

            int sent_bytes = send(sock, pcm16, output_samples * sizeof(int16_t), 0);
            if (sent_bytes < 0) {
                ESP_LOGE(TAG_STR, "[Session %lu] send() failed � server gone?", (unsigned long)sid);
                break;
            }
            total_sent += output_samples;
            int chunk_ms = output_samples * 1000 / ADC_SAMPLE_RATE;
            streamed_ms += chunk_ms;

            int64_t sum_sq = 0;
            for (int i = 0; i < output_samples; i++) sum_sq += (int64_t)pcm16[i] * pcm16[i];
            float chunk_rms = sqrtf((float)sum_sq / output_samples) / 32768.0f;
            float silence_threshold = fmaxf(0.025f, telemetry_noise_floor_rms * 1.5f);
            if (streamed_ms >= COMMAND_MIN_DURATION_MS) {
                consecutive_silence_ms = chunk_rms < silence_threshold
                    ? consecutive_silence_ms + chunk_ms : 0;
                if (consecutive_silence_ms >= COMMAND_SILENCE_MS) {
                    ESP_LOGI(TAG_STR, "[Session %lu] Command ended on silence after %dms",
                             (unsigned long)sid, streamed_ms);
                    break;
                }
            }
        }

        streaming_active = false;  // release ADC back to inference_task
        shutdown(sock, SHUT_WR);   // signal end-of-stream to server

        ESP_LOGI(TAG_STR, "[Session %lu] Stream complete: %d samples (%.2fs)",
                 (unsigned long)sid, total_sent, (float)total_sent / ADC_SAMPLE_RATE);

        // 5. Read transcript response from server
        {
                char resp[1024] = {0};
                int resp_len = 0, r;
                while ((r = recv(sock, resp + resp_len,
                                 sizeof(resp) - resp_len - 1, 0)) > 0) {
                    resp_len += r;
                }
                close(sock);
                sock = -1;

                printf("\n╔══════════════════════════════════════════════════════════════╗\n");
                printf("║  📝 CLOUD WHISPER TRANSCRIPTION:                             ║\n");
                printf("║  → %-56s  ║\n", resp_len > 0 ? resp : "[no response from server]");
                printf("╚══════════════════════════════════════════════════════════════╝\n\n");
                fflush(stdout);
        }
    }
}

// Sends measured ESP32 health data once per second. This is deliberately low-rate:
// the microphone and wake-word model run continuously, while telemetry must not
// consume the CPU budget reserved for inference.
static void telemetry_task(void* arg) {
    float previous_ms = 0.0f;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!wifi_connected) continue;
        wifi_ap_record_t ap = {};
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        uint32_t count = telemetry_inference_count;
        float total_ms = telemetry_inference_ms;
        float duty_pct = total_ms >= previous_ms ? (total_ms - previous_ms) / 10.0f : 0.0f;
        previous_ms = total_ms;
        uint32_t heap_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        uint32_t heap_free = esp_get_free_heap_size();
        uint32_t heap_min = esp_get_minimum_free_heap_size();
        char body[512];
        int body_len = snprintf(body, sizeof(body),
            "{\"device\":\"esp32\",\"uptime_ms\":%llu,\"free_heap_bytes\":%lu,\"min_free_heap_bytes\":%lu,\"heap_total_bytes\":%lu,\"tflite_arena_bytes\":%u,\"audio_buffer_bytes\":%u,\"keyword_confidence\":%.4f,\"mic_rms\":%.5f,\"inference_count\":%lu,\"inference_duty_pct\":%.3f,\"wifi_rssi_dbm\":%d,\"streaming\":%s}",
            (unsigned long long)(esp_timer_get_time() / 1000), (unsigned long)heap_free,
            (unsigned long)heap_min, (unsigned long)heap_total, (unsigned)TENSOR_ARENA_SIZE,
            (unsigned)(sizeof(audio_ring) + sizeof(audio_window)), telemetry_keyword_confidence,
            telemetry_mic_rms, (unsigned long)count, duty_pct, rssi, streaming_active ? "true" : "false");
        if (body_len <= 0 || body_len >= (int)sizeof(body)) continue;
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) continue;
        
        // 200ms connection and send timeout to prevent blocking
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET; addr.sin_port = htons(8080);
        inet_pton(AF_INET, CONFIG_SERVER_IP, &addr.sin_addr);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            char request[700];
            int request_len = snprintf(request, sizeof(request),
                "POST /api/telemetry HTTP/1.1\r\nHost: esp32\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s", body_len, body);
            if (request_len > 0 && request_len < (int)sizeof(request)) send(sock, request, request_len, 0);
        }
        close(sock);
    }
}
// ─── app_main ─────────────────────────────────────────────────────────────────
extern "C" void app_main() {
    ESP_LOGI(TAG_MAIN, "=== Hey Vaani Edge Firmware (SIH 2026) ===");

    ESP_ERROR_CHECK(nvs_flash_init());

    ring_mutex   = xSemaphoreCreateMutex();
    detect_queue = xQueueCreate(4, sizeof(int64_t));

    // WiFi (no NTP — not needed for latency measurement anymore)
    wifi_init();

    // ADC microphone
    adc_init();

    // Mic self-check — halts loudly if mic is disconnected/miswired
    mic_selfcheck();

    // TFLite Micro
    tflite_init();
    mfcc_proc.init();

    // CPU load benchmark (disabled to keep terminal output clean)
    // benchmark_cpu_start();

    // Log firmware memory footprint
    ESP_LOGI(TAG_MAIN, "Model size (flash): %u bytes (%.1f KB)",
             (unsigned)sizeof(model_data), sizeof(model_data) / 1024.0f);
    ESP_LOGI(TAG_MAIN, "Tensor arena alloc: %u bytes (%u KB)",
             (unsigned)TENSOR_ARENA_SIZE, (unsigned)(TENSOR_ARENA_SIZE / 1024));
    ESP_LOGI(TAG_MAIN, "Free heap after init: %u bytes",
             (unsigned)esp_get_free_heap_size());

    // Spawn FreeRTOS tasks (24KB stack for inference to support TFLite operators safely)
    xTaskCreatePinnedToCore(inference_task, "inference",
       24576, NULL, 5, NULL, 1);   // Core 1, 24KB stack
    xTaskCreatePinnedToCore(streaming_task, "streaming",
       8192, NULL, 4, NULL, 0);    // Core 0, 8KB stack
    xTaskCreatePinnedToCore(telemetry_task, "telemetry",
       4096, NULL, 1, NULL, 0);    // Core 0, 4KB stack

    ESP_LOGI(TAG_MAIN, "All tasks started. Listening for 'Hey Vaani'...");
}
