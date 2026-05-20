// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "game_audio.h"

#ifdef DEVICE_CARDPUTER_ADV
#include <M5Cardputer.h>
#endif

struct PMNote {
    uint16_t freq;
    uint16_t ms;
};

#define R   0
#define C4  262
#define CS4 277
#define D4  294
#define DS4 311
#define E4  330
#define F4  349
#define FS4 370
#define G4  392
#define GS4 415
#define A4  440
#define AS4 466
#define B4  494
#define C5  523
#define CS5 554
#define D5  587
#define DS5 622
#define E5  659
#define F5  698
#define FS5 740
#define G5  784
#define A5  880
#define B5  988

static const PMNote KOROBEINIKI[] = {
    {E5,160},{B4,80},{C5,80},{D5,160},{C5,80},{B4,80},
    {A4,160},{A4,80},{C5,80},{E5,160},{D5,80},{C5,80},
    {B4,220},{C5,80},{D5,160},{E5,160},{C5,160},{A4,160},
    {A4,220},{R,80},
    {D5,160},{F5,80},{A5,160},{G5,80},{F5,80},
    {E5,220},{C5,80},{E5,160},{D5,80},{C5,80},
    {B4,160},{B4,80},{C5,80},{D5,160},{E5,160},
    {C5,160},{A4,160},{A4,220},{R,160},
};

static const PMNote SUGAR_PLUM[] = {
    {E5,130},{DS5,130},{E5,130},{DS5,130},{E5,130},{B4,130},{D5,130},{C5,130},
    {A4,220},{R,80},{C4,130},{E4,130},{A4,130},{B4,220},{R,80},
    {E4,130},{GS4,130},{B4,130},{C5,220},{R,80},
    {E4,130},{E5,130},{DS5,130},{E5,130},{DS5,130},{E5,130},
    {B4,130},{D5,130},{C5,130},{A4,260},{R,160},
};

static PMGameSong currentSong = PM_SONG_NONE;
static const PMNote *songNotes = nullptr;
static uint16_t songLen = 0;
static uint16_t songIndex = 0;
static uint32_t nextNoteAt = 0;
static bool audioReady = false;

static void playTone(uint16_t freq, uint16_t ms) {
#ifdef DEVICE_CARDPUTER_ADV
    if (!audioReady || freq == 0) return;
    M5Cardputer.Speaker.tone(freq, ms);
#else
    (void)freq;
    (void)ms;
#endif
}

void pm_game_audio_begin() {
#ifdef DEVICE_CARDPUTER_ADV
    audioReady = true;
#else
    audioReady = false;
#endif
}

void pm_game_audio_stop() {
    currentSong = PM_SONG_NONE;
    songNotes = nullptr;
    songLen = 0;
    songIndex = 0;
#ifdef DEVICE_CARDPUTER_ADV
    if (audioReady) M5Cardputer.Speaker.stop();
#endif
}

void pm_game_audio_start(PMGameSong song) {
    currentSong = song;
    songIndex = 0;
    nextNoteAt = 0;
    if (song == PM_SONG_KOROBEINIKI) {
        songNotes = KOROBEINIKI;
        songLen = sizeof(KOROBEINIKI) / sizeof(KOROBEINIKI[0]);
    } else if (song == PM_SONG_SUGAR_PLUM) {
        songNotes = SUGAR_PLUM;
        songLen = sizeof(SUGAR_PLUM) / sizeof(SUGAR_PLUM[0]);
    } else {
        songNotes = nullptr;
        songLen = 0;
    }
}

void pm_game_audio_tick() {
    if (!songNotes || songLen == 0) return;
    uint32_t now = millis();
    if (now < nextNoteAt) return;

    const PMNote &n = songNotes[songIndex++];
    if (songIndex >= songLen) songIndex = 0;
    if (n.freq) playTone(n.freq, (uint16_t)(n.ms * 85 / 100));
    nextNoteAt = now + n.ms;
}

void pm_game_audio_fx_drop() {
    playTone(196, 45);
}

void pm_game_audio_fx_line() {
    playTone(880, 60);
}

const char* pm_game_audio_song_name() {
    if (currentSong == PM_SONG_KOROBEINIKI) return "KOR";
    if (currentSong == PM_SONG_SUGAR_PLUM) return "PLUM";
    return "OFF";
}
