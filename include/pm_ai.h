// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_ai.h — AI chat backends for Pisces Moon apps
//
//  One call, pm_ai_chat(), answers a prompt from whichever backend the
//  user picked:
//
//    COMPANION  The Pisces AI Companion on the user's own computer
//               (pisces-ai-companion repo), which runs local models
//               through Ollama. Reached over WiFi: the home LAN, or
//               from anywhere through Tailscale Funnel (HTTPS).
//               The reply streams in as it is generated.
//    GEMINI     The existing direct Gemini client (gemini_client.cpp),
//               key compiled in from secrets.h. Reply arrives whole.
//
//  Settings live in NVS (internal flash, namespace "pm_ai"), not on
//  SD: they never touch the shared SPI bus, and the device key does
//  not sit on a removable card.
//
//  Conversation history for the companion lives on the computer, so
//  the device never holds a transcript in RAM.
//
//  Protocol: pisces-ai-companion/docs/PROTOCOL.md
// ─────────────────────────────────────────────

#pragma once

#include <Arduino.h>

enum PmAiBackend : uint8_t {
    PM_AI_BACKEND_GEMINI    = 0,
    PM_AI_BACKEND_COMPANION = 1,
};

struct PmAiConfig {
    uint8_t backend = PM_AI_BACKEND_GEMINI;
    String  url;     // companion URL, e.g. 192.168.1.50:8765 or https://mac.tailnet.ts.net
    String  key;     // device key from `pisces_companion.py pair`
    String  model;   // "" = the companion's default model
};

void pm_ai_load_config(PmAiConfig& cfg);
bool pm_ai_save_config(const PmAiConfig& cfg);

// URL and key are both set.
bool pm_ai_companion_configured(const PmAiConfig& cfg);

// Make sure WiFi is connected in CLIENT mode. On the Cardputer this may
// ask the user to stop a running wardrive (wifi_mode.h). Waits up to
// timeout_ms for association.
bool pm_ai_ensure_wifi(String& err, uint32_t timeout_ms = 15000);

enum PmAiResult {
    PM_AI_OK = 0,
    PM_AI_CANCELLED,
    PM_AI_FAILED,
};

// Receives reply text as it arrives (any size, not line-aligned).
typedef void (*PmAiChunkFn)(const char* text, void* ctx);
// Polled while waiting; return true to stop. May be NULL.
typedef bool (*PmAiCancelFn)(void* ctx);

// Ask the configured backend. reset starts a new conversation.
// On PM_AI_FAILED, err says why in a sentence fit for the screen.
PmAiResult pm_ai_chat(const PmAiConfig& cfg, const char* prompt, bool reset,
                      PmAiChunkFn on_chunk, PmAiCancelFn cancel, void* ctx,
                      String& err);

// Companion only. Checks the URL and key; on success device_name is the
// name this device was paired under.
bool pm_ai_check(const PmAiConfig& cfg, String& device_name, String& err);

// Companion only. Fills up to max model names; def gets the companion's
// default. Returns the count, or -1 on failure (err set).
int pm_ai_models(const PmAiConfig& cfg, String* names, int max,
                 String& def, String& err);
