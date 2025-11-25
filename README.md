# VoxHunt + W.A.L.T.E.R.

**AI-powered voice hunting, transcription, and translation plugin for SDR++**

VoxHunt continuously scans the spectrum, locks onto active voice transmissions, transcribes them in real time with OpenAI Whisper (CUDA-accelerated), and optionally translates with a local Llama-3-8B instruct model.

When someone keys up and talks — W.A.L.T.E.R. hears it, writes it down, and (if you want) translates it before they even let go of the mic.

### W.A.L.T.E.R.
**W**hisper  
**A**ssisted  
**L**istening  
**T**ranscription &  
**E**lectronic  
**R**econnaissance

Yes, he’s named after Corporal Walter “Radar” O’Reilly from M.A.S.H.  
Because just like Radar, he hears things before anyone else does.

### Features
- Automatic voice activity detection & frequency hunting (works with SDR++ scanner module)
- Real-time transcription via Whisper Turbo / Large-v3 (CUDA 13)
- Optional on-the-fly translation (Llama-3-8B-Instruct-Q5_K_M or your model of choice)
- Logging to timestamped plain-text, JSON, or Cursor-on-Target (CoT) events for ATAK/WinTAK
- “Hunt Mode” — set a bandwidth and let it relentlessly sweep for voice
- Zero cloud dependency — everything runs locally

### Requirements
- SDR++ nightly (≥ 1.2.0 recommended, scanner module required)
- NVIDIA GPU + CUDA 13
- whisper.cpp (bundled) + ggml Llama-3 model in models/

### Quick Start
1. Drop `voxhunt.so` / `voxhunt.dll` into SDR++’s modules folder
2. Place your Whisper and Llama ggml models in `./modules/voxhunt/models/`
3. Restart SDR++, enable VoxHunt in the module list
4. Hit the big “HUNT” button or just leave it running in background monitor mode

W.A.L.T.E.R. will start listening. When he hears someone talking, you’ll know.

“Beep-beep-beep… somebody’s on the air, Colonel.”
