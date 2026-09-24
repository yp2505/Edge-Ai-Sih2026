// main.cpp — Hey Vaani ESP32 Edge KWS + Cloud ASR Streaming
//
// FIX LOG (2026-09-23 rev12 — Detection Reliability):
//   Fixes for "needs 10–15 attempts" + "sometimes auto-triggers":
//   1. Two-tier trigger: HARD=0.85 single frame OR SOFT=0.65 with 3 hits
//      inside a 6-frame sliding window (~180 ms). Noise spikes need the hard
//      tier; quiet real speech accumulates soft hits without being wiped by
//      phoneme dips between "Hey" and "Vaani".
//   2. esp_now_fusion: removed hard SUPPRESS for local_conf < 0.82. That
//      mismatch with DETECT_THRESHOLD=0.65 discarded valid detections whenever
//      a peer was known. Fusion now only arbitrates handoff between two valid
//      local detections; peer silence never blocks a local trigger.
//   3. Handoff-loss path no longer stalls 1.2 s or flushes the ring buffer,
//      and no longer shows false "Detected!" UI before fusion decision.
//   4. VAD was blocking ALL inference on this hardware: live mic RMS ~0.004
//      but MIN_SPEECH_RMS=0.008 and SNR≥4dB (floor tracks ~0.0037) meant
//      [KWS] never appeared. Relaxed MIN_RMS/SNR/mult so KWS actually runs.
//
// FIX LOG (2026-09-23 rev11 — State-Reset + Watchdog + Beamforming):
//   All fixes from rev10 preserved plus:
//   1. inference_task: Added else-if branch for should_stream && streaming_active.
//      Previously g_sys_state was permanently stuck in SYS_CAPTURE_COMMAND on
//      overlapping triggers, blocking all future detection until reboot.
//      New branch immediately resets state, LED, and sets cooldown.
//   2. watchdog_task: Added g_sys_capture_start_us + SYS_STATE_WATCHDOG_MS=25s.
//      Independently detects g_sys_state stuck in SYS_CAPTURE_COMMAND (e.g. no WiFi,
//      queue full) and force-resets to SYS_LISTENING after 25 s.
//   3. audio_task: Delay-and-Sum beamforming with Mic A (GPIO22) + Mic B (GPIO33).
//      hop_b[] now stores Mic B HP-filtered samples. When both mics are active:
//        out[i] = (mic_a[i] + mic_b[i]) >> 1  → ~3 dB SNR improvement.
//      Falls back to single-mic if either channel is silent (RMS < 0.003).
//
// FIX LOG (2026-09-22 rev10 — Post-Capacitor Calibration):
//   All fixes from rev9 preserved plus:
//   1. Decoupling capacitor (100nF-10uF) placed between 3V3 and GND near INMP441.
//      Eliminates power rail noise that caused elevated noise floor RMS.
//      Clean noise floor now ~0.003-0.010 RMS (was 0.020-0.035 with ripple).
//   2. VAD thresholds recalibrated for clean rail:
//      MIN_SPEECH_RMS: 0.045 → 0.025 (clean noise << real speech)
//      MIN_PEAK_SAMPLE: 1200 → 700  (clean transients clearer at lower level)
//      DETECT_THRESHOLD: 0.94 → 0.92 (slightly more sensitive with clean audio)
//      DETECTION_HITS_REQUIRED: 4 → 3 (90ms debounce, sufficient post-cap)
//   3. Streaming VAD: CMD_RMS_GATE 0.020 → 0.008 (don't prematurely close stream)
//      COMMAND_SILENCE_MS: 800 → 2000ms (user has more time to speak command)
//      COMMAND_MIN_DURATION_MS: 1000 → 1500ms (captures full command)
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
#include <fcntl.h>

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
#include "esp_now_fusion.h"   // Cross-node confidence fusion (Part 2)

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
#define CONFIG_SERVER_PORT     8080
#endif

#ifndef ENABLE_AES
#define ENABLE_AES             0    // Set to 1 when ESP32↔server AES CTR is verified
#endif

// ─── I2S / INMP441 ───────────────────────────────────────────────────────────
#define I2S_SAMPLE_RATE    16000
#define I2S_PORT           I2S_NUM_0
#define I2S_SCK_PIN        GPIO_NUM_26   // BCLK  (INMP441 Mic A SCK)
#define I2S_WS_PIN         GPIO_NUM_25   // LRCLK (INMP441 Mic A WS)
#define I2S_SD_PIN         GPIO_NUM_22   // DATA  (INMP441 Mic A SD)

// ─── I2S / INMP441 Mic B (second mic, I2S_NUM_1) ─────────────────────────────
// Proposed pins: SCK=GPIO14, WS=GPIO32, SD=GPIO33.
// CONFIRM these are free on your breadboard before flashing.
// Non-fatal: if Mic B is not wired, g_mic_b_ok stays false and inference_task
// falls back to single-mic mode automatically.
#define I2S_PORT_B         I2S_NUM_1
#define I2S_SCK_PIN_B      GPIO_NUM_14   // BCLK  (INMP441 Mic B SCK)
#define I2S_WS_PIN_B       GPIO_NUM_32   // LRCLK (INMP441 Mic B WS)
#define I2S_SD_PIN_B       GPIO_NUM_33   // DATA  (INMP441 Mic B SD)

// ─── KWS / Inference ─────────────────────────────────────────────────────────
// DETECT_THRESHOLD: sigmoid decision boundary calibrated on the custom
// "Hey Vaani" dataset.  Model output: [1,1] sigmoid (0.0–1.0).
//
// TWO-TIER TRIGGER (fixes both failure modes):
//   • HARD (DETECT_HARD_THRESHOLD, 1 frame): very confident hit → instant fire.
//     Catches clear "Hey Vaani" on the first good frame (fast path).
//   • SOFT (DETECT_THRESHOLD, N hits in sliding window): sensitive path for
//     quieter / distant speech that peaks 0.65–0.85. Requires multiple hits
//     inside a short window so a single noise spike cannot fire.
//
// DETECTION_HITS_REQUIRED / DETECTION_WINDOW_FRAMES:
//   Count hits over the last DETECTION_WINDOW_FRAMES inference frames
//   (not strictly consecutive) so natural phoneme dips between "Hey" and
//   "Vaani" do not zero the counter.  Window ≈ frames × SLIDE_STEP_MS.
//
// Previous bug: HITS=1 + no window meant one noisy frame ≥0.65 fired the
// detector (false "automatic" triggers), while fusion hard-suppressed
// [0.65,0.82) when a peer was known (needed 10–15 attempts).  Both fixed.
static const float DETECT_THRESHOLD          = 0.80f;   // Reliable speech confidence threshold
static const float DETECT_HARD_THRESHOLD     = 0.94f;   // HARD tier threshold
static const int   DETECTION_HITS_REQUIRED   = 2;       // 2 consecutive hits (~60ms sustained match)
static const int   DETECTION_WINDOW_FRAMES   = 5;       // sliding window (~150 ms at 30 ms/frame)
static const float MIN_SPEECH_RMS            = 0.010f;  // speech detection floor (above ambient noise ~0.0050)
static const int   MIN_PEAK_SAMPLE           = 700;     // consonant burst threshold (above ambient noise spikes <550)
static const float NOISE_FLOOR_MULTIPLIER    = 1.60f;   // adaptive speech threshold multiplier (~4 dB SNR)
static const int   NOISE_CALIBRATION_FRAMES  = 30;
static const float NOISE_CAL_MAX_RMS         = 0.022f;
static const float MIN_SNR_DB                = 2.0f;    // require speech SNR over noise floor

// ─── Streaming / VAD ─────────────────────────────────────────────────────────
#define SLIDE_STEP_MS           30
#define COMMAND_DURATION_MS     5000       // 5.0s max stream
#define COMMAND_MIN_DURATION_MS 1200       // 1.2s minimum command
#define COMMAND_SILENCE_MS      1000       // 1.0s silence closes stream
#define COOLDOWN_MS             1500       // 1.5s cooldown — prevents instant retrigger loops
#define AUDIO_BUFFER_SAMPLES    16000
#define STREAM_WATCHDOG_MS      25000      // 25s watchdog

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
#define LED_PIN              GPIO_NUM_27   // External LED on breadboard pin 27
#define ONBOARD_LED_PIN      GPIO_NUM_2    // ESP32 DevKit onboard blue LED pin 2

static inline void set_led_state(bool on) {
    gpio_set_level((gpio_num_t)LED_PIN, on ? 1 : 0);
    gpio_set_level((gpio_num_t)ONBOARD_LED_PIN, on ? 1 : 0);
}

// ─── SSD1306 I2C OLED (0.96", 128×64, monochrome) ───────────────────────────
// GPIO21=SDA (default I2C), GPIO19=SCL.
// Note: GPIO33 is used by INMP441 Mic B SD — I2C pins avoid that conflict.
#define OLED_SDA_PIN    GPIO_NUM_21
#define OLED_SCL_PIN    GPIO_NUM_19
#define OLED_I2C_PORT   I2C_NUM_0
#define OLED_I2C_HZ     100000           // 100 kHz Standard Mode (reliable on breadboard with internal pullups)
#define OLED_ADDR       0x3C             // Default address; auto-probes 0x3C / 0x3D in ssd1306_init
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
// SSD1306 framebuffer: 128×64 / 8 = 1024 bytes (1 bit per pixel)
#define OLED_BUF_BYTES  (OLED_WIDTH * OLED_HEIGHT / 8)

static uint8_t s_oled_addr = OLED_ADDR;
static bool    s_oled_ready = false;
static volatile float s_mic_a_rms = 0.0f;
static volatile float s_mic_b_rms = 0.0f;

// ─── WiFi ────────────────────────────────────────────────────────────────────
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10

static const char* TAG_INF   = "INFERENCE";
static const char* TAG_STR   = "STREAM";
static const char* TAG_MAIN  = "MAIN";
static const char* TAG_WIFI  = "WIFI";
static const char* TAG_MIC   = "MIC";
static const char* TAG_DISP  = "DISPLAY";
static const char* TAG_FUSE  = "FUSION";

// ─── Node Identity ───────────────────────────────────────────────────────────
// NODE_ID: 1 or 2 — identifies this node in ESP-NOW fusion logs and handoff.
#ifndef NODE_ID
#define NODE_ID 2
#endif

#ifndef DEVICE_NAME
#define DEVICE_NAME "Hey Vaani Node 2"
#endif

static volatile int  wifi_retry_count = 0;
static volatile bool wifi_connected   = false;

// ─── Streaming state — always via set_streaming_active() ─────────────────────
static volatile bool    streaming_active        = false;
static volatile int64_t streaming_active_set_us  = 0;
static volatile int64_t g_sys_capture_start_us   = 0; // watchdog: set when entering SYS_CAPTURE_COMMAND

// ─── Telemetry ───────────────────────────────────────────────────────────────
static volatile uint32_t telemetry_inference_count    = 0;
static volatile float    telemetry_inference_ms       = 0.0f;
static volatile float    telemetry_keyword_confidence = 0.0f;
static volatile float    telemetry_mic_rms            = 0.0f;
static volatile float    telemetry_noise_floor_rms    = 0.0f;
static volatile float    telemetry_last_infer_ms      = 0.0f;

static int16_t           audio_ring[AUDIO_BUFFER_SAMPLES];
static int16_t*          audio_window = nullptr;
static volatile int      ring_write_pos     = 0;
static volatile int      valid_ring_samples = 0;
static SemaphoreHandle_t ring_mutex;

static void flush_audio_ring() {
    if (ring_mutex && xSemaphoreTake(ring_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset((void*)audio_ring, 0, sizeof(audio_ring));
        ring_write_pos = 0;
        valid_ring_samples = 0;
        xSemaphoreGive(ring_mutex);
    }
}

static QueueHandle_t     detect_queue;
static volatile uint32_t session_id = 0;
static volatile int      g_stream_start_pos = 0;  // ring pos saved at detection time
// Cycle start timestamp — written by inference_task at trigger, read by streaming_task for [CYCLE] log
static int64_t           g_cycle_start_us   = 0;

// ─── TFLite ──────────────────────────────────────────────────────────────────
static const size_t TENSOR_ARENA_SIZE = 32 * 1024;
static uint8_t tflite_arena[32 * 1024];

static tflite::MicroMutableOpResolver<12> resolver;
static tflite::MicroInterpreter*          interpreter   = nullptr;
static TfLiteTensor*                      input_tensor  = nullptr;
static TfLiteTensor*                      output_tensor = nullptr;

// ─── I2S handle ──────────────────────────────────────────────────────────────
static i2s_chan_handle_t g_i2s_rx   = NULL;   // Mic A (I2S_NUM_0)
static i2s_chan_handle_t g_i2s_rx_b = NULL;   // Mic B (I2S_NUM_1) — NULL if not wired
static bool              g_mic_b_ok = false;  // true only if Mic B inited successfully
static int32_t*          s_stereo_rx_buf_a = NULL;
static int32_t*          s_stereo_rx_buf_b = NULL;

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

    if (!s_stereo_rx_buf_a) {
        s_stereo_rx_buf_a = (int32_t*)heap_caps_malloc(480 * 2 * sizeof(int32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_stereo_rx_buf_a) ESP_LOGE(TAG_MAIN, "FATAL: s_stereo_rx_buf_a malloc failed");
    }

    ESP_LOGI(TAG_MAIN, "INMP441 Mic A (I2S_NUM_0) ready: %d Hz  16-bit  stereo-deinterleave  "
             "SCK=GPIO%d  WS=GPIO%d  SD=GPIO%d",
             I2S_SAMPLE_RATE, I2S_SCK_PIN, I2S_WS_PIN, I2S_SD_PIN);
}

// ─── I2S Init: Mic B (I2S_NUM_1, second INMP441) ────────────────────────────────────
// Called after i2s_global_init() in app_main.
// Non-fatal if Mic B is not wired: sets g_mic_b_ok=false and inference_task
// will fall back to single-mic mode automatically.
static void i2s_global_init_b() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_B, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t e = i2s_new_channel(&chan_cfg, NULL, &g_i2s_rx_b);
    if (e != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Mic B: i2s_new_channel(I2S_NUM_1) failed (%s) — single-mic mode",
                 esp_err_to_name(e));
        g_i2s_rx_b = NULL;
        g_mic_b_ok = false;
        return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)(-1),
            .bclk = I2S_SCK_PIN_B,
            .ws   = I2S_WS_PIN_B,
            .dout = (gpio_num_t)(-1),
            .din  = I2S_SD_PIN_B,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    // INMP441 with L/R=GND outputs audio on LEFT channel.
    // Use SLOT_BOTH so DMA receives stereo pairs matching i2s_read_pcm_from de-interleave.
    std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

    e = i2s_channel_init_std_mode(g_i2s_rx_b, &std_cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Mic B: i2s_channel_init_std_mode failed (%s) — single-mic mode",
                 esp_err_to_name(e));
        i2s_del_channel(g_i2s_rx_b);
        g_i2s_rx_b = NULL; g_mic_b_ok = false;
        return;
    }
    e = i2s_channel_enable(g_i2s_rx_b);
    if (e != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "Mic B: i2s_channel_enable failed (%s) — single-mic mode",
                 esp_err_to_name(e));
        i2s_del_channel(g_i2s_rx_b);
        g_i2s_rx_b = NULL; g_mic_b_ok = false;
        return;
    }
    if (!s_stereo_rx_buf_b) {
        s_stereo_rx_buf_b = (int32_t*)heap_caps_malloc(480 * 2 * sizeof(int32_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_stereo_rx_buf_b) ESP_LOGE(TAG_MAIN, "FATAL: s_stereo_rx_buf_b malloc failed");
    }
    g_mic_b_ok = true;
    ESP_LOGI(TAG_MAIN, "INMP441 Mic B (I2S_NUM_1) ready: %d Hz  SCK=GPIO%d  WS=GPIO%d  SD=GPIO%d",
             I2S_SAMPLE_RATE, I2S_SCK_PIN_B, I2S_WS_PIN_B, I2S_SD_PIN_B);
}


// ─── I2S PCM read — STEREO de-interleave for INMP441 ─────────────────────────
// INMP441 outputs 24-bit left-justified in 32-bit frame.  With I2S_SLOT_BIT_WIDTH_32BIT,
// each sample is 4 bytes (int32).  We read int32 stereo, de-interleave LEFT channel,
// and right-shift by 16 to get int16 audio.
static int i2s_read_pcm(int16_t* out_buf, int out_count) {
    if (!s_stereo_rx_buf_a || !g_i2s_rx) return -1;
    int read_stereo = (out_count <= 480) ? out_count : 480;
    size_t bytes_wanted = (size_t)read_stereo * 2 * sizeof(int32_t);  // stereo int32
    size_t bytes_read = 0;
    esp_err_t e = i2s_channel_read(g_i2s_rx,
                                   s_stereo_rx_buf_a,
                                   bytes_wanted,
                                   &bytes_read,
                                   pdMS_TO_TICKS(200));
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) return -1;
    int stereo_samples = (int)(bytes_read / sizeof(int32_t));  // L+R interleaved int32
    int mono_out = 0;
    // INMP441 L/R=GND: audio data is in LEFT channel (even indices). Top 16 bits = audio PCM.
    // 150 Hz 2nd-order Butterworth High-Pass Filter (fs=16000 Hz):
    // Removes hardware DC offset and strongly attenuates 50 Hz / 100 Hz mains hum (-19.1 dB)
    // while keeping 100% of speech frequencies (300 Hz - 4000 Hz) crisp and clear.
    static float x1_a = 0.0f, x2_a = 0.0f;
    static float y1_a = 0.0f, y2_a = 0.0f;
    const float b0 = 0.959203f, b1 = -1.918406f, b2 = 0.959203f;
    const float a1 = -1.916741f, a2 = 0.920071f;
    for (int i = 0; i + 1 < stereo_samples && mono_out < out_count; i += 2) {
        int32_t s32 = s_stereo_rx_buf_a[i];
        float x0 = (float)(s32 >> 16);
        float y0 = b0 * x0 + b1 * x1_a + b2 * x2_a - a1 * y1_a - a2 * y2_a;
        x2_a = x1_a; x1_a = x0;
        y2_a = y1_a; y1_a = y0;
        if (y0 > 32767.0f) y0 = 32767.0f;
        else if (y0 < -32768.0f) y0 = -32768.0f;
        out_buf[mono_out++] = (int16_t)y0;
    }
    return mono_out;
}

// ─── i2s_read_pcm_from — generic INMP441 read from any channel handle ─────────
static int i2s_read_pcm_from(i2s_chan_handle_t chan, int16_t* out_buf, int out_count) {
    int32_t* buf = (chan == g_i2s_rx_b) ? s_stereo_rx_buf_b : s_stereo_rx_buf_a;
    if (!buf || !chan) return -1;
    int read_stereo = (out_count <= 480) ? out_count : 480;
    size_t bytes_wanted = (size_t)read_stereo * 2 * sizeof(int32_t);
    size_t bytes_read = 0;
    esp_err_t e = i2s_channel_read(chan, buf, bytes_wanted,
                                   &bytes_read, pdMS_TO_TICKS(200));
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) return -1;
    int stereo_samples = (int)(bytes_read / sizeof(int32_t));
    int mono_out = 0;
    static float x1_b = 0.0f, x2_b = 0.0f;
    static float y1_b = 0.0f, y2_b = 0.0f;
    const float b0 = 0.959203f, b1 = -1.918406f, b2 = 0.959203f;
    const float a1 = -1.916741f, a2 = 0.920071f;
    for (int i = 0; i + 1 < stereo_samples && mono_out < out_count; i += 2) {
        int32_t s_l = buf[i];
        int32_t s_r = buf[i + 1];
        int32_t s32 = (labs(s_l) >= labs(s_r)) ? s_l : s_r;
        float x0 = (float)(s32 >> 16);
        float y0 = b0 * x0 + b1 * x1_b + b2 * x2_b - a1 * y1_b - a2 * y2_b;
        x2_b = x1_b; x1_b = x0;
        y2_b = y1_b; y1_b = y0;
        if (y0 > 32767.0f) y0 = 32767.0f;
        else if (y0 < -32768.0f) y0 = -32768.0f;
        out_buf[mono_out++] = (int16_t)y0;
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
    if (s_oled_addr == 0) return ESP_ERR_NOT_FOUND;
    uint8_t buf[2] = {0x00, cmd};   // 0x00 = control byte: Co=0, D/C#=0
    return i2c_master_write_to_device(OLED_I2C_PORT, s_oled_addr,
                                      buf, 2, pdMS_TO_TICKS(20));
}

// Flush the full 1024-byte framebuffer to SSD1306 GDDRAM page-by-page (8 pages × 128 columns)
static void ssd_flush() {
    if (s_oled_addr == 0) return;
    uint8_t page_buf[1 + OLED_WIDTH];
    page_buf[0] = 0x40; // Co=0, D/C#=1 (GDDRAM data write)
    for (uint8_t p = 0; p < 8; p++) {
        ssd_cmd(0xB0 + p); // Set page start address (Page 0..7)
        ssd_cmd(0x00);     // Set lower column address
        ssd_cmd(0x10);     // Set higher column address
        memcpy(page_buf + 1, &g_oled_fb[p * OLED_WIDTH], OLED_WIDTH);
        i2c_master_write_to_device(OLED_I2C_PORT, s_oled_addr,
                                   page_buf, sizeof(page_buf), pdMS_TO_TICKS(50));
    }
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
    if (!s_oled_ready || s_oled_addr == 0) return;
    // Clear framebuffer
    ssd_clear(0x00);

    // Header
    const char* hdr = (NODE_ID == 2) ? "VAANI · NODE 2" : "HEY VAANI";
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
    } else if (state == DISP_LISTENING || state == DISP_BOOTING) {
        // Hysteresis filter: hold VOICE state for 6 refreshes (~1.2 seconds) on speech activity
        static int s_m1_hold = 0;
        static int s_m2_hold = 0;
        if (s_mic_a_rms > 0.008f) s_m1_hold = 6; else if (s_m1_hold > 0) s_m1_hold--;
        if (s_mic_b_rms > 0.008f) s_m2_hold = 6; else if (s_m2_hold > 0) s_m2_hold--;

        char m1_str[16], m2_str[16];
        snprintf(m1_str, sizeof(m1_str), "M1:%s", s_m1_hold > 0 ? "VOICE" : "OK");
        snprintf(m2_str, sizeof(m2_str), "M2:%s", !g_mic_b_ok ? "N/C" : (s_m2_hold > 0 ? "VOICE" : "OK"));
        ssd_draw_str(2, 33, m1_str, true, 1);
        ssd_draw_str(2, 44, m2_str, true, 1);

        // Dynamic Audio Energy / Dual VU Level Bars
        // Mic 1 meter box: x=52..125, y=33..40 (8px tall)
        for (int x = 52; x <= 125; x++) { ssd_pixel(x, 33, true); ssd_pixel(x, 40, true); }
        for (int y = 33; y <= 40; y++) { ssd_pixel(52, y, true); ssd_pixel(125, y, true); }
        int fill_1 = (int)((s_mic_a_rms - 0.002f) * 3500.0f);
        if (fill_1 < 0) fill_1 = 0;
        if (fill_1 > 71) fill_1 = 71;
        for (int x = 53; x < 53 + fill_1; x++) {
            for (int y = 35; y <= 38; y++) ssd_pixel(x, y, true);
        }

        // Mic 2 meter box: x=52..125, y=44..51 (8px tall)
        for (int x = 52; x <= 125; x++) { ssd_pixel(x, 44, true); ssd_pixel(x, 51, true); }
        for (int y = 44; y <= 51; y++) { ssd_pixel(52, y, true); ssd_pixel(125, y, true); }
        int fill_2 = (int)((s_mic_b_rms - 0.002f) * 3500.0f);
        if (fill_2 < 0) fill_2 = 0;
        if (fill_2 > 71) fill_2 = 71;
        for (int x = 53; x < 53 + fill_2; x++) {
            for (int y = 46; y <= 49; y++) ssd_pixel(x, y, true);
        }

        // Helper hint at bottom
        ssd_draw_str(2, 55, "Say 'Hey Vaani'...", true, 1);
    }

    // Push framebuffer to display
    ssd_flush();
}

static bool s_i2c_driver_installed = false;

static void ssd1306_delete_driver() {
    if (s_i2c_driver_installed) {
        i2c_driver_delete(OLED_I2C_PORT);
        s_i2c_driver_installed = false;
    }
}

// SSD1306 hardware + I2C bus init with bus recovery and multi-pin detection
static void ssd1306_init() {
    ssd1306_delete_driver();

    // Standard I2C bus recovery: pulse SCL 9 times to free any slave holding SDA low
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_OUTPUT_OD);
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_INPUT);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(GPIO_NUM_19, 0);
        esp_rom_delay_us(10);
        gpio_set_level(GPIO_NUM_19, 1);
        esp_rom_delay_us(10);
    }

    // Check voltage levels on SDA and SCL
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_21, GPIO_PULLUP_ONLY);
    gpio_set_direction(GPIO_NUM_19, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_19, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(5));
    int sda_lvl = gpio_get_level(GPIO_NUM_21);
    int scl_lvl = gpio_get_level(GPIO_NUM_19);

    struct PinPair { gpio_num_t sda; gpio_num_t scl; const char* desc; };
    static const PinPair CANDIDATES[] = {
        { GPIO_NUM_21, GPIO_NUM_19, "SDA=21 SCL=19" },
        { GPIO_NUM_19, GPIO_NUM_21, "SDA=19 SCL=21 (swapped)" },
    };

    bool oled_found = false;

    for (size_t c = 0; c < sizeof(CANDIDATES)/sizeof(CANDIDATES[0]); c++) {
        ssd1306_delete_driver();
        gpio_reset_pin(CANDIDATES[c].sda);
        gpio_reset_pin(CANDIDATES[c].scl);

        i2c_config_t conf = {};
        conf.mode             = I2C_MODE_MASTER;
        conf.sda_io_num       = CANDIDATES[c].sda;
        conf.scl_io_num       = CANDIDATES[c].scl;
        conf.sda_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.scl_pullup_en    = GPIO_PULLUP_ENABLE;
        conf.master.clk_speed = 100000; // 100 kHz Standard Mode (transfers full 128 cols without timeout)
        i2c_param_config(OLED_I2C_PORT, &conf);
        if (i2c_driver_install(OLED_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0) == ESP_OK) {
            s_i2c_driver_installed = true;
            i2c_set_timeout(OLED_I2C_PORT, 0xFFFFF);
            gpio_pullup_en(CANDIDATES[c].sda);
            gpio_pullup_en(CANDIDATES[c].scl);
        }

        vTaskDelay(pdMS_TO_TICKS(20));

        ESP_LOGI(TAG_DISP, "[BUS PRE] %s: SDA(GPIO%d)=%d SCL(GPIO%d)=%d",
                 CANDIDATES[c].desc,
                 CANDIDATES[c].sda, gpio_get_level(CANDIDATES[c].sda),
                 CANDIDATES[c].scl, gpio_get_level(CANDIDATES[c].scl));

        // Probe 0x3C first (default SSD1306) and 0x3D
        static const uint8_t TARGET_ADDRS[] = { 0x3C, 0x3D, 0x27, 0x3F };
        for (size_t a_idx = 0; a_idx < sizeof(TARGET_ADDRS); a_idx++) {
            uint8_t a = TARGET_ADDRS[a_idx];
            i2c_cmd_handle_t cmd = i2c_cmd_link_create();
            i2c_master_start(cmd);
            i2c_master_write_byte(cmd, (a << 1) | I2C_MASTER_WRITE, true);
            i2c_master_stop(cmd);
            esp_err_t ret = i2c_master_cmd_begin(OLED_I2C_PORT, cmd, pdMS_TO_TICKS(30));
            i2c_cmd_link_delete(cmd);

            ESP_LOGI(TAG_DISP, "Probe 0x%02X on %s: ret=%s [POST: SDA=%d SCL=%d]",
                     a, CANDIDATES[c].desc, esp_err_to_name(ret),
                     gpio_get_level(CANDIDATES[c].sda), gpio_get_level(CANDIDATES[c].scl));

            if (ret == ESP_OK) {
                s_oled_addr = a;
                oled_found = true;
                ESP_LOGI(TAG_DISP, "--> I2C DEVICE FOUND on %s at address 0x%02X!",
                         CANDIDATES[c].desc, a);
                break;
            }
        }
        if (oled_found) break;
    }

    if (!oled_found) {
        ssd1306_delete_driver();
        ESP_LOGW(TAG_DISP, "No I2C device on any pins — SDA(GPIO21)=%d SCL(GPIO19)=%d",
                 sda_lvl, scl_lvl);
        return;
    }

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
            ESP_LOGE(TAG_DISP, "SSD1306 init cmd 0x%02X failed", init_cmds[i]);
        }
    }

    s_oled_ready = true;
    ssd_clear(0x00);
    ssd_flush();
    ESP_LOGI(TAG_DISP, "SSD1306 ready: 128x64  I2C  addr=0x%02X", s_oled_addr);

    // Show "Booting..." immediately, before display_task is running
    oled_show_status(DISP_BOOTING, "");
}

extern "C" void display_show_provisioning(void) {
    oled_show_status(DISP_PROVISIONING, "AP: HeyVaani-Setup\nIP: 192.168.4.1");
}

// ─── Mic Self-Check (I2S) ────────────────────────────────────────────────────
// Reads audio using the real 32-bit-to-16-bit de-interleave pipeline for both
// Mic A (I2S_NUM_0) and Mic B (I2S_NUM_1), reporting actual RMS energy.
static void mic_selfcheck() {
    int16_t samples[480];
    int64_t sum_sq_a = 0;
    int count_a = 0;
    // Read ~150 ms of audio from Mic A
    for (int rep = 0; rep < 5; rep++) {
        int n = i2s_read_pcm(samples, 480);
        if (n > 0) {
            for (int i = 0; i < n; i++) sum_sq_a += (int64_t)samples[i] * samples[i];
            count_a += n;
        }
    }
    float rms_a = count_a > 0 ? sqrtf((float)sum_sq_a / count_a) / 32768.0f : 0.0f;
    s_mic_a_rms = rms_a;
    ESP_LOGI(TAG_MIC, "Mic A (I2S_0, SD=GPIO%d SCK=GPIO%d WS=GPIO%d): RMS=%.5f (%s)",
             I2S_SD_PIN, I2S_SCK_PIN, I2S_WS_PIN, (double)rms_a,
             rms_a > 0.0005f ? "ACTIVE" : "SILENT (check wiring/power)");
    if (s_stereo_rx_buf_a) {
        printf("[RAW-MIC-A] L0=%ld (>>16:%d) R0=%ld (>>16:%d) L1=%ld R1=%ld\n",
               (long)s_stereo_rx_buf_a[0], (int)(s_stereo_rx_buf_a[0] >> 16),
               (long)s_stereo_rx_buf_a[1], (int)(s_stereo_rx_buf_a[1] >> 16),
               (long)s_stereo_rx_buf_a[2], (long)s_stereo_rx_buf_a[3]);
        fflush(stdout);
    }

    if (g_mic_b_ok && g_i2s_rx_b != NULL) {
        int64_t sum_sq_b = 0;
        int count_b = 0;
        for (int rep = 0; rep < 5; rep++) {
            int n = i2s_read_pcm_from(g_i2s_rx_b, samples, 480);
            if (n > 0) {
                for (int i = 0; i < n; i++) sum_sq_b += (int64_t)samples[i] * samples[i];
                count_b += n;
            }
        }
        float rms_b = count_b > 0 ? sqrtf((float)sum_sq_b / count_b) / 32768.0f : 0.0f;
        s_mic_b_rms = rms_b;
        // In quiet room ambient RMS is naturally < 0.0005f — do NOT disable g_mic_b_ok!
        ESP_LOGI(TAG_MIC, "Mic B (I2S_1, SD=GPIO%d SCK=GPIO%d WS=GPIO%d): RMS=%.5f (%s)",
                 I2S_SD_PIN_B, I2S_SCK_PIN_B, I2S_WS_PIN_B, (double)rms_b,
                 rms_b > 0.0005f ? "ACTIVE" : "SILENT");
        if (s_stereo_rx_buf_b) {
            printf("[RAW-MIC-B] L0=%ld (>>16:%d) R0=%ld (>>16:%d) L1=%ld R1=%ld\n",
                   (long)s_stereo_rx_buf_b[0], (int)(s_stereo_rx_buf_b[0] >> 16),
                   (long)s_stereo_rx_buf_b[1], (int)(s_stereo_rx_buf_b[1] >> 16),
                   (long)s_stereo_rx_buf_b[2], (long)s_stereo_rx_buf_b[3]);
            fflush(stdout);
        }
    } else {
        ESP_LOGW(TAG_MIC, "Mic B (I2S_1): not active or not initialized");
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
    static int16_t hop[480];      // Mic A — HP-filtered 16-bit mono
    static int16_t hop_b[480];    // Mic B — HP-filtered 16-bit mono (beamforming)

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

        // Live Mic A RMS calculation
        {
            int64_t sum_sq_a = 0;
            for (int i = 0; i < HOP; i++) sum_sq_a += (int64_t)hop[i] * hop[i];
            s_mic_a_rms = sqrtf((float)sum_sq_a / HOP) / 32768.0f;
        }

        // ── Mic B: decode + HP-filter into hop_b[], update RMS, then beamform ──
        // Delay-and-Sum beamforming (τ=0): both INMP441s are mounted close together
        // so inter-mic delay is sub-sample. Simple arithmetic mean:
        //   out[i] = (mic_a[i] + mic_b[i]) / 2
        // → speech (correlated)  amplitude unchanged,  noise (uncorrelated) ÷√2 (~3 dB SNR gain)
        bool beamform_ok = false;
        if (g_mic_b_ok && g_i2s_rx_b != NULL && s_stereo_rx_buf_b != NULL) {
            size_t br_b = 0;
            esp_err_t eb = i2s_channel_read(g_i2s_rx_b, s_stereo_rx_buf_b,
                                            HOP * 2 * sizeof(int32_t),
                                            &br_b, pdMS_TO_TICKS(30));
            if (eb == ESP_OK && br_b >= (size_t)HOP * 2 * sizeof(int32_t)) {
                int64_t sum_sq_b = 0;
                static float x1_b = 0.0f, x2_b = 0.0f, y1_b = 0.0f, y2_b = 0.0f;
                const float b0 = 0.959203f, b1 = -1.918406f, b2 = 0.959203f;
                const float a1 = -1.916741f, a2 = 0.920071f;
                for (int i = 0; i < HOP * 2; i += 2) {
                    int32_t s_l = s_stereo_rx_buf_b[i];
                    int32_t s_r = s_stereo_rx_buf_b[i + 1];
                    int32_t s32 = (labs(s_l) >= labs(s_r)) ? s_l : s_r;
                    float x0 = (float)(s32 >> 16);
                    float y0 = b0 * x0 + b1 * x1_b + b2 * x2_b - a1 * y1_b - a2 * y2_b;
                    x2_b = x1_b; x1_b = x0;
                    y2_b = y1_b; y1_b = y0;
                    int16_t s16 = (int16_t)(y0 > 32767.0f ? 32767.0f : (y0 < -32768.0f ? -32768.0f : y0));
                    int idx = i / 2;
                    if (idx < HOP) hop_b[idx] = s16;
                    sum_sq_b += (int64_t)s16 * s16;
                }
                s_mic_b_rms = sqrtf((float)sum_sq_b / HOP) / 32768.0f;
                beamform_ok = true;

                // ── Apply Delay-and-Sum: average A and B ONLY IF both mics active and in-phase ──────────
                // Guard: Mic A is the primary hardware mic. Never let floating/unwired Mic B override Mic A!
                if (s_mic_a_rms > 0.006f && s_mic_b_rms > 0.006f) {
                    float ratio = s_mic_b_rms / s_mic_a_rms;
                    if (ratio >= 0.4f && ratio <= 2.5f) {
                        int64_t dot = 0;
                        for (int i = 0; i < HOP; i++) dot += ((int32_t)hop[i] * (int32_t)hop_b[i]);
                        if (dot > 0) {
                            for (int i = 0; i < HOP; i++) {
                                int32_t avg = ((int32_t)hop[i] + (int32_t)hop_b[i]) >> 1;
                                hop[i] = (int16_t)(avg > 32767 ? 32767 : (avg < -32768 ? -32768 : avg));
                            }
                        }
                    }
                }
                // Mic A is ALWAYS authoritative; if Mic B is floating/disconnected, hop[] stays Mic A!
            }
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
    audio_window = (int16_t*)malloc(AUDIO_BUFFER_SAMPLES * sizeof(int16_t));
    if (!audio_window) { ESP_LOGE(TAG_INF, "FATAL: audio_window malloc failed"); vTaskDelay(portMAX_DELAY); return; }

    printf("[INFERENCE] Started. VAD: MIN_RMS=%.4f MIN_PEAK=%d SNR>=%.1fdB mult=%.2f SOFT=%.2f HARD=%.2f HITS=%d/%d\n",
           (double)MIN_SPEECH_RMS, MIN_PEAK_SAMPLE, (double)MIN_SNR_DB,
           (double)NOISE_FLOOR_MULTIPLIER,
           (double)DETECT_THRESHOLD, (double)DETECT_HARD_THRESHOLD,
           DETECTION_HITS_REQUIRED, DETECTION_WINDOW_FRAMES);
    fflush(stdout);

    uint32_t infer_count         = 0;
    int      soft_hit_count      = 0;   // hits ≥ DETECT_THRESHOLD in window
    float    noise_floor_rms     = 0.0f;
    int      noise_cal_frames    = 0;
    int      last_read_pos       = 0;
    // Sliding window of recent confidences (oldest → newest ring).
    // Used for soft-tier hit counting so single-frame noise cannot fire,
    // while phoneme dips between "Hey"/"Vaani" do not wipe progress.
    float    conf_window[DETECTION_WINDOW_FRAMES];
    int      conf_win_fill       = 0;   // how many slots are valid (≤ WINDOW)
    int      conf_win_idx        = 0;   // next write index
    memset(conf_window, 0, sizeof(conf_window));
    // Post-cooldown recalibration: re-learn noise floor for ~1 s after cooldown expires
    bool     post_cooldown_recal = false;
    int      recal_frames_done   = 0;
    // Cycle timing for [CYCLE] log — g_cycle_start_us is file-scope (set here, read in streaming_task)

    while (true) {

        // 1. Idle while streaming or in command-capture mode
        if (streaming_active || g_sys_state != SYS_LISTENING) {
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }

        // 1b. Idle during post-detection cooldown to prevent re-trigger loops.
        {
            int64_t now = esp_timer_get_time();
            if (now < g_cooldown_end_us) {
                // Still in cooldown — stay gated
                soft_hit_count = 0;
                conf_win_fill  = 0;
                conf_win_idx   = 0;
                memset(conf_window, 0, sizeof(conf_window));
                vTaskDelay(pdMS_TO_TICKS(20)); continue;
            }
            if (g_cooldown_end_us != 0) {
                // Cooldown expired — reset sliding window cleanly so inference starts fresh
                g_cooldown_end_us = 0;
                soft_hit_count    = 0;
                conf_win_fill     = 0;
                conf_win_idx      = 0;
                memset(conf_window, 0, sizeof(conf_window));
            }
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

        // 3. RMS + peak (dual VAD gates)
        int64_t sum_sq = 0;
        int16_t peak_abs = 0;
        for (int i = 0; i < hop_samples; i++) {
            sum_sq += (int64_t)hop_buf[i] * hop_buf[i];
            int16_t a = hop_buf[i] < 0 ? -hop_buf[i] : hop_buf[i];
            if (a > peak_abs) peak_abs = a;
        }
        float rms    = sqrtf((float)sum_sq / hop_samples) / 32768.0f;
        telemetry_mic_rms = rms;

        // 4. Noise calibration (first 35 frames ~ 1 s)
        if (noise_cal_frames < NOISE_CALIBRATION_FRAMES) {
            noise_cal_frames++;
            if (rms < NOISE_CAL_MAX_RMS) {
                noise_floor_rms += (rms - noise_floor_rms) / (float)noise_cal_frames;
                if (noise_floor_rms > 0.025f) noise_floor_rms = 0.025f;
                telemetry_noise_floor_rms = noise_floor_rms;
            }
            if (noise_cal_frames < 25) {
                soft_hit_count = 0;
                conf_win_fill  = 0;
                conf_win_idx   = 0;
                vTaskDelay(pdMS_TO_TICKS(10)); continue;
            }
        }

        // Unconditional 1s heartbeat log so live microphone energy is always visible on serial
        {
            static int64_t last_rms_log = 0;
            int64_t now_log = esp_timer_get_time();
            if (now_log - last_rms_log >= 1000000) {
                last_rms_log = now_log;
                printf("[AUDIO] mic1_rms=%.5f mic2_rms=%.5f floor=%.5f peak=%d s[0..3]=%d,%d,%d,%d\n",
                       (double)rms, (double)s_mic_b_rms, (double)noise_floor_rms, (int)peak_abs,
                       hop_buf[0], hop_buf[1], hop_buf[2], hop_buf[3]);
                fflush(stdout);
            }
        }

        // 5. Speech VAD gate — reject ambient silence and low-level noise
        if (noise_floor_rms > 0.030f) noise_floor_rms = 0.030f;
        if (noise_floor_rms < 0.002f) noise_floor_rms = 0.002f;
        float speech_thr = fmaxf(MIN_SPEECH_RMS, noise_floor_rms * NOISE_FLOOR_MULTIPLIER);
        bool  speech_ok  = (rms >= speech_thr && peak_abs >= MIN_PEAK_SAMPLE) || (rms >= 0.016f);

        static int consecutive_hits = 0;
        static int consecutive_vad_misses = 0;
        if (!speech_ok) {
            consecutive_hits = 0;
            // Update noise floor smoothly during silence
            noise_floor_rms = 0.02f * rms + 0.98f * noise_floor_rms;
            if (noise_floor_rms > 0.030f) noise_floor_rms = 0.030f;
            telemetry_noise_floor_rms = noise_floor_rms;
            telemetry_keyword_confidence = 0.0f;

            // Soft-tier window: push 0 on silence so stale detections age out smoothly
            conf_window[conf_win_idx] = 0.0f;
            conf_win_idx = (conf_win_idx + 1) % DETECTION_WINDOW_FRAMES;
            if (conf_win_fill < DETECTION_WINDOW_FRAMES) conf_win_fill++;
            soft_hit_count = 0;
            for (int i = 0; i < conf_win_fill; i++)
                if (conf_window[i] >= DETECT_THRESHOLD) soft_hit_count++;

            consecutive_vad_misses++;
            if (consecutive_vad_misses >= 3) {
                soft_hit_count = 0;
                conf_win_fill  = 0;
                conf_win_idx   = 0;
                memset(conf_window, 0, sizeof(conf_window));
            }
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }

        // 6. Linearize ring → 1-second window + MFCC
        consecutive_vad_misses = 0;  // VAD passed — reset miss counter
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

        infer_count++; telemetry_inference_count = telemetry_inference_count + 1;
        telemetry_inference_ms      += infer_us / 1000.0f;
        telemetry_last_infer_ms      = infer_us / 1000.0f;
        telemetry_keyword_confidence = kw_prob;

        // Post-cooldown recal: just count frames and update floor, don't trigger
        if (post_cooldown_recal) {
            if (rms < NOISE_CAL_MAX_RMS) {
                noise_floor_rms += (rms - noise_floor_rms) / (float)(recal_frames_done + 1);
                if (noise_floor_rms > 0.025f) noise_floor_rms = 0.025f;
                telemetry_noise_floor_rms = noise_floor_rms;
            }
            recal_frames_done++;
            soft_hit_count = 0;
            conf_win_fill  = 0;
            conf_win_idx   = 0;
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }

        // ── Sliding-window confidence history (soft + hard tier) ─────────────
        conf_window[conf_win_idx] = kw_prob;
        conf_win_idx = (conf_win_idx + 1) % DETECTION_WINDOW_FRAMES;
        if (conf_win_fill < DETECTION_WINDOW_FRAMES) conf_win_fill++;

        soft_hit_count = 0;
        float conf_peak = 0.0f;
        for (int i = 0; i < conf_win_fill; i++) {
            if (conf_window[i] >= DETECT_THRESHOLD) soft_hit_count++;
            if (conf_window[i] > conf_peak) conf_peak = conf_window[i];
        }

        // Multi-frame verification: A real "Hey Vaani" utterance spans 600-800ms.
        // It produces sustained high confidence across multiple frames.
        // Requiring at least 2 consecutive frames meeting confidence + speech RMS
        // eliminates single-frame false triggers from transient noise or background words.
        if (kw_prob >= DETECT_THRESHOLD && rms >= MIN_SPEECH_RMS) {
            consecutive_hits++;
        } else {
            consecutive_hits = 0;
        }

        bool trigger = (consecutive_hits >= DETECTION_HITS_REQUIRED);

        // Periodic confidence log every 500 ms for visibility on serial & dashboard
        {
            static int64_t last_log_us = 0;
            int64_t now = esp_timer_get_time();
            if (now - last_log_us >= 500000) {
                last_log_us = now;
                float uptime_s = (float)(now / 1000);
                float cpu_pct = uptime_s > 0 ? (telemetry_inference_ms / uptime_s) * 100.0f : 0.0f;
                printf("[KWS] conf=%.4f peak=%.4f soft=%d/%d hard=%.2f thr=%.2f infer=%.0fus mic=%.4f floor=%.4f cpu=%.1f%%\n",
                       (double)kw_prob, (double)conf_peak,
                       soft_hit_count, DETECTION_WINDOW_FRAMES,
                       (double)DETECT_HARD_THRESHOLD, (double)DETECT_THRESHOLD,
                       (double)infer_us,
                       (double)telemetry_mic_rms, (double)telemetry_noise_floor_rms,
                       (double)cpu_pct);
                fflush(stdout);
            }
        }

        if (trigger) {
            int64_t kw_end = esp_timer_get_time();

            printf("[TRIGGER-HIT] node=%d hits=%d conf=%.4f peak=%.4f (thr=%.2f) rms=%.4f\n",
                   NODE_ID, consecutive_hits,
                   (double)kw_prob, (double)conf_peak,
                   (double)DETECT_THRESHOLD, (double)rms);
            fflush(stdout);

            float final_conf = kw_prob;
            if (g_mic_b_ok && g_i2s_rx_b != NULL) {
                printf("[DUAL-MIC] node=%d mic_a=%.4f (RMS: A=%.4f B=%.4f)\n",
                       NODE_ID, (double)kw_prob, (double)s_mic_a_rms, (double)s_mic_b_rms);
                fflush(stdout);
            }

            // Record cycle start time for [CYCLE] log
            g_cycle_start_us = kw_end;

            // ── ESP-NOW Fusion gate (handoff only — never blocks local-only) ──
            bool should_stream = esp_now_fusion_should_trigger(final_conf, rms, NODE_ID);

            // Reset window and consecutive hit counter BEFORE state changes
            consecutive_hits = 0;
            soft_hit_count = 0;
            conf_win_fill  = 0;
            conf_win_idx   = 0;
            memset(conf_window, 0, sizeof(conf_window));

            if (should_stream && !streaming_active) {
                // ── Confirmed detection: enter capture + stream path ─────────
                g_sys_state  = SYS_CAPTURE_COMMAND;
                g_sys_capture_start_us = esp_timer_get_time(); // arm sys_state watchdog
                set_led_state(true);
                g_disp_state = DISP_DETECTED;
                oled_show_status(DISP_DETECTED, "Hey Vaani!");
                g_stream_start_pos = (int)ring_write_pos;
                xQueueSend(detect_queue, &kw_end, 0);
            } else if (should_stream && streaming_active) {
                // Overlapping trigger while stream already running — discard cleanly.
                printf("[TRIGGER-SKIP] node=%d trigger while streaming active — resetting state\n", NODE_ID);
                fflush(stdout);
                set_led_state(false);
                g_sys_state  = SYS_LISTENING;
                g_disp_state = DISP_LISTENING;
                g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
            } else {
                // Fusion handoff: peer won — do NOT show "Detected!" UI.
                printf("[HANDOFF] node=%d suppressed by fusion handoff (peer streams)\n", NODE_ID);
                fflush(stdout);
                set_led_state(false);
                g_sys_state  = SYS_LISTENING;
                g_disp_state = DISP_LISTENING;
                // Short cooldown only — no 1.2 s stall, no ring flush on handoff loss.
                g_cooldown_end_us = esp_timer_get_time() + (int64_t)(COOLDOWN_MS / 2) * 1000;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        // Yield 15ms so FreeRTOS IDLE0 task can run, feeding the task watchdog and preventing watchdog panic
        vTaskDelay(pdMS_TO_TICKS(15));
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
        ESP_LOGI(TAG_STR, "[%lu] Keyword confirmed — streaming command to server", (unsigned long)sid);

        // WiFi is kept alive continuously by wifi_keepalive_task
        if (!wifi_connected) {
            oled_show_status(DISP_DETECTED, "Wake Word OK!\n(WiFi offline)");
            vTaskDelay(pdMS_TO_TICKS(2000));
            set_led_state(false);
            flush_audio_ring();
            g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
            g_sys_state = SYS_LISTENING;
            g_disp_state = DISP_LISTENING;
            continue;
        }

        // Immediately set streaming active to gate inference and pause telemetry
        set_streaming_active(true, "keyword_detected");

        // ── Step 1: Prompt user and connect to server concurrently ───────────
        oled_show_status(DISP_DETECTED, "Hey Vaani!\nSpeak cmd...");
        printf("[STREAM] Wake word detected! Connecting to %s:%d...\n", g_server_ip, CONFIG_SERVER_PORT); fflush(stdout);

        int last_pos = (int)ring_write_pos;
        int sock = -1; uint32_t kw_ms = 0;
        struct sockaddr_in srv = {};
        srv.sin_family = AF_INET; srv.sin_port = htons(CONFIG_SERVER_PORT);
        inet_pton(AF_INET, g_server_ip, &srv.sin_addr);

        for (int attempt = 0; attempt < 2; attempt++) {
            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            int f = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &f, sizeof(f));

            // Non-blocking connect with 1.5-second timeout
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);
            connect(sock, (struct sockaddr*)&srv, sizeof(srv));

            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);
            struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
            if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0) {
                    fcntl(sock, F_SETFL, flags);
                    struct timeval rw_tv = { .tv_sec = 8, .tv_usec = 0 };
                    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &rw_tv, sizeof(rw_tv));
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rw_tv, sizeof(rw_tv));
                    kw_ms = (uint32_t)((esp_timer_get_time() - keyword_end_us) / 1000);
                    printf("[STREAM] Connected OK (kw_to_connect=%lums)\n", (unsigned long)kw_ms); fflush(stdout);
                    break;
                }
            }
            close(sock); sock = -1;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (sock < 0) {
            printf("[STREAM] Server unreachable — aborting.\n"); fflush(stdout);
            oled_show_status(DISP_DETECTED, "Server\nOffline!");
            vTaskDelay(pdMS_TO_TICKS(2000));
            set_streaming_active(false, "no_server");
            set_led_state(false);
            flush_audio_ring();
            g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
            g_sys_state    = SYS_LISTENING;
            g_disp_state   = DISP_LISTENING;
            continue;
        }

        g_disp_state = DISP_STREAMING;
        oled_show_status(DISP_STREAMING, "Listening...");

        // ── Step 3: Send HVP1 header + zero nonce ─────────────────────────────
        const int CHUNK = 480; int16_t pcm[CHUNK];
        hvp1_header_t hdr = { .magic = MAGIC_NUMBER, .sample_rate = I2S_SAMPLE_RATE,
                               .channels = 1, .bits = 16,
                               .audio_len = 0, .kw_to_connect_ms = kw_ms };
        {
            uint8_t* hp = (uint8_t*)&hdr; size_t hl = sizeof(hdr);
            while (hl > 0) { ssize_t n = send(sock, hp, hl, 0); if (n < 0) break; hp += n; hl -= (size_t)n; }
        }
        {
            uint8_t zero_nonce[16] = {0};
            send(sock, zero_nonce, sizeof(zero_nonce), 0);
        }

        // ── Step 4: Stream command audio — silence-aware adaptive duration ────
        printf("[STREAM] Streaming command audio...\n"); fflush(stdout);
        int total_sent = 0, streamed_ms = 0;
        int silence_ms = 0;
        bool speech_detected = false;
        const int MAX_STREAM_MS   = COMMAND_DURATION_MS;       // 6000ms hard cap
        const int MIN_STREAM_MS   = COMMAND_MIN_DURATION_MS;   // 2000ms minimum
        const int SILENCE_GATE_MS = COMMAND_SILENCE_MS;        // 1500ms
        // VAD threshold for command speech: speech is generally >0.020f
        const float CMD_RMS_GATE  = fmaxf(0.015f, telemetry_noise_floor_rms * 1.5f);
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(MAX_STREAM_MS);

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

            // Compute chunk RMS to detect silence vs voice
            int64_t chunk_sq = 0;
            for (int i = 0; i < CHUNK; i++) chunk_sq += (int64_t)pcm[i] * pcm[i];
            float chunk_rms = sqrtf((float)chunk_sq / CHUNK) / 32768.0f;
            int chunk_ms    = CHUNK * 1000 / I2S_SAMPLE_RATE;  // ~30 ms

            if (chunk_rms >= CMD_RMS_GATE) {
                speech_detected = true;
                silence_ms = 0;  // voice detected — reset silence counter
            } else if (speech_detected) {
                silence_ms += chunk_ms;
            }

            uint8_t* send_ptr = (uint8_t*)pcm; size_t send_len = CHUNK * sizeof(int16_t);
            bool send_err = false;
            while (send_len > 0) {
                ssize_t n = send(sock, send_ptr, send_len, 0);
                if (n < 0) { printf("[SEND-ERROR] errno=%d\n", errno); fflush(stdout); send_err = true; break; }
                send_ptr += n; send_len -= (size_t)n;
            }
            if (send_err) break;
            total_sent += CHUNK;
            streamed_ms += chunk_ms;

            // Early close: only after speech has been heard, minimum stream time met, and silence follows
            if (speech_detected && streamed_ms >= MIN_STREAM_MS && silence_ms >= SILENCE_GATE_MS) {
                printf("[STREAM] Speech ended (silence %dms) after %dms — finishing stream.\n",
                       silence_ms, streamed_ms); fflush(stdout);
                break;
            }
        }
        printf("[STREAM] Done: sent %d samples (%d ms)\n", total_sent, streamed_ms); fflush(stdout);

        // Send EOS sentinel
        static const char EOS_MARKER[] = "EOS!";
        send(sock, EOS_MARKER, sizeof(EOS_MARKER) - 1, 0);
        set_streaming_active(false, "stream_done");
        shutdown(sock, SHUT_WR);

        // ── Step 5: Show "Processing..." while server transcribes ────────────
        oled_show_status(DISP_STREAMING, "Processing...");
        printf("[STREAM] Waiting for transcription response...\n"); fflush(stdout);

        // ── Step 6: Receive JSON response from server ─────────────────────────
        char resp[1024] = {0}; int rlen = 0, r;
        struct timeval rcv_tv = { .tv_sec = 12, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
        while ((r = recv(sock, resp + rlen, sizeof(resp) - rlen - 1, 0)) > 0) rlen += r;
        close(sock);
        resp[rlen] = '\0';
        printf("[STREAM] Response (%d bytes): %.*s\n", rlen, rlen > 300 ? 300 : rlen, resp); fflush(stdout);

        // ── Step 7: Parse transcript + vaani_response + display on OLED ─────
        char result_txt[128]  = "No response";
        char vaani_reply[128] = "";
        bool cmd_confirmed    = false;
        if (rlen > 0) {
            cJSON* root = cJSON_ParseWithLength(resp, (size_t)rlen);
            if (root) {
                // Primary: show vaani_response (server-generated natural reply)
                cJSON* jresp = cJSON_GetObjectItemCaseSensitive(root, "vaani_response");
                cJSON* jt    = cJSON_GetObjectItemCaseSensitive(root, "transcript");
                cJSON* jv    = cJSON_GetObjectItemCaseSensitive(root, "verified");
                if (cJSON_IsBool(jv)) cmd_confirmed = cJSON_IsTrue(jv);
                if (cJSON_IsString(jresp) && jresp->valuestring && strlen(jresp->valuestring) > 0) {
                    snprintf(vaani_reply, sizeof(vaani_reply), "%s", jresp->valuestring);
                    snprintf(result_txt, sizeof(result_txt), "%s", jresp->valuestring);
                } else if (cJSON_IsString(jt) && jt->valuestring && strlen(jt->valuestring) > 0) {
                    snprintf(result_txt, sizeof(result_txt), "%s", jt->valuestring);
                }
                cJSON_Delete(root);
            } else {
                snprintf(result_txt, sizeof(result_txt), "%.127s", resp);
            }
        }
        printf("[RESULT] confirmed=%d reply='%s'\n", (int)cmd_confirmed, result_txt); fflush(stdout);

        if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            snprintf(g_disp_text, sizeof(g_disp_text), "%s", result_txt);
            xSemaphoreGive(disp_mutex);
        }
        g_disp_state = DISP_TRANSCRIBED;
        set_led_state(false);
        printf("[RESULT-DISP] Showing '%s' on OLED\n", result_txt); fflush(stdout);

        // Show confirmed command for 1.8s, or brief 800ms for unconfirmed/noise
        vTaskDelay(pdMS_TO_TICKS(cmd_confirmed ? 1800 : 800));

        // ── Return to listening state with fast cooldown ────────────────────
        {
            int64_t cycle_end_us  = esp_timer_get_time();
            int64_t cycle_total_ms = (cycle_end_us - g_cycle_start_us) / 1000;
            printf("[CYCLE] node=%d session=%lu total_cycle_ms=%lld (cooldown=%dms)\n",
                   NODE_ID, (unsigned long)session_id, (long long)cycle_total_ms, COOLDOWN_MS);
            fflush(stdout);
        }
        set_led_state(false);
        g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
        g_sys_state    = SYS_LISTENING;
        g_disp_state   = DISP_LISTENING;
        g_disp_text[0] = '\0';
        if (detect_queue) xQueueReset(detect_queue);
    }
}

// ============================================================================
// TASK 3a: wifi_keepalive_task — connects WiFi at boot (Core 1) and keeps it
// alive. Running on Core 1 avoids IWDT clash with inference_task on Core 0.
// streaming_task checks wifi_connected and skips wifi_start() if already up.
// ============================================================================
static void wifi_keepalive_task(void* arg) {
    ESP_LOGI(TAG_WIFI, "WiFi keepalive task started on core %d", xPortGetCoreID());
    if (wifi_connected)
        ESP_LOGI(TAG_WIFI, "WiFi ready (keepalive).");
    else
        ESP_LOGW(TAG_WIFI, "WiFi not connected at boot — will retry.");

    // ── Reconnection loop ────────────────────────────────────────────────
    // Keeps retrying WiFi in background without wiping credentials or rebooting.
    int fail_streak = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!wifi_connected) {
            fail_streak++;
            if (fail_streak % 5 == 1) {
                ESP_LOGW(TAG_WIFI, "WiFi disconnected — background reconnect (attempt %d)...", fail_streak);
            }
            wifi_start();
            if (wifi_connected) {
                fail_streak = 0;
                ESP_LOGI(TAG_WIFI, "WiFi reconnected successfully!");
            }
        } else {
            fail_streak = 0;
        }
    }
}

// ============================================================================
// TASK 4: wifi_config_poll_task — checks for updated WiFi credentials from server.
// NOTE: server_ip is intentionally NEVER updated from the server response.
// Overwriting the server IP from the server itself causes a boot loop:
// local IP → cloud IP → device can no longer reach local server → broken.
// Only SSID/password may be updated remotely.
// ============================================================================
static void wifi_config_poll_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // Poll every 30 seconds (reduced frequency)
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
                        // NOTE: server_ip from JSON is intentionally IGNORED.
                        // The device always uses the IP stored in NVS at boot.
                        // This prevents a remote server from hijacking the local server IP.
                        
                        if (ssid && ssid->valuestring && pass && pass->valuestring) {
                            // Only update if SSID or password changed (not IP)
                            if (strcmp(ssid->valuestring, g_wifi_ssid) != 0 || 
                                strcmp(pass->valuestring, g_wifi_pass) != 0) {
                                
                                ESP_LOGI(TAG_MAIN, "[OTA-CFG] WiFi credentials updated: SSID %s -> %s",
                                         g_wifi_ssid, ssid->valuestring);
                                // Save with CURRENT server IP — never overwrite it
                                if (wifi_provision_save(ssid->valuestring, pass->valuestring, g_server_ip)) {
                                    ESP_LOGI(TAG_MAIN, "[OTA-CFG] NVS updated. Restarting ESP32...");
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
// Watchdog ceiling for g_sys_state stuck in SYS_CAPTURE_COMMAND (ms)
#define SYS_STATE_WATCHDOG_MS  25000

static void watchdog_task(void* arg) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // --- Existing watchdog: streaming_active stuck > STREAM_WATCHDOG_MS ---
        if (streaming_active && streaming_active_set_us != 0) {
            int64_t held = esp_timer_get_time() - streaming_active_set_us;
            if (held > (int64_t)STREAM_WATCHDOG_MS * 1000) {
                printf("[WDG] streaming_active stuck %.1fs — force clearing\n",
                       (double)held / 1e6);
                fflush(stdout);
                set_streaming_active(false, "watchdog_forced");
                g_sys_capture_start_us = 0; // clear sys watchdog arm too
                g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
                g_sys_state  = SYS_LISTENING;
                set_led_state(false);
                g_disp_state = DISP_LISTENING;
            }
        }

        // --- FIX 2: g_sys_state watchdog: SYS_CAPTURE_COMMAND stuck > 25 s ---
        // Covers the case where streaming never started (no WiFi, queue full, etc.)
        // and streaming_active was never set, leaving g_sys_state permanently stuck.
        if (g_sys_state == SYS_CAPTURE_COMMAND && g_sys_capture_start_us != 0) {
            int64_t stuck_us = esp_timer_get_time() - g_sys_capture_start_us;
            if (stuck_us > (int64_t)SYS_STATE_WATCHDOG_MS * 1000) {
                printf("[WDG] g_sys_state stuck in SYS_CAPTURE_COMMAND %.1fs — force reset\n",
                       (double)stuck_us / 1e6);
                fflush(stdout);
                g_sys_capture_start_us = 0;
                g_cooldown_end_us = esp_timer_get_time() + (int64_t)COOLDOWN_MS * 1000;
                g_sys_state  = SYS_LISTENING;
                set_led_state(false);
                g_disp_state = DISP_LISTENING;
            }
        } else if (g_sys_state != SYS_CAPTURE_COMMAND) {
            // State left SYS_CAPTURE_COMMAND normally — disarm the watchdog
            g_sys_capture_start_us = 0;
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
    disp_state_t last_state = (disp_state_t)(-1);
    char local_txt[256];
    int retry_cnt = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));

        if (!s_oled_ready) {
            retry_cnt++;
            if (retry_cnt >= 20) { // every 2 seconds re-try probe
                retry_cnt = 0;
                ssd1306_init();
            }
            continue;
        }
        disp_state_t cur = g_disp_state;

        // CHECKPOINT 5: always read g_disp_text under mutex
        if (cur == DISP_TRANSCRIBED || cur == DISP_PROMPT) {
            if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                strlcpy(local_txt, g_disp_text, sizeof(local_txt));
                xSemaphoreGive(disp_mutex);
            } else {
                strlcpy(local_txt, g_disp_text, sizeof(local_txt));
            }
            printf("[CHK5-DISP-READ] state=%d text='%s'\n", (int)cur, local_txt); fflush(stdout);
        }

        // Redraw on state change OR periodically (every 200 ms) during LISTENING to update live VU meter
        retry_cnt++;
        bool periodic_refresh = (cur == DISP_LISTENING && (retry_cnt >= 2));
        if (cur != last_state || cur == DISP_TRANSCRIBED || periodic_refresh) {
            if (periodic_refresh) {
                retry_cnt = 0;
            }
            if (cur != last_state) {
                local_txt[0] = '\0';
                if (cur == DISP_TRANSCRIBED || cur == DISP_PROMPT) {
                    if (xSemaphoreTake(disp_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        strlcpy(local_txt, g_disp_text, sizeof(local_txt));
                        xSemaphoreGive(disp_mutex);
                    } else {
                        strlcpy(local_txt, g_disp_text, sizeof(local_txt));
                    }
                }
            }
            oled_show_status(cur, local_txt);
            last_state = cur;
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
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!wifi_connected || streaming_active) continue;
        wifi_ap_record_t ap = {};
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        float total_ms = telemetry_inference_ms;
        float uptime_ms = (float)(esp_timer_get_time() / 1000);
        float cpu_cum_pct = uptime_ms > 0 ? (total_ms / uptime_ms) * 100.0f : 0.0f;
        float snr_db = (telemetry_mic_rms > 0.001f && telemetry_noise_floor_rms > 0.001f)
                       ? 20.0f * log10f(telemetry_mic_rms / telemetry_noise_floor_rms) : 0.0f;
        char body[768];
        int blen = snprintf(body, sizeof(body),
            "{\"device\":\"%s\",\"node_id\":%d,\"uptime_ms\":%llu,"
            "\"free_heap_bytes\":%lu,\"min_free_heap_bytes\":%lu,"
            "\"heap_total_bytes\":%lu,\"tflite_arena_bytes\":%u,"
            "\"audio_buffer_bytes\":%u,\"keyword_confidence\":%.4f,"
            "\"mic_rms\":%.5f,\"inference_count\":%lu,"
            "\"inference_duty_pct\":%.3f,\"wifi_rssi_dbm\":%d,\"streaming\":%s,"
            "\"latency_ms\":%.1f,\"cpu\":%.1f,\"snr\":%.1f,"
            "\"noise_floor_rms\":%.5f}",
            DEVICE_NAME, NODE_ID,
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
        addr.sin_family = AF_INET; addr.sin_port = htons(8080);
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
    printf("[BOOT] NODE_ID=%d (%s) SOFT=%.3f HARD=%.3f HITS=%d/%d COOLDOWN=%d ms  "
           "MODEL=%u bytes  heap_start=%lu\n",
           NODE_ID, DEVICE_NAME,
           (double)DETECT_THRESHOLD, (double)DETECT_HARD_THRESHOLD,
           DETECTION_HITS_REQUIRED, DETECTION_WINDOW_FRAMES, COOLDOWN_MS,
           (unsigned)g_model_data_len,
           (unsigned long)esp_get_free_heap_size());
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG_MAIN, "=== Hey Vaani Edge Firmware (SIH 2026) rev10 — Post-Cap Calibrated ===");
    ESP_ERROR_CHECK(nvs_flash_init());

    ring_mutex   = xSemaphoreCreateMutex();
    disp_mutex   = xSemaphoreCreateMutex();   // guards g_disp_text (issue #3 fix)
    detect_queue = xQueueCreate(4, sizeof(int64_t));

    // ── LEDs: output with 3-blink hardware self-test ────────────────────────
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(ONBOARD_LED_PIN);
    gpio_set_direction(ONBOARD_LED_PIN, GPIO_MODE_OUTPUT);
    for (int b = 0; b < 3; b++) {
        set_led_state(true);
        vTaskDelay(pdMS_TO_TICKS(120));
        set_led_state(false);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    ESP_LOGI(TAG_MAIN, "LEDs ready: External GPIO%d & Onboard GPIO%d", LED_PIN, ONBOARD_LED_PIN);

    // ── OLED: SSD1306 I2C init, immediately shows "Booting..." ──────────────────
    ssd1306_init();   // draws DISP_BOOTING screen at end

    // ── WiFi provisioning ────────────────────────────────────────────────────
    // Loads SSID/password/server-IP from NVS into g_wifi_ssid/pass/ip.
    // If NVS is empty (first boot / factory reset): starts "HeyVaani-Setup"
    // AP + captive portal, blocks here until credentials are saved, then
    // calls esp_restart(). Normal boots return instantly.
    wifi_provision_init();
    if (g_server_ip[0] == '\0') {
        strlcpy(g_server_ip, "13.233.100.83", sizeof(g_server_ip));  // AWS EC2 fallback
    }
    ESP_LOGI(TAG_MAIN, "Target Server IP: %s:%d", g_server_ip, CONFIG_SERVER_PORT);

    // ── WiFi Start (early so PHY RF calibration has unfragmented RAM) ──────
    wifi_start();
    esp_err_t fusion_err = esp_now_fusion_init(NODE_ID, NULL /* auto-discovery */);
    if (fusion_err != ESP_OK) {
        ESP_LOGW(TAG_MAIN, "ESP-NOW fusion init failed (%s) — single-node mode",
                 esp_err_to_name(fusion_err));
    } else {
        ESP_LOGI(TAG_MAIN, "ESP-NOW fusion ready (auto-discovery)");
    }

    // ── I2S: INMP441 Mic A ───────────────────────────────────────────────────
    i2s_global_init();

    // ── Mic B: second INMP441 for dual-mic inference (Part 2) ────────────────
    i2s_global_init_b();

    // ── Mic self-check: serial RMS logging confirms I2S data flow for both mics
    mic_selfcheck();

    // ── TFLite + MFCC ────────────────────────────────────────────────────────
    tflite_init();
    mfcc_proc.init();

    uint32_t heap_after_init = esp_get_free_heap_size();
    ESP_LOGI(TAG_MAIN, "Free heap after init: %u bytes", (unsigned)heap_after_init);

    // Core assignment:
    //   Core 0: inference, watchdog, display, stack_monitor, telemetry
    //   Core 1: audio (I2S drain), streaming (TCP/WiFi), wifi_keepalive
    // NOTE: wifi_keepalive_task on Core 1 connects WiFi at boot so streaming_task
    // finds WiFi already up with zero delay — avoids the 7+ s on-demand delay that
    // caused user commands to be missed. Must be Core 1 to avoid IWDT clash with
    // inference_task on Core 0.
    xTaskCreatePinnedToCore(audio_task,         "audio",       4096,  NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(inference_task,     "inference",   8192,  NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(streaming_task,     "streaming",   6144,  NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(wifi_keepalive_task,"wifi_ka",     4096,  NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(wifi_config_poll_task,"wifi_poll", 3072,  NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(watchdog_task,      "watchdog",    2048,  NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(display_task,       "display",     4096,  NULL, 1, NULL, 1); // [NEW] Moved to Core 1 to avoid I2C crash!
    xTaskCreatePinnedToCore(stack_monitor_task, "stack_mon",   2048,  NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(telemetry_task,     "telemetry",   4096,  NULL, 1, NULL, 0);
    benchmark_cpu_start();

    // display_task will detect this state change and draw "Listening" within 100 ms
    g_disp_state = DISP_LISTENING;
    ESP_LOGI(TAG_MAIN, "All tasks started. Listening for 'Hey Vaani'...");
}

