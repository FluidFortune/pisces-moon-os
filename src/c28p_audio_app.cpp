// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_audio_app.cpp — MP3 file player for C28P
//
//  Scans /audio/ on the SD card for .mp3 files, presents a
//  scrollable touch list, plays the selected file via the
//  ESP32-audioI2S library routed through the ES8311 codec.
//
//  ARCHITECTURE NOTE (v1.3):
//
//    Two audio paths exist for the C28P:
//      1. c28p_audio.cpp — raw I2S tone generation for games
//         (square waves for Korobeiniki etc.)
//      2. c28p_audio_app.cpp (this file) — ESP32-audioI2S
//         library for MP3 streaming
//
//    Both paths share the same ES8311 codec but install
//    different I2S drivers. The audio HAL (include/c28p_audio.h)
//    keeps them honest with explicit mode transitions:
//
//      c28p_audio_enter(C28P_AUDIO_PLAYBACK_AUDIO)
//          → enables amp + codec for DAC; HAL does NOT install
//            an I2S driver — leaves that to the Audio library.
//      audio->connecttoFS(SD_MMC, path)
//          → Audio library installs its own I2S TX driver
//            (via setPinout()) and starts decoding.
//      audio->stopSong() / delete audio
//          → library uninstalls its driver.
//      c28p_audio_release_driver()
//          → tell the HAL the caller already released the
//            driver. HAL drops to IDLE, amp off. Game audio can
//            resume after this without a reboot.
//
//    SD access uses Arduino's SD_MMC directly here — the
//    ESP32-audioI2S Audio::connecttoFS API requires an fs::FS&
//    and pm_storage doesn't pretend to be one. Everywhere else
//    in the OS the pm_storage HAL is preferred.
//
//  TOUCH UI:
//    - Title bar at top with "AUDIO PLAYER"
//    - 8 scrollable file rows
//    - PREV/PLAY-PAUSE/NEXT/BACK transport controls at bottom
//    - Progress bar showing playback position
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include <SD_MMC.h>
#include "Audio.h"
#include <driver/i2s.h>
#include "hal_pins.h"
#include "c28p_audio.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t* x, int16_t* y);
extern volatile bool g_sd_ready;

#define MAX_FILES   32
#define FNAME_LEN   48

static char file_names[MAX_FILES][FNAME_LEN];
static int file_count = 0;
static int sel_idx = 0;
static int scroll_top = 0;
static bool playing = false;
static int current_idx = -1;

static Audio* audio = nullptr;

// ─────────────────────────────────────────────
//  Scan /audio/ on the SD card
//
//  The C28P's SD card lives on the SDMMC peripheral (4-bit SDIO),
//  not on SPI — so we use Arduino's SD_MMC driver, which is
//  already mounted in main.cpp's setup(). No SD.begin() call here:
//  attempting to mount twice fails, and main.cpp owns the global
//  mount lifecycle. We just check g_sd_ready before touching the FS.
// ─────────────────────────────────────────────
static void scan_audio_files() {
    file_count = 0;
    if (!g_sd_ready) {
        Serial.println("[C28P-Audio] SD not ready (mount failed at boot)");
        return;
    }
    File dir = SD_MMC.open("/audio");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[C28P-Audio] /audio/ not found, creating");
        SD_MMC.mkdir("/audio");
        return;
    }
    File f = dir.openNextFile();
    while (f && file_count < MAX_FILES) {
        if (!f.isDirectory()) {
            const char* raw = f.name();
            const char* slash = strrchr(raw, '/');
            const char* name = slash ? slash + 1 : raw;
            int n = strlen(name);
            if (n > 4 && (strcasecmp(name + n - 4, ".mp3") == 0 ||
                          strcasecmp(name + n - 4, ".wav") == 0)) {
                strncpy(file_names[file_count], name, FNAME_LEN - 1);
                file_names[file_count][FNAME_LEN - 1] = 0;
                file_count++;
            }
        }
        f = dir.openNextFile();
    }
    dir.close();
    Serial.printf("[C28P-Audio] Found %d audio files\n", file_count);
}

// ─────────────────────────────────────────────
//  Draw chrome
// ─────────────────────────────────────────────
static void draw_player_chrome() {
    gfx->fillScreen(0x0000);

    // Title bar
    gfx->fillRect(0, 0, 240, 32, 0x18C3);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFD20);
    gfx->setCursor(20, 8);
    gfx->print("AUDIO");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(120, 14);
    gfx->printf("%d files", file_count);

    // Transport controls at the bottom
    const int t_y = 270;
    const int btn_w = 56;
    gfx->fillRect(4,   t_y, btn_w, 44, 0x18C3);
    gfx->fillRect(64,  t_y, btn_w, 44, playing ? 0x07E0 : 0x18C3);
    gfx->fillRect(124, t_y, btn_w, 44, 0x18C3);
    gfx->fillRect(184, t_y, 52,    44, 0x3000);   // BACK
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(20, t_y + 16);  gfx->print("PREV");
    gfx->setCursor(80, t_y + 16);  gfx->print(playing ? "PAUSE" : "PLAY");
    gfx->setCursor(140, t_y + 16); gfx->print("NEXT");
    gfx->setCursor(196, t_y + 16); gfx->print("BACK");
}

// ─────────────────────────────────────────────
//  Draw file list
// ─────────────────────────────────────────────
static void draw_file_list() {
    const int list_y = 40;
    const int row_h = 26;
    const int list_h = 8 * row_h;
    gfx->fillRect(0, list_y, 240, list_h, 0x0000);

    if (file_count == 0) {
        gfx->setTextSize(1);
        gfx->setTextColor(0xF800);
        gfx->setCursor(20, list_y + 40);
        gfx->print("No audio files found.");
        gfx->setTextColor(0x8410);
        gfx->setCursor(20, list_y + 60);
        gfx->print("Copy MP3 files to /audio/");
        gfx->setCursor(20, list_y + 76);
        gfx->print("on the SD card.");
        return;
    }

    for (int i = 0; i < 8 && (scroll_top + i) < file_count; i++) {
        int idx = scroll_top + i;
        int y = list_y + i * row_h;
        bool is_sel = (idx == sel_idx);
        bool is_playing = (idx == current_idx && playing);

        if (is_sel) gfx->fillRect(0, y, 240, row_h - 2, 0x3208);
        if (is_playing) {
            gfx->setTextColor(0x07E0);
            gfx->setCursor(4, y + 8);
            gfx->print(">");
        }
        gfx->setTextSize(1);
        gfx->setTextColor(is_sel ? 0xFFFF : 0x8410);
        gfx->setCursor(14, y + 8);
        // Truncate long filenames
        char buf[28];
        strncpy(buf, file_names[idx], 27);
        buf[27] = 0;
        gfx->print(buf);
    }
}

// ─────────────────────────────────────────────
//  Playback control
// ─────────────────────────────────────────────
static void start_playback(int idx) {
    if (idx < 0 || idx >= file_count) return;
    if (audio) {
        audio->stopSong();
        playing = false;
    }

    // Hand the HAL into PLAYBACK_AUDIO mode — codec + amp on,
    // I2S driver intentionally NOT installed by the HAL so the
    // Audio library can install its own.
    if (!c28p_audio_enter(C28P_AUDIO_PLAYBACK_AUDIO)) {
        Serial.println("[C28P-Audio] HAL refused PLAYBACK_AUDIO mode");
        return;
    }

    if (!audio) {
        audio = new Audio();
        audio->setPinout(PIN_I2S_SCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, I2S_PIN_NO_CHANGE);
        // ESP32-audioI2S v2.1.0 has i2s_mclk_pin_select() but it is
        // hardcoded to ESP32 only (rejects pin != 0/1/3, and the
        // implementation is wrapped in #ifdef CONFIG_IDF_TARGET_ESP32).
        // The library's setPinout() never sets mck_io_num either.
        // So MCLK is never routed without intervention — we manually
        // add it on top of the library's pin config.
        i2s_pin_config_t mclk_cfg = {};
        mclk_cfg.mck_io_num   = PIN_I2S_MCLK;
        mclk_cfg.bck_io_num   = I2S_PIN_NO_CHANGE;
        mclk_cfg.ws_io_num    = I2S_PIN_NO_CHANGE;
        mclk_cfg.data_out_num = I2S_PIN_NO_CHANGE;
        mclk_cfg.data_in_num  = I2S_PIN_NO_CHANGE;
        i2s_set_pin(I2S_NUM_0, &mclk_cfg);
        audio->setVolume(15);   // 0-21, comfortable
    }
    char path[FNAME_LEN + 8];
    snprintf(path, sizeof(path), "/audio/%s", file_names[idx]);
    if (!audio->connecttoFS(SD_MMC, path)) {
        Serial.printf("[C28P-Audio] connecttoFS failed: %s\n", path);
        delete audio;
        audio = nullptr;
        current_idx = -1;
        playing = false;
        c28p_audio_release_driver();
        return;
    }
    current_idx = idx;
    playing = true;
    Serial.printf("[C28P-Audio] Playing %s\n", path);
}

static void stop_playback() {
    if (audio) {
        audio->stopSong();
    }
    playing = false;
    // We DON'T release the HAL here — the user may hit PLAY again on
    // the next track and we don't want to flap the amp. The full
    // HAL release happens in BACK (the exit path) below.
}

// ─────────────────────────────────────────────
//  PUBLIC ENTRY
// ─────────────────────────────────────────────
void c28p_run_audio_player() {
    Serial.println("[C28P-Audio] Player starting");

    // Don't enter PLAYBACK_AUDIO here — only when a track actually
    // starts. Just-launched-into-the-app should not power the amp.
    // The HAL stays in whatever state the launcher left it (IDLE
    // by default).

    scan_audio_files();
    sel_idx = 0;
    scroll_top = 0;
    current_idx = -1;
    playing = false;

    draw_player_chrome();
    draw_file_list();

    const int list_y = 40;
    const int row_h = 26;
    const int t_y = 270;

    bool was_touched = false;
    int touch_start_y = 0;
    while (true) {
        // Service the audio decoder
        if (audio && playing) {
            audio->loop();
            if (!audio->isRunning()) {
                playing = false;
                current_idx = -1;
                draw_player_chrome();
                draw_file_list();
            }
        }

        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            touch_start_y = ty;

            // Transport controls (bottom 50px)
            if (ty >= t_y) {
                if (tx < 60) {
                    // PREV
                    if (sel_idx > 0) sel_idx--;
                    draw_file_list();
                } else if (tx < 120) {
                    // PLAY/PAUSE
                    if (playing) {
                        stop_playback();
                    } else {
                        start_playback(sel_idx);
                    }
                    draw_player_chrome();
                    draw_file_list();
                } else if (tx < 180) {
                    // NEXT
                    if (sel_idx < file_count - 1) sel_idx++;
                    draw_file_list();
                } else {
                    // BACK to launcher — cleanly release the HAL.
                    stop_playback();
                    if (audio) { delete audio; audio = nullptr; }
                    // The Audio library uninstalled its I2S driver in
                    // its destructor. Tell the HAL so it doesn't try
                    // to uninstall again, then drop to IDLE.
                    c28p_audio_release_driver();
                    while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                    return;
                }
            }
            // File list region — tap to select
            else if (ty >= list_y && ty < list_y + 8 * row_h) {
                int row = (ty - list_y) / row_h;
                int idx = scroll_top + row;
                if (idx < file_count) {
                    if (idx == sel_idx) {
                        // Double-tap → play
                        start_playback(idx);
                        draw_player_chrome();
                    } else {
                        sel_idx = idx;
                    }
                    draw_file_list();
                }
            }
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

#endif // DEVICE_C28P
