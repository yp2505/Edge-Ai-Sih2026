# 🎙️ Hey Vaani - Edge AI Keyword Spotting

A highly optimized TinyML Keyword Spotting (KWS) pipeline designed to detect the wake word **"Hey Vaani"** on edge devices like the ESP32 or Raspberry Pi Pico. 

Built for the SIH 2024 problem statement, this project achieves a massive **99.6% accuracy** with an ultra-tiny memory footprint of just **46.9 KB**.

---

## 🌟 Key Features & Compliance
* **Custom Wake Word:** Trained on custom datasets specifically for the phrase *"Hey Vaani"*.
* **TinyML Architecture:** Uses an advanced **Depthwise Separable CNN (DS-CNN)** architecture inspired by MobileNet.
* **INT8 Quantization:** Fully quantized to 8-bit integers using TensorFlow Lite for Microcontrollers (TFLite Micro).
* **Extreme Efficiency:**
  * **Model Size:** 46.9 KB *(Target: < 60 KB)* ✅
  * **RAM / Tensor Arena:** ~50 KB *(Target: < 256 KB)* ✅
  * **Accuracy:** 99.6% *(Target: > 90%)* ✅

---

## 📂 Project Structure

```text
Edge-Ai/
├── training/                 # Neural Network Training & Quantization
│   ├── download_dataset.py   # Fetches negative & background noise
│   ├── augment_data.py       # 10x multiplier data augmentation
│   ├── train_model.py        # TensorFlow/Keras DS-CNN training
│   └── convert_tflite.py     # INT8 Quantization & model_data.h export
├── esp32_firmware/           # Edge Device Code
│   ├── main/
│   │   ├── edge_inference.py # Simulated edge inference logic
│   │   └── model_data.h      # The actual AI model converted to a C++ array
├── cloud_server/             # Backend Cloud API
│   └── asr_server.py         # FastAPI + Deepgram integration
└── docs/                     # Schematics & Wiring Guides
```

---

## 🚀 How to Run the Pipeline

### 1. Training the Model
We used a Python 3.12 virtual environment to ensure TensorFlow compatibility.
```bash
cd training
source .venv312/bin/activate
python train_model.py --epochs 50
```

### 2. Quantization (TFLite)
Squash the model into an 8-bit `.tflite` file and export it to a C++ header for the microcontroller:
```bash
python convert_tflite.py --verify
```

### 3. Edge Inference
The generated `model_data.h` is automatically placed into the `esp32_firmware/main/` folder to be flashed onto the hardware.

---

## ☁️ Cloud Integration
Once the Edge device hears *"Hey Vaani"*, it opens a WebSocket connection to the Python cloud server (`cloud_server/asr_server.py`). The server uses the **Deepgram API** for lightning-fast, highly accurate continuous dictation.

## 🛠️ Tech Stack
* **ML Framework:** TensorFlow / Keras
* **Edge Framework:** TensorFlow Lite for Microcontrollers (TFLite Micro)
* **Backend:** FastAPI, Python, WebSockets
* **Cloud ASR:** Deepgram 
* **Audio Processing:** Librosa, Soundfile, PyDub
