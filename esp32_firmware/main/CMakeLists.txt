idf_component_register(
    SRCS
        "main.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver
        esp_wifi
        esp_netif
        esp_event
        lwip
        nvs_flash
        esp_timer
        esp-tflite-micro
)

# Enable runtime stats for CPU load measurement (benchmark_cpu.h)
# These are also set via menuconfig → FreeRTOS → "Enable FreeRTOS to collect run time stats"
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    configGENERATE_RUN_TIME_STATS=1
    configUSE_TRACE_FACILITY=1
    configUSE_STATS_FORMATTING_FUNCTIONS=1
)
