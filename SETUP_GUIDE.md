# Edge AI Voice Assistant - Setup Guide

This guide explains how to set up the entire "Hey Vaani" voice assistant codebase on a new laptop from scratch.

## Prerequisites

Before starting, ensure your laptop has the following installed:
1. **Python 3.10 or newer**: [Download Python](https://www.python.org/downloads/)
2. **Visual Studio Code (VS Code)**: [Download VS Code](https://code.visualstudio.com/)
3. **PlatformIO IDE**: Open VS Code, go to the Extensions tab, search for "PlatformIO IDE", and install it.

## 1. Transfer the Codebase
You can transfer the codebase by either cloning it from GitHub (if uploaded) or simply transferring the whole `Edge_Ai` folder via a USB drive or ZIP file. 
*Note: Before zipping the folder, you can delete the `.pio/` and `training/.venv312/` folders to save space, as those will be recreated on the new laptop.*

## 2. Setting up the Cloud Server (Python)
The cloud server receives the audio stream and translates it to text.

1. Open a terminal and navigate to the cloud server folder:
   ```bash
   cd Edge_Ai/cloud_server
   ```
2. Create a virtual environment:
   ```bash
   python -m venv venv
   ```
3. Activate the virtual environment:
   * **Windows**: `venv\Scripts\activate`
   * **Linux/Mac**: `source venv/bin/activate`
4. Install the requirements:
   ```bash
   pip install -r requirements.txt
   ```
5. You can now run the server:
   ```bash
   python server.py
   ```

## 3. Setting up the AI Training Environment (Python)
The training folder contains the scripts for recording audio, augmenting data, and training the TensorFlow Lite wake word model.

1. Open a terminal and navigate to the training folder:
   ```bash
   cd Edge_Ai/training
   ```
2. Create a virtual environment:
   ```bash
   python -m venv .venv
   ```
3. Activate the virtual environment:
   * **Windows**: `.venv\Scripts\activate`
   * **Linux/Mac**: `source .venv/bin/activate`
4. Install the requirements:
   ```bash
   pip install -r requirements.txt
   ```

## 4. Setting up the ESP32 Firmware (C++)
The firmware handles real-time audio sampling, ML inference (detecting "Hey Vaani"), and WiFi streaming.

1. Open the `Edge_Ai` folder in VS Code.
2. Because you installed the PlatformIO extension earlier, VS Code will automatically detect the `esp32_firmware` folder.
3. At the very bottom of the VS Code window (in the blue status bar), click the **PlatformIO: Build** button (the checkmark icon) to compile the firmware.
4. Connect the ESP32 to your laptop via USB.
5. Click the **PlatformIO: Upload** button (the right-arrow icon) to flash the code to the ESP32.

### Connecting the Hardware
If you are using a fresh ESP32 on the new laptop, make sure the wiring matches the firmware:
* **Microphone (MAX4466)**: OUT pin connects to **GPIO 32** on the ESP32. Ensure the microphone is connected to a stable 3.3V source.

## Summary of the Pipeline
- **Record Dataset**: Use `training/record_max4466_dataset.py` to record samples directly from the ESP32.
- **Train Model**: Run `training/train_model.py` to generate the `.tflite` file.
- **Convert to C Array**: Run `training/convert_tflite.py` and copy the output into `esp32_firmware/main/model_data.h`.
- **Flash ESP32**: Build and upload the `esp32_firmware` via PlatformIO.
- **Run Server**: Run `cloud_server/server.py` to catch the live audio stream over WiFi.
