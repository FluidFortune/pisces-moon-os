// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "game_input.h"
#include "gamepad.h"
#include "keyboard.h"
#include "pm_input.h"

static bool keyIs(char key, char lower) {
    return key == lower || key == (char)(lower - 'a' + 'A');
}

static bool keyIsNesUp(char key) {
#ifdef DEVICE_CARDPUTER_ADV
    // Cardputer's W key sits directly above A; E gives a cleaner
    // diamond for A/E/D/Z on the small keyboard.
    return keyIs(key, 'e');
#else
    return keyIs(key, 'w');
#endif
}

bool pm_is_nes_quit_key(char key) {
    return key == 27 || keyIs(key, 'q');
}

PMNesInput pm_read_nes_input(bool includeTrackball) {
    PMNesInput input = {};

    input.key = get_keypress();
    input.trackball = includeTrackball ? update_trackball_game() : TrackballState{0, 0, false};

    input.home = gamepad_poll();
    input.quit = input.home || pm_is_nes_quit_key(input.key);

    // Built-in keyboard NES layout:
    // A=left, W=up (Cardputer: E=up), D=right, Z=down, V=select, B=start,
    // K=B button, O=A button. S intentionally does nothing.
    input.left   = keyIs(input.key, 'a') || input.key == PM_KEY_LEFT  || gamepad_held(GP_LEFT);
    input.right  = keyIs(input.key, 'd') || input.key == PM_KEY_RIGHT || gamepad_held(GP_RIGHT);
    input.up     = keyIsNesUp(input.key) || input.key == PM_KEY_UP    || gamepad_held(GP_UP);
    input.down   = keyIs(input.key, 'z') || input.key == PM_KEY_DOWN  || gamepad_held(GP_DOWN);
    input.select = keyIs(input.key, 'v') || gamepad_pressed(GP_SELECT);
    input.start  = keyIs(input.key, 'b') || gamepad_pressed(GP_START);
    input.b      = keyIs(input.key, 'k') || gamepad_pressed(GP_B);
    input.a      = keyIs(input.key, 'o') || gamepad_pressed(GP_A);

    if (includeTrackball) {
        input.left  = input.left  || (input.trackball.x == -1);
        input.right = input.right || (input.trackball.x == 1);
        input.up    = input.up    || (input.trackball.y == -1);
        input.down  = input.down  || (input.trackball.y == 1);
        input.a     = input.a     || input.trackball.clicked;
    }

    return input;
}
