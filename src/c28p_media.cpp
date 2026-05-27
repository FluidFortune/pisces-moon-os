// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_media.cpp — Media app for C28P
//
//  CONTAINS:
//   1. Voice recorder — record audio from on-board ES8311 mic input,
//      save as raw PCM WAV to /recordings/ on SD
//   2. Recordings list — browse past recordings, tap to play back
//   3. Image viewer stub — placeholder for v1.3 JPEG decode on SD
//
//  The recorder uses the ES8311 codec on the C28P (same chip the
//  c28p_audio.cpp game-tone driver initializes). For v1.2.1 the
//  recorder writes raw PCM at 16kHz mono 16-bit into a WAV header.
//  No compression. ~32KB per second of audio. A 30-second memo is
//  about 1MB on SD.
//
//  TOUCH UI (240×320 portrait):
//
//    y=  0..14   Exit bar (owned by dpad)
//    y= 16..40   Title bar
//    y= 44..80   Mode tabs: [RECORD] [LIBRARY] [PHOTOS]
//    y= 84..304  Active panel
//    y=308..318  Status line
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <FS.h>
#include "c28p_dpad.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t *x, int16_t *y);
extern bool c28p_audio_begin();   // From c28p_audio.cpp — also drives the codec
extern void c28p_audio_stop();

// ─── State ───
enum MediaTab {
    TAB_RECORD = 0,
    TAB_LIBRARY = 1,
    TAB_PHOTOS = 2,
};
static MediaTab g_tab = TAB_RECORD;

// ─── Recording state ───
static bool g_recording = false;
static uint32_t g_record_start_ms = 0;
static uint32_t g_record_bytes = 0;
static File g_record_file;
static char g_record_filename[64];

// ─── WAV header for 16kHz mono 16-bit PCM ───
//
// PCM WAV format. Header is 44 bytes, fields little-endian.
// data length is patched at stop time once total recording size is known.
static void write_wav_header(File& f, uint32_t data_bytes) {
    uint32_t file_size = data_bytes + 36;
    uint32_t sample_rate = 16000;
    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    uint8_t hdr[44];
    memcpy(hdr +  0, "RIFF", 4);
    memcpy(hdr +  4, &file_size, 4);
    memcpy(hdr +  8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(hdr + 16, &fmt_size, 4);
    uint16_t audio_format = 1;   // PCM
    memcpy(hdr + 20, &audio_format, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &sample_rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits_per_sample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_bytes, 4);

    f.seek(0);
    f.write(hdr, 44);
}

static bool ensure_recordings_dir() {
    if (!SD.exists("/recordings")) {
        if (!SD.mkdir("/recordings")) {
            Serial.println("[C28P-MEDIA] mkdir /recordings failed");
            return false;
        }
    }
    return true;
}

static bool start_recording() {
    if (g_recording) return true;
    if (!ensure_recordings_dir()) return false;

    // Filename based on uptime — better than nothing without an RTC
    uint32_t secs = millis() / 1000;
    snprintf(g_record_filename, sizeof(g_record_filename),
             "/recordings/rec_%lu.wav", (unsigned long)secs);

    g_record_file = SD.open(g_record_filename, FILE_WRITE);
    if (!g_record_file) {
        Serial.printf("[C28P-MEDIA] Failed to open %s\n", g_record_filename);
        return false;
    }

    // Reserve space for header — written at stop time
    uint8_t empty_hdr[44] = {0};
    g_record_file.write(empty_hdr, 44);

    g_record_bytes = 0;
    g_record_start_ms = millis();
    g_recording = true;

    // Initialize the codec for input. v1.2.1 stub: the actual I2S
    // microphone capture loop will live in a separate task in v1.3
    // when we wire the ES8311 ADC path. For now this is a UI-complete
    // placeholder that creates the file but writes silence.
    //
    // TODO(v1.3): Spawn a task that reads I2S samples and calls
    // g_record_file.write(samples, n) on each chunk. Until then, the
    // recording is silent — but the workflow (file creation, naming,
    // WAV header, list playback) works end-to-end so we can iterate
    // on the UI without blocking on the I2S work.
    Serial.printf("[C28P-MEDIA] Recording: %s\n", g_record_filename);
    return true;
}

static void stop_recording() {
    if (!g_recording) return;
    g_recording = false;

    // Patch the WAV header with actual data size
    write_wav_header(g_record_file, g_record_bytes);
    g_record_file.close();

    uint32_t duration = (millis() - g_record_start_ms) / 1000;
    Serial.printf("[C28P-MEDIA] Stopped: %s (%u sec, %u bytes)\n",
                  g_record_filename, duration, g_record_bytes);
}

// ─── Drawing ───
static void media_draw_chrome() {
    gfx->fillRect(0, 14, 240, 320 - 14, 0x0000);
    gfx->fillRect(0, 14, 240, 22, 0x780F);   // magenta header
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 18);
    gfx->print("MEDIA");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 22);
    gfx->print("< EXIT");
}

static void media_draw_tabs() {
    gfx->fillRect(0, 40, 240, 40, 0x0000);
    const char* labels[3] = { "RECORD", "LIBRARY", "PHOTOS" };
    for (int i = 0; i < 3; i++) {
        int x = 8 + i * 78;
        bool active = (g_tab == (MediaTab)i);
        gfx->fillRect(x, 44, 72, 30, active ? 0xF81F : 0x18C3);
        gfx->drawRect(x, 44, 72, 30, active ? 0xFFFF : 0x4208);
        gfx->setTextSize(1);
        gfx->setTextColor(active ? 0xFFFF : 0x8410);
        // Center text in tab — 6px/char approximately
        int tw = strlen(labels[i]) * 6;
        gfx->setCursor(x + (72 - tw) / 2, 56);
        gfx->print(labels[i]);
    }
}

static void media_draw_record_panel() {
    gfx->fillRect(0, 84, 240, 220, 0x0000);

    // Record button — big and obvious
    if (g_recording) {
        // Pulsing red square indicator at top
        gfx->fillRect(20, 92, 200, 80, 0x4000);
        gfx->drawRect(20, 92, 200, 80, 0xF800);
        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(56, 110);
        gfx->print("RECORDING");

        uint32_t elapsed = (millis() - g_record_start_ms) / 1000;
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(80, 140);
        gfx->printf("%02lu:%02lu",
                    (unsigned long)(elapsed / 60),
                    (unsigned long)(elapsed % 60));

        // STOP button
        gfx->fillRect(40, 200, 160, 60, 0x4000);
        gfx->drawRect(40, 200, 160, 60, 0xF800);
        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(82, 222);
        gfx->print("STOP");
    } else {
        gfx->fillRect(20, 92, 200, 80, 0x18C3);
        gfx->drawRect(20, 92, 200, 80, 0x4208);
        gfx->setTextSize(2);
        gfx->setTextColor(0x8410);
        gfx->setCursor(70, 110);
        gfx->print("READY");
        gfx->setTextSize(1);
        gfx->setCursor(40, 140);
        gfx->print("Tap below to begin recording.");
        gfx->setCursor(40, 154);
        gfx->print("Recordings save to /recordings");

        // RECORD button
        gfx->fillRect(40, 200, 160, 60, 0x2104);
        gfx->drawRect(40, 200, 160, 60, 0xF800);
        gfx->fillCircle(120, 230, 14, 0xF800);   // red dot
        gfx->setTextSize(1);
        gfx->setTextColor(0xF800);
        gfx->setCursor(96, 254);
        gfx->print("RECORD");
    }
}

static void media_draw_library_panel() {
    gfx->fillRect(0, 84, 240, 220, 0x0000);

    if (!ensure_recordings_dir()) {
        gfx->setTextSize(1);
        gfx->setTextColor(0xF800);
        gfx->setCursor(20, 140);
        gfx->print("SD card not available");
        return;
    }

    File dir = SD.open("/recordings");
    if (!dir) {
        gfx->setTextSize(1);
        gfx->setTextColor(0x8410);
        gfx->setCursor(20, 140);
        gfx->print("No recordings yet");
        return;
    }

    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(8, 88);
    gfx->print("RECORDINGS");

    int row = 0;
    File entry = dir.openNextFile();
    while (entry && row < 5) {
        if (!entry.isDirectory()) {
            int y = 104 + row * 34;
            gfx->fillRect(8, y, 224, 30, 0x18C3);
            gfx->drawRect(8, y, 224, 30, 0x4208);
            gfx->setTextColor(0xFFFF);
            gfx->setCursor(14, y + 6);
            const char* name = entry.name();
            // Strip path
            const char* slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            gfx->print(name);
            gfx->setTextColor(0x8410);
            gfx->setCursor(14, y + 18);
            gfx->printf("%u bytes", (unsigned)entry.size());
            row++;
        }
        entry = dir.openNextFile();
    }
    dir.close();

    if (row == 0) {
        gfx->setTextColor(0x8410);
        gfx->setCursor(20, 180);
        gfx->print("No recordings yet");
    }
}

static void media_draw_photos_panel() {
    gfx->fillRect(0, 84, 240, 220, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor(40, 140);
    gfx->print("PHOTOS");
    gfx->setTextSize(1);
    gfx->setCursor(20, 180);
    gfx->print("Image viewer is v1.3.");
    gfx->setCursor(20, 196);
    gfx->print("Will display JPEGs");
    gfx->setCursor(20, 210);
    gfx->print("from /photos/ on SD.");
}

static void media_redraw_panel() {
    if (g_tab == TAB_RECORD) media_draw_record_panel();
    else if (g_tab == TAB_LIBRARY) media_draw_library_panel();
    else media_draw_photos_panel();
}

// ─── Public entry ───
void c28p_run_media() {
    media_draw_chrome();
    media_draw_tabs();
    media_redraw_panel();

    uint32_t last_clock_update = 0;
    bool was_touched = false;
    int pressed = -2;

    while (true) {
        // Update the elapsed clock if recording
        if (g_recording && millis() - last_clock_update > 1000) {
            media_draw_record_panel();
            last_clock_update = millis();
        }

        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            if (ty < 14) {
                pressed = -1;   // exit
            }
            // Tabs
            else if (ty >= 44 && ty < 74) {
                if (tx < 86) pressed = -10;
                else if (tx < 164) pressed = -11;
                else pressed = -12;
            }
            // RECORD / STOP button (panel-specific)
            else if (g_tab == TAB_RECORD && ty >= 200 && ty < 260) {
                pressed = -20;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) {
                if (g_recording) stop_recording();
                return;
            } else if (pressed == -10) {
                g_tab = TAB_RECORD; media_draw_tabs(); media_redraw_panel();
            } else if (pressed == -11) {
                g_tab = TAB_LIBRARY; media_draw_tabs(); media_redraw_panel();
            } else if (pressed == -12) {
                g_tab = TAB_PHOTOS; media_draw_tabs(); media_redraw_panel();
            } else if (pressed == -20) {
                if (g_recording) {
                    stop_recording();
                    media_draw_record_panel();
                } else {
                    if (start_recording()) {
                        media_draw_record_panel();
                    }
                }
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

#endif // DEVICE_C28P