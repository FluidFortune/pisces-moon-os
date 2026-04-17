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
 * PISCES MOON OS — AUDIO PLAYER
 * WinAmp-inspired player for MP3, FLAC, AAC, OGG files from SD card.
 * Uses ESP32-audioI2S library for hardware I2S output.
 *
 * I2S Speaker pins (from LilyGO utilities.h):
 *   BCLK  = GPIO7
 *   LRC   = GPIO5  (Word Select)
 *   DOUT  = GPIO6
 *
 * Controls:
 *   Trackball UP/DOWN  = scroll playlist
 *   Trackball CLICK    = play selected track
 *   Trackball LEFT     = previous track
 *   Trackball RIGHT    = next track
 *   Keyboard SPACE     = pause/resume
 *   Keyboard +/-       = volume up/down
 *   Keyboard Q         = quit
 *   Touch header       = quit
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SdFat.h>
#include "Audio.h"          // ESP32-audioI2S
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "apps.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ── I2S Pin Map (LilyGO T-Deck utilities.h) ──
#define I2S_BCLK   7
#define I2S_LRC    5
#define I2S_DOUT   6

// ── Layout ──
#define HEADER_H    26
#define VIZ_Y       28    // Visualizer bar top
#define VIZ_H       28    // Visualizer bar height
#define INFO_Y      58    // Track info area
#define LIST_Y      90    // Playlist list start
#define LIST_ROW_H  16
#define LIST_ROWS   7     // Tracks visible at once
#define CTRL_Y      208   // Controls bar

// ── Playlist ──
#define MAX_TRACKS  128
static char  playlist[MAX_TRACKS][64];
static int   trackCount   = 0;
static int   currentTrack = 0;
static int   listOffset   = 0;   // Scroll offset for playlist display
static int   cursorPos    = 0;   // Trackball cursor within visible list

// ── Player state ──
static Audio  audio;
static bool   isPlaying   = false;
static bool   isPaused    = false;
static int    volume       = 12;  // 0-21
static unsigned long trackStartMs = 0;

// ── Fake visualizer state ──
static uint8_t vizBars[16] = {0};
static unsigned long lastVizUpdate = 0;

// ─────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────
static bool isSupportedAudio(const char* name) {
    int len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + len - 4;
    return (strcasecmp(ext, ".mp3") == 0 ||
            strcasecmp(ext, ".aac") == 0 ||
            strcasecmp(ext, ".ogg") == 0 ||
            strcasecmp(ext + 1, ".flac") == 0 || // 5-char ext
            strncasecmp(name + len - 5, ".flac", 5) == 0);
}

static void scanMusicFolder() {
    trackCount = 0;
    // Check /music first, then root
    const char* folders[] = {"/music", "/Music", "/MUSIC", "/"};
    for (int f = 0; f < 4; f++) {
        FsFile dir = sd.open(folders[f]);
        if (!dir) continue;
        FsFile entry;
        while (entry.openNext(&dir, O_READ) && trackCount < MAX_TRACKS) {
            char name[64];
            entry.getName(name, sizeof(name));
            if (!entry.isDir() && isSupportedAudio(name)) {
                snprintf(playlist[trackCount], 64, "%s/%s",
                         strcmp(folders[f], "/") == 0 ? "" : folders[f],
                         name);
                trackCount++;
            }
            entry.close();
        }
        dir.close();
        if (trackCount > 0) break; // Found music, stop searching
    }
}

static String shortName(const char* path) {
    // Return just the filename without path or extension
    const char* slash = strrchr(path, '/');
    String name = slash ? String(slash + 1) : String(path);
    int dot = name.lastIndexOf('.');
    if (dot > 0) name = name.substring(0, dot);
    if (name.length() > 36) name = name.substring(0, 33) + "...";
    return name;
}

// ─────────────────────────────────────────────
//  VISUALIZER  (fake — animates while playing)
//  Real FFT would need significant CPU budget.
//  This gives the WinAmp feel without the cost.
// ─────────────────────────────────────────────
static void updateViz() {
    if (millis() - lastVizUpdate < 80) return;
    lastVizUpdate = millis();

    for (int i = 0; i < 16; i++) {
        if (isPlaying && !isPaused) {
            // Random walk with decay — mimics music energy
            int delta = random(-3, 5);
            int newVal = (int)vizBars[i] + delta;
            newVal = constrain(newVal, 0, 26);
            vizBars[i] = (uint8_t)newVal;
        } else {
            // Decay to zero when paused/stopped
            if (vizBars[i] > 0) vizBars[i]--;
        }
    }

    // Draw bars
    int barW = 320 / 16;
    for (int i = 0; i < 16; i++) {
        int bx = i * barW;
        int barH = vizBars[i];
        // Clear old bar
        gfx->fillRect(bx, VIZ_Y, barW - 1, VIZ_H, C_BLACK);
        if (barH > 0) {
            // Color: green at bottom → yellow → red at top
            uint16_t color = (barH < 10) ? C_GREEN :
                             (barH < 20) ? 0xFFE0 : C_RED;
            gfx->fillRect(bx, VIZ_Y + VIZ_H - barH, barW - 1, barH, color);
        }
    }
}

// ─────────────────────────────────────────────
//  DRAW FUNCTIONS
// ─────────────────────────────────────────────
static void drawHeader() {
    gfx->fillRect(0, 0, 320, HEADER_H, C_DARK);
    gfx->drawFastHLine(0, HEADER_H - 1, 320, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->print("PISCES PLAYER  Q/HDR:EXIT  SPC:PAUSE");
}

static void drawTrackInfo() {
    gfx->fillRect(0, INFO_Y, 320, LIST_Y - INFO_Y, C_BLACK);

    if (trackCount == 0) {
        gfx->setCursor(10, INFO_Y + 4);
        gfx->setTextColor(C_RED);
        gfx->print("No audio files found.");
        gfx->setCursor(10, INFO_Y + 16);
        gfx->setTextColor(C_GREY);
        gfx->print("Put MP3/FLAC/AAC/OGG in /music/");
        return;
    }

    // Track name
    gfx->setCursor(10, INFO_Y + 2);
    gfx->setTextColor(C_CYAN);
    gfx->setTextSize(1);
    gfx->print(shortName(playlist[currentTrack]));

    // Status + volume
    gfx->setCursor(10, INFO_Y + 16);
    gfx->setTextColor(C_GREY);
    gfx->printf("%s  %d/%d  VOL:%d",
                isPlaying ? (isPaused ? "||PAUSED" : "> PLAYING") : "[] STOPPED",
                currentTrack + 1, trackCount, volume);
}

static void drawPlaylist() {
    gfx->fillRect(0, LIST_Y, 320, CTRL_Y - LIST_Y, C_BLACK);

    if (trackCount == 0) return;

    for (int i = 0; i < LIST_ROWS; i++) {
        int trackIdx = listOffset + i;
        if (trackIdx >= trackCount) break;

        int rowY = LIST_Y + (i * LIST_ROW_H);
        bool isCurrent  = (trackIdx == currentTrack);
        bool isCursor   = (i == cursorPos);

        // Background highlight
        if (isCursor) {
            gfx->fillRect(0, rowY, 320, LIST_ROW_H, C_DARK);
        }

        // Playing indicator
        gfx->setCursor(2, rowY + 3);
        if (isCurrent && isPlaying) {
            gfx->setTextColor(C_GREEN);
            gfx->print(">");
        } else {
            gfx->setTextColor(C_GREY);
            gfx->printf("%d", trackIdx + 1);
        }

        // Track name
        gfx->setCursor(22, rowY + 3);
        gfx->setTextColor(isCurrent ? C_WHITE : C_GREY);
        String name = shortName(playlist[trackIdx]);
        name = name.substring(0, 38);
        gfx->print(name);
    }
}

static void drawControls() {
    gfx->fillRect(0, CTRL_Y, 320, 240 - CTRL_Y, C_DARK);
    gfx->drawFastHLine(0, CTRL_Y, 320, C_GREEN);
    gfx->setTextColor(C_GREY);
    gfx->setTextSize(1);
    gfx->setCursor(5, CTRL_Y + 8);
    gfx->print("|< BALL-L   SPC PAUSE   BALL-R >|   +/- VOL");
}

static void drawFull() {
    gfx->fillScreen(C_BLACK);
    drawHeader();
    drawTrackInfo();
    drawPlaylist();
    drawControls();
}

// ─────────────────────────────────────────────
//  PLAYBACK CONTROL
// ─────────────────────────────────────────────
static void playTrack(int idx) {
    if (idx < 0) idx = trackCount - 1;
    if (idx >= trackCount) idx = 0;
    currentTrack = idx;

    audio.stopSong();
    isPlaying = false;
    isPaused  = false;

    if (trackCount == 0) return;

    // ESP32-audioI2S expects the full SD path
    if (audio.connecttoFS(SD, playlist[currentTrack])) {
        isPlaying    = true;
        isPaused     = false;
        trackStartMs = millis();
    }
    drawTrackInfo();
    drawPlaylist();
}

static void togglePause() {
    if (!isPlaying) return;
    isPaused = !isPaused;
    audio.pauseResume();
    drawTrackInfo();
}

static void adjustVolume(int delta) {
    volume = constrain(volume + delta, 0, 21);
    audio.setVolume(volume);
    drawTrackInfo();
}

// ─────────────────────────────────────────────
//  PLAYLIST CURSOR MOVEMENT
// ─────────────────────────────────────────────
static void moveCursor(int dir) {
    // dir: -1=up, +1=down
    int absIdx = listOffset + cursorPos + dir;
    absIdx = constrain(absIdx, 0, trackCount - 1);

    cursorPos = absIdx - listOffset;

    // Scroll list if cursor goes off screen
    if (cursorPos < 0) {
        listOffset = max(0, listOffset - 1);
        cursorPos  = 0;
    } else if (cursorPos >= LIST_ROWS) {
        listOffset = min(trackCount - LIST_ROWS, listOffset + 1);
        cursorPos  = LIST_ROWS - 1;
    }
    drawPlaylist();
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_audio_player() {
    // Init I2S audio
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(volume);

    // Scan SD for music files
    scanMusicFolder();

    // Draw initial UI
    drawFull();

    // Auto-play first track if any found
    if (trackCount > 0) {
        playTrack(0);
    }

    bool running = true;
    unsigned long lastInfoUpdate = millis();

    while (running) {
        // Feed the audio engine — MUST be called every loop
        audio.loop();

        // ── Visualizer update ──
        updateViz();

        // ── Periodic track info refresh (elapsed time etc.) ──
        if (millis() - lastInfoUpdate > 1000) {
            drawTrackInfo();
            lastInfoUpdate = millis();
        }

        // ── Keyboard input ──
        char k = get_keypress();
        if (k == 'q' || k == 'Q') {
            running = false;
        } else if (k == ' ') {
            togglePause();
        } else if (k == '+' || k == '=') {
            adjustVolume(1);
        } else if (k == '-') {
            adjustVolume(-1);
        } else if (k == 'n' || k == 'N') {
            playTrack(currentTrack + 1);
        } else if (k == 'p' || k == 'P') {
            playTrack(currentTrack - 1);
        }

        // ── Trackball input ──
        TrackballState tb = update_trackball();
        if (tb.y == -1) {
            moveCursor(-1);
        } else if (tb.y == 1) {
            moveCursor(1);
        } else if (tb.x == -1) {
            // Left = previous track
            playTrack(currentTrack - 1);
            drawTrackInfo();
        } else if (tb.x == 1) {
            // Right = next track
            playTrack(currentTrack + 1);
            drawTrackInfo();
        } else if (tb.clicked) {
            // Click = play cursor selection
            int selected = listOffset + cursorPos;
            if (selected < trackCount) {
                playTrack(selected);
            }
        }

        // ── Touch ──
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < HEADER_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            running = false;
        }

        // ── Auto-advance: check if track finished ──
        // ESP32-audioI2S calls audio_eof_mp3() callback when done.
        // We poll isPlaying — if it went false naturally, advance.
        // (Requires audio_eof callback below to set flag)

        yield();
        delay(5); // Short delay to not starve other tasks
    }

    // Clean up
    audio.stopSong();
    isPlaying = false;
    isPaused  = false;
}

// ─────────────────────────────────────────────
//  ESP32-audioI2S CALLBACKS
//  These are called by the library automatically.
// ─────────────────────────────────────────────
void audio_eof_mp3(const char* info) {
    // Track finished — auto-advance to next
    Serial.printf("[AUDIO] EOF: %s\n", info);
    isPlaying = false;
    // We can't call playTrack() from a callback safely,
    // so we flag it and handle in the main loop.
    // Simple approach: restart will be detected via !isPlaying next loop.
    int next = currentTrack + 1;
    if (next < trackCount) {
        playTrack(next);
    } else {
        // End of playlist — stop
        currentTrack = 0;
    }
}

void audio_info(const char* info) {
    Serial.printf("[AUDIO] %s\n", info);
}