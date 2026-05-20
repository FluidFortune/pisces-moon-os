#ifndef GAME_AUDIO_H
#define GAME_AUDIO_H

#include <Arduino.h>

enum PMGameSong : uint8_t {
    PM_SONG_NONE = 0,
    PM_SONG_KOROBEINIKI,
    PM_SONG_SUGAR_PLUM,
};

void pm_game_audio_begin();
void pm_game_audio_stop();
void pm_game_audio_start(PMGameSong song);
void pm_game_audio_tick();
void pm_game_audio_fx_drop();
void pm_game_audio_fx_line();
const char* pm_game_audio_song_name();

#endif
