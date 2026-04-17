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
 * PISCES MOON OS — VOICE TERMINAL v1.2
 * Speech-to-Text → Gemini AI → Text-to-Speech
 *
 * Controls:
 *   SPACE          = hold to record, release to send
 *   T              = toggle TTS output on/off
 *   K              = switch to keyboard input mode
 *   Q / header tap = exit
 *
 * Requires WiFi + two API keys in secrets.h:
 *   GEMINI_API_KEY       — for AI responses (shared with Gemini Terminal)
 *   GOOGLE_CLOUD_API_KEY — for STT + TTS (optional; leave "" to use keyboard mode)
 *
 * If GOOGLE_CLOUD_API_KEY is empty, voice input/output is disabled and
 * the app starts in keyboard mode automatically.
 *
 * v1.2 changes:
 *   - Removed OAuth/VM dependency — uses API keys from secrets.h directly
 *   - STT/TTS use GOOGLE_CLOUD_API_KEY with ?key= query param
 *   - Gemini calls via ask_gemini() (which uses GEMINI_API_KEY)
 *   - Auto-falls-back to keyboard mode if no Cloud API key configured
 */

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <driver/i2s.h>
#include "mbedtls/base64.h"
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "gemini_client.h"
#include "voice_terminal.h"
#include "secrets.h"   // GEMINI_API_KEY, GOOGLE_CLOUD_API_KEY

extern Arduino_GFX *gfx;
extern SdFat sd;
extern volatile bool wifi_in_use;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG       0x0000
#define COL_HEADER   0x0841
#define COL_GREEN    0x07E0
#define COL_CYAN     0x07FF
#define COL_WHITE    0xFFFF
#define COL_DIM      0x4208
#define COL_RED      0xF800
#define COL_AMBER    0xFD20
#define COL_RECORD   0xF800

// ─────────────────────────────────────────────
//  AUDIO CONFIG (matches audio_recorder.cpp)
// ─────────────────────────────────────────────
#define VT_SAMPLE_RATE   16000
#define VT_I2S_PORT      I2S_NUM_1
#define VT_DMA_BUF_COUNT 8
#define VT_DMA_BUF_LEN   1024
#define VT_READ_BYTES    8192
#define VT_MAX_REC_MS    10000UL
#define VT_MIC_MCLK      48
#define VT_MIC_LRCK      21
#define VT_MIC_SCK       47
#define VT_MIC_DIN       14

// ─────────────────────────────────────────────
//  CONVERSATION HISTORY
// ─────────────────────────────────────────────
#define VT_MAX_LINES 8
static String vtLines[VT_MAX_LINES];
static int    vtLineCount = 0;
static bool   vtTTSEnabled = true;
static bool   vtKeyboardMode = false;

// ─────────────────────────────────────────────
//  WAV HEADER
// ─────────────────────────────────────────────
struct VT_WAVHeader {
    char     riff[4]       = {'R','I','F','F'};
    uint32_t fileSize      = 0;
    char     wave[4]       = {'W','A','V','E'};
    char     fmt[4]        = {'f','m','t',' '};
    uint32_t fmtSize       = 16;
    uint16_t audioFormat   = 1;
    uint16_t numChannels   = 1;
    uint32_t sampleRate    = VT_SAMPLE_RATE;
    uint32_t byteRate      = VT_SAMPLE_RATE * 2;
    uint16_t blockAlign    = 2;
    uint16_t bitsPerSample = 16;
    char     data[4]       = {'d','a','t','a'};
    uint32_t dataSize      = 0;
};

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void vtAddLine(const String& who, const String& text) {
    String full = who + ": " + text;
    if (vtLineCount >= VT_MAX_LINES) {
        for (int i = 0; i < VT_MAX_LINES - 1; i++)
            vtLines[i] = vtLines[i + 1];
        vtLineCount = VT_MAX_LINES - 1;
    }
    if (full.length() > 53) full = full.substring(0, 50) + "...";
    vtLines[vtLineCount++] = full;
}

static void vtDrawScreen(const String& statusMsg, uint16_t statusColor,
                          bool recording) {
    gfx->fillRect(0, 0, 320, 22, COL_HEADER);
    gfx->drawFastHLine(0, 22, 320, COL_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_GREEN);
    gfx->setCursor(6, 7);
    gfx->print("VOICE TERMINAL");
    gfx->setTextColor(vtTTSEnabled ? COL_CYAN : COL_DIM);
    gfx->setCursor(120, 7);
    gfx->print(vtTTSEnabled ? "TTS:ON" : "TTS:OFF");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(240, 7);
    gfx->print("[TAP=EXIT]");

    gfx->fillRect(0, 24, 320, 188, COL_BG);
    gfx->setTextSize(1);
    for (int i = 0; i < vtLineCount; i++) {
        uint16_t color = COL_DIM;
        if (vtLines[i].startsWith("You:"))    color = COL_CYAN;
        if (vtLines[i].startsWith("Gemini:")) color = COL_GREEN;
        if (vtLines[i].startsWith("["))       color = COL_AMBER;
        gfx->setTextColor(color);
        gfx->setCursor(4, 26 + i * 22);
        gfx->print(vtLines[i]);
    }

    gfx->fillRect(0, 213, 320, 27, recording ? COL_RECORD : 0x0821);
    gfx->drawFastHLine(0, 213, 320, recording ? COL_RED : COL_DIM);
    gfx->setTextSize(recording ? 2 : 1);
    gfx->setTextColor(recording ? COL_WHITE : statusColor);
    int sx = (320 - (recording ? 14 : statusMsg.length()) * (recording ? 12 : 6)) / 2;
    gfx->setCursor(sx, recording ? 218 : 222);
    gfx->print(recording ? "RECORDING..." : statusMsg);
}

// ─────────────────────────────────────────────
//  I2S MIC INIT / DEINIT
// ─────────────────────────────────────────────
static bool vtInitMic() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = VT_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = VT_DMA_BUF_COUNT,
        .dma_buf_len          = VT_DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = VT_MIC_MCLK,
        .bck_io_num   = VT_MIC_SCK,
        .ws_io_num    = VT_MIC_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = VT_MIC_DIN
    };
    if (i2s_driver_install(VT_I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(VT_I2S_PORT, &pins) != ESP_OK) {
        i2s_driver_uninstall(VT_I2S_PORT);
        return false;
    }
    uint8_t warmup[VT_READ_BYTES];
    size_t read = 0;
    for (int i = 0; i < 4; i++)
        i2s_read(VT_I2S_PORT, warmup, VT_READ_BYTES, &read, pdMS_TO_TICKS(100));
    return true;
}

static void vtDeinitMic() {
    i2s_stop(VT_I2S_PORT);
    i2s_driver_uninstall(VT_I2S_PORT);
}

// ─────────────────────────────────────────────
//  RECORD TO WAV FILE
// ─────────────────────────────────────────────
static bool vtRecord(const char* path) {
    if (!vtInitMic()) {
        vtAddLine("[ERROR]", "Mic init failed");
        return false;
    }

    FsFile file = sd.open(path, O_WRITE | O_CREAT | O_TRUNC);
    if (!file) {
        vtDeinitMic();
        return false;
    }

    VT_WAVHeader hdr;
    file.write((uint8_t*)&hdr, sizeof(hdr));

    uint8_t buf[VT_READ_BYTES];
    uint32_t totalBytes = 0;
    size_t bytesRead = 0;
    unsigned long startMs = millis();

    while (millis() - startMs < VT_MAX_REC_MS) {
        char k = get_keypress();
        if (k != 0) break;
        i2s_read(VT_I2S_PORT, buf, VT_READ_BYTES, &bytesRead, pdMS_TO_TICKS(50));
        if (bytesRead > 0) {
            file.write(buf, bytesRead);
            totalBytes += bytesRead;
        }
        yield();
    }

    hdr.dataSize = totalBytes;
    hdr.fileSize = totalBytes + sizeof(VT_WAVHeader) - 8;
    file.seek(0);
    file.write((uint8_t*)&hdr, sizeof(hdr));
    file.flush();
    file.close();

    vtDeinitMic();
    return totalBytes > 0;
}

// ─────────────────────────────────────────────
//  SPEECH-TO-TEXT
//  Google Cloud STT v1 REST API.
//  Auth: GOOGLE_CLOUD_API_KEY from secrets.h (?key= query param).
//  Returns empty string if no key configured.
// ─────────────────────────────────────────────
static bool vtHasCloudKey() {
    return strlen(GOOGLE_CLOUD_API_KEY) > 0;
}

static String vtSpeechToText(const char* wavPath) {
    if (!vtHasCloudKey()) {
        vtAddLine("[INFO]", "No GOOGLE_CLOUD_API_KEY — keyboard mode only");
        return "";
    }

    FsFile file = sd.open(wavPath, O_READ);
    if (!file) return "";
    uint32_t fileSize = file.fileSize();
    uint8_t* wavData = (uint8_t*)ps_malloc(fileSize);
    if (!wavData) { file.close(); return ""; }
    file.read(wavData, fileSize);
    file.close();

    size_t b64Len = ((fileSize + 2) / 3) * 4 + 1;
    char* b64Data = (char*)ps_malloc(b64Len);
    if (!b64Data) { free(wavData); return ""; }

    size_t outLen = 0;
    mbedtls_base64_encode((unsigned char*)b64Data, b64Len, &outLen,
                          wavData, fileSize);
    free(wavData);
    b64Data[outLen] = '\0';

    String url  = "https://speech.googleapis.com/v1/speech:recognize?key=";
    url += GOOGLE_CLOUD_API_KEY;
    String body = "{\"config\":{\"encoding\":\"LINEAR16\",\"sampleRateHertz\":16000,"
                  "\"languageCode\":\"en-US\"},\"audio\":{\"content\":\"";
    body += b64Data;
    body += "\"}}";
    free(b64Data);

    wifi_in_use = true;
    WiFiClientSecure sttClient;
    sttClient.setInsecure();
    HTTPClient http;
    http.begin(sttClient, url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);

    String transcript = "";
    if (code == 200) {
        String resp = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, resp) == DeserializationError::Ok)
            transcript = doc["results"][0]["alternatives"][0]["transcript"].as<String>();
    } else {
        Serial.printf("[VT] STT HTTP %d\n", code);
        if (code == 403) vtAddLine("[ERROR]", "Cloud API key invalid or STT not enabled");
    }
    http.end();
    wifi_in_use = false;

    return transcript;
}

// ─────────────────────────────────────────────
//  TEXT-TO-SPEECH
//  Google Cloud TTS v1 REST API.
//  Auth: GOOGLE_CLOUD_API_KEY from secrets.h.
//  Silent no-op if no key configured.
// ─────────────────────────────────────────────
static void vtTextToSpeech(const String& text) {
    if (!vtTTSEnabled || text.length() == 0) return;
    if (!vtHasCloudKey()) return;  // Silent — TTS is non-critical

    String ttsText = text.length() > 500 ? text.substring(0, 500) : text;
    ttsText.replace("\"", "\\\"");

    String body = "{\"input\":{\"text\":\"" + ttsText + "\"},"
                  "\"voice\":{\"languageCode\":\"en-US\",\"name\":\"en-US-Neural2-D\"},"
                  "\"audioConfig\":{\"audioEncoding\":\"MP3\"}}";

    wifi_in_use = true;
    WiFiClientSecure ttsClient;
    ttsClient.setInsecure();
    HTTPClient http;
    String url = "https://texttospeech.googleapis.com/v1/text:synthesize?key=";
    url += GOOGLE_CLOUD_API_KEY;
    http.begin(ttsClient, url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);

    if (code == 200) {
        String resp = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, resp) == DeserializationError::Ok) {
            String b64Audio = doc["audioContent"].as<String>();
            size_t b64Len   = b64Audio.length();
            size_t mp3Len   = (b64Len / 4) * 3 + 4;
            uint8_t* mp3Data = (uint8_t*)ps_malloc(mp3Len);
            if (mp3Data) {
                size_t outLen = 0;
                mbedtls_base64_decode(mp3Data, mp3Len, &outLen,
                    (const unsigned char*)b64Audio.c_str(), b64Len);
                FsFile mp3File = sd.open("/tmp_tts.mp3", O_WRITE | O_CREAT | O_TRUNC);
                if (mp3File) {
                    mp3File.write(mp3Data, outLen);
                    mp3File.flush();
                    mp3File.close();
                    // audio.connecttoFS(sd, "/tmp_tts.mp3");  // pending I2S playback integration
                    Serial.println("[VT] TTS audio written to /tmp_tts.mp3");
                }
                free(mp3Data);
            }
        }
    } else {
        Serial.printf("[VT] TTS HTTP %d\n", code);
    }
    http.end();
    wifi_in_use = false;
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_voice_terminal() {
    gfx->fillScreen(COL_BG);
    vtLineCount   = 0;
    vtTTSEnabled  = true;
    vtKeyboardMode = false;

    if (WiFi.status() != WL_CONNECTED) {
        gfx->setTextColor(COL_RED);
        gfx->setTextSize(1);
        gfx->setCursor(10, 60); gfx->print("Voice Terminal requires WiFi.");
        gfx->setCursor(10, 80); gfx->print("Connect via WIFI JOIN first.");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(10, 110); gfx->print("Tap header to exit.");
        while (true) {
            int16_t tx, ty;
            if (get_touch(&tx, &ty) && ty < 30) return;
            if (get_keypress()) return;
            delay(50);
        }
    }

    // API key check
    if (!gemini_has_key()) {
        vtAddLine("[ERROR]", "No GEMINI_API_KEY in secrets.h");
        vtDrawScreen("Set GEMINI_API_KEY in secrets.h and reflash", COL_RED, false);
        delay(3000);
        return;
    }

    // If no Cloud API key, voice I/O is unavailable — auto-switch to keyboard mode
    if (!vtHasCloudKey()) {
        vtKeyboardMode = true;
        vtTTSEnabled   = false;
        vtAddLine("[INFO]", "No GOOGLE_CLOUD_API_KEY — keyboard mode only");
    }

    vtAddLine("[READY]", vtKeyboardMode ? "Type prompt + ENTER  Q=exit"
                                        : "SPACE=record  T=TTS  K=kbd  Q=exit");
    vtDrawScreen(vtKeyboardMode ? "Type your prompt and press ENTER"
                                : "Hold SPACE to record your prompt", COL_DIM, false);

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) delay(10);
            break;
        }

        char k = get_keypress();
        if (k == 'q' || k == 'Q') break;

        if (k == 't' || k == 'T') {
            vtTTSEnabled = !vtTTSEnabled;
            vtDrawScreen(vtTTSEnabled ? "TTS enabled" : "TTS disabled", COL_AMBER, false);
            continue;
        }

        if (k == 'k' || k == 'K') {
            vtKeyboardMode = !vtKeyboardMode;
            vtDrawScreen(vtKeyboardMode ? "Keyboard mode — type + ENTER" : "Voice mode — hold SPACE", COL_CYAN, false);
            continue;
        }

        if (vtKeyboardMode) {
            if (k >= 32 && k <= 126) {
                String prompt = String((char)k);
                while (true) {
                    char c = get_keypress();
                    if (c == 13 || c == 10) break;
                    if (c == 'q' && prompt.length() == 0) goto exit_loop;
                    if (c == 8 || c == 127) { if (prompt.length() > 0) prompt.remove(prompt.length() - 1); }
                    else if (c >= 32 && c <= 126) prompt += c;
                    gfx->fillRect(0, 213, 320, 27, 0x0821);
                    gfx->setTextSize(1);
                    gfx->setTextColor(COL_WHITE);
                    gfx->setCursor(4, 222);
                    gfx->print("> " + prompt.substring(max(0, (int)prompt.length() - 48)));
                    delay(15);
                }
                if (prompt.length() > 0) {
                    vtAddLine("You", prompt);
                    vtDrawScreen("Asking Gemini...", COL_AMBER, false);
                    wifi_in_use = true;
                    String response = ask_gemini(prompt);
                    wifi_in_use = false;
                    vtAddLine("Gemini", response);
                    vtDrawScreen("Done. Type or hold SPACE for next.", COL_GREEN, false);
                    vtTextToSpeech(response);
                }
            }
        } else {
            if (k == ' ') {
                vtDrawScreen("", COL_WHITE, true);
                const char* wavPath = "/vt_rec.wav";
                if (vtRecord(wavPath)) {
                    vtDrawScreen("Transcribing...", COL_AMBER, false);
                    String transcript = vtSpeechToText(wavPath);
                    sd.remove(wavPath);
                    if (transcript.length() > 0) {
                        vtAddLine("You", transcript);
                        vtDrawScreen("Asking Gemini...", COL_AMBER, false);
                        wifi_in_use = true;
                        String response = ask_gemini(transcript);
                        wifi_in_use = false;
                        vtAddLine("Gemini", response);
                        vtDrawScreen("Done. Hold SPACE for next prompt.", COL_GREEN, false);
                        vtTextToSpeech(response);
                    } else {
                        vtAddLine("[ERROR]", "Could not transcribe — try again");
                        vtDrawScreen("Transcription failed. Try again.", COL_RED, false);
                    }
                } else {
                    vtDrawScreen("Recording failed. Try again.", COL_RED, false);
                }
            }
        }

        delay(20);
        yield();
    }

    exit_loop:
    gfx->fillScreen(COL_BG);
}