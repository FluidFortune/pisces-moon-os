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
 * PISCES MOON OS — AUDIO RECORDER v1.1
 * Records microphone input to WAV files on SD card
 * via the ES7210 I2S microphone codec on T-Deck Plus.
 *
 * ES7210 I2S Input pins (LilyGO T-Deck Plus):
 *   MCLK = GPIO48   (Master clock)
 *   LRCK = GPIO21   (Left/Right clock — word select)
 *   SCK  = GPIO47   (Bit clock)
 *   DIN  = GPIO14   (Data in from mic)
 *
 * Output format: WAV, 16kHz, 16-bit, mono
 * Output path:   /recordings/rec_NNN.wav
 *
 * v1.1 fixes:
 * - Larger DMA buffers (4096 samples) prevent DMA stall/starvation
 * - Increased read buffer to 8KB for sustained throughput
 * - updatePeak() forward-declared before use
 * - i2s_read timeout increased to 100ms
 * - SD write uses flush every 32 blocks to prevent buffer overflow
 *
 * GPIO0 NOTE: GPIO0 (trackball click) is shared with the ES7210
 * microphone hardware. While the I2S mic is active, trackball click
 * is unavailable. Use SPACE key or the on-screen button instead.
 *
 * Controls:
 *   SPACE / on-screen button = start / stop recording
 *   Trackball UP/DOWN        = scroll recordings list
 *   Q / header tap           = exit (stops recording first)
 */

#include <Arduino.h>
#include <FS.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include <driver/i2s.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  HARDWARE
// ─────────────────────────────────────────────
#define ES7210_MCLK     48
#define ES7210_LRCK     21
#define ES7210_SCK      47
#define ES7210_DIN      14

#define SAMPLE_RATE     16000
#define SAMPLE_BITS     16
#define CHANNELS        1
#define I2S_PORT        I2S_NUM_1

#define RECORD_DIR      "/recordings"

// DMA config — conservative sizing to avoid SRAM exhaustion.
// The I2S driver allocates DMA_BUF_COUNT × DMA_BUF_LEN × 2 bytes from
// internal SRAM. With WiFi + BLE + wardrive task already running, large
// DMA allocations cause Guru Meditation at driver install time.
// 4 × 512 = 4KB DMA — safe alongside the running OS stack.
// readBuf lives in PSRAM (ps_malloc) to avoid the same issue.
#define DMA_BUF_COUNT   4
#define DMA_BUF_LEN     512         // samples per DMA buffer
#define READ_BUF_BYTES  4096        // bytes per i2s_read call

// Auto-stop at 5 minutes
#define MAX_REC_MS      (5 * 60 * 1000UL)

// SD flush every N write cycles to prevent buffer overflow
#define FLUSH_EVERY     32

// ─────────────────────────────────────────────
//  WAV HEADER
// ─────────────────────────────────────────────
struct WAVHeader {
    char     riff[4]       = {'R','I','F','F'};
    uint32_t fileSize      = 0;
    char     wave[4]       = {'W','A','V','E'};
    char     fmt[4]        = {'f','m','t',' '};
    uint32_t fmtSize       = 16;
    uint16_t audioFormat   = 1;     // PCM
    uint16_t numChannels   = CHANNELS;
    uint32_t sampleRate    = SAMPLE_RATE;
    uint32_t byteRate      = SAMPLE_RATE * CHANNELS * (SAMPLE_BITS/8);
    uint16_t blockAlign    = CHANNELS * (SAMPLE_BITS/8);
    uint16_t bitsPerSample = SAMPLE_BITS;
    char     data[4]       = {'d','a','t','a'};
    uint32_t dataSize      = 0;
};

// ─────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────
static bool      recording        = false;
static FsFile    recFile;
static uint32_t  recBytesWritten  = 0;
static uint32_t  recStartMs       = 0;
static char      recFilePath[48]  = "";
// readBuf in PSRAM — static arrays this large in SRAM cause Guru Meditation
// when launched alongside WiFi + BLE + wardrive task.
static uint8_t*  readBuf          = nullptr;
static int       writeCount       = 0;

// Recording list
#define MAX_RECS  64
static char recList[MAX_RECS][48];
static int  recCount    = 0;
static int  recScroll   = 0;
static int  recSelected = 0;
#define LIST_ROWS   8
#define LIST_ROW_H  18

// Peak meter
static uint16_t peakLevel  = 0;
static uint16_t peakHold   = 0;
static uint32_t peakHoldMs = 0;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_HDR     0x18C3
#define COL_REC     0xF800
#define COL_READY   0x07E0
#define COL_METER_L 0x07E0
#define COL_METER_M 0xFFE0
#define COL_METER_H 0xF800
#define COL_DIM     0x4208
#define COL_SEL     0x001F

// ─────────────────────────────────────────────
//  FORWARD DECLARATIONS
// ─────────────────────────────────────────────
static void updatePeak(const uint8_t* buf, int bytes);
static void stopRecording();
static void deleteRecording(int idx);
static void drawFull();
static void drawHeader();
static void drawMeter();
static void drawRecordButton();
static void drawRecordingsList();

// ─────────────────────────────────────────────
//  I2S INIT / DEINIT
// ─────────────────────────────────────────────
static bool initI2SMic() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = DMA_BUF_COUNT,
        .dma_buf_len          = DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };

    i2s_pin_config_t pins = {
        .mck_io_num   = ES7210_MCLK,
        .bck_io_num   = ES7210_SCK,
        .ws_io_num    = ES7210_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = ES7210_DIN
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[REC] I2S install failed: %d\n", err);
        return false;
    }
    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[REC] I2S pin config failed: %d\n", err);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    // Warmup — small stack buffer, not READ_BUF_BYTES
    uint8_t warmup[512];
    size_t  warmupRead = 0;
    for (int i = 0; i < 4; i++) {
        i2s_read(I2S_PORT, warmup, sizeof(warmup), &warmupRead, pdMS_TO_TICKS(100));
    }

    Serial.println("[REC] I2S mic initialized.");
    return true;
}

static void deinitI2SMic() {
    i2s_stop(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    Serial.println("[REC] I2S mic released.");
}

// ─────────────────────────────────────────────
//  RECORDING FILE MANAGEMENT
// ─────────────────────────────────────────────
static int getNextRecNumber() {
    int maxNum = 0;
    if (!sd.exists(RECORD_DIR)) { sd.mkdir(RECORD_DIR); return 1; }
    FsFile dir = sd.open(RECORD_DIR);
    if (!dir) return 1;
    FsFile f;
    while (f.openNext(&dir, O_READ)) {
        char name[32]; f.getName(name, sizeof(name)); f.close();
        if (strncmp(name, "rec_", 4) == 0) {
            int n = atoi(name + 4);
            if (n > maxNum) maxNum = n;
        }
    }
    dir.close();
    return maxNum + 1;
}

static void scanRecordings() {
    recCount = 0;
    if (!sd.exists(RECORD_DIR)) return;
    FsFile dir = sd.open(RECORD_DIR);
    if (!dir) return;
    FsFile f;
    while (f.openNext(&dir, O_READ) && recCount < MAX_RECS) {
        char name[32]; f.getName(name, sizeof(name));
        if (!f.isDir() && strstr(name, ".wav")) {
            snprintf(recList[recCount], 48, "%s/%s", RECORD_DIR, name);
            recCount++;
        }
        f.close();
    }
    dir.close();
    // Bubble sort ascending by name
    for (int i = 0; i < recCount-1; i++)
        for (int j = i+1; j < recCount; j++)
            if (strcmp(recList[i], recList[j]) > 0) {
                char tmp[48]; strcpy(tmp, recList[i]);
                strcpy(recList[i], recList[j]);
                strcpy(recList[j], tmp);
            }
}

static bool startRecording() {
    if (!sd.exists(RECORD_DIR)) sd.mkdir(RECORD_DIR);

    int num = getNextRecNumber();
    snprintf(recFilePath, sizeof(recFilePath), "%s/rec_%03d.wav", RECORD_DIR, num);

    recFile = sd.open(recFilePath, O_WRITE | O_CREAT | O_TRUNC);
    if (!recFile) {
        Serial.printf("[REC] Cannot create: %s\n", recFilePath);
        return false;
    }

    // Write placeholder header — patched on stop
    WAVHeader hdr;
    recFile.write((uint8_t*)&hdr, sizeof(hdr));

    recBytesWritten = 0;
    recStartMs      = millis();
    writeCount      = 0;
    recording       = true;
    peakLevel       = 0;
    peakHold        = 0;

    // Clear DMA input queue before we start writing real data.
    // Re-use readBuf (already allocated in PSRAM) instead of
    // a stack array — avoids a 4KB stack allocation here.
    size_t dummy = 0;
    if (readBuf) {
        i2s_read(I2S_PORT, readBuf, READ_BUF_BYTES, &dummy, pdMS_TO_TICKS(10));
    }

    Serial.printf("[REC] Started: %s\n", recFilePath);
    return true;
}

static void stopRecording() {
    if (!recording) return;
    recording = false;

    // Flush any buffered SD data
    recFile.flush();

    // Patch WAV header with final sizes
    uint32_t dataSize = recBytesWritten;
    uint32_t fileSize = dataSize + sizeof(WAVHeader) - 8;

    recFile.seek(4);  recFile.write((uint8_t*)&fileSize, 4);
    recFile.seek(40); recFile.write((uint8_t*)&dataSize, 4);
    recFile.close();

    float secs = (float)recBytesWritten / (SAMPLE_RATE * CHANNELS * 2);
    Serial.printf("[REC] Stopped. %lu bytes = %.1f seconds\n",
                  (unsigned long)recBytesWritten, secs);

    scanRecordings();
    recSelected = recCount - 1;
    if (recSelected < 0) recSelected = 0;
    recScroll = max(0, recSelected - LIST_ROWS + 1);
}

static void deleteRecording(int idx) {
    if (idx < 0 || idx >= recCount) return;
    sd.remove(recList[idx]);
    scanRecordings();
    recSelected = min(recSelected, recCount - 1);
    if (recSelected < 0) recSelected = 0;
}

// ─────────────────────────────────────────────
//  PEAK METER
// ─────────────────────────────────────────────
static void updatePeak(const uint8_t* buf, int bytes) {
    int16_t* samples = (int16_t*)buf;
    int count = bytes / 2;
    uint16_t maxAbs = 0;
    for (int i = 0; i < count; i++) {
        uint16_t a = (uint16_t)abs(samples[i]);
        if (a > maxAbs) maxAbs = a;
    }
    if (maxAbs > peakLevel) peakLevel = maxAbs;
    else peakLevel = peakLevel * 7 / 8;

    if (maxAbs > peakHold) {
        peakHold   = maxAbs;
        peakHoldMs = millis();
    } else if (millis() - peakHoldMs > 1000) {
        peakHold = peakHold > 500 ? peakHold - 500 : 0;
    }
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawHeader() {
    gfx->fillRect(0, 0, 320, 24, COL_HDR);
    gfx->drawFastHLine(0, 23, 320, recording ? COL_REC : COL_READY);
    gfx->setTextSize(1); gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 8);
    gfx->print(recording ? "● RECORDING" : "RECORDER");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(262, 8); gfx->print("[TAP=EXIT]");
}

static void drawMeter() {
    int meterY = 26;
    int meterW = 280;
    gfx->fillRect(0, meterY, 320, 16, 0x0821);
    gfx->drawRect(0, meterY, meterW+2, 16, COL_DIM);

    if (!recording) {
        gfx->setTextColor(COL_DIM); gfx->setTextSize(1);
        gfx->setCursor(8, meterY+4); gfx->print("-- input meter (recording only) --");
        return;
    }

    int fillW = (peakLevel * meterW) / 32767;
    fillW = min(fillW, meterW);

    int g = min(fillW, meterW * 60 / 100);
    int y = min(fillW - g, meterW * 25 / 100);
    int r = fillW - g - y;
    if (g > 0) gfx->fillRect(1,     meterY+1, g, 14, COL_METER_L);
    if (y > 0) gfx->fillRect(1+g,   meterY+1, y, 14, COL_METER_M);
    if (r > 0) gfx->fillRect(1+g+y, meterY+1, r, 14, COL_METER_H);

    int holdX = min((int)(peakHold * meterW) / 32767, meterW);
    if (holdX > 2) gfx->drawFastVLine(holdX, meterY+1, 14, 0xFFFF);

    gfx->setTextColor(0xFFFF); gfx->setTextSize(1);
    gfx->setCursor(meterW + 4, meterY+4);
    uint32_t elapsed = (millis() - recStartMs) / 1000;
    gfx->printf("%02lu:%02lu", elapsed/60, elapsed%60);
}

static void drawRecordButton() {
    int by = 44;
    if (recording) {
        gfx->fillRect(10, by, 300, 30, 0x3000);
        gfx->drawRect(10, by, 300, 30, COL_REC);
        gfx->fillRect(130, by+9, 12, 12, COL_REC);
        gfx->setTextColor(COL_REC); gfx->setTextSize(1);
        gfx->setCursor(148, by+11); gfx->print("STOP  [SPACE]");
    } else {
        gfx->fillRect(10, by, 300, 30, 0x1800);
        gfx->drawRect(10, by, 300, 30, COL_READY);
        gfx->fillCircle(135, by+15, 8, COL_REC);
        gfx->setTextColor(COL_READY); gfx->setTextSize(1);
        gfx->setCursor(148, by+11); gfx->print("RECORD  [SPACE]");
    }
}

static void drawRecordingsList() {
    int listY = 80;
    gfx->fillRect(0, listY, 320, 320-listY, 0x0000);

    gfx->setTextSize(1); gfx->setTextColor(COL_DIM);
    gfx->setCursor(8, listY+2);
    if (recCount == 0) {
        gfx->print("No recordings yet. Press SPACE to record.");
        return;
    }
    gfx->printf("Recordings (%d)  |  DEL=delete", recCount);
    gfx->drawFastHLine(0, listY+12, 320, 0x2104);
    listY += 14;

    int end = min(recScroll + LIST_ROWS, recCount);
    for (int i = recScroll; i < end; i++) {
        int ry  = listY + (i - recScroll) * LIST_ROW_H;
        bool sel = (i == recSelected);
        gfx->fillRect(0, ry, 320, LIST_ROW_H-1,
                      sel ? COL_SEL : (i%2==0 ? 0x0821 : 0x0000));

        const char* slash = strrchr(recList[i], '/');
        const char* fname = slash ? slash+1 : recList[i];

        gfx->setTextColor(sel ? 0xFFFF : 0xC618);
        gfx->setCursor(8, ry+4);
        gfx->print(fname);

        if (sel && sd.exists(recList[i])) {
            FsFile f = sd.open(recList[i], O_READ);
            if (f) {
                uint32_t sz = f.fileSize();
                f.close();
                // Subtract WAV header to get data size, then compute duration
                uint32_t dataSz = sz > sizeof(WAVHeader) ? sz - sizeof(WAVHeader) : 0;
                float secs = (float)dataSz / (SAMPLE_RATE * CHANNELS * 2);
                gfx->setTextColor(COL_DIM);
                gfx->setCursor(220, ry+4);
                gfx->printf("%.1fs", secs);
            }
        }
    }
}

static void drawFull() {
    gfx->fillScreen(0x0000);
    drawHeader();
    drawMeter();
    drawRecordButton();
    drawRecordingsList();
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_audio_recorder() {
    // Allocate read buffer in PSRAM — keeps 4KB out of internal SRAM.
    // Must happen before initI2SMic() so the buffer is valid when
    // recording starts. Free on exit regardless of how we leave.
    readBuf = (uint8_t*)ps_malloc(READ_BUF_BYTES);
    if (!readBuf) {
        gfx->fillScreen(0x0000);
        drawHeader();
        gfx->setTextColor(0xF800); gfx->setTextSize(1);
        gfx->setCursor(10, 50); gfx->print("Out of PSRAM — cannot start recorder.");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(10, 70); gfx->print("Tap header or Q to exit.");
        while (true) {
            int16_t tx, ty;
            if (get_touch(&tx,&ty) && ty<24) { while(get_touch(&tx,&ty)){delay(10);} break; }
            if (get_keypress()) break;
            delay(50);
        }
        return;
    }

    scanRecordings();
    recSelected = recCount > 0 ? recCount-1 : 0;
    recScroll   = max(0, recSelected - LIST_ROWS + 1);

    bool micOk = initI2SMic();
    if (!micOk) {
        free(readBuf); readBuf = nullptr;
        gfx->fillScreen(0x0000);
        drawHeader();
        gfx->setTextColor(0xF800); gfx->setTextSize(1);
        gfx->setCursor(10, 50);  gfx->print("ES7210 mic init failed.");
        gfx->setCursor(10, 66);  gfx->print("Check I2S wiring (GPIO 14,21,47,48)");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(10, 90);  gfx->print("Tap header to exit.");
        while (true) {
            int16_t tx, ty;
            if (get_touch(&tx,&ty) && ty<24) { while(get_touch(&tx,&ty)){delay(10);} break; }
            char k = get_keypress();
            if (k=='q'||k=='Q') break;
            delay(50);
        }
        return;
    }

    drawFull();

    bool running   = true;
    uint32_t lastDraw = millis();

    while (running) {

        // ── Audio capture loop — highest priority ──
        if (recording) {
            size_t bytesRead = 0;
            esp_err_t err = i2s_read(I2S_PORT, readBuf, READ_BUF_BYTES,
                                      &bytesRead, pdMS_TO_TICKS(100));

            if (err == ESP_OK && bytesRead > 0) {
                recFile.write(readBuf, bytesRead);
                recBytesWritten += bytesRead;
                updatePeak(readBuf, bytesRead);

                // Periodic flush to SD card
                writeCount++;
                if (writeCount >= FLUSH_EVERY) {
                    recFile.flush();
                    writeCount = 0;
                }
            }

            // Auto-stop at 5 minutes
            if (millis() - recStartMs > MAX_REC_MS) {
                stopRecording();
                drawFull();
            }
        }

        // ── UI refresh ──
        uint32_t now = millis();
        if (now - lastDraw > (recording ? 150 : 500)) {
            if (recording) {
                drawMeter();
                drawRecordButton();
            }
            lastDraw = now;
        }

        // ── Input handling ──
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        // Header tap = exit
        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (recording) stopRecording();
            running = false;
            continue;
        }

        // Record button tap
        if (get_touch(&tx, &ty) && ty >= 44 && ty <= 74) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (recording) { stopRecording(); drawFull(); }
            else if (startRecording()) { drawFull(); }
            continue;
        }

        // Space = toggle record
        if (k == ' ') {
            if (recording) { stopRecording(); drawFull(); }
            else if (startRecording()) { drawFull(); }
            continue;
        }

        // Q = quit
        if (k == 'q' || k == 'Q') {
            if (recording) stopRecording();
            running = false;
            continue;
        }

        // Delete selected
        if ((k == 127 || k == 8) && !recording && recCount > 0) {
            deleteRecording(recSelected);
            drawRecordingsList();
            continue;
        }

        // Trackball scroll
        if (!recording) {
            bool nav = false;
            if (tb.y == -1 && recSelected > 0) {
                recSelected--;
                if (recSelected < recScroll) recScroll--;
                nav = true;
            }
            if (tb.y == 1 && recSelected < recCount-1) {
                recSelected++;
                if (recSelected >= recScroll + LIST_ROWS) recScroll++;
                nav = true;
            }
            if (nav) drawRecordingsList();
        }

        // Small yield — but don't delay() during recording
        if (!recording) delay(5);
        yield();
    }

    if (recording) stopRecording();
    deinitI2SMic();
    free(readBuf); readBuf = nullptr;
    gfx->fillScreen(0x0000);
}