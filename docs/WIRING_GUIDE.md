# Hey Vaani — Complete Project Wiring Guide

## System Architecture

```
┌─────────────────────────────────────┐       ┌────────────────────────────────┐
│     EDGE (Raspberry Pi 5)           │       │   CLOUD (Laptop/PC)            │
│                                     │ WiFi  │                                │
│  Mic → MFCC → DS-CNN TFLite        │──────►│  asr_server.py                 │
│         [~45KB model, ~1% CPU]      │  TCP  │  [Vosk DNN-HMM, open-source]   │
│                                     │       │                                │
│  "Hey Vaani" detected?              │       │  Transcribes speech            │
│      YES → stream audio ────────────┼──────►│  → returns JSON transcript     │
│          ← show transcript ◄────────┼───────┤                                │
└─────────────────────────────────────┘       └────────────────────────────────┘
```

---

## Step-by-Step Setup (After Training)

### Step 1 — Train the Model (Your Friends Recording Now)

```bash
cd training/

# 1a. Record keyword (friends doing this now)
python record_keyword.py --count 500 --speaker "your_name"

# 1b. Download negative samples (Google Speech Commands)
python download_dataset.py

# 1c. Augment keyword data (500 → ~2000 samples)
python augment_data.py

# 1d. Train DS-CNN model
python train_model.py --epochs 50

# 1e. Convert to TFLite INT8
python convert_tflite.py
# → generates: training/models/ds_cnn_quantized.tflite (~45KB)
```

---

### Step 2 — Setup Cloud ASR Server (on Laptop/PC)

```bash
cd cloud_server/

# Install dependencies
pip install -r requirements.txt

# Download Vosk model (only once, ~50MB)
python asr_server.py --download-model

# Start ASR server
python asr_server.py
# Output will show your IP: e.g., "Configure edge with: --server-ip 192.168.1.10"
```

---

### Step 3 — Setup Edge Device (on Raspberry Pi 5)

```bash
# Install dependencies on RPi 5
pip install sounddevice tflite-runtime numpy

# Copy model to RPi (or use shared network path)
# scp training/models/ds_cnn_quantized.tflite pi@<rpi-ip>:~/

# Run edge inference (replace IP with your laptop's IP from Step 2)
cd esp32_firmware/main/
python edge_inference.py --server-ip 192.168.1.10 --server-port 5000

# List audio devices if mic not detected
python edge_inference.py --list-devices
python edge_inference.py --device 1 --server-ip 192.168.1.10
```

---

### Step 4 — Test It!

```
1. Run asr_server.py on laptop      ← Cloud side
2. Run edge_inference.py on RPi 5   ← Edge side
3. Say "Hey Vaani" into the mic
4. Wait 4 seconds (command capture)
5. See transcript appear on RPi screen!
```

---

## Key Metrics to Show Judges

| Metric | How to Measure | Expected Value |
|--------|---------------|----------------|
| **Model file size** | `ls -lh models/ds_cnn_quantized.tflite` | ~45 KB ✅ |
| **RAM used** | `cat /proc/PID/status \| grep VmRSS` | < 256 KB tensor arena ✅ |
| **CPU idle** | `top` while edge_inference.py runs | ~1–2% ✅ |
| **Inference time** | Printed in edge_inference.py output | ~1–2 ms on RPi 5 ✅ |
| **End-to-end latency** | Printed by asr_server.py | < 2000 ms ✅ |

---

## ASR Engine Compliance

| | **Vosk** (Our Choice) | **Whisper** (NOT used) |
|---|---|---|
| Architecture | DNN-HMM (Kaldi) | Transformer |
| Transformer? | NO | YES |
| Open-source | YES | YES |
| Compliant with PS | YES | Debatable |
| Model size | ~50 MB | 150 MB+ |

**Vosk uses TDNN-F (Time-Delay Neural Network) — NOT a transformer**

---

## File Structure

```
Edge Ai/
├── training/
│   ├── record_keyword.py      ← Step 1: Record "Hey Vaani"
│   ├── download_dataset.py    ← Step 1: Negative samples
│   ├── augment_data.py        ← Step 1: Data augmentation
│   ├── train_model.py         ← Step 1: Train DS-CNN
│   ├── convert_tflite.py      ← Step 1: Export TFLite model
│   └── models/
│       └── ds_cnn_quantized.tflite  ← Generated model (~45KB)
│
├── esp32_firmware/main/
│   └── edge_inference.py      ← Step 3: Run on RPi 5
│
└── cloud_server/
    ├── asr_server.py           ← Step 2: Run on laptop
    ├── requirements.txt
    └── vosk_models/            ← Auto-downloaded Vosk model
        └── vosk-model-small-en-us-0.15/
```
