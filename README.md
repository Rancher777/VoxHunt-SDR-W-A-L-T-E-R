# SIGINT AI (formerly VoxHunt + W.A.L.T.E.R.)

**AI-powered voice hunting and transcription for SDR++**

This module integrates SDR++ with local AI models to continuously scan the spectrum, lock onto active voice transmissions, and transcribe them in real time.

When someone keys up and talks — W*A*L*t*E*R hears it and writes it down.

### W*A*L*t*E*R
**W**hisper
**A**ssisted
**L**istening
**t**ranscription &
**E**lectronic
**R**econnaissance

*Yes, he’s named after Corporal Walter “Radar” O’Reilly from M.A.S.H.*
*Because just like Radar, he hears things before anyone else does.*

---

### Features
- **Automatic Voice Detection:** The "VoxHunt" feature automatically detects voice transmissions.
- **Real-Time Transcription:** Live transcription of signals using a local Whisper model.
- **AI Analysis:** The "W*A*L*t*E*R" feature sends transcripts to a local Ollama LLM for analysis and summarization, based on a configurable system prompt.
- **Model Management:**
    - Automatically detects available Ollama models.
    - "Model Warming" feature: When you select a new model from the dropdown, the module pre-loads it to prevent server errors, and unloads the previous model to conserve resources.
- **File Logging:** All module activity, including raw AI responses, is logged to `/tmp/atak_sigint.log` for easy debugging.
- **Zero Cloud Dependency:** Everything runs 100% locally on your machine.

### Requirements
- SDR++ (The module is built against the main branch)
- A CUDA-capable NVIDIA GPU is recommended for Whisper.
- [Ollama](https://ollama.com/) installed and running.

---

### Installation & Setup

**1. Clone the Repository**

You must clone this repository *with its submodules*. The `whisper.cpp` code is included as a submodule.
```bash
git clone --recurse-submodules https://github.com/Rancher777/VoxHunt-SDR-W-A-L-T-E-R.git
```
Navigate into the main project directory:
```bash
cd VoxHunt-SDR-W-A-L-T-E-R
```

**2. Run the Setup Script**

The module requires several AI models to function. A setup script is provided to download them for you.

Navigate to the module's directory and run the script:
```bash
cd misc_modules/atak_sigint/
./setup.sh
```
This will download the `ggml-tiny.en.bin` Whisper model and pull the default Ollama models.

**3. Build SDR++ and the Module**

Navigate back to the root of the project and follow the standard SDR++ build process.
```bash
cd ../.. 
mkdir build
cd build
cmake ..
make
sudo make install
```
The SDR++ build system will automatically find and compile the `SIGINT AI` module.

**4. Run the Application**

After the installation is complete, you can run SDR++ from your build directory:
`./install_dir/bin/sdrpp`

---

### Usage

1.  Start SDR++.
2.  In the "Module Manager", find "SIGINT AI" in the list and enable it.
3.  Ensure your external Ollama server is running. The module will detect it automatically.
4.  Enable the "VoxHunt" and "W*A*L*t*E*R" checkboxes to begin detection and analysis.
5.  To switch AI models, simply select a new one from the dropdown. The UI will show a "Warming model..." status and will be ready to use once the message disappears.

“Beep-beep-beep… somebody’s on the air, Colonel.”