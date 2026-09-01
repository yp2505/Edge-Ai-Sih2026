// MAX4466 dataset recorder firmware.
//
// This is intentionally a separate PlatformIO source directory.  It records
// 1.5 seconds of real MAX4466 audio and sends it to the PC over USB serial.
// It contains no TFLite model and never performs wake-word inference.
//
// Protocol:
//   PC -> ESP32:  R\n
//   ESP32 -> PC:  "HVR1" | uint32_le sample_rate | uint32_le sample_count |
//                int16_le PCM samples
//
// Use it with:
//   pio run -c platformio_recorder.ini --target upload
//   python ../training/record_max4466_dataset.py --source esp32 --port /dev/ttyUSB0 ...

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_continuous.h"
#include "esp_err.h"
#include "hal/adc_types.h"

static constexpr adc_unit_t ADC_UNIT = ADC_UNIT_1;
static constexpr adc_channel_t ADC_CHANNEL = ADC_CHANNEL_4;  // GPIO 32
static constexpr int ADC_HARDWARE_RATE = 20000;
static constexpr int OUTPUT_SAMPLE_RATE = 16000;
static constexpr int RECORD_SAMPLES = 24000;  // 1.5 seconds at 16 kHz
static constexpr int ADC_DMA_BYTES = 512;
static constexpr int ADC_PCM_SHIFT = 4;

struct __attribute__((packed)) pcm_header_t {
    char magic[4];       // "HVR1"
    uint32_t sample_rate;
    uint32_t sample_count;
};

static adc_continuous_handle_t adc_handle = nullptr;
static float adc_dc_offset = 2048.0f;

static void init_adc() {
    adc_continuous_handle_cfg_t handle_cfg = {};
    handle_cfg.max_store_buf_size = 4096;
    handle_cfg.conv_frame_size = ADC_DMA_BYTES;
    ESP_ERROR_CHECK(adc_continuous_new_handle(&handle_cfg, &adc_handle));

    adc_digi_pattern_config_t pattern = {};
    pattern.atten = ADC_ATTEN_DB_12;
    pattern.channel = ADC_CHANNEL;
    pattern.unit = ADC_UNIT;
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    adc_continuous_config_t config = {};
    config.sample_freq_hz = ADC_HARDWARE_RATE;
    config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    config.pattern_num = 1;
    config.adc_pattern = &pattern;
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &config));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

// Drain data briefly so the EMA DC-removal filter settles before the first clip.
static void warm_up_adc() {
    uint8_t raw[ADC_DMA_BYTES];
    int samples_seen = 0;
    while (samples_seen < ADC_HARDWARE_RATE / 2) {
        uint32_t bytes_read = 0;
        if (adc_continuous_read(adc_handle, raw, sizeof(raw), &bytes_read,
                                pdMS_TO_TICKS(100)) != ESP_OK) {
            continue;
        }
        int count = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
        for (int i = 0; i < count; ++i) {
            auto* value = reinterpret_cast<adc_digi_output_data_t*>(
                raw + i * SOC_ADC_DIGI_RESULT_BYTES);
            adc_dc_offset = 0.005f * value->type1.data + 0.995f * adc_dc_offset;
            ++samples_seen;
        }
    }
}

static bool capture_clip(int16_t* output) {
    uint8_t raw[ADC_DMA_BYTES];
    int written = 0;
    int decimation_phase = 0;

    while (written < RECORD_SAMPLES) {
        uint32_t bytes_read = 0;
        esp_err_t err = adc_continuous_read(adc_handle, raw, sizeof(raw), &bytes_read,
                                            pdMS_TO_TICKS(200));
        if (err != ESP_OK || bytes_read == 0) continue;

        const int count = bytes_read / SOC_ADC_DIGI_RESULT_BYTES;
        for (int i = 0; i < count && written < RECORD_SAMPLES; ++i) {
            auto* value = reinterpret_cast<adc_digi_output_data_t*>(
                raw + i * SOC_ADC_DIGI_RESULT_BYTES);
            const uint32_t raw_adc = value->type1.data;
            adc_dc_offset = 0.005f * raw_adc + 0.995f * adc_dc_offset;
            const int16_t centered = static_cast<int16_t>(raw_adc - adc_dc_offset);

            // Exact 20 kHz -> 16 kHz ratio: retain four samples out of five.
            if (decimation_phase != 4) {
                output[written++] = static_cast<int16_t>(centered << ADC_PCM_SHIFT);
            }
            decimation_phase = (decimation_phase + 1) % 5;
        }
    }
    return true;
}

extern "C" void app_main() {
    // Text is emitted only before a recording.  The PC script synchronises on
    // the HVR1 binary header, so boot text cannot corrupt a saved WAV file.
    printf("MAX4466 recorder ready. Send R followed by Enter to capture 1.5 seconds.\n");
    init_adc();
    warm_up_adc();

    static int16_t pcm[RECORD_SAMPLES];
    while (true) {
        int c = getchar();
        if (c != 'R' && c != 'r') continue;

        if (!capture_clip(pcm)) continue;
        const pcm_header_t header = {{'H', 'V', 'R', '1'}, OUTPUT_SAMPLE_RATE, RECORD_SAMPLES};
        fwrite(&header, 1, sizeof(header), stdout);
        fwrite(pcm, sizeof(pcm[0]), RECORD_SAMPLES, stdout);
        fflush(stdout);
    }
}
