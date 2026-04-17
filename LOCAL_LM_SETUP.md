# Voice Terminal — Local LM Setup Guide

**"The intelligence runs on your metal."**

The Voice Terminal has three swappable backends. By default all three point at
Google Cloud APIs via the Pisces Moon VM proxy. Every one of them can be
replaced with a local alternative — no firmware changes required for the AI
backend, minimal changes for STT/TTS.

---

## How the backend works

```
T-Deck mic → WAV → [STT endpoint] → transcript
transcript → [AI endpoint] → response text
response text → [TTS endpoint] → audio → speaker
```

The AI endpoint is a simple HTTP POST. The STT and TTS endpoints use the same
call signatures as the Google Cloud APIs, so any compatible local server
drops in as a replacement.

---

## Backend 1 — AI (the LM itself)

### Default
Pisces Moon VM (`secrets.h: PISCES_VM_IP / PISCES_VM_PORT`) proxies to Google Gemini.

### To use a local LM

**You need:** Any machine on the same WiFi as the T-Deck running an LM server
that accepts `POST /gemini` with a JSON body `{"prompt": "..."}` and returns
`{"response": "..."}`.

**Change in `secrets.h`:**
```cpp
#define PISCES_VM_IP   "192.168.1.50"   // Your local machine IP
#define PISCES_VM_PORT  5000             // Port your server listens on
```

That's it. No recompile of any other file. The `ask_gemini()` function just
posts to whatever `PISCES_VM_IP:PISCES_VM_PORT/gemini` resolves to.

### Compatible local LM servers

**Ollama** (easiest — macOS, Linux, Windows)
```bash
ollama serve   # Runs on port 11434 by default
```
Write a thin Flask/FastAPI wrapper that receives the Pisces Moon POST format
and forwards to Ollama's `/api/generate`:
```python
from flask import Flask, request, jsonify
import requests

app = Flask(__name__)

@app.route('/gemini', methods=['POST'])
def gemini():
    prompt = request.json.get('prompt', '')
    r = requests.post('http://localhost:11434/api/generate',
                      json={'model': 'llama3', 'prompt': prompt, 'stream': False})
    return jsonify({'response': r.json().get('response', '')})

app.run(host='0.0.0.0', port=5000)
```
Run: `python3 wrapper.py`

**The Phantom** (Fluid Fortune's local AI — designed for this exact pattern)
The Phantom already exposes an HTTP endpoint. Point `PISCES_VM_IP` at the
machine running The Phantom and set the port to match. No wrapper needed if
The Phantom supports the `/gemini` POST format — check its README.

**LM Studio** (GUI — macOS, Windows, Linux)
Enable the local server in LM Studio (port 1234 by default). Same wrapper
pattern as Ollama, substitute LM Studio's OpenAI-compatible endpoint.

**llama.cpp server**
```bash
./server -m model.gguf --host 0.0.0.0 --port 8080
```
Same wrapper approach — bridge the `/gemini` POST to llama.cpp's
`/completion` endpoint.

### Conversation history

The Pisces Moon VM proxy forwards the full conversation history to Gemini,
enabling multi-turn context. If your local wrapper is stateless (ignores the
`history` field in the POST body), you get single-turn responses. For full
multi-turn support, pass the history through to your LM's context window.

The POST body the T-Deck sends:
```json
{
  "prompt": "user's latest message",
  "history": [
    {"role": "user", "content": "previous message"},
    {"role": "model", "content": "previous response"}
  ]
}
```

---

## Backend 2 — Speech-to-Text

### Default
Google Cloud Speech-to-Text v1 REST API, authenticated via OAuth Bearer token.
Called from `vtSpeechToText()` in `voice_terminal.cpp`.

### To use a local STT server

**You need:** A machine running `whisper.cpp` (or faster-whisper, WhisperX)
as an HTTP server that accepts a WAV file POST and returns a transcript.

**whisper.cpp server setup:**
```bash
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp && make
./models/download-ggml-model.sh base.en   # 150MB, fast, good quality
./server -m models/ggml-base.en.bin --host 0.0.0.0 --port 9000
```

The server accepts `POST /inference` with the WAV as form data and returns JSON
with a `text` field.

**Edit `vtSpeechToText()` in `voice_terminal.cpp`:**

Replace the Google STT URL and body construction with:
```cpp
// Local whisper.cpp server
String url = "http://192.168.1.50:9000/inference";
// ... (send WAV as multipart/form-data instead of base64 JSON)
```

A cleaner approach is a thin Python wrapper that accepts the same base64 JSON
format as Google STT and forwards to whisper.cpp — then the firmware needs
no changes, just swap the URL constant.

**Faster alternatives for embedded use:**
- `whisper.cpp` with `ggml-tiny.en.bin` (75MB, ~2s on M-series Mac)
- `faster-whisper` with `tiny.en` model via REST wrapper
- Vosk (offline, very fast, lower accuracy than Whisper)

### Disabling STT entirely

If you don't need voice input, use keyboard mode exclusively:
press `K` in the Voice Terminal to toggle to keyboard input.
No STT calls are made in keyboard mode.

---

## Backend 3 — Text-to-Speech

### Default
Google Cloud Text-to-Speech v1 REST API, authenticated via OAuth Bearer token.
Called from `vtTextToSpeech()` in `voice_terminal.cpp`.

### Option A: Disable TTS

Press `T` in the Voice Terminal to toggle TTS off. The AI response displays
as text in the conversation history. No audio output.

### Option B: Local TTS server

**Piper TTS** (fast, runs on a Pi 4 or Mac)
```bash
pip install piper-tts
python3 -m piper.http_server --model en_US-lessac-medium --port 5500
```
Accepts `GET /synthesize?text=...` and returns WAV audio.

**Coqui TTS**
```bash
pip install TTS
tts-server --model_name tts_models/en/ljspeech/tacotron2-DDC --port 5500
```

**Edit `vtTextToSpeech()` in `voice_terminal.cpp`:**
Replace the Google TTS URL with your local server endpoint. The audio response
handling (WAV playback via I2S) stays the same regardless of source.

---

## Quick reference — what to change for full local stack

| Component | File | What to change |
|-----------|------|----------------|
| AI backend | `secrets.h` | `PISCES_VM_IP` / `PISCES_VM_PORT` + run local wrapper |
| STT | `voice_terminal.cpp` | URL in `vtSpeechToText()` |
| TTS | `voice_terminal.cpp` | URL in `vtTextToSpeech()` |
| All off | T-Deck keyboard | Press `K` for keyboard, `T` to disable TTS |

The minimum viable local setup is just the AI backend — point the IP at a
machine running Ollama with a thin wrapper, keep Google STT/TTS, and you have
a device that processes prompts on your own hardware with cloud audio services.
The maximum local setup runs everything on a laptop or Pi on the same WiFi,
no Google dependency at all.

---

## Network topology

```
T-Deck Plus                 Your LAN
┌──────────────┐            ┌─────────────────────────────────┐
│  Voice       │            │                                 │
│  Terminal    │──WiFi────▶ │  192.168.1.50:5000 (AI wrapper) │
│              │            │  192.168.1.50:9000 (whisper.cpp) │
│  secrets.h:  │            │  192.168.1.50:5500 (Piper TTS)  │
│  VM_IP=      │            │                                 │
│  192.168.1.50│            │  Ollama / The Phantom / llama.cpp│
└──────────────┘            └─────────────────────────────────┘
```

The T-Deck never needs to reach the internet once local servers are running.
All three backends operate over LAN HTTP — no TLS required on the local network,
no API keys, no quotas, no data leaving your perimeter.

---

*Pisces Moon OS — Local Intelligence*
*"We use the network to gather the data. The intelligence runs on your metal."*
