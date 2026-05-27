// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  voice_terminal_v2.cpp — Multi-device voice terminal
//
//  ENTRY: void run_voice_terminal();   (replaces v1)
//
//  Pipeline (device-agnostic):
//    1. Capture audio from mic via I2S → /vt_rec.wav on SD
//    2. POST WAV (base64) to Google Speech-to-Text v1 → transcript
//    3. ask_gemini(transcript) → response text
//    4. POST response to Google Text-to-Speech v1 → MP3 → /tmp_tts.mp3
//    5. Play /tmp_tts.mp3 through speaker via ESP32-audioI2S
//
//  Per-device:
//    C28P / Heltec V4 (portrait 240x320, ES8311, touch):
//      - Mic capture via ES8311 ADC on PIN_I2S_DIN
//      - Touch-and-hold big red button to record
//    T-Deck Plus (landscape 320x240, ES7210 mic + ES8311 spk, keyboard):
//      - Mic capture via separate ES7210 I2S pins (legacy)
//      - SPACE key to record
//    T-LoRa Pager (landscape 480x222, ES8311, encoder + keyboard):
//      - Mic capture via ES8311 ADC
//      - Encoder click or 'r' key to record
//    Cardputer ADV (landscape 240x135, ES8311 via M5 library):
//      - Mic capture NOT YET IMPLEMENTED — auto-falls-back to keyboard mode
//
//  Requires:
//    secrets.h: GEMINI_API_KEY (required)
//                GOOGLE_CLOUD_API_KEY (optional — if empty, keyboard-only)
// ─────────────────────────────────────────────

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
#include "gemini_client.h"
#include "voice_terminal.h"
#include "secrets.h"

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
extern bool c28p_touch_read(int16_t *x, int16_t *y);
extern "C" bool c28p_audio_begin();
extern "C" void c28p_audio_stop();
#endif

#if defined(DEVICE_TDECK_PLUS)
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#endif

#if defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
#include "game_input.h"
#endif

#include <Audio.h>  // ESP32-audioI2S for MP3 TTS playback

extern Arduino_GFX *gfx;
extern SdFat sd;
extern volatile bool wifi_in_use;

// ─── Colors ───
#define COL_BG       0x0000
#define COL_HEADER   0x0841
#define COL_GREEN    0x07E0
#define COL_CYAN     0x07FF
#define COL_WHITE    0xFFFF
#define COL_DIM      0x4208
#define COL_RED      0xF800
#define COL_AMBER    0xFD20
#define COL_RECORD   0xF800

// ─── Audio config ───
#define VT_SAMPLE_RATE   16000
#define VT_DMA_BUF_COUNT 8
#define VT_DMA_BUF_LEN   1024
#define VT_READ_BYTES    4096
#define VT_MAX_REC_MS    8000UL   // 8 second cap

// Which I2S port the mic uses (TX speaker uses I2S_NUM_0 on most devices)
#if defined(DEVICE_TDECK_PLUS)
  #define VT_I2S_MIC_PORT  I2S_NUM_1
  // T-Deck Plus uses ES7210 on separate pins (legacy from v1)
  #define VT_MIC_MCLK      48
  #define VT_MIC_LRCK      21
  #define VT_MIC_SCK       47
  #define VT_MIC_DIN       14
#elif defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4) || defined(DEVICE_TLORAPAGER)
  // Use I2S_NUM_1 for mic so we don't fight the speaker's I2S_NUM_0
  #define VT_I2S_MIC_PORT  I2S_NUM_1
  #define VT_MIC_MCLK      PIN_I2S_MCLK
  #define VT_MIC_LRCK      PIN_I2S_LRCK
  #define VT_MIC_SCK       PIN_I2S_SCLK
  #define VT_MIC_DIN       PIN_I2S_DIN
#else
  // Cardputer ADV and any other device without mic support — vtInitMic
  // returns false, vtRecord short-circuits before touching I2S. These
  // sentinel values exist only so the function body compiles.
  #define VT_I2S_MIC_PORT  I2S_NUM_1
  #define VT_MIC_MCLK      -1
  #define VT_MIC_LRCK      -1
  #define VT_MIC_SCK       -1
  #define VT_MIC_DIN       -1
#endif

// ─── Conversation state ───
#define VT_MAX_LINES 8
static String vtLines[VT_MAX_LINES];
static int    vtLineCount   = 0;
static bool   vtTTSEnabled  = true;
static bool   vtKeyboardMode = false;

// ─── WAV header ───
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

// ─── Helpers ───
static bool vtHasCloudKey() {
    return strlen(GOOGLE_CLOUD_API_KEY) > 0;
}

static void vtAddLine(const char* speaker, const String& text) {
    if (vtLineCount >= VT_MAX_LINES) {
        for (int i = 0; i < VT_MAX_LINES - 1; i++) vtLines[i] = vtLines[i + 1];
        vtLineCount = VT_MAX_LINES - 1;
    }
    vtLines[vtLineCount++] = String(speaker) + ": " + text;
}

// ─────────────────────────────────────────────
//  PER-DEVICE LAYOUT CONSTANTS
// ─────────────────────────────────────────────
#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
  #define VT_SCREEN_W    240
  #define VT_SCREEN_H    320
  #define VT_HEADER_H    14
  #define VT_STATUS_Y    298
  #define VT_RECBTN_Y    250
  #define VT_RECBTN_H    44
  #define VT_LINE_H      14
#elif defined(DEVICE_TDECK_PLUS)
  #define VT_SCREEN_W    320
  #define VT_SCREEN_H    240
  #define VT_HEADER_H    22
  #define VT_STATUS_Y    218
  #define VT_LINE_H      14
#elif defined(DEVICE_TLORAPAGER)
  #define VT_SCREEN_W    480
  #define VT_SCREEN_H    222
  #define VT_HEADER_H    22
  #define VT_STATUS_Y    200
  #define VT_LINE_H      14
#elif defined(DEVICE_CARDPUTER_ADV)
  #define VT_SCREEN_W    240
  #define VT_SCREEN_H    135
  #define VT_HEADER_H    14
  #define VT_STATUS_Y    120
  #define VT_LINE_H      10
#else
  #define VT_SCREEN_W    240
  #define VT_SCREEN_H    320
  #define VT_HEADER_H    14
  #define VT_STATUS_Y    300
  #define VT_LINE_H      14
#endif

// ─────────────────────────────────────────────
//  UI RENDER (device-aware but shared code path)
// ─────────────────────────────────────────────
static void vtDrawHeader() {
    gfx->fillRect(0, 0, VT_SCREEN_W, VT_HEADER_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_CYAN);
    gfx->setCursor(4, (VT_HEADER_H - 8) / 2);
    gfx->print("VOICE TERMINAL");
    gfx->setTextColor(COL_DIM);
    int exit_x = VT_SCREEN_W - 50;
    gfx->setCursor(exit_x, (VT_HEADER_H - 8) / 2);
    gfx->print("< EXIT");
}

// Where the conversation area ends — record button on touch devices,
// status line on keyboard/button devices.
#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
  #define VT_CONV_BOTTOM   VT_RECBTN_Y
#else
  #define VT_CONV_BOTTOM   VT_STATUS_Y
#endif

static void vtDrawConversation() {
    int top = VT_HEADER_H + 4;
    int max_lines_visible = (VT_CONV_BOTTOM - top - 4) / VT_LINE_H;
    if (max_lines_visible < 1) max_lines_visible = 1;
    if (max_lines_visible > VT_MAX_LINES) max_lines_visible = VT_MAX_LINES;

    int start = vtLineCount > max_lines_visible ? vtLineCount - max_lines_visible : 0;
    gfx->fillRect(0, top, VT_SCREEN_W, max_lines_visible * VT_LINE_H, COL_BG);

    gfx->setTextSize(1);
    for (int i = start; i < vtLineCount; i++) {
        int y = top + (i - start) * VT_LINE_H;
        // Color based on speaker
        if (vtLines[i].startsWith("You:")) gfx->setTextColor(COL_GREEN);
        else if (vtLines[i].startsWith("Gemini:")) gfx->setTextColor(COL_CYAN);
        else if (vtLines[i].startsWith("[ERROR]")) gfx->setTextColor(COL_RED);
        else if (vtLines[i].startsWith("[INFO]"))  gfx->setTextColor(COL_AMBER);
        else gfx->setTextColor(COL_WHITE);
        gfx->setCursor(4, y);
        // Truncate to fit screen
        int max_chars = (VT_SCREEN_W - 8) / 6;
        if ((int)vtLines[i].length() > max_chars) {
            gfx->print(vtLines[i].substring(0, max_chars - 3));
            gfx->print("...");
        } else {
            gfx->print(vtLines[i]);
        }
    }
}

static void vtDrawStatus(const String& msg, uint16_t color, bool recording) {
#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
    // Big record button (touch-and-hold) on portrait devices
    int btn_y = VT_RECBTN_Y;
    uint16_t fill = recording ? COL_RECORD : 0x18C3;
    uint16_t border = recording ? COL_RED : COL_DIM;
    gfx->fillRect(20, btn_y, VT_SCREEN_W - 40, VT_RECBTN_H, fill);
    gfx->drawRect(20, btn_y, VT_SCREEN_W - 40, VT_RECBTN_H, border);
    gfx->setTextSize(2);
    gfx->setTextColor(recording ? COL_WHITE : COL_AMBER);
    const char* label = recording ? "RECORDING" : "HOLD TO TALK";
    int label_w = strlen(label) * 12;
    gfx->setCursor((VT_SCREEN_W - label_w) / 2, btn_y + (VT_RECBTN_H - 14) / 2);
    gfx->print(label);

    // Status line below
    gfx->fillRect(0, VT_STATUS_Y, VT_SCREEN_W, 16, COL_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(color);
    gfx->setCursor(4, VT_STATUS_Y);
    int max_chars = (VT_SCREEN_W - 8) / 6;
    gfx->print(msg.length() > (unsigned)max_chars ? msg.substring(0, max_chars) : msg);
#else
    // Status bar at bottom for keyboard/button devices
    gfx->fillRect(0, VT_STATUS_Y, VT_SCREEN_W, VT_SCREEN_H - VT_STATUS_Y, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(recording ? COL_RECORD : color);
    gfx->setCursor(4, VT_STATUS_Y + 6);
    if (recording) gfx->print("[REC] ");
    int max_chars = (VT_SCREEN_W - 32) / 6;
    gfx->print(msg.length() > (unsigned)max_chars ? msg.substring(0, max_chars) : msg);
#endif
}

static void vtRedraw(const String& status, uint16_t color, bool recording) {
    vtDrawHeader();
    vtDrawConversation();
    vtDrawStatus(status, color, recording);
}

// ─────────────────────────────────────────────
//  ES8311 ADC MODE — for devices using ES8311 as both spk and mic
// ─────────────────────────────────────────────
#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)

// ES8311 register addresses we need to touch for ADC enable
#define ES8311_I2C_ADDR        0x18
#define ES8311_REG_SYSTEM_0D   0x0D
#define ES8311_REG_SYSTEM_0E   0x0E
#define ES8311_REG_SYSTEM_14   0x14
#define ES8311_REG_ADC_15      0x15
#define ES8311_REG_ADC_16      0x16
#define ES8311_REG_ADC_17      0x17  // ADC volume

#include <Wire.h>

static bool es8311_adc_enable() {
    // Power up the analog input path on ES8311.
    // These register writes assume es8311_init() has already brought up
    // clock domain and SDP_IN format. We're adding ADC-specific bits.
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(ES8311_REG_SYSTEM_14);
    Wire.write(0x1A);   // PGA enable + mic select
    if (Wire.endTransmission() != 0) return false;

    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(ES8311_REG_ADC_17);
    Wire.write(0xC0);   // ADC volume — high gain for quiet mic
    if (Wire.endTransmission() != 0) return false;

    return true;
}
#endif

// ─────────────────────────────────────────────
//  MIC INIT (per device)
// ─────────────────────────────────────────────
static bool vtInitMic() {
#if defined(DEVICE_CARDPUTER_ADV) || defined(DEVICE_TLORAPAGER)
    // Cardputer ADV: M5 library handles audio internally, raw I2S mic not exposed.
    // T-LoRa Pager: ES8311 ADC bring-up deferred to v1.3 — needs schematic-verified
    //   register sequence and on-device validation. For now, voice terminal on
    //   T-LoRa Pager falls back to keyboard mode.
    return false;
#elif defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4) || defined(DEVICE_TDECK_PLUS)

  #if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
    // Make sure ES8311 is up (c28p_audio_begin idempotent), then flip ADC on
    if (!c28p_audio_begin()) return false;
    if (!es8311_adc_enable()) {
        Serial.println("[VT] ES8311 ADC enable failed");
        return false;
    }
  #endif

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate = VT_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = VT_DMA_BUF_COUNT;
    cfg.dma_buf_len   = VT_DMA_BUF_LEN;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = false;
    cfg.fixed_mclk = 0;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = VT_MIC_MCLK;
    pins.bck_io_num   = VT_MIC_SCK;
    pins.ws_io_num    = VT_MIC_LRCK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = VT_MIC_DIN;

    if (i2s_driver_install(VT_I2S_MIC_PORT, &cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(VT_I2S_MIC_PORT, &pins) != ESP_OK) {
        i2s_driver_uninstall(VT_I2S_MIC_PORT);
        return false;
    }

    // Warmup — discard first DMA buffers
    uint8_t warmup[VT_READ_BYTES];
    size_t read = 0;
    for (int i = 0; i < 4; i++)
        i2s_read(VT_I2S_MIC_PORT, warmup, VT_READ_BYTES, &read, pdMS_TO_TICKS(50));
    return true;
#else
    return false;
#endif
}

static void vtDeinitMic() {
#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4) || defined(DEVICE_TDECK_PLUS)
    i2s_stop(VT_I2S_MIC_PORT);
    i2s_driver_uninstall(VT_I2S_MIC_PORT);
#endif
}

// ─────────────────────────────────────────────
//  RECORD TO WAV (device-agnostic given mic init works)
// ─────────────────────────────────────────────
static bool vtRecord(const char* path,
                     bool (*should_stop)())  // callback: stop when this returns true
{
    if (!vtInitMic()) {
        vtAddLine("[ERROR]", "Mic init failed");
        return false;
    }

    FsFile file = sd.open(path, O_WRITE | O_CREAT | O_TRUNC);
    if (!file) {
        vtDeinitMic();
        vtAddLine("[ERROR]", "Could not open WAV file");
        return false;
    }

    VT_WAVHeader hdr;
    file.write((uint8_t*)&hdr, sizeof(hdr));

    uint8_t buf[VT_READ_BYTES];
    size_t bytesRead = 0;
    uint32_t totalAudio = 0;
    uint32_t startMs = millis();

    while (true) {
        if (should_stop && should_stop()) break;
        if (millis() - startMs > VT_MAX_REC_MS) break;
        i2s_read(VT_I2S_MIC_PORT, buf, VT_READ_BYTES, &bytesRead, pdMS_TO_TICKS(50));
        if (bytesRead > 0) {
            file.write(buf, bytesRead);
            totalAudio += bytesRead;
        }
        yield();
    }

    // Patch WAV header
    hdr.dataSize = totalAudio;
    hdr.fileSize = totalAudio + sizeof(hdr) - 8;
    file.seek(0);
    file.write((uint8_t*)&hdr, sizeof(hdr));
    file.flush();
    file.close();

    vtDeinitMic();
    return totalAudio > 1024;  // require at least a small clip
}

// ─────────────────────────────────────────────
//  SPEECH-TO-TEXT (Google Cloud)
// ─────────────────────────────────────────────
static String vtSpeechToText(const char* wavPath) {
    if (!vtHasCloudKey()) return String();

    FsFile wav = sd.open(wavPath, O_READ);
    if (!wav) return String();
    uint32_t wavSize = wav.size();
    if (wavSize < 64) { wav.close(); return String(); }

    // Read whole WAV into PSRAM
    uint8_t* wavData = (uint8_t*)ps_malloc(wavSize);
    if (!wavData) { wav.close(); return String(); }
    wav.read(wavData, wavSize);
    wav.close();

    // Base64 encode (audio data only, skip 44-byte header)
    size_t audioOffset = 44;
    size_t audioLen    = wavSize - audioOffset;
    size_t b64Cap = ((audioLen + 2) / 3) * 4 + 16;
    char* b64Buf = (char*)ps_malloc(b64Cap);
    if (!b64Buf) { free(wavData); return String(); }
    size_t b64Len = 0;
    mbedtls_base64_encode((unsigned char*)b64Buf, b64Cap, &b64Len,
                          wavData + audioOffset, audioLen);
    free(wavData);
    b64Buf[b64Len] = 0;

    String body = "{\"config\":{\"encoding\":\"LINEAR16\",\"sampleRateHertz\":16000,"
                  "\"languageCode\":\"en-US\"},\"audio\":{\"content\":\"";
    body.reserve(body.length() + b64Len + 32);
    body += b64Buf;
    body += "\"}}";
    free(b64Buf);

    wifi_in_use = true;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://speech.googleapis.com/v1/speech:recognize?key=";
    url += GOOGLE_CLOUD_API_KEY;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);

    String transcript;
    if (code == 200) {
        String resp = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, resp) == DeserializationError::Ok) {
            transcript = doc["results"][0]["alternatives"][0]["transcript"].as<String>();
        }
    } else {
        Serial.printf("[VT] STT HTTP %d\n", code);
    }
    http.end();
    wifi_in_use = false;
    return transcript;
}

// ─────────────────────────────────────────────
//  TEXT-TO-SPEECH (Google Cloud)
// ─────────────────────────────────────────────
static bool vtTextToSpeechToFile(const String& text, const char* outPath) {
    if (!vtTTSEnabled || text.length() == 0) return false;
    if (!vtHasCloudKey()) return false;

    String ttsText = text.length() > 500 ? text.substring(0, 500) : text;
    ttsText.replace("\"", "\\\"");
    ttsText.replace("\n", " ");

    String body = "{\"input\":{\"text\":\"" + ttsText + "\"},"
                  "\"voice\":{\"languageCode\":\"en-US\",\"name\":\"en-US-Neural2-D\"},"
                  "\"audioConfig\":{\"audioEncoding\":\"MP3\"}}";

    wifi_in_use = true;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://texttospeech.googleapis.com/v1/text:synthesize?key=";
    url += GOOGLE_CLOUD_API_KEY;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);

    bool ok = false;
    if (code == 200) {
        String resp = http.getString();
        JsonDocument doc;
        if (deserializeJson(doc, resp) == DeserializationError::Ok) {
            String b64Audio = doc["audioContent"].as<String>();
            size_t b64Len  = b64Audio.length();
            size_t mp3Cap  = (b64Len / 4) * 3 + 4;
            uint8_t* mp3Data = (uint8_t*)ps_malloc(mp3Cap);
            if (mp3Data) {
                size_t outLen = 0;
                if (mbedtls_base64_decode(mp3Data, mp3Cap, &outLen,
                        (const unsigned char*)b64Audio.c_str(), b64Len) == 0) {
                    FsFile f = sd.open(outPath, O_WRITE | O_CREAT | O_TRUNC);
                    if (f) {
                        f.write(mp3Data, outLen);
                        f.flush();
                        f.close();
                        ok = true;
                    }
                }
                free(mp3Data);
            }
        }
    } else {
        Serial.printf("[VT] TTS HTTP %d\n", code);
    }
    http.end();
    wifi_in_use = false;
    return ok;
}

// ─────────────────────────────────────────────
//  TTS PLAYBACK
// ─────────────────────────────────────────────
static void vtPlayTTS(const char* mp3Path) {
#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
    // C28P / Heltec V4: ES8311 currently in tone-output mode from c28p_audio.cpp.
    // Make sure it's brought up (idempotent), then release the I2S driver so
    // the Audio library can take over with its own I2S init.
    if (!c28p_audio_begin()) return;
    c28p_audio_stop();

    Audio audio;
    audio.setPinout(PIN_I2S_SCLK, PIN_I2S_LRCK, PIN_I2S_DOUT);
    audio.setVolume(18);  // 0-21
    audio.connecttoFS(SD, mp3Path);

    uint32_t start = millis();
    while (audio.isRunning() && (millis() - start) < 30000) {
        audio.loop();
        yield();
    }
#elif defined(DEVICE_TLORAPAGER)
    // T-LoRa Pager: ES8311 codec NOT yet brought up in this codebase. The
    // Audio library will configure I2S from scratch on its own pins. If the
    // ES8311 needs I2C-side codec config beyond default register values,
    // that's v1.3 work.
    Audio audio;
    audio.setPinout(PIN_I2S_SCLK, PIN_I2S_LRCK, PIN_I2S_DOUT);
    audio.setVolume(18);
    audio.connecttoFS(SD, mp3Path);
    uint32_t start = millis();
    while (audio.isRunning() && (millis() - start) < 30000) {
        audio.loop();
        yield();
    }
#elif defined(DEVICE_TDECK_PLUS)
    // T-Deck Plus speaker pins are hardcoded in audio_player.cpp:
    //   I2S_BCLK=7, I2S_LRC=5, I2S_DOUT=6
    // These are NOT exposed as PIN_I2S_* macros in platformio.ini —
    // matching audio_player.cpp's private constants instead.
    Audio audio;
    audio.setPinout(7 /*BCLK*/, 5 /*LRC*/, 6 /*DOUT*/);
    audio.setVolume(18);
    audio.connecttoFS(SD, mp3Path);
    uint32_t start = millis();
    while (audio.isRunning() && (millis() - start) < 30000) {
        audio.loop();
        yield();
    }
#else
    // Cardputer or unknown — no TTS playback path yet
    (void)mp3Path;
#endif
}

// ─────────────────────────────────────────────
//  PER-DEVICE RECORD TRIGGER
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
// Touch-and-hold the big record button
static bool g_touchHeld = false;
static bool touch_should_stop() {
    int16_t tx, ty;
    bool touched = c28p_touch_read(&tx, &ty);
    if (!touched) return true;  // released → stop
    return false;
}
static bool wait_for_record_press_and_record(const char* wavPath) {
    // Block until user touches the record button; record while held.
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched) {
            // Header tap → quit signal (handled by caller)
            if (ty < VT_HEADER_H) return false;
            // Record button area
            if (ty >= VT_RECBTN_Y && ty < VT_RECBTN_Y + VT_RECBTN_H) {
                vtRedraw("Recording — release to send", COL_RECORD, true);
                bool ok = vtRecord(wavPath, touch_should_stop);
                return ok;
            }
        }
        delay(20); yield();
    }
}
#endif

#if defined(DEVICE_TDECK_PLUS)
// SPACE held = recording
static bool space_should_stop() {
    // get_keypress() is non-blocking; in v1 the SPACE was checked once
    // and recording ran for a fixed window. Adapt: poll keyboard, stop
    // when SPACE is no longer held. The original keyboard.cpp doesn't
    // expose held-state cleanly; emulate with a short fixed window.
    return false;  // recording runs until VT_MAX_REC_MS expires
}
#endif

#if defined(DEVICE_TLORAPAGER)
// Encoder click or 'r' = start; release/timeout = stop
static bool tlp_should_stop() {
    PMNesInput input = pm_read_nes_input(true);
    return !(input.a || input.key == 'r' || input.key == 'R');
}
#endif

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_voice_terminal() {
    gfx->fillScreen(COL_BG);
    vtLineCount    = 0;
    vtTTSEnabled   = true;
    vtKeyboardMode = false;

    if (WiFi.status() != WL_CONNECTED) {
        vtAddLine("[ERROR]", "WiFi not connected");
        vtRedraw("Connect WiFi first", COL_RED, false);
        delay(2500);
        return;
    }

    if (!gemini_has_key()) {
        vtAddLine("[ERROR]", "No GEMINI_API_KEY in secrets.h");
        vtRedraw("Missing API key — reflash", COL_RED, false);
        delay(2500);
        return;
    }

#if defined(DEVICE_CARDPUTER_ADV)
    vtKeyboardMode = true;
    vtTTSEnabled   = false;
    vtAddLine("[INFO]", "Cardputer: keyboard mode (mic TBD)");
#elif defined(DEVICE_TLORAPAGER)
    vtKeyboardMode = true;
    vtTTSEnabled   = true;   // TTS playback works; only mic capture is deferred
    vtAddLine("[INFO]", "T-LoRa Pager: keyboard mode (mic v1.3)");
#else
    if (!vtHasCloudKey()) {
        vtKeyboardMode = true;
        vtTTSEnabled   = false;
        vtAddLine("[INFO]", "No GOOGLE_CLOUD_API_KEY — keyboard mode");
    }
#endif

    vtAddLine("[READY]", vtKeyboardMode ?
              "Type prompt + ENTER" :
              "Press to record");
    vtRedraw("Ready", COL_GREEN, false);

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
    // Touch loop
    while (true) {
        int16_t tx, ty;
        if (c28p_touch_read(&tx, &ty)) {
            if (ty < VT_HEADER_H) {
                while (c28p_touch_read(&tx, &ty)) { delay(10); yield(); }
                break;  // exit
            }
            if (ty >= VT_RECBTN_Y && ty < VT_RECBTN_Y + VT_RECBTN_H) {
                vtRedraw("Recording...", COL_RECORD, true);
                const char* wav = "/vt_rec.wav";
                if (vtRecord(wav, touch_should_stop)) {
                    vtRedraw("Transcribing...", COL_AMBER, false);
                    String t = vtSpeechToText(wav);
                    sd.remove(wav);
                    if (t.length() > 0) {
                        vtAddLine("You", t);
                        vtRedraw("Asking Gemini...", COL_AMBER, false);
                        wifi_in_use = true;
                        String r = ask_gemini(t);
                        wifi_in_use = false;
                        vtAddLine("Gemini", r);
                        vtRedraw("Speaking...", COL_CYAN, false);
                        if (vtTextToSpeechToFile(r, "/tmp_tts.mp3")) {
                            vtPlayTTS("/tmp_tts.mp3");
                        }
                        vtRedraw("Ready", COL_GREEN, false);
                    } else {
                        vtAddLine("[ERROR]", "Could not transcribe");
                        vtRedraw("Try again", COL_RED, false);
                    }
                } else {
                    vtRedraw("Recording failed", COL_RED, false);
                }
            }
        }
        delay(30); yield();
    }
#elif defined(DEVICE_TLORAPAGER)
    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit || input.b) break;
        if (input.a || input.key == 'r' || input.key == 'R') {
            vtRedraw("Recording...", COL_RECORD, true);
            const char* wav = "/vt_rec.wav";
            if (vtRecord(wav, tlp_should_stop)) {
                vtRedraw("Transcribing...", COL_AMBER, false);
                String t = vtSpeechToText(wav);
                sd.remove(wav);
                if (t.length() > 0) {
                    vtAddLine("You", t);
                    vtRedraw("Asking Gemini...", COL_AMBER, false);
                    wifi_in_use = true;
                    String r = ask_gemini(t);
                    wifi_in_use = false;
                    vtAddLine("Gemini", r);
                    vtRedraw("Speaking...", COL_CYAN, false);
                    if (vtTextToSpeechToFile(r, "/tmp_tts.mp3")) {
                        vtPlayTTS("/tmp_tts.mp3");
                    }
                    vtRedraw("Ready", COL_GREEN, false);
                }
            }
        }
        delay(30); yield();
    }
#elif defined(DEVICE_TDECK_PLUS)
    // SPACE-triggered v1-style loop (keep the existing behavior)
    while (true) {
        char k = get_keypress();
        if (k == 'q' || k == 'Q') break;
        if (k == ' ' && !vtKeyboardMode) {
            vtRedraw("Recording...", COL_RECORD, true);
            const char* wav = "/vt_rec.wav";
            if (vtRecord(wav, space_should_stop)) {
                vtRedraw("Transcribing...", COL_AMBER, false);
                String t = vtSpeechToText(wav);
                sd.remove(wav);
                if (t.length() > 0) {
                    vtAddLine("You", t);
                    vtRedraw("Asking Gemini...", COL_AMBER, false);
                    wifi_in_use = true;
                    String r = ask_gemini(t);
                    wifi_in_use = false;
                    vtAddLine("Gemini", r);
                    vtRedraw("Speaking...", COL_CYAN, false);
                    if (vtTextToSpeechToFile(r, "/tmp_tts.mp3")) {
                        vtPlayTTS("/tmp_tts.mp3");
                    }
                    vtRedraw("Ready", COL_GREEN, false);
                }
            }
        }
        delay(30); yield();
    }
#elif defined(DEVICE_CARDPUTER_ADV)
    // Keyboard-only mode for Cardputer (mic not wired yet)
    PMNesInput input;
    while (true) {
        input = pm_read_nes_input(true);
        if (input.quit) break;
        if (input.key >= 32 && input.key <= 126) {
            // Inline-edit prompt
            String prompt = String((char)input.key);
            vtRedraw(String("> ") + prompt, COL_WHITE, false);
            while (true) {
                input = pm_read_nes_input(true);
                if (input.key == 13 || input.key == 10) break;
                if (input.key == 'q' && prompt.length() == 0) goto cp_exit;
                if (input.key == 8 || input.key == 127) {
                    if (prompt.length() > 0) prompt.remove(prompt.length() - 1);
                } else if (input.key >= 32 && input.key <= 126) {
                    prompt += (char)input.key;
                }
                vtRedraw(String("> ") + prompt, COL_WHITE, false);
                delay(20);
            }
            if (prompt.length() > 0) {
                vtAddLine("You", prompt);
                vtRedraw("Asking Gemini...", COL_AMBER, false);
                wifi_in_use = true;
                String r = ask_gemini(prompt);
                wifi_in_use = false;
                vtAddLine("Gemini", r);
                vtRedraw("Done", COL_GREEN, false);
            }
        }
        delay(30); yield();
    }
    cp_exit: ;
#endif

    gfx->fillScreen(COL_BG);
}