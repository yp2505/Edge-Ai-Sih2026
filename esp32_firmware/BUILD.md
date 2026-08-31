# Building and Flashing with PlatformIO

This project has been migrated to use PlatformIO with the ESP-IDF framework.

## Build the Firmware
To build the firmware, open a terminal in this directory and run:
```bash
pio run
```
*In VS Code: Click the checkmark (✓) in the PlatformIO bottom toolbar.*

## Upload to Device
To flash the firmware to your ESP32-WROOM-32, run:
```bash
pio run --target upload
```
*In VS Code: Click the right-arrow (→) in the PlatformIO bottom toolbar.*

> **Note:** If the upload fails to connect (e.g., `Failed to connect to ESP32: Timed out waiting for packet header`), you may need to **hold down the BOOT button** on your ESP32 dev board when the terminal says `Connecting...`.

## Serial Monitor
To view the output from the ESP32 (at 115200 baud), run:
```bash
pio device monitor
```
*In VS Code: Click the plug icon in the PlatformIO bottom toolbar.*

> **Tip:** You can build, upload, and monitor in a single step with:
> `pio run --target upload --target monitor`
