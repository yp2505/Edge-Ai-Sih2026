// main.cpp — Hey Vaani ESP32 Real Firmware (ESP-IDF)
//
// FIX LOG (2026-09-06 rev5):
//   1-6. (from rev3) WDT, RMS=0, ADC handle, SAMPLE-RATE, STREAM RACE fixes.
//   7. Removed duplicate noise calibration block (double-incremented noise_cal_frames).
//   8. Added SO_SNDTIMEO/SO_RCVTIMEO (5s) on streaming socket to prevent
//      send() blocking forever and trapping streaming_active=true.
//   9. streaming_active watchdog: force-clears if stuck true >10s ([WATCHDOG]).
//   10. Pipeline observability (rev4):
//       [ENERGY]  — unconditional RMS/peak every ~500ms (mic health)
//       [VAD]     — PASS/FAIL decisions (fail rate-limited to every 50th)
//       [INFER]   — raw confidence after every Invoke() (TRIGGER/no)
//       [STAGE-C] — pipeline stage breadcrumbs (linearize/MFCC/quantize/invoke)
//       [TRIGGER->STREAM] — detection handoff confirmation
//       [STREAM]  — TCP connect/send progress/recv diagnostics
//       [INIT]    — model/interpreter startup confirmation
//   11. RING BUFFER WRAP BUG FIX: inference_task avail calculation did not
//       handle wrap-around (ring_write_pos wraps 15999->0, making avail
//       massively negative). Added `if (avail < 0) avail += AUDIO_BUFFER_SAMPLES`
//       matching the streaming_task which already had this fix.
//
// MAX4466 Wiring:  OUT->GPIO32 (ADC1_CH4)  VCC->3.3V  GND->GND

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_continuous.h"
#include "hal/adc_types.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <errno.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "model_data.h"
#include "mfcc.h"
#include "benchmark_cpu.h"

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
#define ADC_SAMPLE_RATE    16000
#define ADC_RAW_RATE_HZ    20000   // DMA rate; decimated 4/5 -> 16000 Hz

// ─── TFLite Configuration ────────────────────────────────────────────────────
static const float DETECT_THRESHOLD         = 0.78f;  // lowered from 0.90 — model rarely exceeds 90%
static const int   DETECTION_HITS_REQUIRED  = 2;      // 2 consecutive hits (~60ms) is enough
static const float MIN_SPEECH_RMS           = 0.050f;
static const float NOISE_FLOOR_MULTIPLIER   = 2.5f;
static const int   NOISE_CALIBRATION_FRAMES = 50;
static const float NOISE_CAL_MAX_RMS        = 0.04f;  // only calibrate on truly silent frames
#define SLIDE_STEP_MS           30
#define COMMAND_DURATION_MS     4000
#define COMMAND_MIN_DURATION_MS 700
#define COMMAND_SILENCE_MS      450
#define AUDIO_BUFFER_SAMPLES    16000

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

// ─── WiFi ─────────────────────────────────────────────────────────────────────
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   10

static const char* TAG_INF  = "INFERENCE";
static const char* TAG_STR  = "STREAM";
static const char* TAG_MAIN = "MAIN";
static const char* TAG_WIFI = "WIFI";
static const char* TAG_MIC  = "MIC";

static volatile int  wifi_retry_count             = 0;
static volatile bool wifi_connected               = false;
static volatile bool streaming_active             = false;
static volatile int64_t streaming_active_set_us   = 0;  // timestamp when set true
static volatile uint32_t telemetry_inference_count = 0;
static volatile float    telemetry_inference_ms    = 0.0f;
static volatile float    telemetry_keyword_confidence = 0.0f;
static volatile float    telemetry_mic_rms          = 0.0f;
static volatile float    telemetry_noise_floor_rms  = 0.0f;

// ─── Audio Globals ────────────────────────────────────────────────────────────
static int16_t           audio_ring[AUDIO_BUFFER_SAMPLES];
static int16_t           audio_window[AUDIO_BUFFER_SAMPLES];
static int               ring_write_pos     = 0;
static int               valid_ring_samples = 0;   // updated by audio_task
static volatile uint32_t adc_dma_ok         = 0;
static volatile uint32_t adc_dma_err        = 0;
static SemaphoreHandle_t ring_mutex;

static QueueHandle_t     detect_queue;
static volatile uint32_t session_id = 0;

// ─── Tensor Arena ─────────────────────────────────────────────────────────────
static const size_t TENSOR_ARENA_SIZE = 32 * 1024;
static uint8_t tflite_arena[32 * 1024];

static tflite::MicroMutableOpResolver<12> resolver;
static tflite::MicroInterpreter*          interpreter   = nullptr;
static TfLiteTensor*                      input_tensor  = nullptr;
static TfLiteTensor*                      output_tensor = nullptr;

// ─── Single global ADC continuous (DMA) handle ───────────────────────────────
// Hardware-paced at ADC_RAW_RATE_HZ — exact timing, unlike oneshot polling.
static adc_continuous_handle_t g_adc = NULL;

// ─── DC offset + gain ─────────────────────────────────────────────────────────
static float     adc_dc_offset    = 2048.0f;
static const int ADC_TO_PCM_SHIFT = 6;   // 4× software gain

// ─── MFCC ─────────────────────────────────────────────────────────────────────
static MFCCProcessor mfcc_proc;
static float         mfcc_output[MFCC_OUTPUT_SIZE];

// ─── TFLite Init ──────────────────────────────────────────────────────────────
static void tflite_init() {
    const tflite::Model* model = tflite::GetModel(g_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG_INF, "TFLite schema version mismatch!");
        esp_restart();
    }
    ESP_LOGI(TAG_INF, "Model size: %u bytes (%.1f KB)", g_model_len, g_model_len / 1024.0f);
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddBatchMatMul();
    resolver.AddRelu();
    resolver.AddSoftmax();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddMean();
    resolver.AddLogistic();

    static tflite::MicroInterpreter static_interpreter(
        model, resolver, tflite_arena, TENSOR_ARENA_SIZE);
    interpreter = &static_interpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG_INF, "AllocateTensors() failed!");
        esp_restart();
    }
    input_tensor  = interpreter->input(0);
    output_tensor = interpreter->output(0);

    size_t used = interpreter->arena_used_bytes();
    ESP_LOGI(TAG_INF, "Arena used: %u bytes (%.1f KB), margin: %u bytes",
             (unsigned)used, used / 1024.0f, (unsigned)(TENSOR_ARENA_SIZE - used));
    ESP_LOGI(TAG_INF, "Model init OK: schema=%d, arena=%u/%u bytes, input=[%d], output=[%d]",
             TFLITE_SCHEMA_VERSION, (unsigned)used, (unsigned)TENSOR_ARENA_SIZE,
             input_tensor->dims->data[1], output_tensor->dims->data[1]);
    printf("[INIT] TFLite model ready: %u bytes model, %u/%u arena, threshold=%.2f, hits_required=%d\n",
           (unsigned)g_model_len, (unsigned)used, (unsigned)TENSOR_ARENA_SIZE,
           (double)DETECT_THRESHOLD, DETECTION_HITS_REQUIRED);
    fflush(stdout);
}

// ─── ADC Init — diag scan (oneshot), then the single global DMA handle ───────
static void adc_global_init() {
    // ── Scan ALL ADC1 channels to find live signal (helps detect wrong wiring) ──
    // Uses a TEMPORARY oneshot handle, deleted before continuous mode starts.
    {
        adc_oneshot_unit_handle_t tmp = NULL;
        adc_oneshot_unit_init_cfg_t init_cfg = {};
        init_cfg.unit_id  = ADC_UNIT_1;
        init_cfg.clk_src  = ADC_RTC_CLK_SRC_DEFAULT;
        init_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
        esp_err_t e = adc_oneshot_new_unit(&init_cfg, &tmp);
        if (e != ESP_OK || tmp == NULL) {
            ESP_LOGE(TAG_MAIN, "adc_oneshot_new_unit FAILED: %d — halting", e);
            while (1) vTaskDelay(1000);
        }

        // ADC1 channel map for ESP32: CH0=GPIO36, CH1=GPIO37, CH2=GPIO38,
        //   CH3=GPIO39, CH4=GPIO32, CH5=GPIO33, CH6=GPIO34, CH7=GPIO35
        const int gpio_for_ch[] = {36, 37, 38, 39, 32, 33, 34, 35};
        printf("\n[ADC-DIAG] Scanning all ADC1 channels (50 reads each):\n");
        printf("  Channel | GPIO | min  | max  | avg  | range | Status\n");
        printf("  --------+------+------+------+------+-------+--------\n");
        for (int ch = 0; ch <= 7; ch++) {
            adc_oneshot_chan_cfg_t scan_cfg = {};
            scan_cfg.atten    = ADC_ATTEN_DB_12;
            scan_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
            adc_oneshot_config_channel(tmp, (adc_channel_t)ch, &scan_cfg);
            int mn = 9999, mx = -1, sm = 0;
            for (int i = 0; i < 50; i++) {
                int raw = 0;
                adc_oneshot_read(tmp, (adc_channel_t)ch, &raw);
                if (raw < mn) mn = raw;
                if (raw > mx) mx = raw;
                sm += raw;
                esp_rom_delay_us(200);
            }
            int avg = sm / 50;
            int range = mx - mn;
            const char* status = (avg > 100 && avg < 3900) ? "<-- LIVE" :
                                  (avg == 0)               ? "dead/0V"  :
                                  (avg >= 3900)            ? "saturat." : "";
            printf("  CH%-6d | GP%-3d | %-4d | %-4d | %-4d | %-5d | %s\n",
                   ch, gpio_for_ch[ch], mn, mx, avg, range, status);
        }
        printf("[ADC-DIAG] Done. Expected: GPIO32 avg ~1800-2200, range >20.\n");
        printf("[ADC-DIAG] If another channel shows <-- LIVE, rewire mic OUT to that GPIO.\n\n");
        fflush(stdout);
        adc_oneshot_del_unit(tmp);
    }

    // ── Continuous (DMA) ADC at exactly ADC_RAW_RATE_HZ — hardware-timed ──────
    adc_continuous_handle_cfg_t hcfg = {};
    hcfg.max_store_buf_size = 8192;
    hcfg.conv_frame_size    = 1024;
    ESP_ERROR_CHECK(adc_continuous_new_handle(&hcfg, &g_adc));

    adc_digi_pattern_config_t pattern = {};
    pattern.atten     = ADC_ATTEN_DB_12;
    pattern.channel   = ADC_CHANNEL_4;   // GPIO32
    pattern.unit      = ADC_UNIT_1;
    pattern.bit_width = ADC_BITWIDTH_12;

    adc_continuous_config_t cfg = {};
    cfg.pattern_num     = 1;
    cfg.adc_pattern     = &pattern;
    cfg.sample_freq_hz  = ADC_RAW_RATE_HZ;
    cfg.conv_mode       = ADC_CONV_SINGLE_UNIT_1;
    cfg.format          = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    ESP_ERROR_CHECK(adc_continuous_config(g_adc, &cfg));
    ESP_ERROR_CHECK(adc_continuous_start(g_adc));
    ESP_LOGI(TAG_MAIN, "ADC MAX4466 initialized (DMA %d Hz, GPIO 32, handle=%p)",
             ADC_RAW_RATE_HZ, (void*)g_adc);

    // ── Measure the REAL DMA sample rate (trust nothing) ─────────────────────
    {
        static uint8_t probe[1024];
        // discard whatever accumulated during boot
        uint32_t discarded = 0;
        adc_continuous_read(g_adc, probe, sizeof(probe), &discarded, 100);

        int64_t t0 = esp_timer_get_time();
        uint32_t total = 0;
        while (total < 4000) {
            uint32_t n = 0;
            adc_continuous_read(g_adc, probe, sizeof(probe), &n, 1000);
            total += n / sizeof(adc_digi_output_data_t);
        }
        float secs = (float)(esp_timer_get_time() - t0) / 1e6f;
        float real_rate = (float)total / secs;
        printf("\n[ADC-RATE] Requested %d Hz, MEASURED %.0f Hz (%.1f%% of target)\n\n",
               ADC_RAW_RATE_HZ, (double)real_rate,
               (double)(100.0f * real_rate / ADC_RAW_RATE_HZ));
        fflush(stdout);
    }
}

// ─── Pull decimated, DC-centred, gained samples from the DMA stream ──────────
// Consumes the DMA stream CONTINUOUSLY (leftover conversions are held over to
// the next call, decimation phase persists) so the output is a gap-less
// 16 kHz PCM stream regardless of how often callers ask for data.
// Fills out_buf with out_count int16 samples. Returns samples written.
static int read_pcm_dma(int16_t* out_buf, int out_count,
                        uint32_t* out_err, uint32_t* out_ok) {
    static uint8_t  hold[4096];    // raw DMA bytes held between calls
    static int      hold_off = 0;  // read offset (conversions)
    static int      hold_len = 0;  // valid conversions in hold
    static int      decim_phase = 0;
    int out_idx = 0;
    int retry_cnt = 0;

    while (out_idx < out_count) {
        if (hold_off >= hold_len) {
            uint32_t out_len = 0;
            esp_err_t err = adc_continuous_read(g_adc, hold, sizeof(hold),
                                                &out_len, 100);
            if (err != ESP_OK || out_len == 0) {
                if (out_err) (*out_err)++;
                retry_cnt++;
                vTaskDelay(pdMS_TO_TICKS(2));
                if (retry_cnt > 20) {
                    // Give up — fill remainder with silence
                    while (out_idx < out_count) out_buf[out_idx++] = 0;
                    break;
                }
                continue;
            }
            retry_cnt = 0;
            hold_off = 0;
            hold_len = (int)(out_len / sizeof(adc_digi_output_data_t));
        }
        const adc_digi_output_data_t* p = (const adc_digi_output_data_t*)hold;
        while (hold_off < hold_len && out_idx < out_count) {
            int raw = (int)p[hold_off++].type2.data;
            if (out_ok) (*out_ok)++;

            adc_dc_offset = 0.005f * (float)raw + 0.995f * adc_dc_offset;
            int32_t centered = (int32_t)raw - (int32_t)adc_dc_offset;

            if (decim_phase != 4) {  // keep 4 of every 5 samples: 20k -> 16k
                int32_t s32 = centered * (1 << ADC_TO_PCM_SHIFT);
                if (s32 >  32767) s32 =  32767;
                if (s32 < -32768) s32 = -32768;
                out_buf[out_idx++] = (int16_t)s32;
            }
            decim_phase = (decim_phase + 1) % 5;
        }
    }
    return out_idx;
}

// ─── Mic Self-Check ───────────────────────────────────────────────────────────
static void mic_selfcheck() {
    const int  check_samples = 8000;
    static int16_t buf[8000];
    int64_t    sum_sq = 0;

    ESP_LOGI(TAG_MIC, "Mic self-check: reading 0.5s of audio...");
    int got = read_pcm_dma(buf, check_samples, NULL, NULL);
    if (got <= 0) {
        ESP_LOGE(TAG_MIC, "DMA read returned nothing — check ADC wiring");
        got = 1;
    }
    for (int i = 0; i < got; i++) sum_sq += (int64_t)buf[i] * buf[i];
    float rms = sqrtf((float)sum_sq / got) / 32768.0f;
    ESP_LOGI(TAG_MIC, "Mic RMS: %.5f", rms);

    if (rms < 1e-4f) {
        ESP_LOGE(TAG_MIC,
            "\n=== MIC SELF-CHECK FAILED ===\n"
            "No audio on GPIO 32.\n"
            "Check: MAX4466 OUT->GPIO32, VCC->3.3V, gain pot CW.\n"
            "Halting.\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG_MIC, "Mic self-check PASSED (RMS=%.5f)", rms);
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
            wifi_retry_count += 1;
            ESP_LOGW(TAG_WIFI, "WiFi lost — retry %d/%d", wifi_retry_count, WIFI_MAX_RETRIES);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0;
        wifi_connected   = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG_WIFI, "WiFi connected — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static void wifi_start() {
    if (wifi_connected) return;
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

    wifi_retry_count = 0;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT)
        ESP_LOGI(TAG_WIFI, "WiFi connected");
    else
        ESP_LOGW(TAG_WIFI, "WiFi not connected — streaming may fail");
}

static void wifi_stop() {
    if (!wifi_connected && wifi_retry_count == 0) return;
    esp_wifi_stop();
    esp_wifi_deinit();
    wifi_connected   = false;
    wifi_retry_count = 0;
    if (wifi_event_group)
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_LOGI(TAG_WIFI, "WiFi stopped");
}



// ─── TASK A: Inference (Core 0, always-on) ───────────────────────────────────
// Reads 30ms hops from the ring buffer (filled by audio_task). Never touches
// the ADC directly — audio_task is the only ADC consumer, so the DMA pool
// is always drained in real time and no samples are ever dropped.
static void inference_task(void* arg) {
    printf(">>> INFERENCE TASK STARTING <<<\n"); fflush(stdout);
    ESP_LOGI(TAG_INF, "Inference task started on core %d", xPortGetCoreID());

    const int hop_samples = ADC_SAMPLE_RATE * SLIDE_STEP_MS / 1000;  // 480
    int16_t*  hop_buf = (int16_t*)malloc(hop_samples * sizeof(int16_t));
    if (!hop_buf) {
        ESP_LOGE(TAG_INF, "FATAL: hop_buf malloc failed (heap=%lu)",
                 (unsigned long)esp_get_free_heap_size());
        vTaskDelay(portMAX_DELAY);
        return;
    }
    ESP_LOGI(TAG_INF, "Buffers OK. Free heap: %lu", (unsigned long)esp_get_free_heap_size());

    uint32_t infer_count        = 0;
    int      consecutive_hits   = 0;
    float    noise_floor_rms    = 0.0f;
    int      noise_cal_frames   = 0;
    uint32_t dbg_loop           = 0;
    int      last_read_pos      = 0;
    int64_t  last_stack_us      = 0;
    int64_t  last_streaming_log_us = 0;
    int64_t  last_energy_log_us = 0;
    uint32_t vad_fail_skip_count = 0;

    while (true) {
        dbg_loop++;

        // Periodic stack high-water-mark logging (~2s) — BEFORE any continue
        int64_t stack_now_us = esp_timer_get_time();
        if (stack_now_us - last_stack_us >= 2000000) {
            last_stack_us = stack_now_us;
            uint32_t hwm = uxTaskGetStackHighWaterMark(NULL);
            printf("[STACK] loop=%lu hwm=%u streaming=%d\n",
                   (unsigned long)dbg_loop, (unsigned)hwm, (int)streaming_active);
            fflush(stdout);
        }

        // streaming_active watchdog: if stuck true for >10s, force-clear it
        if (streaming_active && streaming_active_set_us != 0) {
            int64_t held_us = stack_now_us - streaming_active_set_us;
            if (held_us > 10000000) {  // 10 seconds
                printf("[WATCHDOG] streaming_active stuck true for %.1fs — FORCE CLEARING\n",
                       (double)held_us / 1e6);
                fflush(stdout);
                streaming_active = false;
                streaming_active_set_us = 0;
            }
        }

        // Periodic streaming_active debug (~500ms)
        if (stack_now_us - last_streaming_log_us >= 500000) {
            last_streaming_log_us = stack_now_us;
            if (streaming_active) {
                int64_t held = streaming_active_set_us ? (stack_now_us - streaming_active_set_us) : 0;
                printf("[DBG-SA] streaming_active=TRUE for %.1fs\n", (double)held / 1e6);
                fflush(stdout);
            }
        }

        // 1. Wait until at least one fresh hop is available in the ring.
        //    (While streaming, the ring is still being filled by audio_task —
        //     we just skip inference so nothing contends over the model.)
        int avail;
        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        avail = ring_write_pos - last_read_pos;
        if (avail < 0) avail += AUDIO_BUFFER_SAMPLES;  // ring buffer wrap-around
        if (avail > AUDIO_BUFFER_SAMPLES) {  // we fell behind — resync to newest
            avail = AUDIO_BUFFER_SAMPLES;
            last_read_pos = ring_write_pos - AUDIO_BUFFER_SAMPLES;
            if (last_read_pos < 0) last_read_pos += AUDIO_BUFFER_SAMPLES;
        }
        if (avail < hop_samples || valid_ring_samples < AUDIO_BUFFER_SAMPLES ||
            streaming_active) {
            xSemaphoreGive(ring_mutex);
            if (streaming_active) {
                // Rate-limited log: only print once per second to avoid flooding
                static int64_t last_sa_skip_log = 0;
                if (stack_now_us - last_sa_skip_log >= 1000000) {
                    last_sa_skip_log = stack_now_us;
                    int64_t held = streaming_active_set_us ? (stack_now_us - streaming_active_set_us) : 0;
                    printf("[DBG-SA] inference SKIPPED — streaming_active for %.1fs\n",
                           (double)held / 1e6);
                    fflush(stdout);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        // Copy the newest hop out of the ring
        {
            int start = ring_write_pos - hop_samples;
            if (start < 0) start += AUDIO_BUFFER_SAMPLES;
            for (int i = 0; i < hop_samples; i++)
                hop_buf[i] = audio_ring[(start + i) % AUDIO_BUFFER_SAMPLES];
            last_read_pos = ring_write_pos;
        }
        xSemaphoreGive(ring_mutex);

        // 2. RMS + peak on this hop
        int64_t sum_sq = 0;
        int16_t hop_peak = 0;
        for (int i = 0; i < hop_samples; i++) {
            sum_sq += (int64_t)hop_buf[i] * hop_buf[i];
            int16_t abs_val = hop_buf[i] < 0 ? -hop_buf[i] : hop_buf[i];
            if (abs_val > hop_peak) hop_peak = abs_val;
        }
        float rms = sqrtf((float)sum_sq / hop_samples) / 32768.0f;
        float peak_f = (float)hop_peak / 32768.0f;
        telemetry_mic_rms = rms;

        // 3. [ENERGY] log — unconditional, every ~500ms, mic health visibility
        int64_t now_us = esp_timer_get_time();
        {
            float speech_thr = fmaxf(MIN_SPEECH_RMS, noise_floor_rms * NOISE_FLOOR_MULTIPLIER);
            if (now_us - last_energy_log_us >= 500000) {
                last_energy_log_us = now_us;
                printf("[ENERGY] rms=%.5f peak=%.5f thr=%.5f noise_floor=%.5f streaming=%d\n",
                       (double)rms, (double)peak_f, (double)speech_thr,
                       (double)noise_floor_rms, (int)streaming_active);
                fflush(stdout);
            }
        }

        // 5. Noise calibration — only use SILENT frames so speech doesn't corrupt the floor
        if (noise_cal_frames < NOISE_CALIBRATION_FRAMES) {
            if (rms < NOISE_CAL_MAX_RMS) {  // only truly silent frames
                noise_floor_rms += (rms - noise_floor_rms) / (float)(++noise_cal_frames);
                telemetry_noise_floor_rms = noise_floor_rms;
            }
            // Don't skip inference during calibration — model can still detect
            if (noise_cal_frames < 5) {
                consecutive_hits = 0;
                vTaskDelay(pdMS_TO_TICKS(1));  // yield to idle — prevent TWDT
                continue;  // need at least 5 silent frames before the floor is meaningful
            }
        }

        // 6. VAD gate
        float speech_thr = fmaxf(MIN_SPEECH_RMS, noise_floor_rms * NOISE_FLOOR_MULTIPLIER);
        if (rms < speech_thr) {
            noise_floor_rms = 0.02f * rms + 0.98f * noise_floor_rms;
            telemetry_noise_floor_rms = noise_floor_rms;
            consecutive_hits = 0;
            vad_fail_skip_count++;
            if (vad_fail_skip_count % 50 == 0) {
                printf("[VAD] FAIL #%lu rms=%.5f thr=%.5f\n",
                       (unsigned long)vad_fail_skip_count, (double)rms, (double)speech_thr);
                fflush(stdout);
            }
            vTaskDelay(pdMS_TO_TICKS(1));  // yield to idle — prevent TWDT hang
            continue;
        }
        printf("[VAD] PASS rms=%.5f thr=%.5f noise_floor=%.5f\n",
               (double)rms, (double)speech_thr, (double)noise_floor_rms);
        fflush(stdout);

        // 7. Linearise ring → 1s window
        printf("[STAGE-C] pre-linearize loop=%lu\n", (unsigned long)dbg_loop); fflush(stdout);
        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        {
            int start = ring_write_pos;
            for (int i = 0; i < AUDIO_BUFFER_SAMPLES; i++)
                audio_window[i] = audio_ring[(start + i) % AUDIO_BUFFER_SAMPLES];
        }
        xSemaphoreGive(ring_mutex);
        printf("[STAGE-C] post-linearize\n"); fflush(stdout);

        // 8. MFCC + inference
        printf("[STAGE-C] pre-MFCC\n"); fflush(stdout);
        mfcc_proc.compute(audio_window, mfcc_output);
        printf("[STAGE-C] post-MFCC, pre-quantize\n"); fflush(stdout);

        float   in_scale = input_tensor->params.scale;
        int32_t in_zp    = input_tensor->params.zero_point;
        int8_t* inp      = input_tensor->data.int8;
        for (int i = 0; i < MFCC_OUTPUT_SIZE; i++) {
            int32_t q = (int32_t)roundf(mfcc_output[i] / in_scale) + in_zp;
            q = q < -128 ? -128 : (q > 127 ? 127 : q);
            inp[i] = (int8_t)q;
        }
        printf("[STAGE-C] pre-invoke\n"); fflush(stdout);
        int64_t t0 = esp_timer_get_time();
        interpreter->Invoke();
        float infer_us = (float)(esp_timer_get_time() - t0);
        printf("[STAGE-C] post-invoke (%.0f us)\n", infer_us); fflush(stdout);

        float   out_scale = output_tensor->params.scale;
        int32_t out_zp    = output_tensor->params.zero_point;
        int8_t* out       = output_tensor->data.int8;

        // Model is sigmoid with 1 output element. The old code read out[1]
        // which is past the tensor bounds — always produced 0.0000.
        // Output tensor shape is [1, 1] int8: scale=0.00390625, zp=-128.
        // Dequantize: float = (int8_value - zero_point) * scale
        // For sigmoid: int8=-128 -> 0.0, int8=0 -> 0.5, int8=127 -> 0.996
        float   kw_prob   = (out[0] - out_zp) * out_scale;

        // Debug: dump raw int8 output tensor values for all elements
        int out_elems = output_tensor->dims->data[output_tensor->dims->size - 1];
        printf("[INFER-RAW] out_elems=%d", out_elems);
        for (int oi = 0; oi < out_elems && oi < 8; oi++) {
            printf(" [%d]=%d", oi, out[oi]);
        }
        printf(" scale=%.6f zp=%d\n", (double)out_scale, (int)out_zp);
        fflush(stdout);

        printf("[INFER] loop=%lu conf=%.4f threshold=%.2f result=%s infer_us=%.0f\n",
               (unsigned long)dbg_loop, (double)kw_prob, (double)DETECT_THRESHOLD,
               (kw_prob >= DETECT_THRESHOLD) ? "TRIGGER" : "no",
               (double)infer_us);
        fflush(stdout);

        infer_count++;
        telemetry_inference_count += 1;
        telemetry_inference_ms      += infer_us / 1000.0f;
        telemetry_keyword_confidence = kw_prob;

        consecutive_hits = (kw_prob >= DETECT_THRESHOLD) ? consecutive_hits + 1 : 0;

        if (consecutive_hits >= DETECTION_HITS_REQUIRED) {
            int64_t kw_end = esp_timer_get_time();
            printf("[TRIGGER->STREAM] KEYWORD CONFIRMED conf=%.4f (%.1f%%) — posting to detect_queue\n",
                   (double)kw_prob, (double)(kw_prob * 100.0f));
            fflush(stdout);
            xQueueSend(detect_queue, &kw_end, 0);
            consecutive_hits = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (consecutive_hits > 0) {
            printf("[INFER] analyzing... frame %d/%d conf=%.4f\n",
                   consecutive_hits, DETECTION_HITS_REQUIRED, (double)kw_prob);
            fflush(stdout);
        }
    }
}

// ─── TASK A0: Audio capture — single consumer of the ADC DMA stream ─────────
// Dedicated high-priority task: drains the DMA as fast as hardware produces
// data, so the DMA pool never overflows and no audio samples are ever lost.
static void audio_task(void* arg) {
    const int HOP = ADC_SAMPLE_RATE * SLIDE_STEP_MS / 1000;  // 480 samples
    static int16_t hop[480];



    // Sanity check
    int n = read_pcm_dma(hop, 64, NULL, NULL);
    ESP_LOGI(TAG_INF, "ADC DMA sanity read: got=%d first=%d dc=%.1f",
             n, n > 0 ? (int)hop[0] : -1, (double)adc_dc_offset);
    if (n <= 0) ESP_LOGE(TAG_INF, "FATAL: ADC DMA stream broken!");

    while (true) {
        uint32_t err = 0, ok = 0;
        read_pcm_dma(hop, HOP, &err, &ok);
        if (err) adc_dma_err += err;
        adc_dma_ok += ok;

        xSemaphoreTake(ring_mutex, portMAX_DELAY);
        for (int i = 0; i < HOP; i++) {
            audio_ring[ring_write_pos] = hop[i];
            ring_write_pos = (ring_write_pos + 1) % AUDIO_BUFFER_SAMPLES;
        }
        xSemaphoreGive(ring_mutex);
        if (valid_ring_samples < AUDIO_BUFFER_SAMPLES)
            valid_ring_samples += HOP;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ─── TASK B: Streaming (Core 1) ──────────────────────────────────────────────
static void streaming_task(void* arg) {
    ESP_LOGI(TAG_STR, "Streaming task started (core %d)", xPortGetCoreID());

    // audio_task is the ONLY ADC consumer — we read command audio from the
    // ring buffer, same as the inference task.
    int64_t keyword_end_us;
    while (true) {
        xQueueReceive(detect_queue, &keyword_end_us, portMAX_DELAY);
        session_id += 1;
        uint32_t sid = session_id;
        ESP_LOGI(TAG_STR, "[%lu] Keyword — connecting WiFi", (unsigned long)sid);

        wifi_start();
        if (!wifi_connected) {
            ESP_LOGE(TAG_STR, "[%lu] WiFi failed — drop", (unsigned long)sid);
            wifi_stop();
            continue;
        }

        int      sock       = -1;
        uint32_t kw_ms      = 0;
        uint32_t backoff_ms = 200;

        for (int attempt = 0; attempt < 4; attempt++) {
            struct sockaddr_in srv = {};
            srv.sin_family = AF_INET;
            srv.sin_port   = htons(CONFIG_SERVER_PORT);
            inet_pton(AF_INET, CONFIG_SERVER_IP, &srv.sin_addr);

            sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(backoff_ms)); backoff_ms *= 2; continue; }

            int f = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &f, sizeof(f));

            // Prevent indefinite blocking on send/recv — if server stalls,
            // we must not hang forever (which would keep streaming_active=true
            // and freeze the inference task).
            struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            if (connect(sock, (struct sockaddr*)&srv, sizeof(srv)) == 0) {
                kw_ms = (uint32_t)((esp_timer_get_time() - keyword_end_us) / 1000);
                printf("[STREAM] sid=%lu TCP connected (attempt %d) kw->conn=%lums\n",
                       (unsigned long)sid, attempt + 1, (unsigned long)kw_ms);
                fflush(stdout);
                break;
            }
            printf("[STREAM] sid=%lu TCP connect FAILED attempt %d: errno=%d\n",
                   (unsigned long)sid, attempt + 1, errno);
            fflush(stdout);
            close(sock); sock = -1;
            vTaskDelay(pdMS_TO_TICKS(backoff_ms)); backoff_ms *= 2;
        }

        if (sock < 0) {
            ESP_LOGE(TAG_STR, "[%lu] All TCP attempts failed", (unsigned long)sid);
            wifi_stop();
            continue;
        }

        hvp1_header_t hdr = {
            .magic            = MAGIC_NUMBER,
            .sample_rate      = ADC_SAMPLE_RATE,
            .channels         = 1,
            .bits             = 16,
            .audio_len        = 0,
            .kw_to_connect_ms = kw_ms,
        };
        ssize_t hdr_sent = send(sock, &hdr, sizeof(hdr), 0);
        printf("[STREAM] sid=%lu HVP1 header sent (%d bytes)\n", (unsigned long)sid, (int)hdr_sent);
        fflush(stdout);

        streaming_active = true;
        streaming_active_set_us = esp_timer_get_time();
        printf("[DBG-SA] streaming_active SET TRUE (sid=%lu)\n", (unsigned long)sid);
        fflush(stdout);

        const int  CHUNK = 480;
        int16_t    pcm[CHUNK];
        TickType_t deadline          = xTaskGetTickCount() + pdMS_TO_TICKS(COMMAND_DURATION_MS);
        int        total_sent        = 0;
        int        streamed_ms       = 0;
        int        consec_silence_ms = 0;
        int        last_pos          = ring_write_pos;  // start from "now"

        while (xTaskGetTickCount() < deadline) {
            // ── Pull one fresh chunk from the ring (filled by audio_task) ──
            xSemaphoreTake(ring_mutex, portMAX_DELAY);
            int avail = ring_write_pos - last_pos;
            if (avail < 0) avail += AUDIO_BUFFER_SAMPLES;
            if (avail < CHUNK) {
                xSemaphoreGive(ring_mutex);
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            for (int i = 0; i < CHUNK; i++) {
                pcm[i] = audio_ring[last_pos];
                last_pos = (last_pos + 1) % AUDIO_BUFFER_SAMPLES;
            }
            xSemaphoreGive(ring_mutex);

            if (send(sock, pcm, CHUNK * sizeof(int16_t), 0) < 0) {
                printf("[STREAM] sid=%lu SEND FAILED at %d samples (%dms)\n",
                       (unsigned long)sid, total_sent, streamed_ms);
                fflush(stdout);
                break;
            }
            total_sent  += CHUNK;
            int chunk_ms = CHUNK * 1000 / ADC_SAMPLE_RATE;
            streamed_ms += chunk_ms;

            // Periodic send progress (~500ms)
            if (streamed_ms % 500 < chunk_ms) {
                printf("[STREAM] sid=%lu sending... %d samples (%dms) total_sent=%d\n",
                       (unsigned long)sid, streamed_ms, streamed_ms, total_sent);
                fflush(stdout);
            }

            if (streamed_ms >= COMMAND_MIN_DURATION_MS) {
                int64_t sq = 0;
                for (int i = 0; i < CHUNK; i++) sq += (int64_t)pcm[i] * pcm[i];
                float crms     = sqrtf((float)sq / CHUNK) / 32768.0f;
                float sil_thr  = fmaxf(0.025f, telemetry_noise_floor_rms * 1.5f);
                consec_silence_ms = crms < sil_thr ? consec_silence_ms + chunk_ms : 0;
                if (consec_silence_ms >= COMMAND_SILENCE_MS) {
                    printf("[STREAM] sid=%lu silence -> end stream at %dms\n",
                           (unsigned long)sid, streamed_ms);
                    fflush(stdout);
                    break;
                }
            }
        }

        streaming_active = false;
        streaming_active_set_us = 0;
        printf("[DBG-SA] streaming_active CLEARED (sid=%lu)\n", (unsigned long)sid);
        fflush(stdout);
        shutdown(sock, SHUT_WR);
        ESP_LOGI(TAG_STR, "[%lu] Sent %d samples (%.2fs)",
                 (unsigned long)sid, total_sent, (float)total_sent / ADC_SAMPLE_RATE);

        {
            int64_t recv_start = esp_timer_get_time();
            char resp[1024] = {0};
            int  rlen = 0, r;
            while ((r = recv(sock, resp + rlen, sizeof(resp) - rlen - 1, 0)) > 0) rlen += r;
            float recv_ms = (float)(esp_timer_get_time() - recv_start) / 1000.0f;
            close(sock);
            printf("[STREAM] sid=%lu recv complete: %d bytes in %.0fms\n",
                   (unsigned long)sid, rlen, (double)recv_ms);
            printf("\n=== CLOUD WHISPER TRANSCRIPTION ===\n  -> %s\n===================================\n\n",
                   rlen > 0 ? resp : "[no response]");
            fflush(stdout);
        }

        wifi_stop();
    }
}

// ─── Telemetry Task ───────────────────────────────────────────────────────────
static void telemetry_task(void* arg) {
    float prev_ms = 0.0f;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!wifi_connected) continue;
        wifi_ap_record_t ap = {};
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        float total_ms  = telemetry_inference_ms;
        float duty_pct  = total_ms >= prev_ms ? (total_ms - prev_ms) / 10.0f : 0.0f;
        prev_ms = total_ms;
        char body[512];
        int blen = snprintf(body, sizeof(body),
            "{\"device\":\"esp32\",\"uptime_ms\":%llu,"
            "\"free_heap_bytes\":%lu,\"min_free_heap_bytes\":%lu,"
            "\"heap_total_bytes\":%lu,\"tflite_arena_bytes\":%u,"
            "\"audio_buffer_bytes\":%u,\"keyword_confidence\":%.4f,"
            "\"mic_rms\":%.5f,\"inference_count\":%lu,"
            "\"inference_duty_pct\":%.3f,\"wifi_rssi_dbm\":%d,\"streaming\":%s}",
            (unsigned long long)(esp_timer_get_time() / 1000),
            (unsigned long)esp_get_free_heap_size(),
            (unsigned long)esp_get_minimum_free_heap_size(),
            (unsigned long)heap_caps_get_total_size(MALLOC_CAP_8BIT),
            (unsigned)TENSOR_ARENA_SIZE,
            (unsigned)(sizeof(audio_ring) + sizeof(audio_window)),
            telemetry_keyword_confidence, telemetry_mic_rms,
            (unsigned long)telemetry_inference_count,
            duty_pct, rssi, streaming_active ? "true" : "false");
        if (blen <= 0 || blen >= (int)sizeof(body)) continue;

        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) continue;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(8080);
        inet_pton(AF_INET, CONFIG_SERVER_IP, &addr.sin_addr);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            char req[700];
            int rlen = snprintf(req, sizeof(req),
                "POST /api/telemetry HTTP/1.1\r\nHost: esp32\r\n"
                "Content-Type: application/json\r\nContent-Length: %d\r\n"
                "Connection: close\r\n\r\n%s", blen, body);
            if (rlen > 0 && rlen < (int)sizeof(req))
                send(sock, req, rlen, 0);
        }
        close(sock);
    }
}

// ─── app_main ─────────────────────────────────────────────────────────────────
extern "C" void app_main() {
    // ── 5-second delay so the serial monitor can connect before boot output ────────
    // Open the serial monitor NOW, then wait for the diagnostic output.
    printf("\n\n=== Hey Vaani booting in 5 seconds — open monitor NOW ===\n");
    fflush(stdout);
    for (int i = 5; i > 0; i--) {
        printf("  Starting in %d...\n", i); fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("=== GO ===\n\n"); fflush(stdout);

    ESP_LOGI(TAG_MAIN, "=== Hey Vaani Edge Firmware (SIH 2026) ===");

    ESP_ERROR_CHECK(nvs_flash_init());

    ring_mutex   = xSemaphoreCreateMutex();
    detect_queue = xQueueCreate(4, sizeof(int64_t));

    // Single global ADC handle — kept alive for the lifetime of the firmware
    adc_global_init();

    mic_selfcheck();

    tflite_init();
    mfcc_proc.init();

    ESP_LOGI(TAG_MAIN, "Free heap after init: %u bytes", (unsigned)esp_get_free_heap_size());

    xTaskCreatePinnedToCore(audio_task,     "audio",      4096, NULL, 6, NULL, 1);
    xTaskCreatePinnedToCore(inference_task, "inference", 24576, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(streaming_task, "streaming",  8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(telemetry_task, "telemetry",  4096, NULL, 1, NULL, 0);

    ESP_LOGI(TAG_MAIN, "All tasks started. Listening for 'Hey Vaani'...");
}
