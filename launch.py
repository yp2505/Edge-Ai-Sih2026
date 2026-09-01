#!/usr/bin/env python3
"""
Hey Vaani — Auto Launcher
=========================
Run this ONE script and it will automatically:
  1. Detect the ESP32 on USB
  2. Start the Python ASR server (server.py)
  3. Start the Next.js dashboard (npm run dev)
  4. Open the browser to the dashboard

Usage:
    python3 launch.py

Requirements:
    pip install pyserial
"""

import os
import sys
import time
import signal
import subprocess
import threading
import webbrowser
from pathlib import Path

try:
    import serial.tools.list_ports
except ImportError:
    print("📦 Installing required package: pyserial...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyserial", "-q"])
    import serial.tools.list_ports

# ─── Config ────────────────────────────────────────────────────────────────────
PROJECT_ROOT    = Path(__file__).parent
SERVER_DIR      = PROJECT_ROOT / "cloud_server"
DASHBOARD_DIR   = PROJECT_ROOT / "cloud_server" / "dashboard"
DASHBOARD_URL   = "http://localhost:3000"
SERVER_PORT     = 5000

# ESP32 USB identifiers (works for most CP210x and CH340 chips)
ESP32_USB_VIDS  = {0x10C4, 0x1A86, 0x0403, 0x303A}  # Silabs, CH340, FTDI, Espressif

# ─── Globals ───────────────────────────────────────────────────────────────────
processes = []

# ─── Helpers ───────────────────────────────────────────────────────────────────
def print_banner():
    print("\n" + "═" * 60)
    print("  🎙️  Hey Vaani — Auto Launcher  (SIH 2026)")
    print("═" * 60)

def find_esp32_port():
    """Return the serial port of the first connected ESP32, or None."""
    for port in serial.tools.list_ports.comports():
        if port.vid in ESP32_USB_VIDS:
            return port.device
        # Fallback: check description strings
        desc = (port.description or "").lower()
        if any(k in desc for k in ["cp210", "ch340", "uart", "esp32", "usb serial"]):
            return port.device
    return None

def wait_for_esp32():
    """Block until an ESP32 is detected on USB. Returns the port string."""
    print("\n🔍 Waiting for ESP32 to be connected via USB...")
    print("   → Plug in your ESP32 USB cable now!\n")
    while True:
        port = find_esp32_port()
        if port:
            print(f"✅ ESP32 detected on port: {port}")
            # Grant permission on Linux
            if sys.platform.startswith("linux"):
                os.system(f"sudo chmod 666 {port} 2>/dev/null")
            return port
        time.sleep(1)

def start_process(name, cmd, cwd, env=None):
    """Start a subprocess and track it."""
    print(f"\n🚀 Starting {name}...")
    print(f"   Command: {' '.join(cmd)}")
    proc = subprocess.Popen(
        cmd,
        cwd=str(cwd),
        env={**os.environ, **(env or {})},
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    processes.append((name, proc))

    # Stream output in a background thread
    def stream(n, p):
        prefix = f"[{n}]"
        for line in p.stdout:
            print(f"{prefix} {line}", end="")

    t = threading.Thread(target=stream, args=(name, proc), daemon=True)
    t.start()
    return proc

def shutdown(signum=None, frame=None):
    """Gracefully kill all child processes."""
    print("\n\n🛑 Shutting down Hey Vaani launcher...")
    for name, proc in processes:
        if proc.poll() is None:
            print(f"   Stopping {name}...")
            proc.terminate()
    time.sleep(1)
    for name, proc in processes:
        if proc.poll() is None:
            proc.kill()
    print("👋 Goodbye!")
    sys.exit(0)

def check_npm():
    """Check if node_modules exist, install if not."""
    nm = DASHBOARD_DIR / "node_modules"
    if not nm.exists():
        print("\n📦 Installing dashboard dependencies (npm install)...")
        subprocess.run(["npm", "install"], cwd=str(DASHBOARD_DIR), check=True)
        print("✅ npm install complete!")

def open_browser_when_ready():
    """Wait for the dashboard server to be ready, then open the browser."""
    import urllib.request
    def _wait_and_open():
        for _ in range(60):  # wait up to 60 seconds
            try:
                urllib.request.urlopen(DASHBOARD_URL, timeout=1)
                print(f"\n🌐 Opening dashboard: {DASHBOARD_URL}")
                webbrowser.open(DASHBOARD_URL)
                return
            except Exception:
                time.sleep(1)
        print(f"\n⚠️  Dashboard didn't start. Open manually: {DASHBOARD_URL}")
    t = threading.Thread(target=_wait_and_open, daemon=True)
    t.start()

# ─── Main ──────────────────────────────────────────────────────────────────────
def main():
    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    print_banner()

    # Step 1: Detect ESP32
    esp32_port = wait_for_esp32()
    print(f"\n📋 System Status:")
    print(f"   ESP32 Port  : {esp32_port}")
    print(f"   Server Dir  : {SERVER_DIR}")
    print(f"   Dashboard   : {DASHBOARD_URL}")

    # Step 2: Start Python ASR server
    # Use venv python if available
    venv_python = SERVER_DIR / "venv" / "bin" / "python3"
    python_cmd = str(venv_python) if venv_python.exists() else sys.executable

    server_proc = start_process(
        name    = "Python Server",
        cmd     = [python_cmd, "server.py"],
        cwd     = SERVER_DIR,
    )
    time.sleep(2)  # give server a moment to bind the port

    # Step 3: Check npm deps + start dashboard
    check_npm()
    dash_proc = start_process(
        name = "Dashboard",
        cmd  = ["npm", "run", "dev"],
        cwd  = DASHBOARD_DIR,
    )

    # Step 4: Open browser when ready
    open_browser_when_ready()

    print("\n" + "═" * 60)
    print("  ✅ All services started!")
    print(f"  🎙️  Say 'Hey Vaani' into your ESP32 microphone")
    print(f"  📊 Dashboard: {DASHBOARD_URL}")
    print(f"  🔌 ESP32 on : {esp32_port}")
    print("  Press Ctrl+C to stop everything")
    print("═" * 60 + "\n")

    # Keep main thread alive, monitor child processes
    while True:
        time.sleep(5)
        for name, proc in processes:
            if proc.poll() is not None:
                print(f"\n⚠️  {name} exited with code {proc.returncode}! Restarting...")
                processes.remove((name, proc))
                break

if __name__ == "__main__":
    main()
