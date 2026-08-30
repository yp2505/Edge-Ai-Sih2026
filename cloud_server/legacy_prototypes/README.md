# Legacy Prototypes

This directory contains earlier proof-of-concept server implementations that are **not connected to the firmware** and are **not used in the current pipeline**.

| File | Protocol | ASR Engine | Status |
|---|---|---|---|
| `asr_server_vosk_websocket.py` | FastAPI + WebSocket | Vosk (Kaldi DNN-HMM) | ⛔ Prototype only — not wired to hardware |

## Active Cloud Server

The **only** active cloud server is **`cloud_server/server.py`**:
- Raw TCP socket
- Custom 16-byte HVP1 header (`0x48565031`)  
- `faster-whisper` ASR backend
- This is what `esp32_firmware/main/main.cpp` connects to

Do not use the files in this directory for anything production-related.
