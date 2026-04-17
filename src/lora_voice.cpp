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
 * PISCES MOON OS — LORA VOICE v1.1
 * Push-to-talk walkie-talkie over LoRa using Codec2 audio compression.
 *
 * v1.1 changes:
 *   - Codec2 encode/decode now wrapped in #ifdef CODEC2_AVAILABLE guards.
 *   - To enable real Codec2: add to platformio.ini lib_deps:
 *       meshtastic/ESP32_Codec2 @ ^1.0.0
 *     then add to build_flags:
 *       -DCODEC2_AVAILABLE
 *   - Without the library: transmits raw PCM (proof of concept, lower quality).
 *   - Status bar shows CODEC2:ON or RAW PCM clearly.
 *
 * Architecture:
 *   TX path: ES7210 mic → I2S DMA → Codec2 encode → LoRa SX1262 TX
 *   RX path: LoRa SX1262 RX → Codec2 decode → I2S DAC → Speaker
 *
 * Controls:
 *   SPACE (hold) = Push-to-Talk transmit
 *   SPACE release = Return to receive mode
 *   Q / tap header = Exit
 *
 * Codec2 frame structure at 3200 bps:
 *   Frame size: 8 bytes per 20ms of audio at 8kHz
 *   LoRa packet: 8 frames = 160ms audio per packet = 64 bytes
 *
 * SPI Bus Treaty:
 *   lora_voice_active = true during entire voice session.
 *   wardrive_task checks this before SD writes.
 *
 * Hardware constraint:
 *   GPIO0 (TRK_CLICK) shares pin with ES7210 mic.
 *   Cannot use trackball click as PTT — use SPACE bar.
 */

#include <Arduino.h>
#include <FS.h>
#include <driver/i2s.h>
#include <Arduino_GFX_Library.h>
#include <RadioLib.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "lora_voice.h"

// ── Codec2 optional include ───────────────────────────────────────────────
// Add meshtastic/ESP32_Codec2 to lib_deps and -DCODEC2_AVAILABLE to
// build_flags to enable real encode/decode. Without it the app runs in
// raw-PCM mode — radio and mic still work, audio quality is lower.
#ifdef CODEC2_AVAILABLE
#include <codec2.h>
#endif

extern Arduino_GFX *gfx;
extern volatile bool wifi_in_use;

volatile bool lora_voice_active = false;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define LV_BG        0x0000
#define LV_HEADER    0x0841
#define LV_GREEN     0x07E0
#define LV_CYAN      0x07FF
#define LV_WHITE     0xFFFF
#define LV_DIM       0x4208
#define LV_RED       0xF800
#define LV_AMBER     0xFD20
#define LV_TX_COLOR  0xF800
#define LV_RX_COLOR  0x07E0

// ─────────────────────────────────────────────
//  LORA HARDWARE (T-Deck Plus SX1262)
// ─────────────────────────────────────────────
#define LORA_CS_PIN   9
#define LORA_RST_PIN  17
#define LORA_DIO1_PIN 45
#define LORA_BUSY_PIN 13

// ─────────────────────────────────────────────
//  AUDIO CONFIG
// ─────────────────────────────────────────────
#define LV_SAMPLE_RATE    8000
#define LV_I2S_MIC_PORT   I2S_NUM_1
#define LV_DMA_BUF_COUNT  4
#define LV_DMA_BUF_LEN    512
#define LV_CODEC_SAMPLES  160
#define LV_CODEC_BYTES    8
#define LV_FRAMES_PER_PKT 8
#define LV_PKT_BYTES      (LV_CODEC_BYTES * LV_FRAMES_PER_PKT)

#define LV_MIC_MCLK  48
#define LV_MIC_LRCK  21
#define LV_MIC_SCK   47
#define LV_MIC_DIN   14

// ─────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────
static SX1262        lvRadio      = new Module(LORA_CS_PIN, LORA_DIO1_PIN,
                                               LORA_RST_PIN, LORA_BUSY_PIN);
static bool          lvPTTActive  = false;
static bool          lvReceiving  = false;
static unsigned long lvLastRX     = 0;
static unsigned long lvLastTX     = 0;
static int           lvRSSI       = 0;
static float         lvSNR        = 0.0f;

#ifdef CODEC2_AVAILABLE
static struct CODEC2* lvCodec2State = nullptr;
#endif

// ─────────────────────────────────────────────
//  RADIO INIT
// ─────────────────────────────────────────────
static bool lvInitRadio() {
    int state = lvRadio.beginFSK(915.0, 9.6, 19.5, 39.0, 10, 16);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LORA VOICE] Radio init failed: %d\n", state);
        return false;
    }
    lvRadio.setDio1Action(nullptr);
    Serial.println("[LORA VOICE] Radio OK — 915MHz FSK");
    return true;
}

// ─────────────────────────────────────────────
//  MIC INIT / DEINIT
// ─────────────────────────────────────────────
static bool lvInitMic() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = LV_SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = LV_DMA_BUF_COUNT,
        .dma_buf_len          = LV_DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pins = {
        .mck_io_num   = LV_MIC_MCLK,
        .bck_io_num   = LV_MIC_SCK,
        .ws_io_num    = LV_MIC_LRCK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = LV_MIC_DIN
    };
    if (i2s_driver_install(LV_I2S_MIC_PORT, &cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(LV_I2S_MIC_PORT, &pins) != ESP_OK) {
        i2s_driver_uninstall(LV_I2S_MIC_PORT);
        return false;
    }
    uint8_t buf[LV_DMA_BUF_LEN * 2];
    size_t r = 0;
    for (int i = 0; i < 4; i++)
        i2s_read(LV_I2S_MIC_PORT, buf, sizeof(buf), &r, pdMS_TO_TICKS(100));
    return true;
}

static void lvDeinitMic() {
    i2s_stop(LV_I2S_MIC_PORT);
    i2s_driver_uninstall(LV_I2S_MIC_PORT);
}

// ─────────────────────────────────────────────
//  DRAW UI
// ─────────────────────────────────────────────
static void lvDrawUI(bool ptt, bool receiving) {
    gfx->fillRect(0, 0, 320, 22, LV_HEADER);
    gfx->drawFastHLine(0, 22, 320, LV_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(LV_GREEN);
    gfx->setCursor(6, 7);
    gfx->print("LORA VOICE");

#ifdef CODEC2_AVAILABLE
    gfx->setTextColor(LV_GREEN);
    gfx->setCursor(108, 7);
    gfx->print("CODEC2:ON");
#else
    gfx->setTextColor(LV_AMBER);
    gfx->setCursor(108, 7);
    gfx->print("RAW PCM");
#endif

    gfx->setTextColor(LV_DIM);
    gfx->setCursor(240, 7);
    gfx->print("[TAP=EXIT]");

    gfx->fillRect(0, 24, 320, 160, LV_BG);

    if (ptt) {
        gfx->fillRect(40, 50, 240, 80, LV_TX_COLOR);
        gfx->drawRect(40, 50, 240, 80, 0xF000);
        gfx->setTextSize(3);
        gfx->setTextColor(LV_WHITE);
        gfx->setCursor(90, 80);
        gfx->print("TX");
        gfx->setTextSize(1);
        gfx->setTextColor(LV_WHITE);
        gfx->setCursor(60, 138);
        gfx->print("Transmitting... release SPACE to stop");
    } else if (receiving) {
        gfx->fillRect(40, 50, 240, 80, 0x03E0);
        gfx->drawRect(40, 50, 240, 80, 0x0300);
        gfx->setTextSize(3);
        gfx->setTextColor(LV_WHITE);
        gfx->setCursor(90, 80);
        gfx->print("RX");
        gfx->setTextSize(1);
        gfx->setTextColor(LV_GREEN);
        gfx->setCursor(80, 138);
        gfx->printf("RSSI: %d dBm  SNR: %.1f dB", lvRSSI, lvSNR);
    } else {
        gfx->setTextSize(2);
        gfx->setTextColor(LV_DIM);
        gfx->setCursor(68, 80);
        gfx->print("STANDBY");
        gfx->setTextSize(1);
        gfx->setTextColor(LV_DIM);
        gfx->setCursor(70, 115);
        gfx->print("Listening for transmissions...");
        if (lvLastRX > 0) {
            unsigned long ago = (millis() - lvLastRX) / 1000;
            gfx->setCursor(90, 138);
            gfx->printf("Last RX: %lus ago  RSSI: %d", ago, lvRSSI);
        }
    }

    gfx->fillRect(0, 190, 320, 50, 0x0821);
    gfx->drawFastHLine(0, 190, 320, LV_DIM);
    uint16_t btnColor = ptt ? LV_TX_COLOR : 0x3186;
    gfx->fillRect(80, 198, 160, 34, btnColor);
    gfx->drawRect(80, 198, 160, 34, ptt ? 0xF000 : LV_DIM);
    gfx->setTextSize(2);
    gfx->setTextColor(LV_WHITE);
    gfx->setCursor(108, 207);
    gfx->print(ptt ? "RELEASING" : "HOLD SPACE");
}

// ─────────────────────────────────────────────
//  TX — ENCODE AND TRANSMIT
// ─────────────────────────────────────────────
static void lvTransmit() {
    uint8_t packet[LV_PKT_BYTES];
    int16_t samples[LV_CODEC_SAMPLES * LV_FRAMES_PER_PKT];
    size_t  bytesRead = 0;

    i2s_read(LV_I2S_MIC_PORT, samples,
             LV_CODEC_SAMPLES * LV_FRAMES_PER_PKT * sizeof(int16_t),
             &bytesRead, pdMS_TO_TICKS(200));

    if (bytesRead == 0) return;

#ifdef CODEC2_AVAILABLE
    if (lvCodec2State) {
        for (int i = 0; i < LV_FRAMES_PER_PKT; i++) {
            codec2_encode(lvCodec2State,
                          &packet[i * LV_CODEC_BYTES],
                          &samples[i * LV_CODEC_SAMPLES]);
        }
    }
#else
    // Raw PCM fallback — proves the radio path
    memcpy(packet, samples, min((int)bytesRead, LV_PKT_BYTES));
#endif

    int state = lvRadio.transmit(packet, LV_PKT_BYTES);
    if (state == RADIOLIB_ERR_NONE) lvLastTX = millis();
}

// ─────────────────────────────────────────────
//  RX — RECEIVE AND DECODE
// ─────────────────────────────────────────────
static bool lvReceive() {
    uint8_t packet[LV_PKT_BYTES + 4];
    int state = lvRadio.receive(packet, LV_PKT_BYTES);
    if (state != RADIOLIB_ERR_NONE) return false;

    lvRSSI   = (int)lvRadio.getRSSI();
    lvSNR    = lvRadio.getSNR();
    lvLastRX = millis();

#ifdef CODEC2_AVAILABLE
    if (lvCodec2State) {
        int16_t samples[LV_CODEC_SAMPLES * LV_FRAMES_PER_PKT];
        size_t  written = 0;
        for (int i = 0; i < LV_FRAMES_PER_PKT; i++) {
            codec2_decode(lvCodec2State,
                          &samples[i * LV_CODEC_SAMPLES],
                          &packet[i * LV_CODEC_BYTES]);
        }
        i2s_write(I2S_NUM_0, samples,
                  LV_CODEC_SAMPLES * LV_FRAMES_PER_PKT * sizeof(int16_t),
                  &written, pdMS_TO_TICKS(100));
    }
#else
    Serial.printf("[LORA VOICE] RX %d bytes  RSSI:%d  SNR:%.1f\n",
                  LV_PKT_BYTES, lvRSSI, lvSNR);
#endif

    return true;
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_lora_voice() {
    gfx->fillScreen(LV_BG);
    lvPTTActive = false;
    lvReceiving = false;
    lvLastRX    = 0;
    lvLastTX    = 0;

    gfx->fillRect(0, 0, 320, 22, LV_HEADER);
    gfx->drawFastHLine(0, 22, 320, LV_GREEN);
    gfx->setTextColor(LV_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(6, 7);
    gfx->print("LORA VOICE — INITIALIZING");

    gfx->setTextColor(LV_DIM);
    gfx->setCursor(10, 50);
    gfx->print("Starting radio (FSK 915MHz)...");

    if (!lvInitRadio()) {
        gfx->setTextColor(LV_RED);
        gfx->setCursor(10, 80); gfx->print("Radio init FAILED.");
        gfx->setCursor(10, 96); gfx->print("Check LoRa module. Q/tap to exit.");
        while (true) {
            int16_t tx, ty;
            if (get_touch(&tx, &ty) && ty < 24) return;
            if (get_keypress() == 'q') return;
            delay(100);
        }
    }
    gfx->setTextColor(LV_GREEN);
    gfx->setCursor(10, 70);
    gfx->print("Radio OK.");

    gfx->setTextColor(LV_DIM);
    gfx->setCursor(10, 90);
    gfx->print("Starting microphone (8kHz)...");

    if (!lvInitMic()) {
        gfx->setTextColor(LV_RED);
        gfx->setCursor(10, 110); gfx->print("Mic init FAILED. Tap to exit.");
        while (true) {
            int16_t tx, ty;
            if (get_touch(&tx, &ty) && ty < 24) return;
            if (get_keypress() == 'q') return;
            delay(100);
        }
    }
    gfx->setTextColor(LV_GREEN);
    gfx->setCursor(10, 110);
    gfx->print("Mic OK.");

#ifdef CODEC2_AVAILABLE
    lvCodec2State = codec2_create(CODEC2_MODE_3200);
    if (!lvCodec2State) {
        gfx->setTextColor(LV_RED);
        gfx->setCursor(10, 130);
        gfx->print("Codec2 init FAILED — check library.");
        delay(2000);
        lvDeinitMic();
        return;
    }
    gfx->setTextColor(LV_GREEN);
    gfx->setCursor(10, 130);
    gfx->print("Codec2 3200bps OK.");
    delay(800);
#else
    gfx->setTextColor(LV_AMBER);
    gfx->setCursor(10, 130);
    gfx->print("Codec2 not installed — raw PCM mode.");
    gfx->setTextColor(LV_DIM);
    gfx->setCursor(10, 148);
    gfx->print("Add meshtastic/ESP32_Codec2 to lib_deps");
    gfx->setCursor(10, 160);
    gfx->print("and -DCODEC2_AVAILABLE to build_flags.");
    delay(2000);
#endif

    lora_voice_active = true;
    lvDrawUI(false, false);

    bool spaceWasDown = false;
    unsigned long lastDraw = millis();

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) delay(10);
            break;
        }

        char k = get_keypress();
        if (k == 'q' || k == 'Q') break;

        bool spaceNow = (k == ' ');
        if (spaceNow && !spaceWasDown) {
            lvPTTActive = true; spaceWasDown = true;
            lvDrawUI(true, false);
        } else if (!spaceNow && spaceWasDown) {
            lvPTTActive = false; spaceWasDown = false;
            lvDrawUI(false, false);
        }

        if (lvPTTActive) {
            lvTransmit();
        } else {
            bool rxd = lvReceive();
            if (rxd != lvReceiving) {
                lvReceiving = rxd;
                lvDrawUI(false, lvReceiving);
            }
        }

        if (millis() - lastDraw > 500) {
            if (!lvPTTActive) {
                bool stillRX = (millis() - lvLastRX) < 2000;
                if (stillRX != lvReceiving) {
                    lvReceiving = stillRX;
                    lvDrawUI(false, lvReceiving);
                }
            }
            lastDraw = millis();
        }

        delay(10);
        yield();
    }

    lora_voice_active = false;
    lvDeinitMic();
    lvRadio.standby();

#ifdef CODEC2_AVAILABLE
    if (lvCodec2State) {
        codec2_destroy(lvCodec2State);
        lvCodec2State = nullptr;
    }
#endif

    gfx->fillScreen(LV_BG);
}
