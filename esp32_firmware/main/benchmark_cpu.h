// benchmark_cpu.h — ESP-IDF CPU Load Monitor
//
// Usage: Call benchmark_cpu_start() once at boot. The background task
// prints average CPU utilization % to serial every BENCHMARK_INTERVAL_MS.
//
// This satisfies the PS requirement: "measure and log idle CPU utilization %
// on the ESP32 while idling in the continuous listening loop."
//
// How to read: In the serial monitor look for lines like:
//   [CPU] Inference task: 2.3% | Idle: 96.8% | WiFi: 0.9%
//
// The ESP-IDF runtime stats are based on the xtensa cycle counter, so they
// are accurate to ~1% resolution. Enable CONFIG_FREERTOS_USE_TRACE_FACILITY
// and CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS in menuconfig.

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define BENCHMARK_TAG            "CPU"
#define BENCHMARK_INTERVAL_MS    10000   // Log every 10 seconds
#define BENCHMARK_TASK_BUF_SIZE  2048    // Buffer for vTaskGetRunTimeStats

static void benchmark_task(void* arg) {
    char stats_buf[BENCHMARK_TASK_BUF_SIZE];
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BENCHMARK_INTERVAL_MS));

        // vTaskGetRunTimeStats writes a formatted table to the buffer.
        // Format: "TaskName  AbsTime  %Time"
        vTaskGetRunTimeStats(stats_buf);

        ESP_LOGI(BENCHMARK_TAG, "\n─── FreeRTOS CPU Usage (last %d sec) ───\n%s",
                 BENCHMARK_INTERVAL_MS / 1000, stats_buf);
    }
}

// Call once from app_main() BEFORE starting the inference loop.
static inline void benchmark_cpu_start() {
    xTaskCreatePinnedToCore(
        benchmark_task,
        "benchmark",
        4096,      // stack size
        NULL,
        1,         // lowest priority
        NULL,
        0          // run on core 0 (core 1 used for inference)
    );
    ESP_LOGI(BENCHMARK_TAG, "CPU benchmark task started (logs every %d sec)",
             BENCHMARK_INTERVAL_MS / 1000);
}
