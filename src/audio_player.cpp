// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)

#include <Arduino.h>
#include <new>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include <SdFat.h>
#include "Audio.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "pm_input.h"
#include "theme.h"
#include "apps.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 135;
#else
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
#endif
extern SdFat sd;

#define I2S_BCLK   7
#define I2S_LRC    5
#define I2S_DOUT   6

#define HEADER_H    26
#define VIZ_Y       28
#define VIZ_H       28
#define INFO_Y      58
#define LIST_Y      90
#define LIST_ROW_H  16
#define LIST_ROWS   7
static constexpr int CTRL_Y = DISP_H - 24;

#define MAX_TRACKS  128
static char  (*playlist)[64] = nullptr;
static int   trackCount = 0, currentTrack = 0, listOffset = 0, cursorPos = 0;
static Audio *audio = nullptr;
static bool  isPlaying = false, isPaused = false;
static int   volume = 12;
static unsigned long trackStartMs = 0;
static uint8_t vizBars[16] = {0};
static unsigned long lastVizUpdate = 0;

static bool ensureAudioState() {
    if (!playlist) {
        playlist = (char (*)[64])calloc(MAX_TRACKS, sizeof(*playlist));
        if (!playlist) {
            Serial.println("[AUDIO] Playlist allocation failed");
            return false;
        }
    }
    if (!audio) {
        audio = new (std::nothrow) Audio();
        if (!audio) {
            Serial.println("[AUDIO] Audio allocation failed");
            free(playlist);
            playlist = nullptr;
            return false;
        }
    }
    return true;
}

static void freeAudioState() {
    if (audio) {
        audio->stopSong();
        delete audio;
        audio = nullptr;
    }
    if (playlist) {
        free(playlist);
        playlist = nullptr;
    }
    trackCount = 0;
    currentTrack = 0;
    listOffset = 0;
    cursorPos = 0;
    isPlaying = false;
    isPaused = false;
}

static bool isSupportedAudio(const char* name) {
    int len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + len - 4;
    return (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".aac") == 0 ||
            strcasecmp(ext, ".ogg") == 0 || strcasecmp(ext + 1, ".flac") == 0 ||
            strncasecmp(name + len - 5, ".flac", 5) == 0);
}

static void scanMusicFolder() {
    trackCount = 0;
    if (!playlist) return;
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
                         strcmp(folders[f], "/") == 0 ? "" : folders[f], name);
                trackCount++;
            }
            entry.close();
        }
        dir.close();
        if (trackCount > 0) break;
    }
}

static String shortName(const char* path) {
    const char* slash = strrchr(path, '/');
    String name = slash ? String(slash + 1) : String(path);
    int dot = name.lastIndexOf('.');
    if (dot > 0) name = name.substring(0, dot);
    int maxLen = (DISP_W - 30) / 6;
    if ((int)name.length() > maxLen) name = name.substring(0, maxLen - 3) + "...";
    return name;
}

static void updateViz() {
    if (millis() - lastVizUpdate < 80) return;
    lastVizUpdate = millis();
    for (int i = 0; i < 16; i++) {
        if (isPlaying && !isPaused) {
            int newVal = (int)vizBars[i] + random(-3, 5);
            vizBars[i] = (uint8_t)constrain(newVal, 0, 26);
        } else if (vizBars[i] > 0) vizBars[i]--;
    }
    int barW = DISP_W / 16;
    for (int i = 0; i < 16; i++) {
        int bx = i * barW;
        int barH = vizBars[i];
        gfx->fillRect(bx, VIZ_Y, barW - 1, VIZ_H, C_BLACK);
        if (barH > 0) {
            uint16_t color = (barH < 10) ? C_GREEN : (barH < 20) ? 0xFFE0 : C_RED;
            gfx->fillRect(bx, VIZ_Y + VIZ_H - barH, barW - 1, barH, color);
        }
    }
}

static void drawHeader() {
    gfx->fillRect(0, 0, DISP_W, HEADER_H, C_DARK);
    gfx->drawFastHLine(0, HEADER_H - 1, DISP_W, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
#ifdef DEVICE_TLORAPAGER
    gfx->print("PISCES PLAYER  Q EXIT  SPC:PAUSE");
#else
    gfx->print("PISCES PLAYER  Q/HDR:EXIT  SPC:PAUSE");
#endif
}

static void drawTrackInfo() {
    gfx->fillRect(0, INFO_Y, DISP_W, LIST_Y - INFO_Y, C_BLACK);
    if (trackCount == 0) {
        gfx->setCursor(10, INFO_Y + 4); gfx->setTextColor(C_RED);
        gfx->print("No audio files found.");
        gfx->setCursor(10, INFO_Y + 16); gfx->setTextColor(C_GREY);
        gfx->print("Put MP3/FLAC/AAC/OGG in /music/");
        return;
    }
    gfx->setCursor(10, INFO_Y + 2); gfx->setTextColor(C_CYAN); gfx->setTextSize(1);
    gfx->print(shortName(playlist[currentTrack]));
    gfx->setCursor(10, INFO_Y + 16); gfx->setTextColor(C_GREY);
    gfx->printf("%s  %d/%d  VOL:%d",
                isPlaying ? (isPaused ? "||PAUSED" : "> PLAYING") : "[] STOPPED",
                currentTrack + 1, trackCount, volume);
}

static void drawPlaylist() {
    gfx->fillRect(0, LIST_Y, DISP_W, CTRL_Y - LIST_Y, C_BLACK);
    if (trackCount == 0) return;
    int maxNameChars = (DISP_W - 30) / 6;
    for (int i = 0; i < LIST_ROWS; i++) {
        int trackIdx = listOffset + i;
        if (trackIdx >= trackCount) break;
        int rowY = LIST_Y + (i * LIST_ROW_H);
        bool isCurrent = (trackIdx == currentTrack);
        bool isCursor  = (i == cursorPos);
        if (isCursor) gfx->fillRect(0, rowY, DISP_W, LIST_ROW_H, C_DARK);
        gfx->setCursor(2, rowY + 3);
        if (isCurrent && isPlaying) { gfx->setTextColor(C_GREEN); gfx->print(">"); }
        else { gfx->setTextColor(C_GREY); gfx->printf("%d", trackIdx + 1); }
        gfx->setCursor(22, rowY + 3);
        gfx->setTextColor(isCurrent ? C_WHITE : C_GREY);
        String name = shortName(playlist[trackIdx]);
        if ((int)name.length() > maxNameChars) name = name.substring(0, maxNameChars);
        gfx->print(name);
    }
}

static void drawControls() {
    gfx->fillRect(0, CTRL_Y, DISP_W, DISP_H - CTRL_Y, C_DARK);
    gfx->drawFastHLine(0, CTRL_Y, DISP_W, C_GREEN);
    gfx->setTextColor(C_GREY); gfx->setTextSize(1);
    gfx->setCursor(5, CTRL_Y + 8);
    gfx->print("|< BALL-L   SPC PAUSE   BALL-R >|   +/- VOL");
}

static void drawFull() {
    gfx->fillScreen(C_BLACK);
    drawHeader(); drawTrackInfo(); drawPlaylist(); drawControls();
}

static void playTrack(int idx) {
    if (!audio || !playlist) return;
    if (idx < 0) idx = trackCount - 1;
    if (idx >= trackCount) idx = 0;
    currentTrack = idx;
    audio->stopSong();
    isPlaying = false; isPaused = false;
    if (trackCount == 0) return;
    if (audio->connecttoFS(SD, playlist[currentTrack])) {
        isPlaying = true; isPaused = false; trackStartMs = millis();
    }
    drawTrackInfo(); drawPlaylist();
}

static void togglePause() { if (!isPlaying || !audio) return; isPaused = !isPaused; audio->pauseResume(); drawTrackInfo(); }
static void adjustVolume(int delta) { volume = constrain(volume + delta, 0, 21); if (audio) audio->setVolume(volume); drawTrackInfo(); }

static void moveCursor(int dir) {
    int absIdx = listOffset + cursorPos + dir;
    absIdx = constrain(absIdx, 0, trackCount - 1);
    cursorPos = absIdx - listOffset;
    if (cursorPos < 0) { listOffset = max(0, listOffset - 1); cursorPos = 0; }
    else if (cursorPos >= LIST_ROWS) { listOffset = min(trackCount - LIST_ROWS, listOffset + 1); cursorPos = LIST_ROWS - 1; }
    drawPlaylist();
}

void run_audio_player() {
    if (!ensureAudioState()) {
        gfx->fillScreen(C_BLACK);
        gfx->setTextSize(1);
        gfx->setTextColor(C_RED);
        gfx->setCursor(10, INFO_Y);
        gfx->print("Audio memory unavailable.");
        delay(1200);
        return;
    }
    audio->setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio->setVolume(volume);
    scanMusicFolder();
    drawFull();
    if (trackCount > 0) playTrack(0);
    bool running = true;
    unsigned long lastInfoUpdate = millis();
    while (running) {
        if (audio) audio->loop();
        updateViz();
        if (millis() - lastInfoUpdate > 1000) { drawTrackInfo(); lastInfoUpdate = millis(); }
        char k = get_keypress();
        if      (pm_is_exit_key(k))      running = false;
        else if (k == ' ')                  togglePause();
        else if (k == '+' || k == '=')      adjustVolume(1);
        else if (k == '-')                  adjustVolume(-1);
        else if (k == 'n' || k == 'N')      playTrack(currentTrack + 1);
        else if (k == 'p' || k == 'P')      playTrack(currentTrack - 1);
        TrackballState tb = update_trackball();
        if      (tb.y == -1) moveCursor(-1);
        else if (tb.y ==  1) moveCursor( 1);
        else if (tb.x == -1) { playTrack(currentTrack - 1); drawTrackInfo(); }
        else if (tb.x ==  1) { playTrack(currentTrack + 1); drawTrackInfo(); }
        else if (tb.clicked) {
            int selected = listOffset + cursorPos;
            if (selected < trackCount) playTrack(selected);
        }
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < HEADER_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            running = false;
        }
        yield(); delay(5);
    }
    freeAudioState();
}

void audio_eof_mp3(const char* info) {
    Serial.printf("[AUDIO] EOF: %s\n", info);
    if (!audio) return;
    isPlaying = false;
    int next = currentTrack + 1;
    if (next < trackCount) playTrack(next); else currentTrack = 0;
}
void audio_info(const char* info) { Serial.printf("[AUDIO] %s\n", info); }
