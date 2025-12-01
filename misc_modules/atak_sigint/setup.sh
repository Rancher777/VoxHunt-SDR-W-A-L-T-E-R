#!/bin/bash
# Setup script for the SIGINT AI SDR++ Module

echo "=== SIGINT AI Module Setup ==="

# Set the directory of this script as the working directory
cd "$(dirname "$0")"

# Download Whisper Model
echo "[*] Downloading Whisper model (ggml-tiny.en.bin)..."
wget -q --show-progress -O ggml-tiny.en.bin https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin
if [ $? -ne 0 ]; then
    echo "[!] Failed to download Whisper model. Please check your internet connection."
    exit 1
fi
echo "[+] Whisper model downloaded successfully."
echo ""

# Download Recommended Ollama Model
echo "[*] Downloading recommended lightweight Ollama model (phi)..."
ollama pull phi
if [ $? -ne 0 ]; then
    echo "[!] Failed to pull 'phi' model. Please ensure Ollama is installed and running."
    exit 1
fi
echo "[+] Ollama model 'phi' pulled successfully."
echo ""

echo "=== Setup Complete ==="
echo "You can now build SDR++ and run the module."
echo "For more powerful hardware, you can manually pull other models (e.g., 'ollama pull llama3.1:70b')."
