<!--
Pisces Moon OS
Copyright (C) 2026 Eric Becker / Fluid Fortune
SPDX-License-Identifier: AGPL-3.0-or-later

This program is free software: you can redistribute it
and/or modify it under the terms of the GNU Affero General
Public License as published by the Free Software Foundation,
either version 3 of the License, or any later version.

fluidfortune.com
-->

# Getting Your Google API Key

Pisces Moon OS uses two separate Google APIs, each with its own key. This guide walks you through getting both.

---

## The Short Version

| Key | What It Does | Required? | Cost |
|-----|-------------|-----------|------|
| **Gemini API Key** | Powers the AI Terminal and Voice Terminal responses | Yes, for AI features | Free |
| **Google Cloud API Key** | Powers voice input (microphone → text) and voice output (text → speaker) | No — keyboard mode works without it | Free tier, then paid |

You can use Pisces Moon OS fully without either key. The Gemini Terminal and Voice Terminal just won't connect to AI. Everything else — wardrive, CYBER tools, games, file manager, GPS, LoRa — works without any API key.

---

## Part 1: Gemini API Key (Free)

This key enables the **INTEL → GEMINI** terminal and the AI responses in **COMMS → VOICE**.

### Step 1 — Go to Google AI Studio

Open a browser and go to:
```
https://aistudio.google.com/apikey
```

Sign in with any Google account. You do not need a Google Cloud account. You do not need to enable billing.

### Step 2 — Create an API Key

Click **"Create API key"**.

If prompted to select a project, either create a new one (name it anything) or select an existing Google Cloud project. If you don't have one, click "Create API key in new project" — Google creates it automatically.

Your key appears immediately. It looks like:
```
AIzaSyXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
```

Copy it now. You will not be shown it again, but you can always create a new one.

### Step 3 — Add It to secrets.h

In your Pisces Moon OS project, open `include/secrets.h`:

```c
#define GEMINI_API_KEY  "AIzaSyXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"
```

Replace the placeholder with your actual key. Save the file.

### Step 4 — Build and Flash

```bash
pio run -e esp32s3 --target upload
```

### Free Tier Limits

Google AI Studio's free tier (as of 2026):
- 15 requests per minute
- 1,000,000 tokens per day
- No credit card required

This is sufficient for heavy personal use. The T-Deck typically uses 200–500 tokens per exchange.

### Troubleshooting

**"ERROR: Invalid API key"** — The key in `secrets.h` doesn't match what Google issued. Copy it again carefully; no spaces, no line breaks.

**"ERROR: Rate limit hit"** — You've exceeded 15 requests per minute. Wait 60 seconds and try again. The OS implements automatic retry with backoff.

**"ERROR: No WiFi connection"** — Connect to WiFi via COMMS → WIFI JOIN before opening the terminal.

---

## Part 2: Google Cloud API Key (Optional)

This key enables voice input and voice output in the **COMMS → VOICE** terminal. Without it, the Voice Terminal works in keyboard mode — you type prompts, the AI responds in text. Everything still works, just without the microphone and speaker.

### What You're Enabling

- **Speech-to-Text (STT)** — Google Cloud Speech-to-Text API. Converts your spoken words (recorded by the ES7210 microphone) into text for the AI.
- **Text-to-Speech (TTS)** — Google Cloud Text-to-Speech API. Converts the AI's response into spoken audio through the T-Deck's speaker.

### Free Tier

Google Cloud STT: **60 minutes per month** free, then ~$0.006/minute.
Google Cloud TTS: **1 million characters per month** free, then ~$0.000004/character.

For typical use (a few minutes of voice interaction per day), you will likely stay within the free tier. Enable billing anyway — without it, the APIs are blocked even within the free tier limits.

### Step 1 — Create a Google Cloud Project

Go to:
```
https://console.cloud.google.com
```

If you already created a project for the Gemini key, you can use the same one. Otherwise, click **"New Project"** in the top navigation, give it a name, and click **Create**.

### Step 2 — Enable Billing

In the left menu, go to **Billing**. Link a credit card. You will not be charged for usage within the free tier, but the APIs require billing to be enabled.

### Step 3 — Enable the APIs

In the search bar at the top, search for and enable each of these:

**"Cloud Speech-to-Text API"**
- Click it in the results
- Click **"Enable"**
- Wait for the confirmation

**"Cloud Text-to-Speech API"**
- Search for it
- Click **"Enable"**

### Step 4 — Create an API Key

In the left menu, go to **APIs & Services → Credentials**.

Click **"+ Create Credentials"** → **"API Key"**.

Your key appears. Copy it.

**Optional but recommended:** Click **"Restrict Key"** → under "API restrictions", select "Restrict key" → check both "Cloud Speech-to-Text API" and "Cloud Text-to-Speech API". This limits the key to only those two APIs if it's ever exposed.

### Step 5 — Add It to secrets.h

```c
#define GOOGLE_CLOUD_API_KEY  "AIzaSyYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY"
```

This is a different key from the Gemini key. Both go in `secrets.h`. Rebuild and reflash.

### Step 6 — Verify in Voice Terminal

Open **COMMS → VOICE**. The status line should show:
```
SPACE=record  T=TTS  K=keyboard  Q=exit
```

If it instead shows keyboard mode is active and mentions no Cloud API key, the key either wasn't set or didn't get compiled in. Double-check `secrets.h` and rebuild.

Hold **SPACE** to record a voice prompt. Release to send. The T-Deck will transcribe, send to Gemini, and speak the response.

### Troubleshooting

**"Cloud API key invalid or STT not enabled"** — Either the key is wrong, or the Speech-to-Text API isn't enabled in your Cloud project. Go back to Cloud Console → APIs & Services → Enabled APIs and confirm both APIs are listed.

**Voice Terminal starts in keyboard mode automatically** — `GOOGLE_CLOUD_API_KEY` is empty or not defined. Add it to `secrets.h` and rebuild.

**TTS produces no audio** — The Text-to-Speech response is currently saved to `/tmp_tts.mp3` on the SD card (I2S playback integration is pending in a future update). The transcription and AI response still work.

---

## Security Notes

**`secrets.h` is in `.gitignore`** and will never be committed to the repository. Do not add it manually. Do not paste your API keys anywhere in the codebase outside of `secrets.h`.

If you accidentally expose a key, revoke it immediately in the Google console and generate a new one. Revocation is instant.

**API keys on embedded devices:** The keys in `secrets.h` are compiled into the firmware binary. Anyone who can extract the flash contents of your T-Deck can recover them. For personal use this is acceptable. If you are distributing modified firmware to others, do not include your personal API keys — instruct users to add their own.

---

## Both Keys in secrets.h

The complete `secrets.h` for a fully configured device:

```c
// Required — Gemini AI Terminal and Voice Terminal AI responses
#define GEMINI_API_KEY  "AIzaSyXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"

// Optional — Voice Terminal microphone input and speaker output
// Leave as "" to use keyboard mode in Voice Terminal
#define GOOGLE_CLOUD_API_KEY  "AIzaSyYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY"
```

Save, rebuild, flash. That's the entire configuration.

---

*Pisces Moon OS — fluidfortune.com*
