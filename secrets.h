// ─────────────────────────────────────────────
//  PISCES MOON OS — SECRETS
//  Copy this file to secrets.h and fill in your keys.
//  secrets.h is in .gitignore — never commit your keys.
//
//  HOW TO GET KEYS:
//
//  GEMINI_API_KEY:
//    1. Go to https://aistudio.google.com/apikey
//    2. Sign in with your Google account
//    3. Click "Create API Key"
//    4. Copy the key here
//    Free tier: 15 req/min, 1M tokens/day — plenty for personal use.
//
//  GOOGLE_CLOUD_API_KEY (optional — Voice Terminal only):
//    Only needed if you want speech-to-text and text-to-speech in
//    the Voice Terminal app. Gemini text mode works without it.
//    1. Go to https://console.cloud.google.com
//    2. Create a project (or use existing)
//    3. Enable: "Cloud Speech-to-Text API" and "Cloud Text-to-Speech API"
//    4. APIs & Services → Credentials → Create API Key
//    5. Copy the key here
//    Note: Cloud STT/TTS are paid APIs after free tier (60 min/month STT free).
//    If you leave this blank, Voice Terminal will show a "no STT key" message
//    and fall back to keyboard input mode.
// ─────────────────────────────────────────────

// Required — Gemini text AI (Terminal, Voice Terminal AI responses)
#define GEMINI_API_KEY  "YOUR_GEMINI_API_KEY_HERE"

// Optional — Google Cloud STT + TTS (Voice Terminal microphone/speaker only)
// Leave as empty string "" to disable voice input/output
#define GOOGLE_CLOUD_API_KEY  ""