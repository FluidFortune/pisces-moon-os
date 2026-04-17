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

#ifndef VOICE_TERMINAL_H
#define VOICE_TERMINAL_H

/**
 * PISCES MOON OS — VOICE TERMINAL
 * Speech-to-Text input + Gemini AI + Text-to-Speech output
 *
 * Addresses the T-Deck keyboard limitation for longer text entry.
 * Press SPACE to record, speak your prompt, Gemini responds in text
 * and synthesized speech via the onboard speaker.
 *
 * STT: Google Speech-to-Text API (cloud, requires WiFi)
 * TTS: Google Text-to-Speech API (cloud, requires WiFi)
 * AI:  Gemini via existing ask_gemini() infrastructure
 *
 * Hardware note: GPIO0 (trackball click) shares pin with ES7210 mic.
 * Trackball click unavailable during active recording — use SPACE.
 *
 * SPI Bus Treaty: WiFi HTTP calls use wifi_in_use flag.
 * SD card writes (vault) follow hit-and-run pattern.
 */

void run_voice_terminal();

#endif