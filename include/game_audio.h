#ifndef GAME_AUDIO_H
#define GAME_AUDIO_H

#include <Arduino.h>

enum PMGameSong : uint8_t {
    PM_SONG_NONE = 0,
    PM_SONG_KOROBEINIKI,   // Tetris theme (Korobeiniki) — single melody line
};

void pm_game_audio_begin();
void pm_game_audio_stop();
void pm_game_audio_start(PMGameSong song);
void pm_game_audio_tick();
void pm_game_audio_fx_drop();
void pm_game_audio_fx_line();
// Arcade SFX shared across games (Mario flip/kick/POW/death, etc.).
// All are short, single- or two-voice blips played through the same
// tone engine as the songs; safe to call from any game's main loop.
void pm_game_audio_fx_jump();
void pm_game_audio_fx_flip();
void pm_game_audio_fx_kick();
void pm_game_audio_fx_pow();
void pm_game_audio_fx_die();
void pm_game_audio_fx_phase();
// Pac-Man / Galaga arcade vocabulary.
void pm_game_audio_fx_dot();      // Pac-Man waka (alternates pitch each call)
void pm_game_audio_fx_eat();      // power-pellet / energizer rumble
void pm_game_audio_fx_laser();    // Galaga player shot
void pm_game_audio_fx_explode();  // enemy/ship explosion
const char* pm_game_audio_song_name();

#endif
