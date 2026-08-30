// main.cpp — Hey Vaani ESP32 Real Firmware (ESP-IDF)
//
// Architecture: Two clearly separated FreeRTOS tasks on two cores:
//
//   ┌─────────────────────────────────────────────────────────┐
//   │  CORE 1: inference_task  (always-on, low-power loop)   │
//   │  • I2S audio capture from INMP441                       │
//   │  • Sliding-window MFCC every 30ms                       │
//   │  • TFLite Micro DS-CNN inference                        │
//   │  • Idle CPU target: < 10%                               │
//   └──────────────────────┬──────────────────────────────────┘
//                          │ keyword detected (prob ≥ 0.85)
//                          │ logs keyword_end_timestamp (NTP ms)
//                          ▼
//   ┌─────────────────────────────────────────────────────────┐
//   │  CORE 0: streaming_task  (triggered, high-bandwidth)    │
//   │  • Opens WebSocket to cloud_server/asr_server.py        │
//   │  • Streams raw PCM audio with HVP1 header               │
//   │  • Receives Vosk transcript JSON                         │
//   │  • Logs latency delta for evaluation                     │
//   └─────────────────────────────────────────────────────────┘
//
// INMP441 Wiring:
//   BCLK  → GPIO 26
//   WS    → GPIO 25
//   SD    → GPIO 34  (input only)
//   VDD   → 3.3V
//   GND   → GND
//   L/R   → GND  (selects left channel = mono)

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

// TFLite Micro headers
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model_data.h"    // 46.9 KB INT8 TFLite model as C array
#include "mfcc.h"          // MFCC matching train_model.py exactly
#include "benchmark_cpu.h" // CPU load monitor

// ─── User Config (set via menuconfig or edit here) ────────────────────────
#ifndef CONFIG_WIFI_SSID
#define CONFIG_WIFI_SSID       "YourWiFiName"
#endif
#ifndef CONFIG_WIFI_PASSWORD
#define CONFIG_WIFI_PASSWORD   "YourWiFiPass"
#endif
#ifndef CONFIG_SERVER_IP
#define CONFIG_SERVER_IP       "192.168.1.100"   // Cloud server IP
#endif
#ifndef CONFIG_SERVER_PORT
#define CONFIG_SERVER_PORT     5000
#endif

// ─── I2S / Audio Configuration ────────────────────────────────────────────
#define I2S_PORT            I2S_NUM_0
#define I2S_BCLK_PIN        26
#define I2S_WS_PIN          25
#define I2S_DATA_PIN        34
#define I2S_SAMPLE_RATE     16000
#define I2S_DMA_BUF_COUNT   8
#define I2S_DMA_BUF_LEN     512

// ─── Keyword Detection Configuration ──────────────────────────────────────
#define DETECT_THRESHOLD    0.85f   // Probability above this = "Hey Vaani"
#define SLIDE_STEP_MS       30      // Run inference every 30ms
#define COMMAND_DURATION_MS 2000    // Capture 2s of command audio after keyword
#define AUDIO_BUFFER_SAMPLES 16000  // 1 second at 16kHz

// ─── Protocol (must match asr_server.py) ──────────────────────────────────
#define MAGIC_NUMBER        0x48565031  // "HVP1"
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t audio_len;    // 0 = streaming mode
    uint32_t session_id;
} hvp1_header_t;

// ─── Globals ───────────────────────────────────────────────────────────────
static const char* TAG_INF  = "INFERENCE";
static const char* TAG_STR  = "STREAM";
static const char* TAG_MAIN = "MAIN";

// Ring buffer: 1 second of int16 PCM audio
static int16_t  audio_ring[AUDIO_BUFFER_SAMPLES];
static int      ring_write_pos = 0;
static SemaphoreHandle_t ring_mutex;

// Detection signal: inference_task → streaming_task
static QueueHandle_t detect_queue;  // sends: uint64_t keyword_end_ms

// Session counter
static volatile uint32_t session_id = 0;

// TFLite Micro arena (tensor memory on heap)
// ~50 KB as measured — set slightly above for safety
static uint8_t  tflite_arena[60 * 1024];

// NTP time in ms since boot (used for latency logging)
static volatile int64_t ntp_offset_ms = 0;  // set after SNTP sync

// ─── TFLite Micro setup ───────────────────────────────────────────────────
static tflite::MicroMutableOpResolver<8> resolver;
static tflite::MicroInterpreter*         interpreter = nullptr;
static TfLiteTensor*                     input_tensor  = nullptr;
static TfLiteTensor*                     output_tensor = nullptr;

static void tflite_init() {
    const tflite::Model* model = tflite::GetModel(model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG_INF, "TFLite schema version mismatch!");
        return;
    }

    // Register only ops actually used by the DS-CNN model
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddBatchMatMul();
    resolver.AddRelu();
    resolver.AddSoftmax();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddMean();  // GlobalAveragePooling2D uses reduce_mean

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tflite_arena, sizeof(tflite_arena)
    );
    interpreter = &static_interpreter;

    TfLiteStatus status = interpreter->AllocateTensors();
    if (status != kTfLiteOk) {
        ESP_LOGE(TAG_INF, "AllocateTensors() failed");
        return;
    }

    input_tensor  = interpreter->input(0);
    output_tensor = interpreter->output(0);

    // Log actual arena usage for the RAM footprint metric
    size_t used = interpreter->arena_used_bytes();
    ESP_LOGI(TAG_INF, "TFLite arena used: %u bytes (%.1f KB)", (unsigned)used, used / 1024.0f);
    ESP_LOGI(TAG_INF, "Free heap after TFLite init: %u bytes", (unsigned)esp_get_free_heap_size());
}

// ─── I2S Initialisation (INMP441) ────────────────────────────────────────
static i2s_chan_handle_t i2s_rx_chan;

static void i2s_init() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = I2S_DMA_BUF_LEN;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws   = (gpio_num_t)I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_DATA_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };
    // INMP441 is a left-channel device (L/R pin tied to GND)
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));
    ESP_LOGI(TAG_MAIN, "I2S INMP441 initialized: %d Hz mono", I2S_SAMPLE_RATE);
}

// ─── WiFi Initialisation ──────────────────────────────────────────────────
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init() {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid,     CONFIG_WIFI_SSID,     sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG_MAIN, "WiFi connected to: %s", CONFIG_WIFI_SSID);
}

// ─── NTP Time Sync ────────────────────────────────────────────────────────
static void ntp_sync() {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    // Wait up to 10 seconds for sync
    int retries = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && retries++ < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        ESP_LOGI(TAG_MAIN, "NTP synced — clocks aligned with cloud server for latency measurement");
    } else {
        ESP_LOGW(TAG_MAIN, "NTP sync failed — latency measurement will use relative timestamps");
    }
}

// Helper: milliseconds since epoch (NTP-synced)
static int64_t now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

// ─── TASK A: Inference Loop (CORE 1, always-on) ───────────────────────────
// This is the low-power continuous listening loop.
// Target: < 10% CPU utilization while idling.
static MFCCProcessor mfcc_proc;
static float mfcc_output[MFCC_OUTPUT_SIZE];  // [49 * 13]

static void inference_task(void* arg) {
    ESP_LOGI(TAG_INF, "Inference task started on core %d", xPortGetCoreID());
    ESP_LOGI(TAG_INF, "Threshold: %.2f | Slide step: %d ms", DETECT_THRESHOLD, SLIDE_STEP_MS);

    // Read buffer: one hop of new audio per cycle
    const int hop_samples = (I2S_SAMPLE_RATE * SLIDE_STEP_MS) / 1000; // 480 samples @ 30ms
    int32_t  i2s_raw[hop_samples];   // INMP441 sends 32-bit (MSB aligned)
    int16_t  hop_buf[hop_samples];

    uint32_t infer_count = 0;
    float    infer_total_ms = 0.0f;

    while (true) {
        // ── 1. Capture one hop of audio via I2S ──────────────────────────
        size_t bytes_read = 0;
        i2s_channel_read(i2s_rx_chan, i2s_raw, hop_samples * sizeof(int32_t),
                         &bytes_read, pdMS_TO_TICKS(100));

        // INMP441: 32-bit word, audio data in upper 18 bits → shift to 16-bit
        for (int i = 0; i < hop_samples; i++) {
            hop_buf[i] = (int16_t)(i2s_raw[i] >> 14);
        }

        // ── 2. Write into ring buffer ────────────────────────────────────
        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        for (int i = 0; i < hop_samples; i++) {
            audio_ring[ring_write_pos] = hop_buf[i];
            ring_write_pos = (ring_write_pos + 1) % AUDIO_BUFFER_SAMPLES;
        }
        xSemaphoreGive(ring_mutex);

        // ── 3. Lightweight VAD: skip inference if silent ─────────────────
        // Compute RMS energy of the new hop — cheap, no malloc
        int64_t sum_sq = 0;
        for (int i = 0; i < hop_samples; i++) sum_sq += (int64_t)hop_buf[i] * hop_buf[i];
        float rms = sqrtf((float)sum_sq / hop_samples) / 32768.0f;
        if (rms < 0.003f) {
            // Silence — skip MFCC + inference (major CPU saving)
            continue;
        }

        // ── 4. MFCC feature extraction ────────────────────────────────────
        // Linearise ring buffer → 1s contiguous audio window
        int16_t audio_window[AUDIO_BUFFER_SAMPLES];
        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        int start = ring_write_pos;  // oldest sample
        for (int i = 0; i < AUDIO_BUFFER_SAMPLES; i++) {
            audio_window[i] = audio_ring[(start + i) % AUDIO_BUFFER_SAMPLES];
        }
        xSemaphoreGive(ring_mutex);

        int64_t t0 = esp_timer_get_time();
        mfcc_proc.compute(audio_window, mfcc_output);
        int64_t t_mfcc = esp_timer_get_time();

        // ── 5. TFLite Micro inference ─────────────────────────────────────
        // Input tensor: INT8 [1, 49, 13, 1]
        // Quantise float MFCC → INT8 using model's scale/zero_point
        float in_scale    = input_tensor->params.scale;
        int32_t in_zp     = input_tensor->params.zero_point;
        int8_t* inp_data  = input_tensor->data.int8;

        for (int i = 0; i < MFCC_OUTPUT_SIZE; i++) {
            int32_t q = (int32_t)roundf(mfcc_output[i] / in_scale) + in_zp;
            if (q < -128) q = -128;
            if (q >  127) q =  127;
            inp_data[i] = (int8_t)q;
        }

        interpreter->Invoke();

        // Dequantise output: INT8 → float probabilities
        float out_scale = output_tensor->params.scale;
        int32_t out_zp  = output_tensor->params.zero_point;
        int8_t* out_data = output_tensor->data.int8;

        float not_kw_prob = (out_data[0] - out_zp) * out_scale;
        float kw_prob     = (out_data[1] - out_zp) * out_scale;

        int64_t t1 = esp_timer_get_time();
        float inference_ms = (t1 - t_mfcc) / 1000.0f;

        infer_count++;
        infer_total_ms += inference_ms;

        // Log every 100 inferences
        if (infer_count % 100 == 0) {
            ESP_LOGI(TAG_INF, "avg inference: %.1f ms | last kw_prob: %.3f",
                     infer_total_ms / infer_count, kw_prob);
        }

        // ── 6. Keyword detected? ──────────────────────────────────────────
        if (kw_prob >= DETECT_THRESHOLD) {
            int64_t keyword_end_ts = now_ms();   // NTP-synced timestamp ← METRIC

            ESP_LOGI(TAG_INF,
                     "🔔 HEY VAANI DETECTED! prob=%.3f  keyword_end_timestamp=%lld ms",
                     kw_prob, keyword_end_ts);

            // Signal streaming task — non-blocking send
            xQueueSend(detect_queue, &keyword_end_ts, 0);

            // Cool-down: don't detect again for 3 seconds to avoid repeat triggers
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

// ─── TASK B: Streaming Task (CORE 0, triggered only on detection) ─────────
// Wakes up when inference_task detects the keyword.
// Opens TCP socket to cloud, streams HVP1 header + raw PCM, logs latency.
static void streaming_task(void* arg) {
    ESP_LOGI(TAG_STR, "Streaming task started on core %d", xPortGetCoreID());

    int64_t keyword_end_ts;

    while (true) {
        // Block until keyword detected
        xQueueReceive(detect_queue, &keyword_end_ts, portMAX_DELAY);
        session_id++;
        ESP_LOGI(TAG_STR, "[Session %lu] Streaming triggered", (unsigned long)session_id);

        // ── 1. Capture COMMAND_DURATION_MS of audio ───────────────────────
        int command_samples = (I2S_SAMPLE_RATE * COMMAND_DURATION_MS) / 1000;
        int16_t* cmd_buf = (int16_t*)malloc(command_samples * sizeof(int16_t));
        if (!cmd_buf) {
            ESP_LOGE(TAG_STR, "OOM for command buffer");
            continue;
        }

        // Drain I2S DMA for COMMAND_DURATION_MS
        int32_t raw32[512];
        int captured = 0;
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(COMMAND_DURATION_MS + 200);
        while (captured < command_samples && xTaskGetTickCount() < deadline) {
            int want = (command_samples - captured);
            if (want > 512) want = 512;
            size_t bytes_read = 0;
            i2s_channel_read(i2s_rx_chan, raw32, want * sizeof(int32_t),
                             &bytes_read, pdMS_TO_TICKS(100));
            int got = bytes_read / sizeof(int32_t);
            for (int i = 0; i < got; i++) {
                cmd_buf[captured++] = (int16_t)(raw32[i] >> 14);
            }
        }

        // ── 2. Connect to cloud server ────────────────────────────────────
        struct sockaddr_in server_addr = {};
        server_addr.sin_family      = AF_INET;
        server_addr.sin_port        = htons(CONFIG_SERVER_PORT);
        inet_pton(AF_INET, CONFIG_SERVER_IP, &server_addr.sin_addr);

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG_STR, "socket() failed");
            free(cmd_buf);
            continue;
        }

        // TCP_NODELAY: disable Nagle's algorithm for low-latency streaming
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) != 0) {
            ESP_LOGE(TAG_STR, "connect() to %s:%d failed", CONFIG_SERVER_IP, CONFIG_SERVER_PORT);
            close(sock);
            free(cmd_buf);
            continue;
        }

        // ── 3. Send HVP1 header ────────────────────────────────────────
        hvp1_header_t hdr = {
            .magic       = MAGIC_NUMBER,
            .sample_rate = I2S_SAMPLE_RATE,
            .channels    = 1,
            .bits        = 16,
            .audio_len   = (uint32_t)(captured * sizeof(int16_t)),
            .session_id  = session_id
        };
        send(sock, &hdr, sizeof(hdr), 0);

        // ── 4. Stream audio in chunks (low framing overhead) ───────────
        const int chunk = 3200;  // 0.1s per chunk
        int sent = 0;
        while (sent < captured) {
            int n = (captured - sent < chunk) ? (captured - sent) : chunk;
            send(sock, cmd_buf + sent, n * sizeof(int16_t), 0);
            sent += n;
        }
        shutdown(sock, SHUT_WR);

        int64_t stream_sent_ts = now_ms();
        ESP_LOGI(TAG_STR, "[Session %lu] Audio sent (%d samples, %.1f sec)",
                 (unsigned long)session_id, captured, captured / (float)I2S_SAMPLE_RATE);

        // ── 5. Receive transcript from Vosk ASR server ────────────────
        char resp_buf[1024] = {0};
        int resp_len = 0;
        int r;
        while ((r = recv(sock, resp_buf + resp_len,
                         sizeof(resp_buf) - resp_len - 1, 0)) > 0) {
            resp_len += r;
        }
        close(sock);
        free(cmd_buf);

        int64_t cloud_resp_ts = now_ms();

        // ── 6. Log latency metric ──────────────────────────────────────
        // keyword_end_ts: when keyword was confirmed (NTP-synced)
        // The cloud logs "cloud_receive_timestamp" when first audio byte arrives.
        // This delta (computed post-hoc by latency_analyzer.py) = end-to-end latency.
        ESP_LOGI(TAG_STR,
                 "[Session %lu] keyword_end_timestamp=%lld | stream_sent=%lld | "
                 "round_trip=%lld ms | transcript: %s",
                 (unsigned long)session_id,
                 keyword_end_ts,
                 stream_sent_ts,
                 cloud_resp_ts - keyword_end_ts,
                 resp_len > 0 ? resp_buf : "[no response]");
    }
}

// ─── app_main ─────────────────────────────────────────────────────────────
extern "C" void app_main() {
    ESP_LOGI(TAG_MAIN, "=== Hey Vaani Edge Firmware ===");

    // NVS (required for WiFi)
    ESP_ERROR_CHECK(nvs_flash_init());

    // Shared resources
    ring_mutex   = xSemaphoreCreateMutex();
    detect_queue = xQueueCreate(4, sizeof(int64_t));

    // WiFi + NTP
    wifi_init();
    ntp_sync();

    // I2S microphone
    i2s_init();

    // TFLite Micro model
    tflite_init();
    mfcc_proc.init();

    // CPU benchmark logger (every 10 sec)
    benchmark_cpu_start();

    // Log flash/RAM footprint for metrics
    ESP_LOGI(TAG_MAIN, "Model size (flash): %u bytes (%.1f KB)",
             (unsigned)sizeof(model_data), sizeof(model_data) / 1024.0f);
    ESP_LOGI(TAG_MAIN, "Free heap before tasks: %u bytes",
             (unsigned)esp_get_free_heap_size());

    // ── Spawn the two tasks ───────────────────────────────────────────────
    // TASK A: Inference — pinned to core 1, high priority
    xTaskCreatePinnedToCore(
        inference_task, "inference",
        8192,  // stack
        NULL,
        5,     // priority (higher = higher priority)
        NULL,
        1      // core 1
    );

    // TASK B: Streaming — pinned to core 0, medium priority
    xTaskCreatePinnedToCore(
        streaming_task, "streaming",
        8192,
        NULL,
        4,
        NULL,
        0      // core 0
    );

    ESP_LOGI(TAG_MAIN, "All tasks started. Listening for 'Hey Vaani'...");
}
