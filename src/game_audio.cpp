// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ─────────────────────────────────────────────
//  game_audio.cpp — chiptune music + SFX for games
//
//  Songs are stored as two-voice sequences: a melody note and a bass
//  note play simultaneously for each event. On the C28P this drives
//  c28p_audio_tone2() (a real square-wave mixer), giving genuine
//  melody+bass polyphony. On the Cardputer (M5 speaker, monophonic)
//  only the melody voice is played. Other devices are silent for now.
//
//  Timing is non-blocking: pm_game_audio_tick() is called from each
//  game's main loop and advances the song based on millis(). The note
//  is sounded for ~85% of its slot (staccato gap) while the schedule
//  advances by the full slot.
// ─────────────────────────────────────────────

#include "game_audio.h"

#ifdef DEVICE_CARDPUTER_ADV
#include <M5Cardputer.h>
#endif

#ifdef DEVICE_C28P
// C28P audio is implemented in c28p_audio.cpp — ES8311 codec init
// over I2C and I2S tone playback via DMA. Forward-declared here so
// game_audio.cpp can call them without the driver's internal headers.
extern "C" {
    bool c28p_audio_begin();
    void c28p_audio_stop();
    void c28p_audio_tone(uint16_t freq, uint16_t ms);
    void c28p_audio_tone2(uint16_t f1, uint16_t f2, uint16_t ms);
    // Non-blocking music engine (v1.3.1) — c28p_note_t layout matches
    // PMVoice exactly (three uint16_t), so we cast the song arrays.
    typedef struct {
        uint16_t mel;
        uint16_t bass;
        uint16_t ms;
    } c28p_note_t;
    bool c28p_audio_music_start(void);
    void c28p_audio_music_stop(void);
    void c28p_audio_music_set_song(const c28p_note_t* notes, uint16_t count);
    void c28p_audio_music_fx(uint16_t freq, uint16_t ms);
    bool c28p_audio_music_is_running(void);
}
#endif

// ── Two-voice event: melody + bass, both sounding for `ms`. ──
struct PMVoice {
    uint16_t mel;    // melody frequency (0 = rest)
    uint16_t bass;   // bass frequency (0 = no bass on this beat)
    uint16_t ms;     // slot duration in ms
};

// ── Note table ──
#define R   0
// melody octaves 4–5 (+ a couple of 6)
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
#define GS5 831
#define A5  880
#define AS5 932
#define B5  988
#define C6  1047
#define D6  1175
#define E6  1319
// bass octaves 2–3
#define BE2 82
#define BG2 98
#define BA2 110
#define BB2 123
#define BC3 131
#define BD3 147
#define BE3 165
#define BF3 175
#define BFS3 185
#define BG3 196
#define BGS3 208
#define BA3 220
#define BB3 247

// ─────────────────────────────────────────────
//  KOROBEINIKI — Tetris theme, single melody line (two halves).
//
//  The original pre-polyphonic transcription, restored verbatim —
//  this is the version that sounded right before the two-voice work.
//  ONE voice: the bass field of every slot is 0 so exactly one note
//  sounds at a time (the C28P speaker turns summed square waves into
//  intermodulation mush — a single clean line is what it renders).
//
//  Two halves, then it loops: the high A-phrase, then the answer
//  that climbs D–F–A and settles back. Durations are the known-good
//  values (eighth 80, quarter 160, dotted/half 220 ms).
// ─────────────────────────────────────────────
static const PMVoice KOROBEINIKI[] = {
    // ── First half ──
    {E5,0,160},{B4,0,80},{C5,0,80},{D5,0,160},{C5,0,80},{B4,0,80},
    {A4,0,160},{A4,0,80},{C5,0,80},{E5,0,160},{D5,0,80},{C5,0,80},
    {B4,0,220},{C5,0,80},{D5,0,160},{E5,0,160},{C5,0,160},{A4,0,160},
    {A4,0,220},{R,0,80},
    // ── Second half ──
    {D5,0,160},{F5,0,80},{A5,0,160},{G5,0,80},{F5,0,80},
    {E5,0,220},{C5,0,80},{E5,0,160},{D5,0,80},{C5,0,80},
    {B4,0,160},{B4,0,80},{C5,0,80},{D5,0,160},{E5,0,160},
    {C5,0,160},{A4,0,160},{A4,0,220},{R,0,160},
};

static PMGameSong currentSong = PM_SONG_NONE;
static const PMVoice *songNotes = nullptr;
static uint16_t songLen = 0;
static uint16_t songIndex = 0;
static uint32_t nextNoteAt = 0;
static bool audioReady = false;

// ── Two-voice playback (melody + bass). ──
//
// On C28P, songs run through the non-blocking music engine: pm_game_
// audio_start() hands the entire song array to c28p_audio_music_set_
// song(), then the engine task on Core 0 sequences and synthesizes
// polyphony autonomously while the game runs on Core 1. playMix() is
// only used by FX paths (kick/POW/etc.) — those re-route to the
// engine's FX overlay when the engine is running so we don't race for
// the I2S driver, and fall back to the blocking tone2 when it isn't.
static void playMix(uint16_t mel, uint16_t bass, uint16_t ms) {
#ifdef DEVICE_CARDPUTER_ADV
    if (!audioReady || mel == 0) return;
    M5Cardputer.Speaker.tone(mel, ms);   // monophonic: melody only
    (void)bass;
#elif defined(DEVICE_C28P)
    if (!audioReady) return;
    if (c28p_audio_music_is_running()) {
        // Engine owns I2S — use its FX overlay (melody voice only;
        // bass would need a second overlay channel, not worth it for
        // 70-130ms one-shot FX).
        if (mel > 0) c28p_audio_music_fx(mel, ms);
    } else {
        c28p_audio_tone2(mel, bass, ms);
    }
#else
    (void)mel; (void)bass; (void)ms;
#endif
}

// ── Single-voice FX (drops, line clears). ──
static void playTone(uint16_t freq, uint16_t ms) {
#ifdef DEVICE_CARDPUTER_ADV
    if (!audioReady || freq == 0) return;
    M5Cardputer.Speaker.tone(freq, ms);
#elif defined(DEVICE_C28P)
    if (!audioReady) return;
    if (c28p_audio_music_is_running()) {
        c28p_audio_music_fx(freq, ms);
    } else {
        c28p_audio_tone(freq, ms);
    }
#else
    (void)freq; (void)ms;
#endif
}

void pm_game_audio_begin() {
#ifdef DEVICE_CARDPUTER_ADV
    audioReady = true;
#elif defined(DEVICE_C28P)
    // Start the non-blocking music engine on Core 0. From here on,
    // every song change is just a c28p_audio_music_set_song() call —
    // no blocking, no per-note ticks, the engine sequences everything.
    audioReady = c28p_audio_music_start();
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
#elif defined(DEVICE_C28P)
    if (audioReady) {
        c28p_audio_music_set_song(nullptr, 0);   // silence
        c28p_audio_music_stop();                  // kill task + I2S
    }
    audioReady = false;
#endif
}

void pm_game_audio_start(PMGameSong song) {
    currentSong = song;
    songIndex = 0;
    nextNoteAt = 0;
    switch (song) {
        case PM_SONG_KOROBEINIKI:
            songNotes = KOROBEINIKI;
            songLen = sizeof(KOROBEINIKI) / sizeof(KOROBEINIKI[0]);
            break;
        default:
            songNotes = nullptr;
            songLen = 0;
            break;
    }

#ifdef DEVICE_C28P
    // Hand the song to the engine. PMVoice and c28p_note_t share the
    // same {uint16_t mel, uint16_t bass, uint16_t ms} layout so we
    // can cast directly. The engine picks up the change at its next
    // note boundary (≤ 4ms).
    if (audioReady) {
        c28p_audio_music_set_song(
            reinterpret_cast<const c28p_note_t*>(songNotes),
            songLen);
    }
#endif
}

void pm_game_audio_tick() {
#ifdef DEVICE_C28P
    // No-op on C28P — the engine sequences autonomously on Core 0.
    // We keep the function as a no-op for API compatibility so games
    // don't need device-specific call-site changes.
    return;
#else
    if (!songNotes || songLen == 0) return;
    uint32_t now = millis();
    if (now < nextNoteAt) return;

    const PMVoice &n = songNotes[songIndex++];
    if (songIndex >= songLen) songIndex = 0;
    // Cardputer + others: blocking single-voice melody only. Bass voice
    // is ignored on monophonic devices.
    playMix(n.mel, n.bass, (uint16_t)(n.ms * 85 / 100));
    nextNoteAt = now + n.ms;
#endif
}

void pm_game_audio_fx_drop() {
    playTone(196, 45);
}

void pm_game_audio_fx_line() {
    playTone(880, 60);
}

// ── Arcade SFX (shared) ──
// Short blips designed to read as classic arcade cues. They play
// through the same square-wave engine as the music; each is brief
// enough to drop into a game loop without a perceptible stall.
void pm_game_audio_fx_jump() {
    playTone(523, 38);                 // quick upward blip
}

void pm_game_audio_fx_flip() {
    playTone(330, 55);                 // mid "thunk" when an enemy flips
}

void pm_game_audio_fx_kick() {
    playMix(784, 392, 70);             // bright two-tone hit on the kick
}

void pm_game_audio_fx_pow() {
    playMix(110, 73, 130);             // low rumble for the POW block
}

void pm_game_audio_fx_die() {
    // Descending two-step "lost a life" sting.
    playTone(196, 90);
    playTone(147, 130);
}

void pm_game_audio_fx_phase() {
    // Rising two-step "phase clear" jingle.
    playTone(659, 80);
    playTone(988, 130);
}

// ── Pac-Man / Galaga vocabulary ──
void pm_game_audio_fx_dot() {
    // Iconic Pac-Man waka: alternate two pitches every call so a run
    // of dots reads as the classic up/down chomp. Kept very brief so
    // the per-dot blocking doesn't stutter Pac-Man at full speed.
    static bool toggle = false;
    toggle = !toggle;
    playTone(toggle ? 622 : 466, 18);
}

void pm_game_audio_fx_eat() {
    // Power-pellet rumble (lower, longer than a dot).
    playTone(220, 90);
}

void pm_game_audio_fx_laser() {
    // Quick high "pew" for Galaga player shots.
    playTone(1175, 22);
}

void pm_game_audio_fx_explode() {
    // Descending two-step hit for an enemy/ship explosion.
    playTone(196, 40);
    playTone(110, 60);
}

const char* pm_game_audio_song_name() {
    switch (currentSong) {
        case PM_SONG_KOROBEINIKI: return "KOR";
        default:                  return "OFF";
    }
}
