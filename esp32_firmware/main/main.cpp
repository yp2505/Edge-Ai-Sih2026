// main.cpp — Hey Vaani ESP32 Edge KWS + Cloud ASR Streaming
//
// FIX LOG (2026-09-07 rev7 — INMP441 I2S + ST7735 OLED + LED + Buzzer):
//   All fixes from rev6 preserved plus:
//   1. ADC/MAX4466 removed; INMP441 I2S (i2s_std, 16 kHz, mono-left).
//      adc_global_init() + read_pcm_dma() gone.
//      i2s_global_init() + i2s_read_pcm() replace them.
//      Ring buffer, mutex, inference_task, streaming_task: UNCHANGED.
//   2. ST7735 SPI OLED via ESP-IDF SPI master (SPI3_HOST/VSPI, no Arduino lib).
//      display_task (Core 0, pri 1) polls g_disp_state every 100 ms — zero
//      impact on inference or streaming critical paths.
//   3. LED GPIO27: on at keyword confirm, off after transcription received.
//      watchdog_task also forces LED off if streaming gets stuck.
//   4. Buzzer GPIO14: 150 ms LEDC PWM beep (2.5 kHz) at keyword confirm,
//      implemented as a self-deleting beep_task (no delay in inference_task).
//
// HARDWARE WIRING:
//   INMP441:  SCK=GPIO26  WS=GPIO25   SD=GPIO22   L/R=GND (left channel)
//   ST7735:   DIN=GPIO23  CLK=GPIO18  CS=GPIO5    DC=GPIO2  RST=GPIO4  BL→3.3V
//   LED:      GPIO27 (through 220 Ω resistor to GND)
//   Buzzer:   GPIO14 (active or passive — LEDC PWM handles both types)
//
// DATA PIPELINE (unchanged — MUST MATCH TRAINING EXACTLY — mfcc.h):
//   SAMPLE_RATE=16000 Hz  N_FFT=512  HOP=320  N_MEL=40  N_MFCC=13  N_FRAMES=49
//   Mel 20–8000 Hz | power=|STFT| | DCT=Type-II norm | quant=tensor scale/zp
// ============================================================================

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
#include "driver/i2c.h"
#include "driver/gpio.h"
// ledc header removed — no buzzer hardware connected
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "esp_http_client.h"
#include "cJSON.h"            // ESP-IDF built-in JSON parser — no extra deps
#include "aes/esp_aes.h"      // Hardware-accelerated AES-128-CTR encryption
#include "esp_random.h"       // Hardware RNG for nonce generation

#include "model_data.h"
#include "mfcc.h"
#include "benchmark_cpu.h"
#include "wifi_provision.h"   // NVS-backed WiFi credentials + captive portal

// ─── AES-128-CTR Encryption ──────────────────────────────────────────────────────
// All audio and responses are encrypted with AES-128-CTR (no licence required).
// Key MUST exactly match AES_KEY in server.py / HV_AES_KEY env var.
// In production: move key to NVS (wifi_provision) so it is not in flash binary.
//
// Protocol:
//   [20-byte HVP1 header] [16-byte random nonce] [encrypted audio...]
//   [encrypted JSON response]
// Response nonce = audio_nonce with last byte XOR 0xFF (derived, not transmitted).
//
// mbedTLS AES-CTR context is initialised fresh per session — no global state.
static const uint8_t AES_KEY[16] = {
    // Hex: 48657956616e6e69534948323032362a  = "HeyVaaniSIH2026*"
    // MUST match _HV_AES_KEY_HEX in server.py
    0x48, 0x65, 0x79, 0x56, 0x61, 0x6E, 0x6E, 0x69,
    0x53, 0x49, 0x48, 0x32, 0x30, 0x32, 0x36, 0x2A
};

// ─── User Config ─────────────────────────────────────────────────────────────
// WiFi SSID, password and server IP are now stored in NVS (non-volatile flash)
// and loaded at boot by wifi_provision_init() into:
//   g_wifi_ssid / g_wifi_pass / g_server_ip   (declared in wifi_provision.h)
// On first boot (or after wifi_provision_clear()), the device starts the
// "HeyVaani-Setup" captive portal so credentials can be entered via browser.
//
// Only the server PORT remains hardcoded here — it never changes per-network.
#ifndef CONFIG_SERVER_PORT
#define CONFIG_SERVER_PORT     80
#endif

#ifndef ENABLE_AES
#define ENABLE_AES             0    // Set to 1 when ESP32↔server AES CTR is verified
#endif

// ─── I2S / INMP441 ───────────────────────────────────────────────────────────
#define I2S_SAMPLE_RATE    16000
#define I2S_PORT           I2S_NUM_0
#define I2S_SCK_PIN        GPIO_NUM_26   // BCLK  (INMP441 SCK)
#define I2S_WS_PIN         GPIO_NUM_25   // LRCLK (INMP441 WS)
#define I2S_SD_PIN         GPIO_NUM_22   // DATA  (INMP441 SD)  — wired to GPIO22

// ─── KWS / Inference ─────────────────────────────────────────────────────────
// DETECT_THRESHOLD: sigmoid decision boundary calibrated on the custom
// "Hey Vaani" dataset.  Model output: [1,1] sigmoid (0.0–1.0).
//   0.78 = good balance of TPR vs FPR for 5-speaker trained model.
//   Lower toward 0.60 only if real-world misses are unacceptable.
//   Raise toward 0.90 only if false triggers persist after retraining.
//
// DETECTION_HITS_REQUIRED=2: both consecutive inferences must exceed
// threshold before the trigger fires — halves false-positive rate
// at cost of ~30 ms extra latency (one extra hop).
static const float DETECT_THRESHOLD          = 0.70f;   // Raise to reduce false triggers — only real "Hey Vaani" should pass
static const int   DETECTION_HITS_REQUIRED  = 1;        // Single frame trigger — model peaks in 1 frame during "Hey Vaani"
static const float MIN_SPEECH_RMS           = 0.030f;   // Gate out fan/AC noise
static const float NOISE_FLOOR_MULTIPLIER   = 2.5f;     // VAD gate: must be 2.5x above noise floor
static const int   NOISE_CALIBRATION_FRAMES = 50;
static const float NOISE_CAL_MAX_RMS        = 0.15f;

// ─── Streaming / VAD ─────────────────────────────────────────────────────────
// Command capture: after "How can I help you?", keep streaming long enough
// for the user to speak their command.  Minimum 2s before EOS eligible,
// then close after 1s of silence.  Max 15s hard cap.
#define SLIDE_STEP_MS           30
#define COMMAND_DURATION_MS     10000
#define COMMAND_MIN_DURATION_MS 2000       // 2s minimum before EOS — user needs time to speak
#define COMMAND_SILENCE_MS      1500       // 1.5s silence to end stream — give time between words
#define COOLDOWN_MS             3000       // 3s cooldown after each detection cycle
#define AUDIO_BUFFER_SAMPLES    16000
#define STREAM_WATCHDOG_MS      5000

// ─── HVP1 Protocol ───────────────────────────────────────────────────────────
#define MAGIC_NUMBER 0x48565031u
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    uint32_t audio_len;
    uint32_t kw_to_connect_ms;
} hvp1_header_t;

// ─── LED / Buzzer ────────────────────────────────────────────────────────────
#define LED_PIN              GPIO_NUM_27
// BUZZER not connected — PWM/LEDC removed

// ─── SSD1306 I2C OLED (0.96", 128×64, monochrome) ───────────────────────────
// GPIO21=SDA (default I2C), GPIO19=SCL (avoids GPIO33 used by INMP441 SD).
#define OLED_SDA_PIN    GPIO_NUM_21
#define OLED_SCL_PIN    GPIO_NUM_19
#define OLED_I2C_PORT   I2C_NUM_0
#define OLED_I2C_HZ     400000           // 400 kHz Fast Mode
#define OLED_ADDR       0x3C             // 0x3C most common; try 0x3D if blank
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
// SSD1306 framebuffer: 128×64 / 8 = 1024 bytes (1 bit per pixel)
#define OLED_BUF_BYTES  (OLED_WIDTH * OLED_HEIGHT / 8)

// ─── WiFi ────────────────────────────────────────────────────────────────────
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10

static const char* TAG_INF  = "INFERENCE";
static const char* TAG_STR  = "STREAM";
static const char* TAG_MAIN = "MAIN";
static const char* TAG_WIFI = "WIFI";
static const char* TAG_MIC  = "MIC";
static const char* TAG_DISP = "DISPLAY";

static volatile int  wifi_retry_count = 0;
static volatile bool wifi_connected   = false;

// ─── Streaming state — always via set_streaming_active() ─────────────────────
static volatile bool    streaming_active        = false;
static volatile int64_t streaming_active_set_us = 0;

// ─── Telemetry ───────────────────────────────────────────────────────────────
static volatile uint32_t telemetry_inference_count    = 0;
static volatile float    telemetry_inference_ms       = 0.0f;
static volatile float    telemetry_keyword_confidence = 0.0f;
static volatile float    telemetry_mic_rms            = 0.0f;
static volatile float    telemetry_noise_floor_rms    = 0.0f;
static volatile float    telemetry_last_infer_ms      = 0.0f;

// ─── Audio ring buffer ───────────────────────────────────────────────────────
static int16_t           audio_ring[AUDIO_BUFFER_SAMPLES];
static int16_t           audio_window[AUDIO_BUFFER_SAMPLES];
static volatile int      ring_write_pos     = 0;
static volatile int      valid_ring_samples = 0;
static SemaphoreHandle_t ring_mutex;

static QueueHandle_t     detect_queue;
static volatile uint32_t session_id = 0;
static volatile int      g_stream_start_pos = 0;  // ring pos saved at detection time

// ─── TFLite ──────────────────────────────────────────────────────────────────
static const size_t TENSOR_ARENA_SIZE = 32 * 1024;
static uint8_t tflite_arena[32 * 1024];

static tflite::MicroMutableOpResolver<12> resolver;
static tflite::MicroInterpreter*          interpreter   = nullptr;
static TfLiteTensor*                      input_tensor  = nullptr;
static TfLiteTensor*                      output_tensor = nullptr;

// ─── I2S handle ──────────────────────────────────────────────────────────────
static i2s_chan_handle_t g_i2s_rx = NULL;

// ─── MFCC ────────────────────────────────────────────────────────────────────
static MFCCProcessor mfcc_proc;
static float         mfcc_output[MFCC_OUTPUT_SIZE];

// ─── OLED display state (written at trigger/stream/receive sites) ─────────────
typedef enum {
    DISP_BOOTING = 0,
    DISP_PROVISIONING,
    DISP_LISTENING,
    DISP_DETECTED,
    DISP_STREAMING,
    DISP_TRANSCRIBED,
    DISP_PROMPT,          // "How can I help you?" — command-capture mode
} disp_state_t;

static volatile disp_state_t g_disp_state = DISP_BOOTING;

// ─── System state machine ────────────────────────────────────────────────────
// Gates inference_task: only runs in SYS_LISTENING.
typedef enum {
    SYS_LISTENING = 0,     // normal KWS mode — inference_task runs
    SYS_CAPTURE_COMMAND,   // keyword detected — streaming_task active, inference gated
} SystemState;

static volatile SystemState g_sys_state = SYS_LISTENING;
static int64_t g_cooldown_end_us = 0;  // timestamp when cooldown expires (0 = no cooldown)
// Transcription result — written by streaming_task BEFORE setting DISP_TRANSCRIBED.
// MUST be accessed under disp_mutex to prevent torn reads in display_task.
static char              g_disp_text[256] = "";
static SemaphoreHandle_t disp_mutex;            // guards g_disp_text
// SSD1306 framebuffer — modified in DRAM, flushed to display each redraw
// I2C: SDA=GPIO21, SCL=GPIO19 (GPIO33 reserved for INMP441 SD).
static uint8_t g_oled_fb[OLED_BUF_BYTES];

// ─── Minimal 5×8 fixed font (ASCII 32–126, 95 entries × 5 col-bytes) ─────────
// Each byte = one column, bit 0 = top row.  Public domain.
static const uint8_t FONT5X8[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // '!'
    {0x00,0x07,0x00,0x07,0x00}, // '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // '$'
    {0x23,0x13,0x08,0x64,0x62}, // '%'
    {0x36,0x49,0x55,0x22,0x50}, // '&'
    {0x00,0x05,0x03,0x00,0x00}, // '\''
    {0x00,0x1C,0x22,0x41,0x00}, // '('
    {0x00,0x41,0x22,0x1C,0x00}, // ')'
    {0x14,0x08,0x3E,0x08,0x14}, // '*'
    {0x08,0x08,0x3E,0x08,0x08}, // '+'
    {0x00,0x50,0x30,0x00,0x00}, // ','
    {0x08,0x08,0x08,0x08,0x08}, // '-'
    {0x00,0x60,0x60,0x00,0x00}, // '.'
    {0x20,0x10,0x08,0x04,0x02}, // '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // '0'
    {0x00,0x42,0x7F,0x40,0x00}, // '1'
    {0x42,0x61,0x51,0x49,0x46}, // '2'
    {0x21,0x41,0x45,0x4B,0x31}, // '3'
    {0x18,0x14,0x12,0x7F,0x10}, // '4'
    {0x27,0x45,0x45,0x45,0x39}, // '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // '6'
    {0x01,0x71,0x09,0x05,0x03}, // '7'
    {0x36,0x49,0x49,0x49,0x36}, // '8'
    {0x06,0x49,0x49,0x29,0x1E}, // '9'
    {0x00,0x36,0x36,0x00,0x00}, // ':'
    {0x00,0x56,0x36,0x00,0x00}, // ';'
    {0x08,0x14,0x22,0x41,0x00}, // '<'
    {0x14,0x14,0x14,0x14,0x14}, // '='
    {0x00,0x41,0x22,0x14,0x08}, // '>'
    {0x02,0x01,0x51,0x09,0x06}, // '?'
    {0x32,0x49,0x79,0x41,0x3E}, // '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // '['
    {0x02,0x04,0x08,0x10,0x20}, // '\'
    {0x00,0x41,0x41,0x7F,0x00}, // ']'
    {0x04,0x02,0x01,0x02,0x04}, // '^'
    {0x40,0x40,0x40,0x40,0x40}, // '_'
    {0x00,0x01,0x02,0x04,0x00}, // '`'
    {0x20,0x54,0x54,0x54,0x78}, // 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 't'
    {0x3C,0x40,0x40,0x40,0x3C}, // 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, // 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 'z'
    {0x00,0x08,0x36,0x41,0x00}, // '{'
    {0x00,0x00,0x7F,0x00,0x00}, // '|'
    {0x00,0x41,0x36,0x08,0x00}, // '}'
    {0x10,0x08,0x08,0x10,0x08}, // '~'
};

// ============================================================================
// set_streaming_active — SINGLE POINT OF MUTATION for streaming_active.
// UNCHANGED from rev6.
// ============================================================================
static void set_streaming_active(bool value, const char* reason) {
    if (value) {
        streaming_active        = true;
        streaming_active_set_us = esp_timer_get_time();
    } else {
        streaming_active        = false;
        streaming_active_set_us = 0;
    }
}

// ─── TFLite Init — UNCHANGED from rev6 ───────────────────────────────────────
static void tflite_init() {
    const tflite::Model* model = tflite::GetModel(g_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG_INF, "TFLite schema version mismatch!"); esp_restart();
    }
    ESP_LOGI(TAG_INF, "Model size: %u bytes (%.1f KB)", g_model_data_len, g_model_data_len / 1024.0f);
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddBatchMatMul();
    resolver.AddRelu();
    resolver.AddSoftmax();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddMean();
    resolver.AddLogistic();

    static tflite::MicroInterpreter static_interpreter(model, resolver, tflite_arena, TENSOR_ARENA_SIZE);
    interpreter = &static_interpreter;
    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG_INF, "AllocateTensors() failed!"); esp_restart();
    }
    input_tensor  = interpreter->input(0);
    output_tensor = interpreter->output(0);
    size_t used   = interpreter->arena_used_bytes();
    printf("[INIT] TFLite: model=%uB arena=%u/%u\n",
           (unsigned)g_model_data_len, (unsigned)used, (unsigned)TENSOR_ARENA_SIZE);
    fflush(stdout);
}

// ─── I2S Init: INMP441 16 kHz, 16-bit, mono-left ─────────────────────────────
static void i2s_global_init() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // zero DMA buffer on underrun
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &g_i2s_rx));

    // FIX: Use STEREO mode — ESP-IDF 5/6 INMP441 MONO mode causes interleaved
    // stereo pairs in the DMA buffer (L=mic, R=zero), which halves effective RMS.
    // We read STEREO and de-interleave in i2s_read_pcm() below.
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)(-1),
            .bclk = I2S_SCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = (gpio_num_t)(-1),
            .din  = I2S_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // Both slots enabled; INMP441 L/R=GND → LEFT slot has mic data, RIGHT=0
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(g_i2s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(g_i2s_rx));
    ESP_LOGI(TAG_MAIN, "INMP441 I2S ready: %d Hz  16-bit  stereo-deinterleave  "
             "SCK=GPIO%d  WS=GPIO%d  SD=GPIO%d",
             I2S_SAMPLE_RATE, I2S_SCK_PIN, I2S_WS_PIN, I2S_SD_PIN);
}

// ─── I2S PCM read — STEREO de-interleave for INMP441 ─────────────────────────
// INMP441 outputs 24-bit left-justified in 32-bit frame.  With I2S_SLOT_BIT_WIDTH_32BIT,
// each sample is 4 bytes (int32).  We read int32 stereo, de-interleave LEFT channel,
// and right-shift by 16 to get int16 audio.
static int i2s_read_pcm(int16_t* out_buf, int out_count) {
    // Read stereo frames as int32 (4 bytes per sample)
    static int32_t stereo_buf32[960 * 2];  // 32-bit stereo buffer
    int read_stereo = (out_count <= 960) ? out_count : 960;
    size_t bytes_wanted = (size_t)read_stereo * 2 * sizeof(int32_t);  // stereo int32
    size_t bytes_read = 0;
    esp_err_t e = i2s_channel_read(g_i2s_rx,
                                   stereo_buf32,
                                   bytes_wanted,
                                   &bytes_read,
                                   pdMS_TO_TICKS(200));
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) return -1;
    int stereo_samples = (int)(bytes_read / sizeof(int32_t));  // L+R interleaved int32
    int mono_out = 0;
    // Extract LEFT channel (even indices = mic data), shift 24-bit → 16-bit
    for (int i = 0; i + 1 < stereo_samples && mono_out < out_count; i += 2) {
        int32_t s32 = stereo_buf32[i];          // 24-bit left-justified in 32 bits
        out_buf[mono_out++] = (int16_t)(s32 >> 16);  // top 16 bits of 24-bit = audio
    }
    return mono_out;
}

// ============================================================================
// SSD1306 I2C OLED — minimal framebuffer driver (ESP-IDF I2C master)
// 128×64 monochrome.  All functions called only from display_task or app_main.
// I2C: SDA=GPIO21, SCL=GPIO19 (GPIO22 reserved for INMP441 SD).
// ============================================================================

// Send one byte to SSD1306 as a command (Co=0, D/C#=0)
static esp_err_t ssd_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};   // 0x00 = control byte: Co=0, D/C#=0
    return i2c_master_write_to_device(OLED_I2C_PORT, OLED_ADDR,
                                      buf, 2, pdMS_TO_TICKS(10));
}

// Flush the full 1024-byte framebuffer to SSD1306 GDDRAM
static void ssd_flush() {
    // Set column 0..127, page 0..7
    ssd_cmd(0x21); ssd_cmd(0); ssd_cmd(127);   // column address
    ssd_cmd(0x22); ssd_cmd(0); ssd_cmd(7);     // page address
    // Data transfer: control byte 0x40 = Co=0, D/C#=1 (data)
    // i2c_master_write_to_device needs a single contiguous buffer, so we
    // prepend the 0x40 control byte to g_oled_fb via a local header trick.
    static uint8_t txbuf[1 + OLED_BUF_BYTES];
    txbuf[0] = 0x40;
    memcpy(txbuf + 1, g_oled_fb, OLED_BUF_BYTES);
    i2c_master_write_to_device(OLED_I2C_PORT, OLED_ADDR,
                               txbuf, sizeof(txbuf), pdMS_TO_TICKS(50));
}

// Set or clear a single pixel in the framebuffer (does NOT flush)
static inline void ssd_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    int byte_idx = x + (y / 8) * OLED_WIDTH;
    uint8_t bit  = (uint8_t)(1 << (y & 7));
    if (on) g_oled_fb[byte_idx] |=  bit;
    else    g_oled_fb[byte_idx] &= ~bit;
}

// Fill entire framebuffer (0x00=black, 0xFF=white), then flush
static void ssd_clear(uint8_t fill) {
    memset(g_oled_fb, fill, OLED_BUF_BYTES);
}

// Draw one 5×8 glyph into framebuffer at pixel (x, y).  scale: 1 or 2.
static void ssd_draw_char(int x, int y, char c, bool on, uint8_t scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* glyph = FONT5X8[(uint8_t)c - 32];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 8; row++) {
            bool bit = (glyph[col] >> row) & 1;
            for (int sc = 0; sc < scale; sc++)
                for (int sr = 0; sr < scale; sr++)
                    ssd_pixel(x + col*scale + sc, y + row*scale + sr, bit && on);
        }
    }
}

// Draw a null-terminated string; returns x position after last char
static int ssd_draw_str(int x, int y, const char* s, bool on, uint8_t scale) {
    int cw = 6 * scale;   // 5px glyph + 1px gap
    while (*s && x + cw <= OLED_WIDTH) {
        ssd_draw_char(x, y, *s++, on, scale);
        x += cw;
    }
    return x;
}

// Draw a horizontal line into framebuffer
static void ssd_hline(int x0, int x1, int y, bool on) {
    for (int x = x0; x <= x1; x++) ssd_pixel(x, y, on);
}

// Render full status screen to framebuffer then flush to display.
// SSD1306 is monochrome 128×64 — layout designed for this resolution:
//   Row 0..7   : "HEY VAANI" header (scale=1, 8px tall)
//   Row 8      : horizontal divider
//   Row 9..24  : state label (scale=2, 16px tall, inverted for emphasis)
//   Row 25     : divider
//   Row 26..63 : transcription text (scale=1, 8px per row, up to 4 lines)
static void oled_show_status(disp_state_t state, const char* text) {
    // Clear framebuffer
    ssd_clear(0x00);

    // Header
    const char* hdr = "HEY VAANI";
    int hdr_w = (int)strlen(hdr) * 6;   // scale=1
    int hdr_x = (OLED_WIDTH - hdr_w) / 2;
    ssd_draw_str(hdr_x, 0, hdr, true, 1);

    // Divider
    ssd_hline(0, OLED_WIDTH - 1, 9, true);

    // State label (scale=2 → 16px tall)
    const char* label;
    switch (state) {
        case DISP_BOOTING:      label = "Booting";   break;
        case DISP_PROVISIONING: label = "Setup WiFi"; break;
        case DISP_LISTENING:    label = "Listening"; break;
        case DISP_DETECTED:     label = "Detected!"; break;
        case DISP_STREAMING:    label = "Streaming"; break;
        case DISP_TRANSCRIBED:  label = "Received";  break;
        case DISP_PROMPT:       label = "I'm listening"; break;
        default:                label = "Unknown";   break;
    }
    int lbl_w = (int)strlen(label) * 12;   // scale=2 → 12px/char
    int lbl_x = (OLED_WIDTH - lbl_w) / 2;
    if (lbl_x < 0) lbl_x = 0;

    // Draw state label in clean white text (no solid white background block)
    ssd_draw_str(lbl_x, 12, label, true, 2);

    // Second divider
    ssd_hline(0, OLED_WIDTH - 1, 30, true);

    // Transcription / detail text (scale=1, y=33, 21 chars/line, up to 3 lines)
    // Word-wrap: only break between words, never mid-character.
    if (text && text[0]) {
        const int COLS = OLED_WIDTH / 6;   // 21 chars per line
        int y_pos = 33;
        const char* p = text;
        while (*p && y_pos <= OLED_HEIGHT - 8) {
            char line[22]; int n = 0;
            // Scan ahead to find the longest word-boundary-respecting prefix
            const char* scan = p;
            int last_space_n = 0;           // length at last seen space+1
            const char* last_space_p = p;   // pointer after last seen space
            while (*scan && n < COLS) {
                if (*scan == ' ') {
                    last_space_n = n + 1;   // include the space on this line
                    last_space_p = scan + 1;
                }
                line[n++] = *scan++;
            }
            if (*scan && last_space_n > 0) {
                // More text remains and we saw a space: cut at word boundary
                n = last_space_n;
                p = last_space_p;
            } else {
                // Remaining text fits OR no space seen (single long word): keep as-is
                p = scan;
            }
            line[n] = '\0';
            // Trim trailing space if we cut at boundary
            while (n > 0 && line[n-1] == ' ') line[--n] = '\0';
            ssd_draw_str(0, y_pos, line, true, 1);
            y_pos += 9;
            // Skip leading space on next line
            if (*p == ' ') p++;
        }
    }

    // Push framebuffer to display
    ssd_flush();
}

// SSD1306 hardware + I2C bus init
static void ssd1306_init() {
    // I2C bus configuration
    i2c_config_t conf = {};
    conf.mode             = I2C_MODE_MASTER;
    conf.sda_io_num       = OLED_SDA_PIN;
    conf.scl_io_num       = OLED_SCL_PIN;
    conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = OLED_I2C_HZ;
    ESP_ERROR_CHECK(i2c_param_config(OLED_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    vTaskDelay(pdMS_TO_TICKS(50));  // SSD1306 needs >1 ms after VCC stable
    // I2C scanner removed — it blocked Core 0 for ~1.26s (126 addrs × 10ms timeout)
    // causing IWDT crash. OLED address is hardcoded as 0x3C.

    // SSD1306 init sequence (works for all common 128×64 modules)
    static const uint8_t init_cmds[] = {
        0xAE,         // display OFF
        0xD5, 0x80,   // set display clock divide / oscillator frequency
        0xA8, 0x3F,   // set multiplex ratio: 63 (for 64 rows)
        0xD3, 0x00,   // set display offset: 0
        0x40,         // set start line: 0
        0x8D, 0x14,   // charge pump: enable
        0x20, 0x00,   // memory addressing mode: horizontal
        0xA1,         // segment remap: col 127 mapped to SEG0
        0xC8,         // COM scan direction: remapped (top-to-bottom)
        0xDA, 0x12,   // COM pins hardware config: alternative
        0x81, 0xCF,   // contrast: 207
        0xD9, 0xF1,   // pre-charge period
        0xDB, 0x40,   // VCOMH deselect level
        0xA4,         // entire display on: follow RAM
        0xA6,         // normal display (not inverted)
        0xAF,         // display ON
    };
    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        if (ssd_cmd(init_cmds[i]) != ESP_OK) {
            ESP_LOGE(TAG_DISP, "SSD1306 init cmd 0x%02X failed — check SDA=GPIO%d SCL=GPIO%d addr=0x%02X",
                     init_cmds[i], OLED_SDA_PIN, OLED_SCL_PIN, OLED_ADDR);
        }
    }

    ESP_LOGI(TAG_DISP, "SSD1306 ready: %dx%d  I2C  addr=0x%02X  SDA=GPIO%d  SCL=GPIO%d",
             OLED_WIDTH, OLED_HEIGHT, OLED_ADDR, OLED_SDA_PIN, OLED_SCL_PIN);

    // Show "Booting..." immediately, before display_task is running
    oled_show_status(DISP_BOOTING, "");
}

extern "C" void display_show_provisioning(void) {
    oled_show_status(DISP_PROVISIONING, "AP: HeyVaani-Setup\nIP: 192.168.4.1");
}

// ─── Mic Self-Check (I2S) ────────────────────────────────────────────────────
// Reads 0.5 s of audio, de-interleaves L/R channels, and logs diagnostics.
// Expected RMS in silence: ~0.001–0.010 (INMP441 noise floor ≈ −26 dBFS).
//
// DIAGNOSTIC GUIDE — interpret [MIC] log output:
//   L_rms > 0.001, R_rms ≈ 0    → Mic working, correct wiring (L/R=GND)
//   L_rms ≈ 0,    R_rms > 0.001 → L/R PIN CONNECTED TO VDD — swap to GND!
//   L_rms ≈ 0,    R_rms ≈ 0     → Hardware issue: check wiring/power/defective mic
//   No data (got=0)              → I2S not reading: check SCK/WS/SD wiring
//
// This function is NON-FATAL — does NOT call esp_restart().
static void mic_selfcheck() {
    static int16_t buf[8000];   // 0.5 s stereo @ 16 kHz — static → DRAM, DMA-safe
    ESP_LOGI(TAG_MIC, "Mic self-check: reading 0.5s of I2S stereo audio...");
    size_t bytes_read = 0;
    esp_err_t e = i2s_channel_read(g_i2s_rx, buf, sizeof(buf),
                                   &bytes_read, pdMS_TO_TICKS(1000));
    int got = (e == ESP_OK || e == ESP_ERR_TIMEOUT)
              ? (int)(bytes_read / sizeof(int16_t)) : 0;
    if (got <= 0) {
        ESP_LOGE(TAG_MIC, "I2S self-check: NO DATA (err=0x%x, bytes=%d)",
                 (unsigned)e, (int)bytes_read);
        ESP_LOGE(TAG_MIC, "  → Check wiring: SCK=GPIO26  WS=GPIO25  SD=GPIO22");
        ESP_LOGE(TAG_MIC, "  → Check INMP441 power: VCC=3.3V, GND=GND");
        ESP_LOGW(TAG_MIC, "  Continuing anyway — check [ENERGY] log after boot.");
        return;
    }

    // De-interleave stereo: even indices = LEFT (mic if L/R=GND), odd = RIGHT
    int64_t sum_sq_l = 0, sum_sq_r = 0;
    int mono_count = got / 2;   // number of L-R pairs
    for (int i = 0; i + 1 < got; i += 2) {
        sum_sq_l += (int64_t)buf[i]     * buf[i];      // LEFT
        sum_sq_r += (int64_t)buf[i + 1] * buf[i + 1];  // RIGHT
    }
    float rms_l = mono_count > 0 ? sqrtf((float)sum_sq_l / mono_count) / 32768.0f : 0.0f;
    float rms_r = mono_count > 0 ? sqrtf((float)sum_sq_r / mono_count) / 32768.0f : 0.0f;

    // Also compute raw (non-deinterleaved) RMS for backward compat
    int64_t sum_sq_all = 0;
    for (int i = 0; i < got; i++) sum_sq_all += (int64_t)buf[i] * buf[i];
    float rms_all = got > 0 ? sqrtf((float)sum_sq_all / got) / 32768.0f : 0.0f;

    printf("[MIC] INMP441 stereo check (%.2fs, %d stereo pairs):\n",
           (float)got / I2S_SAMPLE_RATE / 2.0f, mono_count);
    printf("[MIC]   L_rms=%.5f  R_rms=%.5f  combined=%.5f\n",
           (double)rms_l, (double)rms_r, (double)rms_all);

    // Show first 8 raw samples (4 L-R pairs) for debugging
    printf("[MIC]   raw[0..7]:");
    for (int i = 0; i < 8 && i < got; i++) printf(" %d", (int)buf[i]);
    printf("\n");
    fflush(stdout);

    // Diagnostic verdict
    if (rms_l > 0.001f && rms_r < 0.001f) {
        ESP_LOGI(TAG_MIC, "Mic OK — data on LEFT channel (L/R=GND correct)");
    } else if (rms_l < 0.001f && rms_r > 0.001f) {
        ESP_LOGE(TAG_MIC, "WRONG CHANNEL! Data is on RIGHT — L/R pin is wired to VDD!");
        ESP_LOGE(TAG_MIC, "  FIX: Connect INMP441 L/R pin to GND (not VCC)");
        ESP_LOGE(TAG_MIC, "  Current: L_rms=%.5f R_rms=%.5f (data on wrong side)", rms_l, rms_r);
    } else if (rms_l < 1e-6f && rms_r < 1e-6f) {
        ESP_LOGE(TAG_MIC, "MIC SILENT — no signal on either channel!");
        ESP_LOGE(TAG_MIC, "  Check: 1) INMP441 VCC=3.3V?  2) SCK/WS/SD wired correctly?");
        ESP_LOGE(TAG_MIC, "          3) L/R=GND?  4) Module defective?");
        ESP_LOGW(TAG_MIC, "  Continuing. Inference will likely get no triggers.");
    } else {
        ESP_LOGI(TAG_MIC, "Mic self-check PASSED (L=%.5f R=%.5f)", rms_l, rms_r);
    }
}

// ─── WiFi — UNCHANGED from rev6 ──────────────────────────────────────────────
static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (wifi_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect(); wifi_retry_count = wifi_retry_count + 1;
            ESP_LOGW(TAG_WIFI, "WiFi lost — retry %d/%d", wifi_retry_count, WIFI_MAX_RETRIES);
        } else xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0; wifi_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG_WIFI, "WiFi connected — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static void wifi_start() {
    if (wifi_connected) return;
    static bool wifi_infra_ready = false;
    if (!wifi_infra_ready) {
        wifi_event_group = xEventGroupCreate();
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
        wifi_infra_ready = true;
    } else {
        esp_wifi_stop();
    }
    wifi_config_t wcfg = {};
    // Use credentials loaded from NVS by wifi_provision_init() at boot.
    strlcpy((char*)wcfg.sta.ssid,     g_wifi_ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char*)wcfg.sta.password, g_wifi_pass, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  // allow open networks too
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    wifi_retry_count = 0;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_start();
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) ESP_LOGI(TAG_WIFI, "WiFi connected");
    else                           ESP_LOGW(TAG_WIFI, "WiFi not connected — streaming may fail");
}

static void wifi_stop() {
    if (!wifi_connected && wifi_retry_count == 0) return;
    esp_wifi_stop();
    wifi_connected = false; wifi_retry_count = 0;
    if (wifi_event_group) xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_LOGI(TAG_WIFI, "WiFi stopped");
}

// ============================================================================
// Forward declaration — beep_task is defined after streaming_task but called
// from inference_task which appears before it in the file.
// ============================================================================
// beep_task forward declaration removed — no buzzer

// ============================================================================
// TASK 0: audio_task — sole I2S consumer; writes to ring buffer.
// i2s_read_pcm() blocks ~30 ms/hop — provides natural pacing, no vTaskDelay.
// Bounded mutex wait (50 ms): logs and drops frame if ring_mutex held too long.
// ============================================================================
static void audio_task(void* arg) {
    const int HOP = I2S_SAMPLE_RATE * SLIDE_STEP_MS / 1000;  // 480 samples
    static int16_t hop[480];   // static → DRAM, DMA-safe for i2s_channel_read

    // I2S sanity: first read to confirm data flows before entering main loop
    {
        static int32_t sanity_stereo[128];
        size_t br = 0;
        esp_err_t e = i2s_channel_read(g_i2s_rx, sanity_stereo, sizeof(sanity_stereo),
                                       &br, pdMS_TO_TICKS(200));
        int n = (e == ESP_OK) ? (int)(br / sizeof(int32_t)) : -1;
        if (n <= 0) {
            ESP_LOGW(TAG_INF, "WARNING: I2S no data on sanity read — check SCK/WS/SD wiring");
        }
    }

    while (true) {
        int got = i2s_read_pcm(hop, HOP);
        if (got < HOP) {
            if (got < 0) ESP_LOGW(TAG_INF, "I2S read error (ret=%d)", got);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (xSemaphoreTake(ring_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (int i = 0; i < HOP; i++) {
                audio_ring[ring_write_pos] = hop[i];
                ring_write_pos = (ring_write_pos + 1) % AUDIO_BUFFER_SAMPLES;
            }
            xSemaphoreGive(ring_mutex);
            if (valid_ring_samples < AUDIO_BUFFER_SAMPLES) valid_ring_samples += HOP;
        } else {
            // frame dropped — ring_mutex held too long
        }
        // No vTaskDelay — i2s_read_pcm blocks ~30 ms naturally
    }
}

// ============================================================================
// TASK 1: inference_task — VAD + MFCC + TFLite Invoke on Core 0.
// MFCC computation and TFLite invoke are COMPLETELY UNCHANGED from rev6.
// New additions (marked NEW): LED on, display state, beep_task spawn.
// ============================================================================
static void inference_task(void* arg) {
    ESP_LOGI(TAG_INF, "Inference task started on core %d", xPortGetCoreID());
    const int hop_samples = I2S_SAMPLE_RATE * SLIDE_STEP_MS / 1000;
    int16_t* hop_buf = (int16_t*)malloc(hop_samples * sizeof(int16_t));
    if (!hop_buf) { ESP_LOGE(TAG_INF, "FATAL: hop_buf malloc failed"); vTaskDelay(portMAX_DELAY); return; }

    uint32_t infer_count      = 0;
    int      consecutive_hits = 0;
    float    noise_floor_rms  = 0.0f;
    int      noise_cal_frames = 0;
    int      last_read_pos    = 0;

    while (true) {

        // 1. Idle while streaming or in command-capture mode
        if (streaming_active || g_sys_state != SYS_LISTENING) {
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }

        // 1b. Idle during post-detection cooldown to prevent re-trigger loops
        {
            int64_t now = esp_timer_get_time();
            if (now < g_cooldown_end_us) {
                vTaskDelay(pdMS_TO_TICKS(50)); continue;
            }
            g_cooldown_end_us = 0;
        }

        // 2. Read hop from ring (bounded mutex)
        int avail;
        if (xSemaphoreTake(ring_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        avail = (int)ring_write_pos - last_read_pos;
        if (avail < 0) avail += AUDIO_BUFFER_SAMPLES;
        if (avail > AUDIO_BUFFER_SAMPLES) {
            avail = hop_samples;
            last_read_pos = (int)ring_write_pos - hop_samples;
            if (last_read_pos < 0) last_read_pos += AUDIO_BUFFER_SAMPLES;
        }
        if (avail < hop_samples || valid_ring_samples < AUDIO_BUFFER_SAMPLES) {
            xSemaphoreGive(ring_mutex); vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        {
            int start = (int)ring_write_pos - hop_samples;
            if (start < 0) start += AUDIO_BUFFER_SAMPLES;
            for (int i = 0; i < hop_samples; i++)
                hop_buf[i] = audio_ring[(start + i) % AUDIO_BUFFER_SAMPLES];
            last_read_pos = (int)ring_write_pos;
        }
        xSemaphoreGive(ring_mutex);

        // 3. RMS + peak (VAD)
        int64_t sum_sq = 0;
        for (int i = 0; i < hop_samples; i++) {
            sum_sq += (int64_t)hop_buf[i] * hop_buf[i];
        }
        float rms    = sqrtf((float)sum_sq / hop_samples) / 32768.0f;
        telemetry_mic_rms = rms;

        // 4. Noise calibration
        if (noise_cal_frames < NOISE_CALIBRATION_FRAMES) {
            if (rms < NOISE_CAL_MAX_RMS) {
                noise_floor_rms += (rms - noise_floor_rms) / (float)(++noise_cal_frames);
                telemetry_noise_floor_rms = noise_floor_rms;
            }
            if (noise_cal_frames < 5) { consecutive_hits = 0; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        }

        // 5. VAD gate — skip MFCC+TFLite if silence (CPU savings)
        float speech_thr = fmaxf(MIN_SPEECH_RMS, noise_floor_rms * NOISE_FLOOR_MULTIPLIER);
        if (rms < speech_thr) {
            noise_floor_rms = 0.02f * rms + 0.98f * noise_floor_rms;
            telemetry_noise_floor_rms = noise_floor_rms;
            consecutive_hits = 0;
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }

        // 6. Linearize ring → 1-second window + MFCC
        if (xSemaphoreTake(ring_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            int start = (int)ring_write_pos;
            for (int i = 0; i < AUDIO_BUFFER_SAMPLES; i++)
                audio_window[i] = audio_ring[(start + i) % AUDIO_BUFFER_SAMPLES];
            xSemaphoreGive(ring_mutex);
        } else { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        mfcc_proc.compute(audio_window, mfcc_output);

        // 7. Quantise input
        float   in_scale = input_tensor->params.scale;
        int32_t in_zp    = input_tensor->params.zero_point;
        int8_t* inp      = input_tensor->data.int8;
        for (int i = 0; i < MFCC_OUTPUT_SIZE; i++) {
            int32_t q = (int32_t)roundf(mfcc_output[i] / in_scale) + in_zp;
            q = q < -128 ? -128 : (q > 127 ? 127 : q); inp[i] = (int8_t)q;
        }

        // 8. TFLite invoke
        int64_t t0 = esp_timer_get_time();
        TfLiteStatus invoke_ok = interpreter->Invoke();
        float infer_us = (float)(esp_timer_get_time() - t0);
        if (invoke_ok != kTfLiteOk) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        // 9. Dequantise output — sigmoid model: 1 element = keyword probability
        float   out_scale = output_tensor->params.scale;
        int32_t out_zp    = output_tensor->params.zero_point;
        int8_t* out       = output_tensor->data.int8;
        int     out_elems = output_tensor->dims->data[output_tensor->dims->size - 1];
        if (out_elems < 1) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        float kw_prob = (out[0] - out_zp) * out_scale;
        bool  trigger = (kw_prob >= DETECT_THRESHOLD);

        infer_count++; telemetry_inference_count = telemetry_inference_count + 1;
        telemetry_inference_ms      += infer_us / 1000.0f;
        telemetry_last_infer_ms      = infer_us / 1000.0f;
        telemetry_keyword_confidence = kw_prob;
        consecutive_hits = trigger ? consecutive_hits + 1 : 0;

        // Periodic confidence log every 2 seconds for dashboard visibility
        {
            static int64_t last_log_us = 0;
            int64_t now = esp_timer_get_time();
            if (now - last_log_us >= 2000000) {
                last_log_us = now;
                float uptime_s = (float)(now / 1000);
                float cpu_pct = uptime_s > 0 ? (telemetry_inference_ms / uptime_s) * 100.0f : 0.0f;
                printf("[KWS] conf=%.4f thr=%.2f infer=%.0fus mic=%.4f floor=%.4f cpu=%.1f%% heap=%lu\n",
                       (double)kw_prob, (double)DETECT_THRESHOLD, (double)infer_us,
                       (double)telemetry_mic_rms, (double)telemetry_noise_floor_rms,
                       (double)cpu_pct,
                       (unsigned long)esp_get_free_heap_size());
                fflush(stdout);
            }
        }

        if (consecutive_hits >= DETECTION_HITS_REQUIRED) {
            int64_t kw_end = esp_timer_get_time();
            printf("[TRIGGER] conf=%.4f infer=%.0fus\n", (double)kw_prob, (double)infer_us); fflush(stdout);

            // Transition to command-capture mode
            g_sys_state = SYS_CAPTURE_COMMAND;
            gpio_set_level((gpio_num_t)LED_PIN, 1);

            // Step 1: Flash "Hey Vaani Detected!" on OLED for 400ms
            g_disp_state = DISP_DETECTED;
            vTaskDelay(pdMS_TO_TICKS(400));

            // Step 2: Show "How can I help you?" — command-capture mode
            if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                snprintf(g_disp_text, sizeof(g_disp_text), "How can I\nhelp you?");
                xSemaphoreGive(disp_mutex);
            }
            g_disp_state = DISP_PROMPT;
            consecutive_hits = 0;

            if (!streaming_active) {
                // Start from current ring position — guaranteed fresh data
                g_stream_start_pos = (int)ring_write_pos;
                xQueueSend(detect_queue, &kw_end, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// ============================================================================
// TASK 2: streaming_task — TCP + HVP1 + audio stream.
// SO_SNDTIMEO/SO_RCVTIMEO=5 s: send() can NEVER block forever. UNCHANGED.
// [NEW] display state updates + LED off after transcription received.
// ============================================================================
static void streaming_task(void* arg) {
    ESP_LOGI(TAG_STR, "Streaming task started (core %d)", xPortGetCoreID());
    int64_t keyword_end_us;
    while (true) {
        xQueueReceive(detect_queue, &keyword_end_us, portMAX_DELAY);
        session_id = session_id + 1;
        uint32_t sid = session_id;
        ESP_LOGI(TAG_STR, "[%lu] Keyword — connecting WiFi", (unsigned long)sid);

        // WiFi is kept always-on since boot — only reconnect if it dropped
        if (!wifi_connected) {
            wifi_start();
        }
        if (!wifi_connected) {
            g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
            g_sys_state = SYS_LISTENING;
            g_disp_state = DISP_LISTENING;
            gpio_set_level(LED_PIN, 0);
            continue;
        }

        const int  CHUNK = 480; int16_t pcm[CHUNK];
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(COMMAND_DURATION_MS);
        int total_sent = 0, streamed_ms = 0, consec_silence_ms = 0;

        set_streaming_active(true, "keyword_detected");

        // ── Phase 1: "Listening" — wait for user to speak ──────────────────
        // Don't connect to server yet.  Just monitor mic for speech.
        // This gives the user time to read "How can I help you?" and respond.
        g_disp_state = DISP_LISTENING;
        printf("[STREAM] Waiting for speech...\n"); fflush(stdout);

        int last_pos = (int)ring_write_pos;   // start reading from current position
        bool speech_detected = false;
        int wait_speech_ms = 0;
        const int SPEECH_WAIT_TIMEOUT_MS = 8000;  // max 8s waiting for speech
        const int PRE_BUFFER_MS = 300;             // 300ms pre-buffer before speech
        const int PRE_BUF_SAMPLES = I2S_SAMPLE_RATE * PRE_BUFFER_MS / 1000;  // 4800
        int16_t* pre_buf = (int16_t*)heap_caps_malloc(PRE_BUF_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
        bool pre_buf_ok = (pre_buf != NULL);
        if (!pre_buf_ok) { pre_buf = pcm; }  // fallback: no pre-buffering (pcm too small)
        int pre_buf_write = 0;
        int pre_buf_count = 0;

        while (!speech_detected && xTaskGetTickCount() < deadline) {
            if (wait_speech_ms >= SPEECH_WAIT_TIMEOUT_MS) {
                printf("[STREAM] No speech after %dms — giving up\n", wait_speech_ms); fflush(stdout);
                break;
            }
            if (xSemaphoreTake(ring_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
                vTaskDelay(pdMS_TO_TICKS(10)); continue;
            }
            int avail = (int)ring_write_pos - last_pos;
            if (avail < 0) avail += AUDIO_BUFFER_SAMPLES;
            if (avail < CHUNK) { xSemaphoreGive(ring_mutex); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            for (int i = 0; i < CHUNK; i++) {
                pcm[i] = audio_ring[last_pos];
                last_pos = (last_pos + 1) % AUDIO_BUFFER_SAMPLES;
            }
            xSemaphoreGive(ring_mutex);

            // Check RMS for speech
            int64_t sq = 0;
            for (int i = 0; i < CHUNK; i++) sq += (int64_t)pcm[i] * pcm[i];
            float crms = sqrtf((float)sq / CHUNK) / 32768.0f;
            float sil_thr = fmaxf(0.015f, telemetry_noise_floor_rms * 2.5f);

            if (crms >= sil_thr) {
                speech_detected = true;
                printf("[STREAM] Speech detected! rms=%.4f thr=%.4f after %dms\n",
                       (double)crms, (double)sil_thr, wait_speech_ms); fflush(stdout);
            } else {
                // Store in circular pre-buffer (only if heap-allocated, not pcm fallback)
                if (pre_buf_ok) {
                    for (int i = 0; i < CHUNK; i++) {
                        pre_buf[pre_buf_write] = pcm[i];
                        pre_buf_write = (pre_buf_write + 1) % PRE_BUF_SAMPLES;
                    }
                    if (pre_buf_count < PRE_BUF_SAMPLES) pre_buf_count += CHUNK;
                }
                wait_speech_ms += CHUNK * 1000 / I2S_SAMPLE_RATE;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        if (!speech_detected) {
            // No speech — go back to listening
            set_streaming_active(false, "no_speech");
            if (pre_buf && pre_buf != pcm) heap_caps_free(pre_buf);
            g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
            g_sys_state = SYS_LISTENING;
            g_disp_state = DISP_LISTENING;
            gpio_set_level(LED_PIN, 0);
            continue;
        }

        // ── Phase 2: Connect to server and stream command ──────────────────
        g_disp_state = DISP_STREAMING;
        printf("[STREAM] Connecting to server...\n"); fflush(stdout);

        int sock = -1; uint32_t kw_ms = 0, backoff_ms = 200;
        for (int attempt = 0; attempt < 4; attempt++) {
            struct sockaddr_in srv = {};
            srv.sin_family = AF_INET; srv.sin_port = htons(CONFIG_SERVER_PORT);
            inet_pton(AF_INET, g_server_ip, &srv.sin_addr);
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(backoff_ms)); backoff_ms *= 2; continue; }
            int f = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &f, sizeof(f));
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            if (connect(sock, (struct sockaddr*)&srv, sizeof(srv)) == 0) {
                kw_ms = (uint32_t)((esp_timer_get_time() - keyword_end_us) / 1000);
                break;
            }
            close(sock); sock = -1; vTaskDelay(pdMS_TO_TICKS(backoff_ms)); backoff_ms *= 2;
        }
        if (sock < 0) {
            g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
            g_sys_state    = SYS_LISTENING;
            g_disp_state = DISP_LISTENING;
            gpio_set_level(LED_PIN, 0);
            continue;
        }

        hvp1_header_t hdr = { .magic = MAGIC_NUMBER, .sample_rate = I2S_SAMPLE_RATE,
                               .channels = 1, .bits = 16,
                               .audio_len = 0, .kw_to_connect_ms = kw_ms };
        {
            uint8_t* hp = (uint8_t*)&hdr; size_t hl = sizeof(hdr);
            while (hl > 0) {
                ssize_t n = send(sock, hp, hl, 0);
                if (n < 0) break;
                hp += n; hl -= (size_t)n;
            }
        }

#if ENABLE_AES
        uint8_t aes_nonce[16] = {0};
        uint8_t aes_stream_blk[16] = {0};
        size_t  aes_nc_off = 0;
        esp_fill_random(aes_nonce, sizeof(aes_nonce));
        {
            ssize_t n = send(sock, aes_nonce, sizeof(aes_nonce), 0);
            if (n != (ssize_t)sizeof(aes_nonce)) {
                printf("[AES] nonce send failed\n"); fflush(stdout);
            }
        }

        esp_aes_context aes_ctx;
        esp_aes_init(&aes_ctx);
        esp_aes_setkey(&aes_ctx, AES_KEY, 128);
#else
        {
            uint8_t zero_nonce[16] = {0};
            send(sock, zero_nonce, sizeof(zero_nonce), 0);
        }
#endif

        // ── Phase 3: Stream pre-buffer + live audio ────────────────────────
        printf("[STREAM] Sending pre-buffer (%d samples) + streaming...\n", pre_buf_count); fflush(stdout);

        // Send pre-buffer first (speech context)
        if (pre_buf_count > 0) {
            int pre_start = (pre_buf_write - pre_buf_count + PRE_BUF_SAMPLES) % PRE_BUF_SAMPLES;
            int remaining = pre_buf_count;
            while (remaining > 0) {
                int batch = remaining < CHUNK ? remaining : CHUNK;
                uint8_t* pp = (uint8_t*)&pre_buf[pre_start];
                size_t pl = batch * sizeof(int16_t);
                pre_start = (pre_start + batch) % PRE_BUF_SAMPLES;
                remaining -= batch;
                ssize_t n = send(sock, pp, pl, 0);
                if (n < 0) break;
                total_sent += batch;
            }
        }

        // Now stream live audio (current chunk that triggered speech + subsequent)
        // The pcm buffer already has the speech-triggering chunk
        {
            uint8_t* pp = (uint8_t*)pcm; size_t pl = CHUNK * sizeof(int16_t);
            ssize_t n = send(sock, pp, pl, 0);
            if (n > 0) total_sent += CHUNK;
        }

        deadline = xTaskGetTickCount() + pdMS_TO_TICKS(COMMAND_DURATION_MS);
        consec_silence_ms = 0;
        streamed_ms = pre_buf_count * 1000 / I2S_SAMPLE_RATE + CHUNK * 1000 / I2S_SAMPLE_RATE;

        while (xTaskGetTickCount() < deadline) {
            if (xSemaphoreTake(ring_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
                vTaskDelay(pdMS_TO_TICKS(10)); continue;
            }
            int avail = (int)ring_write_pos - last_pos;
            if (avail < 0) avail += AUDIO_BUFFER_SAMPLES;
            if (avail < CHUNK) { xSemaphoreGive(ring_mutex); vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            for (int i = 0; i < CHUNK; i++) {
                pcm[i] = audio_ring[last_pos];
                last_pos = (last_pos + 1) % AUDIO_BUFFER_SAMPLES;
            }
            xSemaphoreGive(ring_mutex);

            {
                uint8_t* pp = (uint8_t*)pcm; size_t pl = CHUNK * sizeof(int16_t); bool se = false;

#if ENABLE_AES
                static uint8_t enc_buf[CHUNK * sizeof(int16_t)];
                esp_aes_crypt_ctr(&aes_ctx, pl, &aes_nc_off, aes_nonce, aes_stream_blk, pp, enc_buf);
                uint8_t* send_ptr = enc_buf; size_t send_len = pl;
#else
                uint8_t* send_ptr = pp; size_t send_len = pl;
#endif
                while (send_len > 0) {
                    ssize_t n = send(sock, send_ptr, send_len, 0);
                    if (n < 0) { printf("[SEND-ERROR] sid=%lu errno=%d (%s)\n", (unsigned long)sid, errno, strerror(errno)); fflush(stdout); se = true; break; }
                    send_ptr += n; send_len -= (size_t)n;
                }
                if (se) break;
            }
            total_sent += CHUNK;
            int chunk_ms = CHUNK * 1000 / I2S_SAMPLE_RATE; streamed_ms += chunk_ms;
            if (streamed_ms >= COMMAND_MIN_DURATION_MS) {
                int64_t sq = 0; for (int i = 0; i < CHUNK; i++) sq += (int64_t)pcm[i]*pcm[i];
                float crms    = sqrtf((float)sq / CHUNK) / 32768.0f;
                float sil_thr = fmaxf(0.015f, telemetry_noise_floor_rms * 2.5f);
                consec_silence_ms = crms < sil_thr ? consec_silence_ms + chunk_ms : 0;
                if (consec_silence_ms >= COMMAND_SILENCE_MS) {
                    break;
                }
            }
        }

        set_streaming_active(false, "stream_loop_ended");
        shutdown(sock, SHUT_WR);

#if ENABLE_AES
        esp_aes_free(&aes_ctx);   // release AES context after streaming done
#endif

        {
            char resp[1024] = {0}; int rlen = 0, r;
            // Increase recv timeout to wait for server transcription (Vosk takes 1-3s)
            struct timeval rcv_tv = { .tv_sec = 10, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
            while ((r = recv(sock, resp + rlen, sizeof(resp) - rlen - 1, 0)) > 0) rlen += r;
            printf("[STREAM] recv response: %d bytes rlen=%d\n", (int)r, rlen); fflush(stdout);
            close(sock);

            // ── Decrypt response using derived nonce (audio_nonce XOR 0xFF last byte) ──
#if ENABLE_AES
            if (rlen > 0) {
                uint8_t resp_nonce[16];
                memcpy(resp_nonce, aes_nonce, 16);
                resp_nonce[15] ^= 0xFF;   // same derivation as server-side _response_nonce()

                uint8_t resp_stream_blk[16] = {0};
                size_t  resp_nc_off = 0;
                esp_aes_context resp_aes;
                esp_aes_init(&resp_aes);
                esp_aes_setkey(&resp_aes, AES_KEY, 128);
                esp_aes_crypt_ctr(&resp_aes, (size_t)rlen, &resp_nc_off,
                                  resp_nonce, resp_stream_blk,
                                  (uint8_t*)resp, (uint8_t*)resp);  // decrypt in-place
                esp_aes_free(&resp_aes);
                resp[rlen] = '\0';
            }
#else
            resp[rlen] = '\0';
#endif

            // ── cJSON: Parse cloud JSON response ─────────────────────────
            // Cloud sends: {"verified": bool, "transcript": "...",
            //               "intent": {"intent":"...","action":"...", ...}, ...}
            // Uses ESP-IDF built-in cJSON — no extra dependency.
            bool cloud_verified = false;
            char clean_txt[128] = "[No Response]";
            char intent_str[64] = "";

            if (rlen > 0) {
                cJSON* root = cJSON_ParseWithLength(resp, (size_t)rlen);
                if (root) {
                    // "verified"
                    cJSON* jv = cJSON_GetObjectItemCaseSensitive(root, "verified");
                    cloud_verified = cJSON_IsTrue(jv);

                    // "transcript"
                    cJSON* jt = cJSON_GetObjectItemCaseSensitive(root, "transcript");
                    if (cJSON_IsString(jt) && jt->valuestring) {
                        snprintf(clean_txt, sizeof(clean_txt), "%s", jt->valuestring);
                    }

                    // "intent" (optional — only present when --intent flag used)
                    cJSON* ji = cJSON_GetObjectItemCaseSensitive(root, "intent");
                    if (cJSON_IsObject(ji)) {
                        cJSON* ja = cJSON_GetObjectItemCaseSensitive(ji, "intent");
                        cJSON* jac = cJSON_GetObjectItemCaseSensitive(ji, "action");
                        if (cJSON_IsString(ja) && cJSON_IsString(jac)) {
                            snprintf(intent_str, sizeof(intent_str), "%s:%s",
                                     ja->valuestring, jac->valuestring);
                        }
                    }

                    cJSON_Delete(root);
                } else {
                    snprintf(clean_txt, sizeof(clean_txt), "%.127s", resp);
                }
            }

            if (cloud_verified) {
                // "Detected!" was already shown at the local KWS stage (inference_task).
                // Now write the cloud transcript directly to OLED so it is visible immediately.
                if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    // Prefer intent string when available, otherwise show raw transcript
                    if (intent_str[0]) {
                        snprintf(g_disp_text, sizeof(g_disp_text), "%s", intent_str);
                    } else {
                        snprintf(g_disp_text, sizeof(g_disp_text), "%s", clean_txt);
                    }
                    xSemaphoreGive(disp_mutex);
                }
                printf("[TRANSCRIPT] %s\n", clean_txt); fflush(stdout);
                g_disp_state = DISP_TRANSCRIBED;   // OLED now shows the transcript text
                gpio_set_level(LED_PIN, 0);         // LED off — transcription complete
                vTaskDelay(pdMS_TO_TICKS(4000));    // hold transcript visible for 4s
            } else {
                gpio_set_level(LED_PIN, 0);
            }
        }

        // ── Return to listening state with cooldown ────────────────────────
        // Start cooldown: inference stays gated for COOLDOWN_MS to prevent
        // re-trigger loops.  The OLED shows "Listening" immediately so the
        // user knows the system is ready again after the cooldown expires.
        if (pre_buf && pre_buf != pcm) heap_caps_free(pre_buf);
        g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
        g_sys_state    = SYS_LISTENING;
        g_disp_state   = DISP_LISTENING;
        g_disp_text[0] = '\0';
        if (detect_queue) {
            xQueueReset(detect_queue);
        }
        // ─────────────────────────────────────────────────────────────────────
    }
}

// ============================================================================
// TASK 3a: wifi_keepalive_task — connects WiFi at boot (Core 1) and keeps it
// alive. Running on Core 1 avoids IWDT clash with inference_task on Core 0.
// streaming_task checks wifi_connected and skips wifi_start() if already up.
// ============================================================================
static void wifi_keepalive_task(void* arg) {
    ESP_LOGI(TAG_WIFI, "WiFi keepalive task started on core %d — connecting...", xPortGetCoreID());
    wifi_start();
    if (wifi_connected)
        ESP_LOGI(TAG_WIFI, "WiFi ready (keepalive).");
    else
        ESP_LOGW(TAG_WIFI, "WiFi not connected at boot — streaming will retry on trigger.");

    // ── Reconnection loop + 3-strike portal fallback ──────────────────────
    // If WiFi fails 3 times in a row (e.g. wrong password, IP change),
    // clear NVS credentials and restart into the captive portal so the
    // user can update them without needing a reflash.
    int fail_streak = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!wifi_connected) {
            fail_streak++;
            ESP_LOGW(TAG_WIFI, "WiFi dropped — reconnecting (strike %d/3)...", fail_streak);
            wifi_start();
            if (wifi_connected) {
                fail_streak = 0;
            } else if (fail_streak >= 3) {
                ESP_LOGE(TAG_WIFI, "3 consecutive WiFi failures — clearing NVS and"
                                   " restarting into setup portal");
                wifi_provision_clear();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            fail_streak = 0;
        }
    }
}

// ============================================================================
// TASK 4: wifi_config_poll_task — checks for updated WiFi credentials from server
// ============================================================================
static void wifi_config_poll_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000)); // Poll every 10 seconds
        if (!wifi_connected || g_server_ip[0] == '\0') continue;

        char url[128];
        snprintf(url, sizeof(url), "http://%s:80/api/wifi-config?raw=1", g_server_ip);

        esp_http_client_config_t config = {};
        config.url = url;
        config.timeout_ms = 3000;
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) continue;

        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            int status_code = esp_http_client_get_status_code(client);
            if (status_code == 200) {
                char buf[512] = {0};
                int read_len = esp_http_client_read(client, buf, sizeof(buf) - 1);
                if (read_len > 0) {
                    buf[read_len] = '\0';
                    cJSON* root = cJSON_Parse(buf);
                    if (root) {
                        cJSON* ssid = cJSON_GetObjectItem(root, "ssid");
                        cJSON* pass = cJSON_GetObjectItem(root, "password");
                        cJSON* s_ip = cJSON_GetObjectItem(root, "server_ip");
                        
                        if (ssid && ssid->valuestring && pass && pass->valuestring && s_ip && s_ip->valuestring) {
                            // Check if different from current
                            if (strcmp(ssid->valuestring, g_wifi_ssid) != 0 || 
                                strcmp(pass->valuestring, g_wifi_pass) != 0 || 
                                strcmp(s_ip->valuestring, g_server_ip) != 0) {
                                
                                ESP_LOGI(TAG_MAIN, "New WiFi config received via OTA!");
                                ESP_LOGI(TAG_MAIN, "SSID: %s -> %s", g_wifi_ssid, ssid->valuestring);
                                ESP_LOGI(TAG_MAIN, "IP: %s -> %s", g_server_ip, s_ip->valuestring);
                                
                                if (wifi_provision_save(ssid->valuestring, pass->valuestring, s_ip->valuestring)) {
                                    ESP_LOGI(TAG_MAIN, "NVS updated. Restarting ESP32...");
                                    vTaskDelay(pdMS_TO_TICKS(1000));
                                    esp_restart();
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
}

// ============================================================================
// TASK 3b: watchdog_task — force-clears streaming_active if stuck >10 s.
// UNCHANGED from rev6, plus: also resets LED and display state on force-clear.
// ============================================================================
static void watchdog_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (streaming_active && streaming_active_set_us != 0) {
            int64_t held = esp_timer_get_time() - streaming_active_set_us;
            if (held > (int64_t)STREAM_WATCHDOG_MS * 1000) {
                set_streaming_active(false, "watchdog_forced");
                g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
                g_sys_state  = SYS_LISTENING;
                gpio_set_level(LED_PIN, 0);
                g_disp_state = DISP_LISTENING;
            }
        }
    }
}

// ============================================================================
// TASK 4 (NEW): beep_task — fire-and-forget 150 ms PWM beep, then self-deletes.
// Created by inference_task at keyword confirm. Stack 1024 B is sufficient.
// Runs concurrently — does NOT block inference_task critical path.
// ============================================================================
// beep_task removed — no buzzer hardware connected

// ============================================================================
// TASK 5 (NEW): display_task — ST7735 status screen, Core 0, priority 1.
// Polls g_disp_state every 100 ms; redraws only on state change.
// All SPI operations are isolated here — inference_task and streaming_task
// are NEVER stalled by display updates.
// Stack: 4096 B (SPI master + static px/row buffers in oled_draw_char/fill).
// ============================================================================
static void display_task(void* arg) {
    ESP_LOGI(TAG_DISP, "Display task started (core %d)", xPortGetCoreID());
    disp_state_t last_state = (disp_state_t)(-1);  // force draw on first iteration
    char local_txt[256];   // local copy — read under mutex
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        disp_state_t cur = g_disp_state;
        if (cur != last_state) {
            local_txt[0] = '\0';
            if (cur == DISP_TRANSCRIBED || cur == DISP_PROMPT) {
                // Take disp_mutex to safely read g_disp_text written by other tasks
                if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    strlcpy(local_txt, g_disp_text, sizeof(local_txt));
                    xSemaphoreGive(disp_mutex);
                } else {
                    strlcpy(local_txt, g_disp_text, sizeof(local_txt));
                }
            }
            oled_show_status(cur, local_txt);
            last_state = cur;
            ESP_LOGI(TAG_DISP, "State → %d  (%s)", (int)cur,
                     cur==DISP_BOOTING    ? "Booting"   :
                     cur==DISP_LISTENING  ? "Listening" :
                     cur==DISP_DETECTED   ? "Detected"  :
                     cur==DISP_STREAMING  ? "Streaming" :
                     cur==DISP_PROMPT     ? "Prompt"    : "Transcribed");
        }
    }
}

// ============================================================================
// TASK 6: stack_monitor_task — periodic HWM log for all key tasks.
// Updated: now also tracks display_task HWM.
// ============================================================================
static void stack_monitor_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// ─── Telemetry Task — UNCHANGED from rev6 ────────────────────────────────────
static void telemetry_task(void* arg) {
    ESP_LOGI(TAG_STR, "Telemetry task started (core %d)", xPortGetCoreID());
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!wifi_connected) continue;
        wifi_ap_record_t ap = {};
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        float total_ms = telemetry_inference_ms;
        float uptime_ms = (float)(esp_timer_get_time() / 1000);
        float cpu_cum_pct = uptime_ms > 0 ? (total_ms / uptime_ms) * 100.0f : 0.0f;
        float snr_db = (telemetry_mic_rms > 0.001f && telemetry_noise_floor_rms > 0.001f)
                       ? 20.0f * log10f(telemetry_mic_rms / telemetry_noise_floor_rms) : 0.0f;
        char body[768];
        int blen = snprintf(body, sizeof(body),
            "{\"device\":\"esp32\",\"uptime_ms\":%llu,"
            "\"free_heap_bytes\":%lu,\"min_free_heap_bytes\":%lu,"
            "\"heap_total_bytes\":%lu,\"tflite_arena_bytes\":%u,"
            "\"audio_buffer_bytes\":%u,\"keyword_confidence\":%.4f,"
            "\"mic_rms\":%.5f,\"inference_count\":%lu,"
            "\"inference_duty_pct\":%.3f,\"wifi_rssi_dbm\":%d,\"streaming\":%s,"
            "\"latency_ms\":%.1f,\"cpu\":%.1f,\"snr\":%.1f,"
            "\"noise_floor_rms\":%.5f}",
            (unsigned long long)(esp_timer_get_time() / 1000),
            (unsigned long)esp_get_free_heap_size(),
            (unsigned long)esp_get_minimum_free_heap_size(),
            (unsigned long)heap_caps_get_total_size(MALLOC_CAP_8BIT),
            (unsigned)TENSOR_ARENA_SIZE,
            (unsigned)(sizeof(audio_ring) + sizeof(audio_window)),
            telemetry_keyword_confidence, telemetry_mic_rms,
            (unsigned long)telemetry_inference_count,
            cpu_cum_pct, rssi, streaming_active ? "true" : "false",
            (double)telemetry_last_infer_ms,
            (double)cpu_cum_pct,
            (double)snr_db,
            (double)telemetry_noise_floor_rms);
        if (blen <= 0 || blen >= (int)sizeof(body)) continue;
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) continue;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET; addr.sin_port = htons(80);
        inet_pton(AF_INET, g_server_ip, &addr.sin_addr);  // from NVS / portal
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            char req[700];
            int rlen = snprintf(req, sizeof(req),
                "POST /api/telemetry HTTP/1.1\r\nHost: esp32\r\n"
                "Content-Type: application/json\r\nContent-Length: %d\r\n"
                "Connection: close\r\n\r\n%s", blen, body);
            if (rlen > 0 && rlen < (int)sizeof(req)) send(sock, req, rlen, 0);
        }
        close(sock);
    }
}

// ─── app_main ────────────────────────────────────────────────────────────────
extern "C" void app_main() {
    printf("[BOOT] DETECT_THRESHOLD=%.3f  HITS=%d  COOLDOWN=%d  MODEL=%u bytes\n", 
           (double)DETECT_THRESHOLD, DETECTION_HITS_REQUIRED, COOLDOWN_MS, (unsigned)g_model_data_len);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG_MAIN, "=== Hey Vaani Edge Firmware (SIH 2026) rev9 ===");
    ESP_ERROR_CHECK(nvs_flash_init());

    ring_mutex   = xSemaphoreCreateMutex();
    disp_mutex   = xSemaphoreCreateMutex();   // guards g_disp_text (issue #3 fix)
    detect_queue = xQueueCreate(4, sizeof(int64_t));

    // ── LED: output, initially off ────────────────────────────────────────────
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);
    ESP_LOGI(TAG_MAIN, "LED ready (GPIO%d)", LED_PIN);

    // Buzzer LEDC init removed — no buzzer hardware connected
    ESP_LOGI(TAG_MAIN, "LED ready on GPIO%d (buzzer not connected)", LED_PIN);

    // ── OLED: SSD1306 I2C init, immediately shows "Booting..." ──────────────────
    ssd1306_init();   // draws DISP_BOOTING screen at end

    // ── WiFi provisioning ────────────────────────────────────────────────────
    // Loads SSID/password/server-IP from NVS into g_wifi_ssid/pass/ip.
    // If NVS is empty (first boot / factory reset): starts "HeyVaani-Setup"
    // AP + captive portal, blocks here until credentials are saved, then
    // calls esp_restart(). Normal boots return instantly.
    wifi_provision_init();

    // ── I2S: INMP441 ─────────────────────────────────────────────────────────
    i2s_global_init();

    // ── Mic self-check: serial RMS logging confirms I2S data flow ────────────
    mic_selfcheck();

    // ── TFLite + MFCC ────────────────────────────────────────────────────────
    tflite_init();
    mfcc_proc.init();

    ESP_LOGI(TAG_MAIN, "Free heap after init: %u bytes", (unsigned)esp_get_free_heap_size());

    // Core assignment:
    //   Core 0: inference, watchdog, display, stack_monitor, telemetry
    //   Core 1: audio (I2S drain), streaming (TCP/WiFi), wifi_keepalive
    // NOTE: wifi_keepalive_task on Core 1 connects WiFi at boot so streaming_task
    // finds WiFi already up with zero delay — avoids the 7+ s on-demand delay that
    // caused user commands to be missed. Must be Core 1 to avoid IWDT clash with
    // inference_task on Core 0.
    xTaskCreatePinnedToCore(audio_task,         "audio",       4096,  NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(inference_task,     "inference",   16384, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(streaming_task,     "streaming",   6144,  NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(wifi_keepalive_task,"wifi_ka",     4096,  NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(wifi_config_poll_task,"wifi_poll", 4096,  NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(watchdog_task,      "watchdog",    2048,  NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(display_task,       "display",     4096,  NULL, 1, NULL, 1); // [NEW] Moved to Core 1 to avoid I2C crash!
    xTaskCreatePinnedToCore(stack_monitor_task, "stack_mon",   2048,  NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(telemetry_task,     "telemetry",   4096,  NULL, 1, NULL, 0);
    benchmark_cpu_start();

    // display_task will detect this state change and draw "Listening" within 100 ms
    g_disp_state = DISP_LISTENING;
    ESP_LOGI(TAG_MAIN, "All tasks started. Listening for 'Hey Vaani'...");
}
