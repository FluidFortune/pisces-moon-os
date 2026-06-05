// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_platformer.h — shared platformer core
//
//  Primitives shared by Mario Bros and Donkey Kong. Deliberately
//  small: structs + free functions + explicit FSM enums. NO virtual
//  OOP — the entity arrays (enemies[], barrels[], fireballs[]) stay
//  Plain Old Data so they cost only their fields on the ESP32-S3,
//  with no per-entity vtable and tight cache locality.
//
//  Each game keeps its OWN GameState struct (Mario has phases/POW/
//  flip-enemies; DK has girders/ladders/barrels/fireballs) — there
//  is intentionally no unified parent state. Only these primitives
//  and the renderer-boundary convention are shared.
//
//  RENDERER BOUNDARY (a rule, not a type):
//    Every game .cpp is split into two sections with a hard wall:
//      • SIM   — update_*(GameState&, input, dt): mutates state,
//                ZERO gfx-> calls. Pure logic.
//      • DRAW  — render(const GameState&): reads state, draws. ALL
//                gfx-> calls live here and ONLY here.
//    run_*() owns the loop:
//      poll input → steps_due() → update × N → render.
//    This lets a game's logic be completely blind to the display,
//    so porting between a 320x240 T-Deck and an 800x480 Maxine (or
//    a future device) touches the DRAW section only.
// ─────────────────────────────────────────────

#ifndef PM_PLATFORMER_H
#define PM_PLATFORMER_H

#include <Arduino.h>

namespace pmp {

// ─────────────────────────────────────────────
//  True fixed-timestep accumulator
//
//  Replaces the old clamped-dt loop. The simulation advances in
//  fixed FIXED_DT slices so collision and feel are identical at any
//  render frame rate. MAX_STEPS bounds the "spiral of death": if a
//  frame ran long (SD flush, WiFi stall), we run at most MAX_STEPS
//  sim steps and drop the excess accumulated time rather than trying
//  to catch up forever (which would freeze the game).
//
//  Usage in run_*():
//      pmp::FixedTimestep ts; ts.reset(millis());
//      while (!gs.quit) {
//          poll_input(&in);
//          int n = ts.steps_due(millis());
//          for (int i = 0; i < n; i++) step_world(gs, in, pmp::FixedTimestep::FIXED_DT);
//          render(gs);
//      }
// ─────────────────────────────────────────────
struct FixedTimestep {
    static constexpr float FIXED_DT  = 1.0f / 60.0f;  // 60 Hz simulation
    static constexpr int   MAX_STEPS = 5;             // spiral-of-death clamp

    float    accumulator = 0.0f;
    uint32_t last_ms     = 0;
    bool     started     = false;

    // Initialise/realign the clock (call before the loop, and after
    // any long pause such as returning from a sub-menu).
    void reset(uint32_t now_ms) {
        last_ms = now_ms;
        accumulator = 0.0f;
        started = true;
    }

    // Returns how many FIXED_DT simulation steps to run this frame
    // (0..MAX_STEPS). Advances internal clock by the consumed time.
    int steps_due(uint32_t now_ms) {
        if (!started) { reset(now_ms); return 0; }
        uint32_t elapsed = now_ms - last_ms;
        last_ms = now_ms;
        accumulator += (float)elapsed / 1000.0f;
        int steps = 0;
        while (accumulator >= FIXED_DT && steps < MAX_STEPS) {
            accumulator -= FIXED_DT;
            steps++;
        }
        // If we hit the clamp, discard the backlog so we don't spiral.
        if (steps >= MAX_STEPS && accumulator > FIXED_DT) {
            accumulator = 0.0f;
        }
        return steps;
    }
};

// ─────────────────────────────────────────────
//  Axis-aligned bounding box (actors + trigger zones only — NOT
//  for sloped girders, which are modeled as height functions).
// ─────────────────────────────────────────────
struct AABB { float x, y, w, h; };

inline bool aabb_overlap(const AABB& a, const AABB& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

// ─────────────────────────────────────────────
//  Small math helpers (shared)
// ─────────────────────────────────────────────
inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

// ─────────────────────────────────────────────
//  Player FSM — superset enum; each game uses its own subset.
//    Mario: IDLE RUNNING SKIDDING JUMPING FALLING DYING
//    DK:    IDLE RUNNING JUMPING CLIMBING HAMMERING DYING
// ─────────────────────────────────────────────
enum PlayerState : uint8_t {
    PS_IDLE = 0,
    PS_RUNNING,
    PS_SKIDDING,
    PS_JUMPING,
    PS_FALLING,
    PS_CLIMBING,
    PS_HAMMERING,
    PS_DYING,
};

}  // namespace pmp

#endif  // PM_PLATFORMER_H
