// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_audio.h — C28P audio HAL (idle / playback / record)
//
//  The C28P has one ES8311 codec on a single I2S serial port. ADC and
//  DAC share clocks (MCLK/BCLK/LRCK), so the codec, the ESP32-S3 I2S
//  driver, and the FM8002E amplifier all need to be in a coherent state
//  at any moment — you can't run TX and RX simultaneously without a
//  driver reinstall, and you don't want the amp enabled during record
//  (feedback).
//
//  v1.2.x called c28p_audio_begin() / c28p_audio_stop() from games and
//  the mic probe, but a static "i2s_initialized" flag inside the file
//  meant the mic probe could leave the driver in a state nobody could
//  recover from without a reboot. v1.3 replaces that with an explicit
//  state machine.
//
//  MODES:
//    IDLE            no I2S driver installed, codec quiet, amp off
//    PLAYBACK_TONE   I2S TX driver owned by this HAL, codec DAC on,
//                    amp on. Game audio (c28p_audio_tone) plays here.
//    PLAYBACK_AUDIO  codec set up for DAC output and amp on, but the
//                    I2S driver is installed and owned by the caller
//                    (e.g. the ESP32-audioI2S Audio library streaming
//                    an MP3 from SD). Use c28p_audio_release_driver()
//                    to tell the HAL we've removed any driver of our
//                    own before the caller installs theirs.
//    RECORD          I2S RX driver owned by this HAL, codec ADC on,
//                    amp off. Caller polls i2s_read() on I2S_NUM_0.
//
//  TRANSITIONS:
//    Any mode → IDLE is always safe (no-op if already idle).
//    IDLE → any mode performs the full bring-up for that mode.
//    Non-idle → non-idle: HAL tears down current mode (release driver,
//    quiet codec, disable amp) before installing the new mode. So you
//    can call c28p_audio_enter(RECORD) directly after PLAYBACK_TONE —
//    the HAL handles the teardown internally.
//
//  THREAD SAFETY:
//    Not reentrant. All calls assumed to come from the same task
//    context (main app loop). The mic probe / game audio / Audio
//    library never run concurrently in practice.
//
//  BACKWARDS COMPAT:
//    c28p_audio_begin() / c28p_audio_stop() / c28p_audio_tone() are
//    preserved as thin wrappers that route through the mode machine,
//    so any existing call sites work without changes. New code should
//    prefer c28p_audio_enter()/c28p_audio_release() for clarity.
// ─────────────────────────────────────────────

#pragma once

#ifdef DEVICE_C28P

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    C28P_AUDIO_IDLE           = 0,
    C28P_AUDIO_PLAYBACK_TONE  = 1,
    C28P_AUDIO_PLAYBACK_AUDIO = 2,
    C28P_AUDIO_RECORD         = 3,
} c28p_audio_mode_t;

#ifdef __cplusplus
extern "C" {
#endif

// ── New explicit-mode API ──

// Transition into `mode`. Safe to call from any other mode (the HAL
// tears down the current state first). Returns true on success;
// false if the codec failed to initialise (I2C bus problem / hardware
// absent) — in which case the HAL ends up in IDLE.
bool c28p_audio_enter(c28p_audio_mode_t mode);

// Returns the current mode. Useful for debug / diagnostics.
c28p_audio_mode_t c28p_audio_current_mode(void);

// Drop straight to IDLE. Always safe to call. After this returns,
// no I2S driver is installed and the amp is disabled.
void c28p_audio_release(void);

// Tell the HAL that the caller has just uninstalled the I2S driver
// they were using (e.g. the ESP32-audioI2S Audio library calling
// stopSong()). The HAL forgets it ever installed a driver itself
// and moves to IDLE without trying to uninstall again. Use ONLY
// after PLAYBACK_AUDIO ends and the caller has done their own
// i2s_driver_uninstall(I2S_NUM_0).
void c28p_audio_release_driver(void);

// ── Backwards-compat wrappers ──
// These continue to work for v1.2.x callers. Implementations route
// through c28p_audio_enter() so the mode state stays consistent.

bool c28p_audio_begin(void);        // unconditionally enters PLAYBACK_TONE
void c28p_audio_stop(void);          // → IDLE
void c28p_audio_tone(uint16_t freq, uint16_t ms);  // requires PLAYBACK_TONE

// Two-voice (polyphonic) tone — mixes two square waves so games can
// play a melody + bass line. f1/f2 of 0 mean "this voice silent".
// Requires PLAYBACK_TONE mode, same as c28p_audio_tone().
void c28p_audio_tone2(uint16_t f1, uint16_t f2, uint16_t ms);

// ─────────────────────────────────────────────
//  NON-BLOCKING MUSIC ENGINE (v1.3.1)
//
//  c28p_audio_tone / tone2 block the calling thread for the full
//  tone duration (i2s_write with portMAX_DELAY). Fine for short FX,
//  fatal for songs — a 430ms quarter note freezes the game loop.
//
//  The music engine instead runs a FreeRTOS task on Core 0 that owns
//  I2S TX and continuously synthesizes polyphonic square-wave samples
//  from a song descriptor. The game on Core 1 calls non-blocking
//  setters and never waits on audio. Tones play their full duration,
//  polyphony intact.
//
//  Memory: ~4 KB task stack, ~64 bytes shared state.
//  CPU: pegged near 100% of one core (square-wave gen + I2S writes),
//       but it's Core 0 — game runs unimpeded on Core 1.
// ───────────────────────────────────────────────

typedef struct {
    uint16_t mel;    // melody frequency in Hz (0 = voice silent)
    uint16_t bass;   // bass frequency in Hz (0 = voice silent)
    uint16_t ms;     // note duration in milliseconds
} c28p_note_t;

// Start the music engine. Creates a task on Core 0, installs I2S TX,
// enables the amp. After this returns, the engine is running and
// outputting silence (until a song is set). Idempotent.
bool c28p_audio_music_start(void);

// Stop the music engine cleanly. Kills the task, releases I2S, mutes
// the amp. Idempotent.
void c28p_audio_music_stop(void);

// Set the active song. The engine plays it on a continuous loop.
// Pass (NULL, 0) to silence the engine without stopping it.
// Non-blocking — the task picks up the change at the next note
// boundary (≤ 4 ms).
void c28p_audio_music_set_song(const c28p_note_t* notes, uint16_t count);

// Queue a one-shot FX tone to play OVER the music. The engine mixes
// it in as a third voice for the requested duration. Non-blocking.
// Pass freq=0 to cancel any pending FX.
void c28p_audio_music_fx(uint16_t freq, uint16_t ms);

// True if the engine task is currently running.
bool c28p_audio_music_is_running(void);

#ifdef __cplusplus
}
#endif

#endif  // DEVICE_C28P
