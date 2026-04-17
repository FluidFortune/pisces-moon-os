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

#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

// WinAmp-style audio player.
// Scans /music/ on SD card for MP3, FLAC, AAC, OGG files.
// I2S output: BCLK=GPIO7, LRC=GPIO5, DOUT=GPIO6
void run_audio_player();

#endif