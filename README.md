# 🎙️ Hey Vaani — Edge AI Keyword Spotting (SIH 2026)

> **Wake-word detection on ESP32 using TFLite Micro + live streaming ASR to cloud server.**

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.1-blue)](https://docs.espressif.com/projects/esp-idf/en/latest/)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-Espressif32-orange)](https://platformio.org/)
[![Python](https://img.shields.io/badge/Python-3.10%2B-green)](https://python.org)

---

## 📐 System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  ESP32 (Edge Device)                                            │
│  ┌──────────┐   ┌────────────┐   ┌──────────────────────────┐  │
│  │ MAX4466  │──▶│ ADC@20kHz  │──▶│  MFCC + TFLite Micro KWS │  │
│  │   Mic    │   │ →16kHz DS  │   │  "Hey Vaani" detection   │  │
│  └──────────┘   └────────────┘   └────────────┬─────────────┘  │
└────────────────────────────────────────────────┼────────────────┘
                                                 │ TCP (WiFi)
                                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│  Cloud Server (Laptop / PC)                                     │
│  ┌──────────────┐   ┌──────────────┐   ┌────────────────────┐  │
│  │ Python Flask │──▶│ Faster-Whisper│──▶│  Next.js Dashboard │  │
│  │   server.py  │   │     (ASR)    │   │  localhost:3000     │  │
│  └──────────────┘   └──────────────┘   └────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 🛒 Hardware Required

| Component | Details |
|:---|:---|
| **ESP32-WROOM-32** | Any generic ESP32 dev board (30-pin or 38-pin) |
| **MAX4466 Microphone** | Electret mic with adjustable gain |
| **Micro-USB cable** | Data + power (not charge-only!) |
| **Breadboard + Jumper Wires** | For connections |

### Wiring:

| MAX4466 Pin | ESP32 Pin |
|:---|:---|
| VCC | 3.3V |
| GND | GND |
| OUT | GPIO 32 (ADC1_CH4) |

---

## 🚀 Setup Guide

### Prerequisites

- [PlatformIO](https://platformio.org/install/cli) installed
- Python 3.10+
- Node.js 18+
- Git

---

### Step 1: Clone the repo

```bash
git clone https://github.com/yp2505/Edge-Ai-Sih2026.git
cd Edge-Ai-Sih2026
```

---

### Step 2: Configure WiFi + Server IP in firmware

Open `esp32_firmware/main/main.cpp` and update lines **77–85**:

```cpp
#define CONFIG_WIFI_SSID       "YourWiFiName"      // ← Change this
#define CONFIG_WIFI_PASSWORD   "YourWiFiPassword"  // ← Change this
#define CONFIG_SERVER_IP       "192.168.x.x"       // ← Your laptop's IP (run: hostname -I)
```

> ⚠️ **IMPORTANT:** The ESP32 only supports **2.4 GHz WiFi**. If your network name has "5G" in it, use a different network or your phone's mobile hotspot.
>
> To find your laptop's IP: open a terminal and run `hostname -I`

---

### Step 3: Flash the ESP32

```bash
cd esp32_firmware

# Grant USB port access (Linux only)
sudo chmod 666 /dev/ttyUSB0

# Build and flash
~/.platformio/penv/bin/pio run -t upload -t monitor
```

> 💡 If upload gets stuck at `Connecting.........`, press and hold the **BOOT** button on the ESP32 for 2 seconds.

You should see in the serial monitor:
```
✅ WiFi connected — IP: 10.x.x.x
✅ Mic self-check PASSED
Listening for 'Hey Vaani'...
```

---

### Step 4: Start the Python server

```bash
cd cloud_server

# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install dependencies
pip install -r requirements.txt

# Start server
python3 server.py
```

Server starts on port **5000** and waits for ESP32 connections.

---

### Step 5: Start the dashboard

```bash
cd cloud_server/dashboard

# Install Node dependencies
npm install

# Start development server
npm run dev
```

Open your browser and go to: **http://localhost:3000**

---

## 🎙️ Testing

1. Ensure ESP32 serial monitor shows `Listening for 'Hey Vaani'...`
2. Say **"Hey Vaani"** clearly into the MAX4466 microphone
3. Watch the serial monitor for:
   ```
   🔔 HEY VAANI DETECTED! prob=0.985
   ```
4. The Python server terminal will show the live ASR transcript
5. The dashboard will update with the result

---

## 📊 Performance (on ESP32-WROOM-32)

| Metric | Value |
|:---|:---|
| Model size (Flash) | **46.9 KB** |
| Tensor arena (RAM) | **28.5 KB** |
| Inference CPU usage | **~2%** |
| Keyword-to-connect latency | **~40ms** |
| Total end-to-end latency | **~50ms** |
| IDLE CPU headroom | **~97%** |

---

## 📁 Project Structure

```
Edge-Ai-Sih2026/
├── esp32_firmware/           # ESP32 firmware (PlatformIO / ESP-IDF)
│   ├── main/
│   │   ├── main.cpp          # Core firmware logic
│   │   ├── mfcc.h            # MFCC feature extractor
│   │   ├── model_data.h      # TFLite model as C array
│   │   └── benchmark_cpu.h   # CPU profiling utility
│   ├── platformio.ini        # PlatformIO config
│   └── sdkconfig.defaults    # ESP-IDF config
├── cloud_server/             # Python backend + ASR
│   ├── server.py             # TCP server + Faster-Whisper ASR
│   ├── requirements.txt      # Python dependencies
│   └── dashboard/            # Next.js web dashboard
├── training/                 # Model training scripts
│   └── train_model.py        # Keyword spotting model training
└── README.md
```

---

## 🔧 Troubleshooting

| Problem | Fix |
|:---|:---|
| `Permission denied: /dev/ttyUSB0` | Run `sudo chmod 666 /dev/ttyUSB0` |
| ESP32 stuck at `Connecting.......` | Hold the **BOOT** button during upload |
| `WiFi lost — retry X/10` | Make sure you are using **2.4 GHz** WiFi, not 5 GHz |
| `Mic self-check FAILED` | Check MAX4466 OUT wire is on **GPIO 32** |
| ESP32 keeps rebooting | Mic not detected or memory issue — check serial logs |
| Server says "Waiting for ESP32" | Normal! It waits until "Hey Vaani" is detected |
