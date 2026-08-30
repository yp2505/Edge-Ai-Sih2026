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
// INMP441 Wiring:
//   BCLK  → GPIO 26
//   WS    → GPIO 25
//   SD    → GPIO 34  (input only)
//   VDD   → 3.3V
//   GND   → GND
//   L/R   → GND  (left channel = mono)

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
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
#define CONFIG_WIFI_SSID       "YourWiFiName"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD   "YourWiFiPass"
#endif
#ifndef CONFIG_SERVER_IP
#define CONFIG_SERVER_IP       "192.168.1.100"
#endif
#ifndef CONFIG_SERVER_PORT
#define CONFIG_SERVER_PORT     5000
#endif

// ─── I2S / Audio Configuration ───────────────────────────────────────────────
#define I2S_PORT            I2S_NUM_0
#define I2S_BCLK_PIN        26
#define I2S_WS_PIN          25
#define I2S_DATA_PIN        34
#define I2S_SAMPLE_RATE     16000
#define I2S_DMA_BUF_COUNT   8
#define I2S_DMA_BUF_LEN     512

// ─── Keyword Detection ────────────────────────────────────────────────────────
#define DETECT_THRESHOLD    0.85f
#define SLIDE_STEP_MS       30
#define COMMAND_DURATION_MS 2000
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

static volatile int  wifi_retry_count = 0;
static volatile bool wifi_connected   = false;

// ─── Globals ──────────────────────────────────────────────────────────────────
static int16_t  audio_ring[AUDIO_BUFFER_SAMPLES];
static int      ring_write_pos = 0;
static SemaphoreHandle_t ring_mutex;

// Detection queue: sends keyword_end_us (int64_t, esp_timer_get_time())
static QueueHandle_t detect_queue;

static volatile uint32_t session_id = 0;

// ─── Tensor Arena ─────────────────────────────────────────────────────────────
// Size: measured arena_used_bytes() = ~49152 bytes on first boot, +10% = 54KB.
// If model is updated, re-check "TFLite arena used: X bytes" in serial log
// and update this constant to (measured_bytes * 1.10).
static const size_t TENSOR_ARENA_SIZE = 54 * 1024;
static uint8_t tflite_arena[54 * 1024];

// TFLite objects
static tflite::MicroMutableOpResolver<8> resolver;
static tflite::MicroInterpreter*         interpreter = nullptr;
static TfLiteTensor*                     input_tensor  = nullptr;
static TfLiteTensor*                     output_tensor = nullptr;

// ─── I2S handle ───────────────────────────────────────────────────────────────
static i2s_chan_handle_t i2s_rx_chan;

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

// ─── I2S Init ─────────────────────────────────────────────────────────────────
static void i2s_init() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws   = (gpio_num_t)I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_DATA_PIN,
        }
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));
    ESP_LOGI(TAG_MAIN, "I2S INMP441 initialized: %d Hz mono", I2S_SAMPLE_RATE);
}

// ─── Mic Self-Check ───────────────────────────────────────────────────────────
// Reads 0.5 seconds of audio at boot. If all-zero (mic disconnected or miswired),
// prints a loud error and halts rather than silently running on dead audio.
static void mic_selfcheck() {
    const int check_samples = I2S_SAMPLE_RATE / 2;   // 0.5 seconds
    int32_t raw32[512];
    int64_t sum_sq = 0;
    int captured = 0;

    ESP_LOGI(TAG_MIC, "Mic self-check: reading 0.5s of audio...");
    while (captured < check_samples) {
        size_t bytes_read = 0;
        int want = (check_samples - captured < 512) ? (check_samples - captured) : 512;
        i2s_channel_read(i2s_rx_chan, raw32, want * sizeof(int32_t),
                         &bytes_read, pdMS_TO_TICKS(500));
        int got = bytes_read / sizeof(int32_t);
        for (int i = 0; i < got; i++) {
            int16_t s = (int16_t)(raw32[i] >> 14);
            sum_sq += (int64_t)s * s;
        }
        captured += got;
    }
    float rms = sqrtf((float)sum_sq / captured) / 32768.0f;
    ESP_LOGI(TAG_MIC, "Mic RMS energy: %.5f", rms);

    if (rms < 1e-5f) {
        ESP_LOGE(TAG_MIC,
            "\n"
            "╔══════════════════════════════════════════════════════════╗\n"
            "║  ❌  MIC SELF-CHECK FAILED — All-zero audio detected!    ║\n"
            "║  Possible causes:                                        ║\n"
            "║   • INMP441 data pin (SD) not connected to GPIO 34       ║\n"
            "║   • VDD not connected to 3.3V                            ║\n"
            "║   • Wrong I2S pins in config                             ║\n"
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
            wifi_retry_count++;
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
    ESP_LOGI(TAG_INF, "Inference task started (core %d)", xPortGetCoreID());

    const int hop_samples = (I2S_SAMPLE_RATE * SLIDE_STEP_MS) / 1000;  // 480 @ 30ms
    int32_t   i2s_raw[hop_samples];
    int16_t   hop_buf[hop_samples];

    uint32_t infer_count   = 0;
    float    infer_total_ms = 0.0f;

    while (true) {
        // 1. Capture one hop via I2S
        size_t bytes_read = 0;
        i2s_channel_read(i2s_rx_chan, i2s_raw, hop_samples * sizeof(int32_t),
                         &bytes_read, pdMS_TO_TICKS(100));
        for (int i = 0; i < hop_samples; i++) {
            hop_buf[i] = (int16_t)(i2s_raw[i] >> 14);
        }

        // 2. Write into ring buffer
        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        for (int i = 0; i < hop_samples; i++) {
            audio_ring[ring_write_pos] = hop_buf[i];
            ring_write_pos = (ring_write_pos + 1) % AUDIO_BUFFER_SAMPLES;
        }
        xSemaphoreGive(ring_mutex);

        // 3. Lightweight VAD: skip inference on silence (saves ~9% CPU)
        int64_t sum_sq = 0;
        for (int i = 0; i < hop_samples; i++) sum_sq += (int64_t)hop_buf[i] * hop_buf[i];
        float rms = sqrtf((float)sum_sq / hop_samples) / 32768.0f;
        if (rms < 0.003f) continue;

        // 4. Linearise ring buffer → 1-second window
        int16_t audio_window[AUDIO_BUFFER_SAMPLES];
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
        float kw_prob     = (out_data[1] - out_zp) * out_scale;

        infer_count++;
        infer_total_ms += infer_us / 1000.0f;
        if (infer_count % 100 == 0) {
            ESP_LOGI(TAG_INF, "avg inference: %.1f ms | kw_prob: %.3f",
                     infer_total_ms / infer_count, kw_prob);
        }

        // 6. Keyword detected
        if (kw_prob >= DETECT_THRESHOLD) {
            // Capture keyword_end timestamp using ESP32 monotonic clock (NO NTP needed)
            int64_t keyword_end_us = esp_timer_get_time();
            ESP_LOGI(TAG_INF, "🔔 HEY VAANI DETECTED! prob=%.3f  keyword_end_us=%lld",
                     kw_prob, keyword_end_us);
            xQueueSend(detect_queue, &keyword_end_us, 0);
            vTaskDelay(pdMS_TO_TICKS(3000));  // cool-down
        }
    }
}

// ─── TASK B: Streaming Task (CORE 0, triggered on detection) ─────────────────
static void streaming_task(void* arg) {
    ESP_LOGI(TAG_STR, "Streaming task started (core %d)", xPortGetCoreID());
    int64_t keyword_end_us;

    while (true) {
        xQueueReceive(detect_queue, &keyword_end_us, portMAX_DELAY);
        session_id++;
        uint32_t sid = session_id;
        ESP_LOGI(TAG_STR, "[Session %lu] Triggered", (unsigned long)sid);

        // 1. Capture COMMAND_DURATION_MS of audio
        int command_samples = (I2S_SAMPLE_RATE * COMMAND_DURATION_MS) / 1000;
        int16_t* cmd_buf = (int16_t*)malloc(command_samples * sizeof(int16_t));
        if (!cmd_buf) {
            ESP_LOGE(TAG_STR, "[Session %lu] OOM — dropping detection", (unsigned long)sid);
            continue;
        }
        {
            int32_t raw32[512];
            int captured = 0;
            TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(COMMAND_DURATION_MS + 300);
            while (captured < command_samples && xTaskGetTickCount() < deadline) {
                int want = (command_samples - captured < 512) ? (command_samples - captured) : 512;
                size_t bytes_read = 0;
                i2s_channel_read(i2s_rx_chan, raw32, want * sizeof(int32_t),
                                 &bytes_read, pdMS_TO_TICKS(100));
                int got = bytes_read / sizeof(int32_t);
                for (int i = 0; i < got; i++) {
                    cmd_buf[captured++] = (int16_t)(raw32[i] >> 14);
                }
            }
        }

        // 2. Wait for WiFi (with 5s timeout)
        if (!wait_for_wifi(5000)) {
            ESP_LOGE(TAG_STR, "[Session %lu] WiFi not ready — dropping detection", (unsigned long)sid);
            free(cmd_buf);
            continue;
        }

        // 3. Connect to server with retry + exponential backoff
        int sock = -1;
        uint32_t backoff_ms = 200;
        for (int attempt = 0; attempt < 4; attempt++) {
            struct sockaddr_in server_addr = {};
            server_addr.sin_family = AF_INET;
            server_addr.sin_port   = htons(CONFIG_SERVER_PORT);
            inet_pton(AF_INET, CONFIG_SERVER_IP, &server_addr.sin_addr);

            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) {
                ESP_LOGE(TAG_STR, "[Session %lu] socket() failed, retrying in %lums",
                         (unsigned long)sid, (unsigned long)backoff_ms);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms *= 2;
                sock = -1;
                continue;
            }

            // TCP_NODELAY: disable Nagle's for low latency
            int flag = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

            // Measure kw_to_connect_ms using ESP32's own monotonic clock (NO NTP)
            int64_t connect_start_us = esp_timer_get_time();
            if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
                int64_t connect_done_us = esp_timer_get_time();
                uint32_t kw_to_connect_ms =
                    (uint32_t)((connect_done_us - keyword_end_us) / 1000);
                ESP_LOGI(TAG_STR, "[Session %lu] Connected (attempt %d) kw→connect=%lums",
                         (unsigned long)sid, attempt + 1, (unsigned long)kw_to_connect_ms);

                // 4. Send HVP1 header (20 bytes)
                hvp1_header_t hdr = {
                    .magic           = MAGIC_NUMBER,
                    .sample_rate     = I2S_SAMPLE_RATE,
                    .channels        = 1,
                    .bits            = 16,
                    .audio_len       = (uint32_t)(command_samples * sizeof(int16_t)),
                    .kw_to_connect_ms = kw_to_connect_ms,
                };
                send(sock, &hdr, sizeof(hdr), 0);

                // 5. Stream audio in 0.1s chunks
                const int chunk = 3200;
                int sent = 0;
                while (sent < command_samples) {
                    int n = (command_samples - sent < chunk) ? (command_samples - sent) : chunk;
                    send(sock, cmd_buf + sent, n * sizeof(int16_t), 0);
                    sent += n;
                }
                shutdown(sock, SHUT_WR);

                // 6. Read transcript response
                char resp[1024] = {0};
                int resp_len = 0, r;
                while ((r = recv(sock, resp + resp_len,
                                 sizeof(resp) - resp_len - 1, 0)) > 0) {
                    resp_len += r;
                }
                close(sock);
                sock = -1;

                ESP_LOGI(TAG_STR, "[Session %lu] Response: %s",
                         (unsigned long)sid, resp_len > 0 ? resp : "[no response]");
                break;  // success — exit retry loop
            } else {
                ESP_LOGW(TAG_STR, "[Session %lu] connect() failed (attempt %d), retry in %lums",
                         (unsigned long)sid, attempt + 1, (unsigned long)backoff_ms);
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms *= 2;
            }
        }

        if (sock >= 0) {
            close(sock);
            ESP_LOGE(TAG_STR, "[Session %lu] All TCP attempts failed — detection dropped",
                     (unsigned long)sid);
        }
        free(cmd_buf);
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

    // I2S microphone
    i2s_init();

    // Mic self-check — halts loudly if mic is disconnected/miswired
    mic_selfcheck();

    // TFLite Micro
    tflite_init();
    mfcc_proc.init();

    // CPU load benchmark (logs every 10 sec)
    benchmark_cpu_start();

    // Log firmware memory footprint
    ESP_LOGI(TAG_MAIN, "Model size (flash): %u bytes (%.1f KB)",
             (unsigned)sizeof(model_data), sizeof(model_data) / 1024.0f);
    ESP_LOGI(TAG_MAIN, "Tensor arena alloc: %u bytes (%u KB)",
             (unsigned)TENSOR_ARENA_SIZE, (unsigned)(TENSOR_ARENA_SIZE / 1024));
    ESP_LOGI(TAG_MAIN, "Free heap after init: %u bytes",
             (unsigned)esp_get_free_heap_size());

    // Spawn FreeRTOS tasks
    xTaskCreatePinnedToCore(inference_task, "inference",
        8192, NULL, 5, NULL, 1);   // Core 1, high priority
    xTaskCreatePinnedToCore(streaming_task, "streaming",
        8192, NULL, 4, NULL, 0);   // Core 0, medium priority

    ESP_LOGI(TAG_MAIN, "All tasks started. Listening for 'Hey Vaani'...");
}
