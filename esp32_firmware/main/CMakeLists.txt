idf_component_register(
    SRCS
        "main.cpp"
        "wifi_provision.cpp"
        "esp_now_fusion.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_http_client
        lwip
        nvs_flash
        esp_timer
        esp-tflite-micro
        mbedtls          # AES-128-CTR (mbedtls/aes.h) + hardware RNG (esp_random.h)
        esp_hw_support   # esp_fill_random()
        cjson            # cJSON.h (cJSON parser from espressif/cjson)
        # NOTE: esp_now is NOT a separate component in ESP-IDF 6.1 — it is
        # bundled inside esp_wifi (esp_wifi/include/esp_now.h). No extra REQUIRES needed.
)

# Enable runtime stats for CPU load measurement (benchmark_cpu.h)
# These are also set via menuconfig → FreeRTOS → "Enable FreeRTOS to collect run time stats"
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    configGENERATE_RUN_TIME_STATS=1
    configUSE_TRACE_FACILITY=1
    configUSE_STATS_FORMATTING_FUNCTIONS=1
)
