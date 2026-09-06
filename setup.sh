#!/bin/bash

# Exit on error
set -e

echo "============================================"
echo "    Hey Vaani - Project Setup Script        "
echo "============================================"
echo ""

# Ensure uv is installed (faster python package installation), optional
if ! command -v uv &> /dev/null; then
    echo "💡 Suggestion: install 'uv' for much faster Python package installations:"
    echo "curl -LsSf https://astral.sh/uv/install.sh | sh"
    echo ""
fi

# 1. Setup Cloud Server Environment
echo "[1/3] Setting up Cloud Server Environment..."
cd cloud_server

if [ ! -d "venv" ]; then
    python3 -m venv venv
    echo "✅ Created virtual environment in cloud_server/venv"
fi

echo "📦 Installing Cloud Server requirements..."
source venv/bin/activate
pip install -r requirements.txt
deactivate
cd ..

# 2. Setup Training Environment
echo ""
echo "[2/3] Setting up Training Environment..."
cd training

if [ ! -d ".venv312" ] && [ ! -d ".venv" ]; then
    python3 -m venv .venv
    echo "✅ Created virtual environment in training/.venv"
fi

echo "📦 Installing Training requirements..."
if [ -d ".venv312" ]; then
    source .venv312/bin/activate
else
    source .venv/bin/activate
fi
pip install -r requirements.txt
deactivate
cd ..

# 3. Setup Dashboard
echo ""
echo "[3/3] Setting up Dashboard (Node.js)..."
cd cloud_server/dashboard

if command -v npm &> /dev/null; then
    echo "📦 Installing Dashboard dependencies..."
    npm install
else
    echo "⚠️  npm is not installed. Please install Node.js and npm to run the dashboard."
fi
cd ../..

echo ""
echo "============================================"
echo "🎉 Setup Complete!                          "
echo "============================================"
echo ""
echo "To run the project, you can use the launch script:"
echo "python3 launch.py"
echo ""
echo "Note: Make sure you have the PlatformIO extension installed in VS Code"
echo "to build and flash the ESP32 firmware in the 'esp32_firmware' folder."
