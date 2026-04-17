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

/**
 * PISCES MOON OS — GEMINI CLIENT v4.0
 * Direct API Key — no VM proxy, no OAuth
 *
 * Architecture:
 *   T-Deck  ──HTTPS──►  generativelanguage.googleapis.com/v1beta/models/
 *                        gemini-2.0-flash:generateContent?key=API_KEY
 *
 * Setup:
 *   1. Get a free API key at https://aistudio.google.com/apikey
 *   2. Copy secrets.h.example → secrets.h
 *   3. Set GEMINI_API_KEY in secrets.h
 *   4. Build and flash
 *
 * Free tier limits (as of 2026):
 *   15 requests/minute, 1,000,000 tokens/day
 *   Sufficient for personal use — no billing required.
 *
 * Conversation history:
 *   Rolling 20-message window (10 exchanges) kept in PSRAM via ArduinoJson.
 *   Each ask_gemini() call includes full history for context continuity.
 *   Saved to /vault/ and NoSQL store for offline browsing.
 *
 * SPI Bus Treaty:
 *   wifi_in_use = true during all HTTP calls.
 *   Wardrive checks this flag before starting WiFi scans.
 *
 * v4.0 changes:
 *   - Removed OAuth device flow, VM proxy, token storage/refresh
 *   - Removed NTP sync (no longer needed without token expiry)
 *   - Direct HTTPS to Google with ?key= query param
 *   - gemini_has_token() → gemini_has_key() (checks compile-time constant)
 *   - Voice Terminal STT/TTS: use GOOGLE_CLOUD_API_KEY from secrets.h
 *   - secrets.h: PISCES_VM_IP/PORT replaced with GEMINI_API_KEY
 */

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include "secrets.h"
#include "gemini_client.h"
#include "database.h"
#include "nosql_store.h"

extern Arduino_GFX *gfx;

// 🚦 TRAFFIC LIGHT — prevents wardrive WiFi conflicts
volatile bool wifi_in_use = false;

// 🧠 CONVERSATIONAL MEMORY BUFFER
JsonDocument chatHistory;

#define GEMINI_NOSQL_CATEGORY "gemini"

// ─────────────────────────────────────────────
//  GEMINI API ENDPOINT
//  Model: gemini-2.0-flash — best speed/quality ratio for embedded use.
//  Change to gemini-1.5-pro for longer context, at higher cost.
// ─────────────────────────────────────────────
#define GEMINI_MODEL    "gemini-2.0-flash"
#define GEMINI_ENDPOINT "https://generativelanguage.googleapis.com/v1beta/models/" \
                         GEMINI_MODEL ":generateContent"

// ─────────────────────────────────────────────
//  KEY CHECK
// ─────────────────────────────────────────────
bool gemini_has_key() {
    // True if key has been set in secrets.h (not the placeholder)
    return strlen(GEMINI_API_KEY) > 10 &&
           strcmp(GEMINI_API_KEY, "YOUR_GEMINI_API_KEY_HERE") != 0;
}

// ─────────────────────────────────────────────
//  INIT
// ─────────────────────────────────────────────
void init_gemini() {
    if (chatHistory.isNull()) {
        chatHistory.to<JsonArray>();
    }
    nosql_init(GEMINI_NOSQL_CATEGORY);

    if (gemini_has_key()) {
        Serial.println("[GEMINI] API key present. Ready.");
    } else {
        Serial.println("[GEMINI] WARNING: No API key set in secrets.h");
    }
}

void reset_gemini_memory() {
    chatHistory.clear();
    chatHistory.to<JsonArray>();
}

void add_message_to_history(String role, String text) {
    JsonArray arr = chatHistory.as<JsonArray>();
    JsonObject msg  = arr.add<JsonObject>();
    msg["role"] = role;
    JsonObject part = msg["parts"].to<JsonArray>().add<JsonObject>();
    part["text"] = text;

    // Rolling 20-message window (10 exchanges)
    if (arr.size() > 20) {
        arr.remove(0);
        arr.remove(0);
    }
}

// ─────────────────────────────────────────────
//  ASK GEMINI — Direct HTTPS to Google API
// ─────────────────────────────────────────────
String ask_gemini(String prompt) {
    if (WiFi.status() != WL_CONNECTED) return "ERROR: No WiFi connection.";

    if (chatHistory.isNull()) init_gemini();

    if (!gemini_has_key()) {
        return "ERROR: No API key. Add GEMINI_API_KEY to secrets.h\n"
               "Get a free key at: aistudio.google.com/apikey";
    }

    // Build request — Gemini v1beta generateContent format
    add_message_to_history("user", prompt);
    save_chat_to_vault("user", prompt.c_str());

    // Construct contents array from history
    // Format: [{"role":"user","parts":[{"text":"..."}]}, ...]
    JsonDocument reqDoc;
    JsonArray contents = reqDoc["contents"].to<JsonArray>();

    // Copy history into request format
    JsonArray history = chatHistory.as<JsonArray>();
    for (JsonObject msg : history) {
        JsonObject c = contents.add<JsonObject>();
        c["role"] = msg["role"].as<String>();
        JsonArray parts = c["parts"].to<JsonArray>();
        JsonObject part = parts.add<JsonObject>();
        part["text"] = msg["parts"][0]["text"].as<String>();
    }

    // Generation config — reasonable defaults
    JsonObject genCfg = reqDoc["generationConfig"].to<JsonObject>();
    genCfg["maxOutputTokens"] = 1024;
    genCfg["temperature"]     = 0.7;

    String payload;
    serializeJson(reqDoc, payload);

    // Build URL with API key
    String url = String(GEMINI_ENDPOINT) + "?key=" + String(GEMINI_API_KEY);

    wifi_in_use = true;
    WiFi.setSleep(true);  // Keep modem sleep — prevents abort() with BT active

    // HTTPS client — Gemini API requires TLS
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert verification — acceptable for API calls
                           // on embedded hardware without a cert store.
                           // The API key itself is the auth mechanism.

    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(60000);  // Gemini can take time on complex prompts
    http.addHeader("Content-Type", "application/json");

    Serial.printf("[GEMINI] POST %d bytes to %s\n", payload.length(), GEMINI_MODEL);

    String ai_response = "";
    int    max_retries  = 2;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        int httpCode = http.POST(payload);
        String raw   = http.getString();

        Serial.printf("[GEMINI] HTTP %d (attempt %d)\n", httpCode, attempt + 1);

        if (httpCode == 200) {
            JsonDocument resp;
            DeserializationError err = deserializeJson(resp, raw);

            if (!err) {
                // Response path: candidates[0].content.parts[0].text
                ai_response = resp["candidates"][0]["content"]["parts"][0]["text"]
                              .as<String>();

                if (ai_response.isEmpty()) {
                    // Check for finish reason / safety block
                    String reason = resp["candidates"][0]["finishReason"] | "UNKNOWN";
                    if (reason == "SAFETY") {
                        ai_response = "Response blocked by safety filter.";
                    } else {
                        ai_response = "ERROR: Empty response (reason: " + reason + ")";
                    }
                }
            } else {
                ai_response = "ERROR: Could not parse response.";
            }

            if (!ai_response.startsWith("ERROR:")) {
                add_message_to_history("model", ai_response);
                save_chat_to_vault("model", ai_response.c_str());

                String entryTitle = prompt.substring(0, 60);
                if (prompt.length() > 60) entryTitle += "...";
                String tag = prompt;
                int sp = tag.indexOf(' ');
                if (sp > 0) tag = tag.substring(0, sp);
                tag.toLowerCase();

                nosql_save_entry(GEMINI_NOSQL_CATEGORY,
                                 entryTitle.c_str(),
                                 ("Q: " + prompt + "\n\nA: " + ai_response).c_str(),
                                 tag.c_str());
            } else {
                // Roll back the user message we added
                JsonArray arr = chatHistory.as<JsonArray>();
                arr.remove(arr.size() - 1);
            }
            break;

        } else if (httpCode == 400) {
            // Bad request — usually prompt too long or malformed
            Serial.printf("[GEMINI] 400 Bad Request: %.200s\n", raw.c_str());
            ai_response = "ERROR: Bad request. Prompt may be too long. Try /clear";
            JsonArray arr = chatHistory.as<JsonArray>();
            arr.remove(arr.size() - 1);
            break;

        } else if (httpCode == 401 || httpCode == 403) {
            Serial.printf("[GEMINI] Auth error %d — check GEMINI_API_KEY\n", httpCode);
            ai_response = "ERROR: Invalid API key. Check secrets.h\n"
                          "Get a key at: aistudio.google.com/apikey";
            JsonArray arr = chatHistory.as<JsonArray>();
            arr.remove(arr.size() - 1);
            break;

        } else if (httpCode == 429) {
            // Rate limit — back off and retry
            Serial.printf("[GEMINI] 429 Rate limit (attempt %d)\n", attempt + 1);
            if (attempt < max_retries) {
                delay(3000 * (attempt + 1));  // 3s, 6s backoff
                continue;
            }
            ai_response = "ERROR: Rate limit hit. Wait a moment and try again.";
            JsonArray arr = chatHistory.as<JsonArray>();
            arr.remove(arr.size() - 1);
            break;

        } else if (httpCode >= 500) {
            Serial.printf("[GEMINI] Server error %d (attempt %d)\n", httpCode, attempt + 1);
            if (attempt < max_retries) {
                delay(2000);
                continue;
            }
            ai_response = "ERROR: Google server error. Try again shortly.";
            JsonArray arr = chatHistory.as<JsonArray>();
            arr.remove(arr.size() - 1);
            break;

        } else {
            Serial.printf("[GEMINI] Unexpected HTTP %d\n", httpCode);
            ai_response = "ERROR: HTTP " + String(httpCode);
            JsonArray arr = chatHistory.as<JsonArray>();
            arr.remove(arr.size() - 1);
            break;
        }
    }

    http.end();
    wifi_in_use = false;
    return ai_response;
}
