// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  donkey_kong.h — Pisces Moon original arcade-style climbing game
//
//  Single-screen climbing/avoidance arcade game in the spirit of
//  Nintendo's 1981 Donkey Kong arcade cabinet. Entirely original
//  code — our own engine, collision model, level data, and tuning.
//  Built fresh on pm_platformer.h (POD entities, fixed timestep,
//  strict SIM/DRAW boundary).
//
//  GENRE / BOARD (the 25m board):
//    - Fixed single-screen. No scrolling.
//    - A stack of SLOPED girders connected by ladders. DK sits top-
//      left and hurls barrels; Pauline is above him. An oil drum
//      bottom-left emits fireballs. Jumpman starts bottom-right and
//      must climb to the top.
//    - GIRDERS ARE HEIGHT FUNCTIONS y=f(x), not AABBs. The floor
//      height at any x is a linear interpolation of the girder's two
//      endpoints (see girder_y_at). AABB is used only for actor-vs-
//      actor and ladder trigger zones.
//    - Ladders are FULL (Jumpman climbs) or BROKEN (Jumpman cannot;
//      barrels may still drop down them).
//
//  PHYSICS (the 1981 feel — see the FSM transition comments):
//    - RIGID committed jump: once airborne, horizontal input is
//      IGNORED. No air control. (The deliberate opposite of Mario.)
//    - Strict terminal velocity.
//    - Fall damage: landing after a fall taller than 1.5 x PLAYER_H
//      is fatal — standard girder-to-girder descent is safe, but
//      missing the intended girder and dropping ~two levels kills.
//
//  CROSS-DEVICE GEOMETRY:
//    The board is defined ONCE in board-units (arcade-portrait
//    proportions) as BOARD_GIRDERS[] / BOARD_LADDERS[] (in the .cpp)
//    and projected to each device's pixels at launch by project_board()
//    using a uniform scale + centering (letterbox). Uniform scale
//    preserves slope angles on every screen. Portrait kiosks (C28P,
//    Maxine) nearly fill; landscape devices (T-Deck, Pager, Cardputer)
//    render a centered portrait column with HUD in the side margins —
//    authentic to DK's vertical CRT. Endpoints are projected once;
//    per-frame collision lerps already-projected pixels.
//
//  DEVICES: all five. Per-device VIEW_W/VIEW_H/HUD_H mirror the
//  Mario Bros pattern; the canonical board projects into that rect.
//
//  PERSISTENCE: high score to /donkey_kong_hs.txt via SdFat (T-Deck,
//  Pager, Cardputer, Maxine) or SD_MMC (C28P), under the same SPI
//  Bus Treaty as the rest of the suite.
// ─────────────────────────────────────────────

#ifndef DONKEY_KONG_H
#define DONKEY_KONG_H

#include "pm_platformer.h"
#include "game_input.h"

// Public entry — added to every launcher (c28p_boot.cpp arcade list,
// launcher.cpp T-Deck/Pager GAMES, launcher_cardputer.cpp, maxine_boot.cpp).
void run_donkey_kong();

// ─────────────────────────────────────────────
//  Internal types — only the .cpp (which #defines DK_INTERNAL before
//  including this header) sees these. Keeps the public surface to the
//  single run_donkey_kong() entry while still letting the structures
//  be reviewed/tested in isolation.
// ─────────────────────────────────────────────
#ifdef DK_INTERNAL
namespace dk {

// Board topology counts (identical on every device — only pixel scale
// changes per device). Tuned values for the 25m board live in the .cpp.
static constexpr int N_GIRDERS   = 6;
static constexpr int N_LADDERS   = 9;
static constexpr int MAX_BARRELS = 8;
static constexpr int MAX_FIREBALLS = 3;

// ── Jumpman FSM ──
enum JumpmanState : uint8_t {
    JM_IDLE = 0,
    JM_RUNNING,
    JM_JUMPING,    // RIGID: update_jumpman ignores left/right while here
    JM_FALLING,    // airborne + descending (walked off an edge, or past jump apex)
    JM_CLIMBING,
    JM_HAMMERING,  // invincible to barrels; CANNOT jump
    JM_DYING,
};

struct Jumpman {
    float x, y, vx, vy;
    int   facing;          // -1 / +1
    JumpmanState state;
    float fall_origin_y;   // y where the current fall began. On landing,
                           // (land_y - fall_origin_y) > FALL_DAMAGE_LIMIT
                           // (= 1.5 * PLAYER_H) is fatal.
    float hammer_timer;    // >0 while HAMMERING
    int   girder;          // current girder index, -1 if airborne
    int   ladder;          // ladder index while CLIMBING, else -1
};

// ─────────────────────────────────────────────
//  Level geometry
//
//  *Def structs are the CANONICAL board in board-units (one source
//  of truth, in the .cpp). The plain structs are the per-device
//  PROJECTED runtime copies (pixel space) filled by project_board().
// ─────────────────────────────────────────────
enum LadderType : uint8_t { LADDER_FULL, LADDER_BROKEN };

struct GirderDef { float xl, yl, xr, yr; };          // board-unit endpoints
struct LadderDef { float x, yt, yb; uint8_t type; }; // board-unit

struct Girder {                 // projected (pixels)
    float x_left, x_right;      // horizontal span
    float y_left, y_right;      // endpoint heights — surface is the lerp between
};
struct Ladder {                 // projected (pixels)
    float x;                    // climb snaps Jumpman's x to this
    float y_top, y_bottom;
    LadderType type;            // BROKEN: Jumpman can't climb; barrels may drop
};

// ── Barrels ──
enum BarrelState : uint8_t {
    BAR_SPAWNED,   // just thrown by DK, settling onto the top girder
    BAR_ROLLING,   // following girder slope (speed modulated by slope sign)
    BAR_FALLING,   // dropping off a girder edge or down a ladder
};
struct Barrel {
    float x, y, vx, vy;
    BarrelState state;
    int   girder;          // girder being rolled, -1 if falling
    bool  active;
    // At a ladder TOP, update_barrel rolls an RNG check to decide
    // continue-rolling vs drop-down-ladder (see DROP_CHANCE in .cpp).
};

// ── Fireballs (from the oil drum) ──
struct Fireball {
    float x, y, vx, vy;
    int   girder, ladder;
    bool  active;
    bool  climbing;
    // update_fireball biases velocity toward Jumpman's x/y, constrained
    // to girders + ladders — simple tracking, not full pathfinding.
};

// ─────────────────────────────────────────────
//  GameState — DK's own encapsulated state (no shared parent).
//  girders[]/ladders[] are the projected per-device layout; they're
//  filled once by project_board() and treated as read-only by the
//  sim, so they live in file scope rather than inside GameState.
// ─────────────────────────────────────────────
struct GameState {
    int   level, lives;
    long  score, high_score;
    Jumpman  player;
    Barrel   barrels[MAX_BARRELS];
    Fireball fireballs[MAX_FIREBALLS];
    float barrel_spawn_timer;
    float bonus;               // countdown timer / bonus points
    float death_timer;         // >0 while player is in JM_DYING (death anim)
    bool  quit;
};

// ─────────────────────────────────────────────
//  Geometry — projection + on-the-fly surface height
// ─────────────────────────────────────────────

// Project the canonical BOARD_* tables into the device's pixel space
// (uniform scale + centering). Called once at launch with the device's
// view rect. Fills the file-scope projected girders[]/ladders[] arrays.
void project_board(int view_w, int view_h, int hud_h);

// Floor height of a girder at world-x: linear lerp of its projected
// endpoints, clamped to the span. This is the "interpolate the floor's
// Y on the fly" operation — two muls, no AABB.
inline float girder_y_at(const Girder& g, float x) {
    if (g.x_right == g.x_left) return g.y_left;          // vertical guard
    float t = pmp::clampf((x - g.x_left) / (g.x_right - g.x_left), 0.0f, 1.0f);
    return pmp::lerpf(g.y_left, g.y_right, t);
}

// Resolve which girder an entity is standing on / would land on.
// Girders zig-zag, so x-spans overlap; this returns the topmost girder
// whose surface at center_x is within `tol` of feet_y (or -1).
int girder_under(float center_x, float feet_y, float tol);

// ─────────────────────────────────────────────
//  FSM transitions — the DK physics rules made explicit
// ─────────────────────────────────────────────
void to_idle    (Jumpman&);
void to_running (Jumpman&, int dir);
void to_jumping (Jumpman&);                 // RIGID: locks the launch; while
                                            // JM_JUMPING, update_jumpman does
                                            // not read left/right (no air control)
void to_climbing(Jumpman&, const Ladder&);  // snaps x = ladder.x
void to_hammering(Jumpman&);                // sets hammer_timer; jump disabled
void to_falling (Jumpman&);                 // records fall_origin_y
void to_dying   (GameState&);

// ─────────────────────────────────────────────
//  SIM — no gfx. Strict terminal velocity in update_jumpman.
// ─────────────────────────────────────────────
void update_jumpman (GameState&, const PMNesInput&, float dt);
void update_barrel  (GameState&, int slot, float dt);   // incl. RNG ladder drop
void update_fireball(GameState&, int slot, float dt);   // incl. tracking
void step_world     (GameState&, const PMNesInput&, float dt);  // one FIXED_DT

// ─────────────────────────────────────────────
//  DRAW — reads state only.
// ─────────────────────────────────────────────
void render(const GameState&);

}  // namespace dk
#endif  // DK_INTERNAL

#endif  // DONKEY_KONG_H
