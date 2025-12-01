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

# Download Ollama Models
echo "[*] Downloading Ollama models. This may take a while..."
echo "    - Pulling qwen2.5-coder:32b-instruct-q6_k..."
ollama pull qwen2.5-coder:32b-instruct-q6_k
echo "    - Pulling gemma2:27b..."
ollama pull gemma2:27b
echo "    - Pulling llama3.1:70b..."
ollama pull llama3.1:70b
echo "[+] Ollama models pulled successfully."
echo ""

echo "=== Setup Complete ==="
