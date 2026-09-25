# 🚀 Hey Vaani - Project Setup Guide

Follow these instructions to completely set up the Hey Vaani project on a new laptop.

## 📋 Prerequisites
Before you begin, ensure you have the following installed on your system:
1. **Python 3.10 or higher**: [Download here](https://www.python.org/downloads/) (Make sure to check "Add Python to PATH" during installation)
2. **Node.js (v18 or higher)**: [Download here](https://nodejs.org/)
3. **VS Code**: [Download here](https://code.visualstudio.com/) 
   - Install the **PlatformIO IDE** extension in VS Code if you need to flash the ESP32 chips.

---

## 🛠️ Step 1: Install Python Backend Dependencies
The backend runs the Whisper ASR engine and handles WebSocket connections.

1. Open a terminal and navigate to the `cloud_server` directory:
   ```bash
   cd cloud_server
   ```
2. (Optional but Recommended) Create a virtual environment:
   ```bash
   python -m venv venv
   # On Windows:
   venv\Scripts\activate
   # On Mac/Linux:
   source venv/bin/activate
   ```
3. Install the required Python packages:
   ```bash
   pip install -r requirements.txt
   ```
4. Go back to the root directory:
   ```bash
   cd ..
   ```

---

## 💻 Step 2: Install Frontend Dashboard Dependencies
The dashboard is built with React (Vite).

1. Open a terminal and navigate to the dashboard directory:
   ```bash
   cd cloud_server/dashboard
   ```
2. Install the Node.js packages:
   ```bash
   npm install
   ```
3. Go back to the root directory:
   ```bash
   cd ../..
   ```

---

## ⚡ Step 3: Flash the ESP32 Firmware (Only if necessary)
*Note: If the ESP32 chips are already flashed, you can skip this step!*

**⚠️ Important Windows Note**: The ESP-IDF framework requires that your project path does **not** contain any spaces (e.g., avoid folders like `Other files from K drive`). If your path has spaces, temporarily copy the `esp32_firmware` folder to a simple path like `C:\temp_esp32` before building.

1. Open the `esp32_firmware` directory in VS Code.
2. Click the **PlatformIO: Build** (checkmark icon) in the bottom toolbar.
3. Plug in the ESP32 via USB and click **PlatformIO: Upload** (right arrow icon) to flash the chip.

---

## 🚀 Step 4: Run the Project
We have an auto-launcher script that will automatically start the backend server, launch the frontend dashboard, and open your browser!

1. Open a terminal in the main project folder.
2. Run the launch script:
   ```bash
   python launch.py
   ```
3. The script will automatically:
   - Detect the ESP32 connection (Wait for it to find the COM port).
   - Start the backend API server.
   - Start the React frontend on `http://localhost:5173`.
   - Open your browser to the dashboard!

---

### 🎉 You're all set! 
Say **"Hey Vaani"** near the ESP32 to see the real-time speech recognition trigger on your new dashboard!
