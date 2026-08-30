cmake_minimum_required(VERSION 3.16)

# ESP-IDF project configuration
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hey_vaani_kws)

# ─── Component Dependencies ───────────────────────────────────────────────
# TFLite Micro: add as an extra component from ESP-IDF component registry.
# Install: idf.py add-dependency "espressif/esp-tflite-micro^1.3.1"
#
# This pulls in:
#   - tensorflow/lite/micro/micro_interpreter.h
#   - tensorflow/lite/schema/schema_generated.h
#   - tensorflow/lite/micro/micro_mutable_op_resolver.h
