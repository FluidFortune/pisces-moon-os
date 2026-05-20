// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef GAME_INPUT_H
#define GAME_INPUT_H

#include <Arduino.h>
#include "trackball.h"

struct PMNesInput {
    bool left;
    bool right;
    bool up;
    bool down;
    bool a;
    bool b;
    bool start;
    bool select;
    bool quit;
    bool home;
    char key;
    TrackballState trackball;
};

bool pm_is_nes_quit_key(char key);
PMNesInput pm_read_nes_input(bool includeTrackball = true);

#endif
