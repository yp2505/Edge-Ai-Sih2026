# Hey Vaani — EC2 Cloud Server Setup Guide
### Full Stack: Vosk ASR + Ollama SLM Intent Detection (100% Open-Source, Offline)

> **What this does**: Your friend sets up an EC2 server that receives audio from the ESP32,
> converts speech to text (Vosk), then uses a tiny AI model (Qwen2.5) to understand
> what the user wants and returns a structured command back to the ESP32.
> **Zero cloud APIs. Zero API keys. Runs fully offline after setup.**

---

## Architecture Overview

```
ESP32 (wake word detected)
    │
    │  TCP audio stream (port 5000)
    ▼
EC2 t3.micro Server
    ├── Vosk ASR  → "turn on oxygen flow"
    ├── Qwen2.5 (Ollama) → { "intent": "OXYGEN", "action": "ON" }
    └── JSON response → back to ESP32
```

---

## Part 1 — Launch EC2 Instance

### Step 1: Go to AWS Console
1. Open https://aws.amazon.com → Sign In → AWS Console
2. Search for **EC2** → Click "Launch Instance"

### Step 2: Configure the Instance
Fill these settings:

| Setting | Value |
|---|---|
| **Name** | `hey-vaani-server` |
| **OS** | Ubuntu Server 22.04 LTS (Free tier eligible) |
| **Instance type** | `t3.micro` (2 vCPU, 1GB RAM) |
| **Key pair** | Create new → name it `hey-vaani-key` → Download `.pem` file |
| **Storage** | 20 GB gp3 (default is 8GB — change to 20GB for the Vosk + Ollama models) |

### Step 3: Configure Security Group
Click **"Edit"** next to Security Group. Add these **Inbound Rules**:

| Type | Port | Source | Why |
|---|---|---|---|
| SSH | 22 | My IP | For SSH access |
| Custom TCP | 5000 | 0.0.0.0/0 | ESP32 audio stream port |
| Custom TCP | 11434 | 127.0.0.1/32 | Ollama (local only — do NOT expose to internet) |

Click **"Launch Instance"** → Wait 1-2 minutes.

### Step 4: Get Your Server IP
- In EC2 Console → click your instance → copy **"Public IPv4 address"**
- Save this — you'll need it for the ESP32 firmware

---

## Part 2 — Connect to the Server (SSH)

### On Windows (use PowerShell or WSL):
```powershell
# Move the .pem file somewhere safe first
cd ~/Downloads
icacls hey-vaani-key.pem /inheritance:r /grant:r "%USERNAME%":R

# Connect
ssh -i hey-vaani-key.pem ubuntu@YOUR_EC2_IP
```

### On Linux/Mac:
```bash
chmod 400 hey-vaani-key.pem
ssh -i hey-vaani-key.pem ubuntu@YOUR_EC2_IP
```

---

## Part 3 — Install System Dependencies

Run these commands inside the EC2 SSH session:

```bash
# Update system
sudo apt update && sudo apt upgrade -y

# Install Python + tools
sudo apt install -y python3 python3-pip python3-venv git ffmpeg curl wget unzip

# Check Python version (needs 3.8+)
python3 --version
```

---

## Part 4 — Clone the Project

```bash
# Clone the Hey Vaani repository
git clone https://github.com/yp2505/Edge-Ai-Sih2026.git
cd Edge-Ai-Sih2026/cloud_server

# Create virtual environment
python3 -m venv venv
source venv/bin/activate

# Install Python packages
pip install --upgrade pip
pip install faster-whisper vosk numpy soundfile requests
```

---

## Part 5 — Download Vosk Speech Model

```bash
# Make sure you're in cloud_server/ directory
cd ~/Edge-Ai-Sih2026/cloud_server

# Download the small English model (~40MB, fast on CPU)
wget https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip

# Unzip and rename to vosk_model/
unzip vosk-model-small-en-us-0.15.zip
mv vosk-model-small-en-us-0.15 vosk_model

# Verify it's there
ls vosk_model/
# Should show: am/  conf/  graph/  ivector/  README
```

> **Why Vosk?** It runs 100% offline — no internet needed after this download.
> No API key, no account, no rate limits. Runs on 1GB RAM easily.

---

## Part 6 — Install Ollama + Intent Model

Ollama runs the SLM (Small Language Model) that understands natural language commands.

```bash
# Install Ollama (takes ~30 seconds)
curl -fsSL https://ollama.com/install.sh | sh

# Start Ollama service
ollama serve &

# Wait 5 seconds for it to start, then pull the model
sleep 5
ollama pull qwen2.5:0.5b
```

> **What is Qwen2.5-0.5b?**
> A tiny 500-million parameter language model (~400MB).
> Fast enough on a t3.micro CPU (~2-3 seconds per response).
> Understands natural language and extracts intent as structured JSON.

**Test it works:**
```bash
ollama run qwen2.5:0.5b "Reply with JSON only: {\"test\": true}"
# Should print: {"test": true}
# Press Ctrl+D to exit
```

---

## Part 7 — Run the Server

```bash
# Go to cloud_server directory
cd ~/Edge-Ai-Sih2026/cloud_server
source venv/bin/activate

# Make sure Ollama is running (in background)
ollama serve &

# Run with Vosk ASR + Ollama intent detection
python server.py --asr vosk --intent
```

You should see:
```
🚀 Hey Vaani ASR Server
   Port:     5000
   ASR:      Vosk (offline)
   Intent:   Ollama qwen2.5:0.5b (localhost)
✅ Vosk model loaded
✅ Ollama connected
⏳ Waiting for ESP32 connections...
```

---

## Part 8 — Keep Server Running (use screen)

```bash
# Install screen
sudo apt install -y screen

# Start a named screen session
screen -S vaani

# Inside screen: activate venv and start server
cd ~/Edge-Ai-Sih2026/cloud_server
source venv/bin/activate
ollama serve &
sleep 3
python server.py --asr vosk --intent

# Detach from screen (server keeps running after you close SSH)
# Press: Ctrl+A then D

# To re-attach later:
screen -r vaani
```

---

## Part 9 — Update ESP32 Firmware

In `esp32_firmware/main/main.cpp`, find and change the server IP:

```cpp
// Change this:
const char* SERVER_IP = "192.168.1.100";

// To your EC2 public IP:
const char* SERVER_IP = "YOUR_EC2_IP_HERE";
```

Then reflash:
```bash
cd esp32_firmware
pio run -t upload
```

---

## Part 10 — Test the Full Pipeline

1. ESP32 powered on + connected to WiFi
2. Say **"Hey Vaani"** → ESP32 detects wake word
3. Speak: *"turn on the lights"*
4. Watch EC2 terminal — you should see:

```
📋 [1] HVP1: 16000Hz/1ch/16bit streaming
📊 [1] 32,000B (2.00s)
🗣️  [1] Transcribing with Vosk...
🟢 [1] Vosk result: 'turn on the lights'
🤖 [1] INTENT: {"intent": "LIGHTS", "action": "ON", "target": "lights", "value": null}
⏱️  END-TO-END: 1240ms
```

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Connection refused on port 5000` | Check EC2 Security Group — port 5000 must be open to 0.0.0.0/0 |
| `Vosk model not found` | Run: `ls cloud_server/vosk_model/` — must exist |
| `Ollama not running` | Run: `ollama serve &` then wait 5 seconds |
| `Out of memory / OOM kill` | Upgrade to t3.small (2GB RAM) — Ollama needs ~600MB |
| `SSH timeout` | Security Group must have port 22 open for your IP |

---

## Quick Command Reference

```bash
# Vosk only (no intent)
python server.py --asr vosk

# Vosk + SLM intent (recommended)
python server.py --asr vosk --intent

# Whisper (higher accuracy, more RAM)
python server.py --asr whisper --whisper-model base

# Test with a local audio file
python server.py --test path/to/audio.wav

# Check server health
curl http://YOUR_EC2_IP:8080/api/health
```
