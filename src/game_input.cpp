// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "game_input.h"
#include "gamepad.h"
#include "keyboard.h"
#include "pm_input.h"
#ifdef DEVICE_C28P
#include "c28p_dpad.h"
#endif
#ifdef DEVICE_MAXINE
#include "maxine_dpad.h"
#endif
#ifdef DEVICE_C5
#include "c5_dpad.h"
#endif
#ifdef DEVICE_TEMBED_CC1101
#include "tembed_input.h"
#endif

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

#if defined(DEVICE_TEMBED_CC1101)
    // ────────────────────────────────────────────────────────
    //  T-EMBED: encoder + two buttons, nothing else.
    //
    //  Kept apart from the keyboard/trackball/gamepad path below on
    //  purpose: the T-Deck trackball pins (1/2/3/15) are IR RX, IR TX,
    //  CC1101 GDO0 and the board power-enable on this device.
    //
    //  Wheel:  each detent queues kFramesPerDetent frames of left/right,
    //          so one click is a short step and a steady turn steers.
    //          Turning the other way cancels the queue at once.
    //  Push:   A (held = autofire, games rate-limit shots themselves).
    //  Side:   tap = START (pause / resume), hold 0.8 s = quit.
    // ────────────────────────────────────────────────────────
    (void)includeTrackball;
    constexpr int kFramesPerDetent = 6;
    constexpr int kMaxQueued       = 18;
    static int      s_move     = 0;
    static bool     s_backWas  = false;
    static bool     s_backLong = false;
    static uint32_t s_backAt   = 0;

    int d = tembed_encoder_delta();
    if (d != 0) {
        if ((d > 0 && s_move < 0) || (d < 0 && s_move > 0)) s_move = 0;
        s_move += d * kFramesPerDetent;
        if (s_move >  kMaxQueued) s_move =  kMaxQueued;
        if (s_move < -kMaxQueued) s_move = -kMaxQueued;
    }
    if (s_move > 0)      { input.right = true; s_move--; }
    else if (s_move < 0) { input.left  = true; s_move++; }

    input.a = tembed_btn_select_down();

    bool back = tembed_btn_back_down();
    uint32_t now = millis();
    if (back && !s_backWas) { s_backAt = now; s_backLong = false; }
    if (back && !s_backLong && now - s_backAt > 800) { s_backLong = true; input.quit = true; }
    if (!back && s_backWas && !s_backLong && now - s_backAt > 25) input.start = true;
    s_backWas = back;
    if (input.quit) s_move = 0;

#elif !defined(DEVICE_MAXINE) && !defined(DEVICE_C5)
    // T-Deck Plus / T-LoRa Pager / Cardputer / C28P input path — reads
    // the physical keyboard, trackball, and BLE gamepad. Maxine and C5
    // are pure-touch kiosks (no keyboard/trackball/gamepad hardware on
    // either, and the C5's src_filter intentionally excludes
    // keyboard.cpp / trackball.cpp / gamepad.cpp), so the whole block
    // is gated out for them. The dpad poll calls below are the only
    // input source on those boards.
    input.key = get_keypress();

    // ────────────────────────────────────────────────────────
    //  CARDPUTER ARROW-PUNCTUATION TRANSLATION
    //
    //  DO NOT REMOVE THIS BLOCK. IT IS NOT REDUNDANT WITH THE
    //  Fn-LAYER HANDLING IN keyboard.cpp.
    //
    //  Background: the Cardputer's keycaps for `; , . /` are
    //  PRINTED WITH ARROW ICONS. The hardware emits the punctuation
    //  character when you press them unshifted, and emits arrow
    //  PM_KEY codes only when Fn is held (matches the keycap
    //  printing — Fn shows the secondary symbols).
    //
    //  Two pieces of code have to know about that:
    //    1. launcher_cardputer.cpp (launcherKeyTranslate)
    //    2. THIS FUNCTION
    //
    //  Without (2), the games receive raw `, . ; /` characters that
    //  match no input mapping and feel broken. Users expect the
    //  keycap-printed arrows to JUST WORK in games the same way
    //  they JUST WORK in the launcher. Holding Fn while playing a
    //  game is unergonomic and not what's printed on the keys.
    //
    //  This translation has been added and removed from the codebase
    //  multiple times because it LOOKS like a duplicate of the Fn
    //  layer logic. It is not. The Fn layer handles `Fn + ,`. This
    //  handles plain `,`. Both are necessary because the keys are
    //  PHYSICALLY THE SAME PHYSICAL KEYS but with different intent
    //  (typing vs. navigating). Apps that need to TYPE punctuation
    //  (notepad, calculator, terminal) call get_keypress() directly
    //  and see the raw character. Games go through this function
    //  and see the arrow.
    //
    //  If you find yourself thinking "this is redundant" or "this
    //  belongs in keyboard.cpp" — stop. It does not. keyboard.cpp
    //  cannot tell whether the caller is a game or a typing app.
    //  game_input.cpp is the right place because by definition it's
    //  only called from games.
    // ────────────────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
    switch (input.key) {
        case ',': input.key = PM_KEY_LEFT;  break;
        case '/': input.key = PM_KEY_RIGHT; break;
        case ';': input.key = PM_KEY_UP;    break;
        case '.': input.key = PM_KEY_DOWN;  break;
        default: break;
    }
#endif

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
#endif // !DEVICE_MAXINE && !DEVICE_C5

#ifdef DEVICE_C28P
    // C28P: OR in virtual D-pad touch state. The dpad layer writes
    // directly into the input struct's direction/A/B fields.
    // Calling poll() also handles selective redraw of the on-screen
    // controller chrome when buttons change state.
    c28p_dpad_poll(&input);
#endif
#ifdef DEVICE_MAXINE
    // Maxine: same virtual D-pad model as the C28P, scaled to 480x800.
    maxine_dpad_poll(&input);
#endif
#ifdef DEVICE_C5
    // C5: same layout as the C28P (240×320 portrait, 200/120 split),
    // single-touch only (XPT2046 resistive). poll() OR-s touch state
    // into the input fields and redraws any buttons whose state changed.
    c5_dpad_poll(&input);
#endif

    return input;
}