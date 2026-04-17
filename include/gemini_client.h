// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This program is free software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or any later version.
//
// fluidfortune.com

#ifndef GEMINI_CLIENT_H
#define GEMINI_CLIENT_H

#include <Arduino.h>
#include "secrets.h"   // GEMINI_API_KEY, GOOGLE_CLOUD_API_KEY

// ─────────────────────────────────────────────
//  GEMINI CLIENT v4.0 — Direct API Key
//
//  Architecture:
//    T-Deck  ──HTTPS──►  generativelanguage.googleapis.com
//
//  No VM proxy. No OAuth. No token refresh.
//  API key set in secrets.h (gitignored).
//  Copy secrets.h.example → secrets.h and fill in your key.
//
//  Get a key free at: https://aistudio.google.com/apikey
//  Free tier: 15 req/min, 1M tokens/day.
//
//  Model: gemini-2.0-flash (fast, capable, free tier friendly)
//
//  secrets.h must define:
//    GEMINI_API_KEY        — required for all Gemini calls
//    GOOGLE_CLOUD_API_KEY  — optional, Voice Terminal STT/TTS only
// ─────────────────────────────────────────────

// Fallback defines — prevent compile errors if secrets.h is missing keys.
// Replace these with real values in secrets.h before use.
#ifndef GEMINI_API_KEY
  #define GEMINI_API_KEY ""
#endif
#ifndef GOOGLE_CLOUD_API_KEY
  #define GOOGLE_CLOUD_API_KEY ""
#endif

// Initializes chat history. Call in setup() after SD mounts.
void init_gemini();

// Clears conversational memory (start fresh).
void reset_gemini_memory();

// Returns true if GEMINI_API_KEY is set (not the placeholder).
bool gemini_has_key();

// Send a prompt, get a response. Maintains conversation history.
// Returns error string on failure, response text on success.
String ask_gemini(String prompt);

// Adds a message to rolling conversation history (used by terminal_app).
void add_message_to_history(String role, String text);

// Note: save_chat_to_vault() is declared in database.h

#endif
