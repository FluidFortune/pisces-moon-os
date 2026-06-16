// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  donkey_kong.cpp — original arcade-style climbing game
//
//  Complete implementation. The 1981-spirit single-screen climbing
//  game: Jumpman starts bottom-right and must reach Pauline at the
//  top while dodging barrels thrown by Donkey Kong, fireballs from
//  the oil drum, and the gaps in broken ladders. Hammers along the
//  way give him a brief window of smash-everything offense.
//
//  WHAT'S HERE (everything):
//    • Sloped-girder collision (height-function y=f(x)) with the
//      arcade-feel zigzag layout.
//    • Jumpman FSM: idle / run / jump (RIGID, no air control) /
//      climb / hammering / fall / dying.
//    • Fall damage tuned to the 32-BU girder gap so one-level drops
//      are safe, two-level drops are fatal.
//    • Barrels: DK throws them on a level-scaled cadence; they roll
//      with the girder slope (zigzag down the stack), RNG-drop down
//      ladders, and either kill Jumpman, are smashed by his hammer
//      (+300), or are jumped over for points (+100, one credit per
//      barrel).
//    • Fireballs from the oil drum patrol girders, occasionally
//      climb ladders, and weakly track Jumpman's x. Hammer-smashable
//      for +500.
//    • Two hammer pickups per level. While hammering, Jumpman is
//      invincible but cannot jump or climb (HAMMER_DURATION = 5s).
//    • Bonus counter (5000 → 0 at 100/s) added to score on level
//      clear. Reaching Pauline ends the level and escalates the
//      next: faster barrel cadence, more fireballs.
//    • DK arm-raise animation each time he throws.
//    • Per-device geometry projection (uniform-scale letterbox) so
//      the feel is identical on every screen and the slope angles
//      stay correct.
//
//  ARCHITECTURE (per pm_platformer.h): strict SIM / DRAW split. The
//  SIM section never calls gfx->. The DRAW section (render + its
//  helpers) only reads GameState. run_donkey_kong() owns the loop.
//
//  GEOMETRY: girders are HEIGHT FUNCTIONS, not AABBs. The board is
//  defined once in board-units (BOARD_GIRDERS/BOARD_LADDERS) and
//  projected to each device's pixels by project_board() with a
//  uniform scale + centering (letterbox). Physics constants are in
//  board-units too and scaled at projection time, so the feel is
//  identical on every screen and the fall-damage threshold tracks
//  the girder gap automatically.
// ─────────────────────────────────────────────

#include <Arduino.h>
#include <math.h>

#define DK_INTERNAL
#include "donkey_kong.h"

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_input.h"
#include "game_audio.h"
#include "theme.h"

#ifdef DEVICE_C28P
#include "c28p_dpad.h"
#include <FS.h>
#include <SD_MMC.h>
#else
#ifdef DEVICE_C5
#include "c5_dpad.h"
#endif
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
extern SdFat sd;
extern SemaphoreHandle_t spi_mutex;
#endif

#ifdef DEVICE_MAXINE
#include "maxine_dpad.h"
#endif

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

namespace dk {

// ─────────────────────────────────────────────
//  PER-DEVICE VIEWPORT
//
//  DK is portrait-native. The canonical board projects into this
//  rect; on landscape devices it becomes a centered portrait column
//  (HUD/margins on the sides — authentic to DK's vertical CRT).
// ─────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
static constexpr int VIEW_W = 240, VIEW_H = 135, HUD_H = 10;
static constexpr int FRAME_DELAY_MS = 33;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 320, VIEW_H = 222, HUD_H = 12;
static constexpr int FRAME_DELAY_MS = 16;
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
static constexpr int VIEW_W = 240, VIEW_H = 200, HUD_H = 14;  // top 200px; dpad below
static constexpr int FRAME_DELAY_MS = 16;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480, VIEW_H = 520, HUD_H = 22;
static constexpr int FRAME_DELAY_MS = 16;
#else  // T-Deck Plus
static constexpr int VIEW_W = 320, VIEW_H = 240, HUD_H = 14;
static constexpr int FRAME_DELAY_MS = 16;
#endif

// ─────────────────────────────────────────────
//  CANONICAL BOARD UNITS + PHYSICS (resolution-independent)
//
//  Girder vertical spacing is a clean 32 board-units. FALL_DMG_BU is
//  48 (= 1.5 x the gap): a one-level drop (32) is safe, a two-level
//  drop (64) is fatal — exactly the arcade rule. Jump height works
//  out to ~19 BU, comfortably under 48, so hopping in place never
//  triggers fall damage. (We map the threshold to the girder gap, NOT
//  to PLAYER_H, because here PLAYER_H != the gap — mapping to height
//  would cause cheap deaths on normal descents.)
// ─────────────────────────────────────────────
static constexpr float BOARD_W = 224.0f;
static constexpr float BOARD_H = 256.0f;

static constexpr float GRAVITY_BU  = 520.0f;   // BU/s²
static constexpr float JUMP_VEL_BU = -140.0f;  // ~19 BU hop
static constexpr float WALK_BU     = 60.0f;    // BU/s
static constexpr float CLIMB_BU    = 46.0f;    // BU/s
static constexpr float MAX_FALL_BU = 280.0f;   // terminal velocity
static constexpr float FALL_DMG_BU = 48.0f;    // 1.5 × 32-BU girder gap
static constexpr float PLAYER_W_BU = 11.0f;
static constexpr float PLAYER_H_BU = 16.0f;
static constexpr float GIRDER_T_BU = 5.0f;

// ── Barrels (board-units)
static constexpr float BARREL_W_BU      = 9.0f;
static constexpr float BARREL_H_BU      = 9.0f;
static constexpr float BARREL_ROLL_BU   = 50.0f;
static constexpr float BARREL_TERM_BU   = 220.0f;
static constexpr float BARREL_DROP_BASE = 0.22f;   // prob/frame at ladder TOP, base
// DK throws on a cadence that tightens as the level climbs. We keep an
// inviolable floor (BARREL_SPAWN_MIN) so even at high levels the screen
// never floods past what MAX_BARRELS can hold.
static constexpr float BARREL_SPAWN_BASE = 3.5f;
static constexpr float BARREL_SPAWN_MIN  = 1.6f;
static constexpr float BARREL_SPAWN_DECAY = 0.30f;  // s/level

// ── Fireballs (board-units)
static constexpr float FIREBALL_W_BU         = 10.0f;
static constexpr float FIREBALL_H_BU         = 11.0f;
static constexpr float FIREBALL_WALK_BU      = 22.0f;
static constexpr float FIREBALL_CLIMB_BU     = 18.0f;
static constexpr float FIREBALL_LADDER_RATE  = 0.6f;   // prob/sec at a ladder bottom
static constexpr float FIREBALL_SPAWN_BASE   = 14.0f;
static constexpr float FIREBALL_SPAWN_MIN    = 7.0f;

// ── Hammer (board-units)
static constexpr float HAMMER_W_BU    = 9.0f;
static constexpr float HAMMER_H_BU    = 11.0f;
static constexpr float HAMMER_DURATION = 5.0f;

// ── DK / Pauline animation
static constexpr float DK_THROW_ANIM    = 0.45f;
static constexpr float PAULINE_W_BU     = 8.0f;
static constexpr float PAULINE_H_BU     = 14.0f;
static constexpr float LEVEL_CLEAR_HOLD = 2.4f;     // freeze + flourish duration

// ── Bonus / scoring
static constexpr float BONUS_START = 5000.0f;
static constexpr float BONUS_RATE  = 100.0f;        // points/sec drain
static constexpr int   SCORE_BARREL_JUMP    = 100;
static constexpr int   SCORE_BARREL_SMASH   = 300;
static constexpr int   SCORE_FIREBALL_SMASH = 500;
static constexpr int   SCORE_PAULINE_KISS   = 100;  // flat bonus on top of remaining bonus

// ─────────────────────────────────────────────
//  COLORS (RGB565)
// ─────────────────────────────────────────────
static constexpr uint16_t COL_BG        = 0x0000;
static constexpr uint16_t COL_HUD        = 0xFFFF;
static constexpr uint16_t COL_GIRDER     = 0xFB4C;  // salmon-red girder
static constexpr uint16_t COL_GIRDER_RIV = 0x9000;  // rivet/shadow
static constexpr uint16_t COL_LADDER     = 0x07FF;  // cyan full ladder
static constexpr uint16_t COL_LADDER_BRK = 0x03B0;  // dim teal broken ladder
static constexpr uint16_t COL_JM_CAP     = 0xF800;  // red cap/shirt
static constexpr uint16_t COL_JM_OVERALL = 0x041F;  // blue overalls
static constexpr uint16_t COL_JM_FACE    = 0xFD60;  // tan face
static constexpr uint16_t COL_JM_DYING   = 0xFFE0;  // yellow flash on death
static constexpr uint16_t COL_DK_BODY    = 0x8B40;  // brown
static constexpr uint16_t COL_DK_FACE    = 0xFCC0;  // muzzle
static constexpr uint16_t COL_PAULINE    = 0xF81F;  // pink dress
static constexpr uint16_t COL_PAULINE_HAIR = 0xFFE0;
static constexpr uint16_t COL_BARREL     = 0xFCA0;  // amber
static constexpr uint16_t COL_BARREL_BAND = 0x6A40;
static constexpr uint16_t COL_FIRE_HOT   = 0xFFE0;  // yellow core
static constexpr uint16_t COL_FIRE_MID   = 0xFD20;  // orange
static constexpr uint16_t COL_FIRE_RIM   = 0xF800;  // red
static constexpr uint16_t COL_HAMMER     = 0xFFE0;  // bright yellow head
static constexpr uint16_t COL_HAMMER_GRIP = 0x9320;
static constexpr uint16_t COL_OIL_DRUM   = 0x8B40;
static constexpr uint16_t COL_TITLE      = 0xFFE0;

// ─────────────────────────────────────────────
//  THE 25m BOARD (board-units; one source of truth)
//
//  Six girders bottom→top, 32-BU spacing, gentle alternating slope
//  (zig-zag). G0–G4 span the FULL board width (0→224) wall-to-wall,
//  so there are no side gutters and the screen edges act as solid
//  walls (clamp_to_board) — Jumpman can't stroll off an end into open
//  air. G5 is the short flat top platform DK stands on. Ladder
//  endpoints (yt/yb) sit on the girder surfaces at their x; the
//  slope LINES are unchanged by the wall-to-wall extension, so every
//  ladder still meets its girder exactly. Full climb path: L0→L4.
//  Broken ladders (for chunk-2 barrels) can't be climbed.
// ─────────────────────────────────────────────
static const GirderDef BOARD_GIRDERS[N_GIRDERS] = {
    //  xl   yl    xr   yr
    {   0, 244,  224, 244 },   // G0 bottom — FLAT, full width
    {   0, 201,  224, 215 },   // G1        — down-right, wall-to-wall
    {   0, 183,  224, 169 },   // G2        — down-left,  wall-to-wall
    {   0, 137,  224, 151 },   // G3        — down-right, wall-to-wall
    {   0, 119,  224, 105 },   // G4        — down-left,  wall-to-wall
    {  60,  80,  164,  80 },   // G5 top    — flat (DK platform, short by design)
};

static const LadderDef BOARD_LADDERS[N_LADDERS] = {
    //  x    yt    yb   type
    { 190, 213,  244, LADDER_FULL   },   // L0  G0→G1  (climb path)
    {  40, 181,  204, LADDER_FULL   },   // L1  G1→G2
    { 190, 149,  171, LADDER_FULL   },   // L2  G2→G3
    {  40, 117,  140, LADDER_FULL   },   // L3  G3→G4
    {  70,  80,  115, LADDER_FULL   },   // L4  G4→G5
    {  40, 204,  244, LADDER_BROKEN },   // L5  G0→G1  (broken)
    { 190, 171,  213, LADDER_BROKEN },   // L6  G1→G2  (broken)
    {  40, 140,  181, LADDER_FULL   },   // L7  G2→G3  (alt route)
    { 190, 107,  149, LADDER_BROKEN },   // L8  G3→G4  (broken)
};

// ─────────────────────────────────────────────
//  PROJECTED RUNTIME STATE (pixels) — filled by project_board()
// ─────────────────────────────────────────────
static Girder g_girders[N_GIRDERS];
static Ladder g_ladders[N_LADDERS];
static float  g_scale = 1.0f, g_off_x = 0.0f, g_off_y = 0.0f;
static float  g_gravity, g_jump_vel, g_walk, g_climb, g_max_fall, g_fall_dmg;
static float  g_pw, g_ph, g_girder_t;
static float  g_barrel_w, g_barrel_h, g_barrel_roll, g_barrel_term;
static float  g_fireball_w, g_fireball_h, g_fireball_walk, g_fireball_climb;
static float  g_hammer_w, g_hammer_h;
static float  g_pauline_w, g_pauline_h;

// Static-per-level data computed at projection time and after every
// level reset. Sprite rects for DK / Pauline / oil drum live here so
// render() can stay branch-free and the Pauline-collision test reads
// them as a plain AABB.
struct SpriteRect { float x, y, w, h; };
static SpriteRect g_dk_rect;
static SpriteRect g_pauline_rect;
static SpriteRect g_oil_rect;

// Hammer pickup positions — picked to sit on the climb path:
//   spot 0: mid of G3 (above the bottom-half barrel storm)
//   spot 1: right side of G1 (rewards taking the long way around)
struct HammerSpotDef { int girder; float frac; };
static const HammerSpotDef HAMMER_SPOTS[2] = {
    { 3, 0.50f },
    { 1, 0.78f },
};

// ─────────────────────────────────────────────
//  VIEWPORT HELPERS (centre VIEW within a larger physical screen,
//  mirroring the rest of the game suite). Touch kiosks anchor at 0.
// ─────────────────────────────────────────────
static int vx() {
    int w = gfx->width();
    return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0;
}
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height();
    return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}
static inline int scr_x(float gx) { return vx() + (int)gx; }
static inline int scr_y(float gy) { return vy() + (int)gy; }

// ─────────────────────────────────────────────
//  PROJECTION — board-units → device pixels (uniform scale + centre)
// ─────────────────────────────────────────────
void project_board(int view_w, int view_h, int hud_h) {
    float avail_h = (float)(view_h - hud_h);
    float sx = (float)view_w / BOARD_W;
    float sy = avail_h / BOARD_H;
    g_scale = (sx < sy) ? sx : sy;                       // uniform — preserve slope angles
    g_off_x = ((float)view_w - BOARD_W * g_scale) * 0.5f;
    g_off_y = (float)hud_h + (avail_h - BOARD_H * g_scale) * 0.5f;

    for (int i = 0; i < N_GIRDERS; i++) {
        const GirderDef& d = BOARD_GIRDERS[i];
        g_girders[i].x_left  = g_off_x + d.xl * g_scale;
        g_girders[i].x_right = g_off_x + d.xr * g_scale;
        g_girders[i].y_left  = g_off_y + d.yl * g_scale;
        g_girders[i].y_right = g_off_y + d.yr * g_scale;
    }
    for (int i = 0; i < N_LADDERS; i++) {
        const LadderDef& d = BOARD_LADDERS[i];
        g_ladders[i].x        = g_off_x + d.x  * g_scale;
        g_ladders[i].y_top    = g_off_y + d.yt * g_scale;
        g_ladders[i].y_bottom = g_off_y + d.yb * g_scale;
        g_ladders[i].type     = (LadderType)d.type;
    }

    // Physics scaled from board-units → pixels (one set of numbers,
    // identical feel on every device).
    g_gravity  = GRAVITY_BU  * g_scale;
    g_jump_vel = JUMP_VEL_BU * g_scale;
    g_walk     = WALK_BU     * g_scale;
    g_climb    = CLIMB_BU    * g_scale;
    g_max_fall = MAX_FALL_BU * g_scale;
    g_fall_dmg = FALL_DMG_BU * g_scale;
    g_pw       = PLAYER_W_BU * g_scale;
    g_ph       = PLAYER_H_BU * g_scale;
    g_girder_t = GIRDER_T_BU * g_scale;
    if (g_girder_t < 3.0f) g_girder_t = 3.0f;

    g_barrel_w       = BARREL_W_BU       * g_scale;
    g_barrel_h       = BARREL_H_BU       * g_scale;
    g_barrel_roll    = BARREL_ROLL_BU    * g_scale;
    g_barrel_term    = BARREL_TERM_BU    * g_scale;
    g_fireball_w     = FIREBALL_W_BU     * g_scale;
    g_fireball_h     = FIREBALL_H_BU     * g_scale;
    g_fireball_walk  = FIREBALL_WALK_BU  * g_scale;
    g_fireball_climb = FIREBALL_CLIMB_BU * g_scale;
    g_hammer_w       = HAMMER_W_BU       * g_scale;
    g_hammer_h       = HAMMER_H_BU       * g_scale;
    g_pauline_w      = PAULINE_W_BU      * g_scale;
    g_pauline_h      = PAULINE_H_BU      * g_scale;
    if (g_barrel_w < 4.0f) g_barrel_w = 4.0f;
    if (g_barrel_h < 4.0f) g_barrel_h = 4.0f;
    if (g_fireball_w < 5.0f) g_fireball_w = 5.0f;
    if (g_fireball_h < 5.0f) g_fireball_h = 5.0f;
    if (g_hammer_w < 4.0f) g_hammer_w = 4.0f;
    if (g_hammer_h < 5.0f) g_hammer_h = 5.0f;
    if (g_pauline_w < 4.0f) g_pauline_w = 4.0f;
    if (g_pauline_h < 7.0f) g_pauline_h = 7.0f;

    // Sprite rects for DK, Pauline, oil drum (pixel space).
    {
        const Girder& g5 = g_girders[5];
        float dk_w = 20.0f * g_scale; if (dk_w < 12.0f) dk_w = 12.0f;
        float dk_h = 20.0f * g_scale; if (dk_h < 12.0f) dk_h = 12.0f;
        float dk_x = g5.x_left + 2.0f * g_scale;
        float dk_surf = girder_y_at(g5, dk_x + dk_w * 0.5f);
        g_dk_rect = { dk_x, dk_surf - dk_h, dk_w, dk_h };

        // Pauline stands above & to the right of DK.
        float pl_x = dk_x + dk_w + 6.0f * g_scale;
        g_pauline_rect = { pl_x, dk_surf - dk_h - g_pauline_h - 2.0f,
                           g_pauline_w + 2.0f, g_pauline_h };

        // Oil drum at G0 left side — fireball spawn point.
        const Girder& g0 = g_girders[0];
        float od_w = 14.0f * g_scale; if (od_w < 10.0f) od_w = 10.0f;
        float od_h = 16.0f * g_scale; if (od_h < 12.0f) od_h = 12.0f;
        float od_x = g0.x_left + 4.0f * g_scale;
        float od_surf = girder_y_at(g0, od_x + od_w * 0.5f);
        g_oil_rect = { od_x, od_surf - od_h, od_w, od_h };
    }
}

// ─────────────────────────────────────────────
//  Which girder is the entity standing on (surface within tol of
//  feet, cx within span)? Topmost wins. Used for ladder arrival and
//  airborne landing. (girder_y_at is inline in the header.)
// ─────────────────────────────────────────────
int girder_under(float cx, float feet_y, float tol) {
    int best = -1;
    float best_surf = 1e9f;
    for (int i = 0; i < N_GIRDERS; i++) {
        const Girder& g = g_girders[i];
        float lo = (g.x_left < g.x_right) ? g.x_left : g.x_right;
        float hi = (g.x_left < g.x_right) ? g.x_right : g.x_left;
        if (cx < lo || cx > hi) continue;
        float surf = girder_y_at(g, cx);
        if (feet_y >= surf - tol && feet_y <= surf + tol) {
            if (surf < best_surf) { best_surf = surf; best = i; }
        }
    }
    return best;
}

// True if cx is within girder i's horizontal span.
static bool in_girder_span(int i, float cx) {
    const Girder& g = g_girders[i];
    float lo = (g.x_left < g.x_right) ? g.x_left : g.x_right;
    float hi = (g.x_left < g.x_right) ? g.x_right : g.x_left;
    return cx >= lo && cx <= hi;
}

// Keep Jumpman within the board's horizontal bounds — he can't walk or
// drift off the left/right screen edges. With G0–G4 spanning the full
// board width, these bounds coincide with the girder ends, so the
// edges are solid walls: walking into one stops him dead instead of
// dropping him into a gutter. Only G5 (the short top platform) has
// interior ends; stepping off those is a safe one-level drop onto G4.
static inline void clamp_to_board(Jumpman& p) {
    float lo = g_off_x;
    float hi = g_off_x + BOARD_W * g_scale - g_pw;
    if (p.x < lo) p.x = lo;
    if (p.x > hi) p.x = hi;
}

// ─────────────────────────────────────────────
//  HIGH SCORE I/O (SPI Bus Treaty — same as the rest of the suite)
// ─────────────────────────────────────────────
static constexpr const char* HS_PATH = "/donkey_kong_hs.txt";

static long load_high_score() {
    long hs = 0;
#ifdef DEVICE_C28P
    if (SD_MMC.exists(HS_PATH)) {
        fs::File f = SD_MMC.open(HS_PATH, FILE_READ);
        if (f) { char b[24] = {0}; f.read((uint8_t*)b, 23); f.close(); hs = atol(b); }
    }
#else
    if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (sd.exists(HS_PATH)) {
            FsFile f = sd.open(HS_PATH, O_READ);
            if (f) { char b[24] = {0}; f.read(b, 23); f.close(); hs = atol(b); }
        }
        xSemaphoreGiveRecursive(spi_mutex);
    }
#endif
    return hs;
}

static void save_high_score(long hs) {
#ifdef DEVICE_C28P
    fs::File f = SD_MMC.open(HS_PATH, FILE_WRITE);
    if (f) { f.printf("%ld", hs); f.close(); }
#else
    if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        FsFile f = sd.open(HS_PATH, O_WRITE | O_CREAT | O_TRUNC);
        if (f) { f.printf("%ld", hs); f.close(); }
        xSemaphoreGiveRecursive(spi_mutex);
    }
#endif
}

// ═════════════════════════════════════════════
//  SIM SECTION — NO gfx-> CALLS BELOW THIS LINE
// ═════════════════════════════════════════════

static inline bool airborne(const Jumpman& p) {
    return p.state == JM_JUMPING || p.state == JM_FALLING;
}

// ── FSM transitions ──
void to_idle(Jumpman& p)            { p.state = JM_IDLE;     p.vx = 0; }
void to_running(Jumpman& p, int dir){ p.state = JM_RUNNING;  p.facing = dir; }
void to_climbing(Jumpman& p, const Ladder& L) {
    p.state  = JM_CLIMBING;
    p.x      = L.x - g_pw * 0.5f;   // snap centre to the ladder rail
    p.vx     = 0;
    p.vy     = 0;
}
void to_hammering(Jumpman& p)       { p.state = JM_HAMMERING; }  // chunk 3
void to_falling(Jumpman& p) {
    p.state = JM_FALLING;
    p.girder = -1;
    p.ladder = -1;
    p.fall_origin_y = p.y;          // start tracking the drop from here
}
void to_jumping(Jumpman& p) {
    // RIGID jump: capture the launch and lock it. update_jumpman never
    // reads left/right while airborne, so vx stays whatever it was at
    // takeoff (0 for a standing hop) — no mid-air steering.
    p.state = JM_JUMPING;
    p.vy = g_jump_vel;
    p.girder = -1;
    p.ladder = -1;
    p.fall_origin_y = p.y;
}

void to_dying(GameState& gs) {
    gs.player.state = JM_DYING;
    gs.player.vx = 0;
    gs.player.vy = 0;
    gs.death_timer = 1.1f;
    pm_game_audio_fx_die();
}

static void place_jumpman_start(GameState& gs) {
    const Girder& g = g_girders[0];
    float startx = g.x_right - g_pw - 4.0f;     // bottom girder, right end
    Jumpman& p = gs.player;
    p.x = startx;
    p.y = girder_y_at(g, startx + g_pw * 0.5f) - g_ph;
    p.vx = 0; p.vy = 0;
    p.facing = -1;
    p.state = JM_IDLE;
    p.girder = 0;
    p.ladder = -1;
    p.fall_origin_y = p.y;
    p.hammer_timer = 0;
}

// Try to start climbing from a grounded state. Returns true if a climb
// began. up=true means climb up (this girder is a ladder's bottom),
// down=true means climb down (this girder is a ladder's top).
static bool try_start_climb(GameState& gs, bool up, bool down) {
    Jumpman& p = gs.player;
    float cx = p.x + g_pw * 0.5f;
    float feet = p.y + g_ph;
    float xtol = g_pw;                       // must be roughly over the rail
    float ytol = fmaxf(3.0f, g_ph * 0.5f);

    for (int i = 0; i < N_LADDERS; i++) {
        const Ladder& L = g_ladders[i];
        if (L.type != LADDER_FULL) continue;   // BROKEN: Jumpman can't climb
        if (fabsf(cx - L.x) > xtol) continue;
        if (up && fabsf(feet - L.y_bottom) <= ytol) {
            to_climbing(p, L);
            p.ladder = i;
            // place feet just below the top of travel so we read as on-ladder
            p.y = L.y_bottom - g_ph - 1.0f;
            return true;
        }
        if (down && fabsf(feet - L.y_top) <= ytol) {
            to_climbing(p, L);
            p.ladder = i;
            p.y = L.y_top - g_ph + 1.0f;
            return true;
        }
    }
    return false;
}

// Swept landing: did feet cross any girder surface going down this
// frame? Returns the topmost girder index crossed, or -1.
static int landing_girder(float prev_y, float curr_y, float cx) {
    float prev_feet = prev_y + g_ph;
    float curr_feet = curr_y + g_ph;
    if (curr_feet < prev_feet) return -1;       // not descending
    int best = -1;
    float best_surf = 1e9f;
    for (int i = 0; i < N_GIRDERS; i++) {
        if (!in_girder_span(i, cx)) continue;
        float surf = girder_y_at(g_girders[i], cx);
        if (prev_feet <= surf && curr_feet >= surf) {
            if (surf < best_surf) { best_surf = surf; best = i; }
        }
    }
    return best;
}

// Land the player on girder gi at center cx. Applies the fall-damage
// rule using the distance from the tracked fall origin.
static void land_on_girder(GameState& gs, int gi, float cx) {
    Jumpman& p = gs.player;
    float surf = girder_y_at(g_girders[gi], cx);
    float drop = (surf - g_ph) - p.fall_origin_y;   // top-left delta of the fall
    if (drop > g_fall_dmg) {
        // Missed the safe landing — fell too far.
        p.y = surf - g_ph;
        to_dying(gs);
        return;
    }
    p.y = surf - g_ph;
    p.vy = 0;
    p.girder = gi;
    p.ladder = -1;
    p.state = (fabsf(p.vx) > 1.0f) ? JM_RUNNING : JM_IDLE;
}

void update_jumpman(GameState& gs, const PMNesInput& in, float dt) {
    Jumpman& p = gs.player;

    // ── DYING: count down, then respawn or end ──
    if (p.state == JM_DYING) {
        gs.death_timer -= dt;
        if (gs.death_timer <= 0.0f) {
            gs.lives--;
            if (gs.lives > 0) place_jumpman_start(gs);
            // lives==0 → run loop sees it and exits to game over.
        }
        return;
    }

    if (p.hammer_timer > 0) p.hammer_timer -= dt;   // chunk 3

    // ── CLIMBING ──
    if (p.state == JM_CLIMBING) {
        const Ladder& L = g_ladders[p.ladder];
        if (in.up)   p.y -= g_climb * dt;
        if (in.down) p.y += g_climb * dt;
        p.x = L.x - g_pw * 0.5f;                 // stay snapped to the rail

        float feet = p.y + g_ph;
        // Reached the top → step onto the upper girder.
        if (feet <= L.y_top) {
            int gi = girder_under(L.x, L.y_top, fmaxf(4.0f, g_ph * 0.6f));
            if (gi >= 0) { p.y = girder_y_at(g_girders[gi], L.x) - g_ph;
                           p.girder = gi; p.ladder = -1; p.state = JM_IDLE; p.vx = 0; }
            else { p.y = L.y_top - g_ph; }       // safety clamp
            return;
        }
        // Reached the bottom → step onto the lower girder.
        if (feet >= L.y_bottom) {
            int gi = girder_under(L.x, L.y_bottom, fmaxf(4.0f, g_ph * 0.6f));
            if (gi >= 0) { p.y = girder_y_at(g_girders[gi], L.x) - g_ph;
                           p.girder = gi; p.ladder = -1; p.state = JM_IDLE; p.vx = 0; }
            else { p.y = L.y_bottom - g_ph; }
            return;
        }
        return;   // still climbing
    }

    // ── GROUNDED (IDLE / RUNNING / HAMMERING) ──
    if (!airborne(p)) {
        bool hammering = (p.state == JM_HAMMERING);

        // Climb intent takes priority over walking — but not while
        // hammering. (DK arcade: hammer can't climb.)
        if (!hammering) {
            if (in.up   && try_start_climb(gs, true,  false)) return;
            if (in.down && try_start_climb(gs, false, true )) return;
        }

        // Horizontal walk (full ground control). While hammering we
        // still move but keep the JM_HAMMERING state so the draw layer
        // shows the hammer overhead and collisions resolve as smash.
        if (in.left)       { p.vx = -g_walk; p.facing = -1; if (!hammering) p.state = JM_RUNNING; }
        else if (in.right) { p.vx =  g_walk; p.facing = +1; if (!hammering) p.state = JM_RUNNING; }
        else               { p.vx = 0;       if (!hammering) p.state = JM_IDLE; }

        // Jump (rigid). Disabled while hammering — the arcade rule.
        if (!hammering && in.a) { to_jumping(p); pm_game_audio_fx_jump(); }

        // Move horizontally, then resolve against the girder.
        if (!airborne(p)) {
            p.x += p.vx * dt;
            clamp_to_board(p);                      // can't leave the screen sides
            float cx = p.x + g_pw * 0.5f;
            if (p.girder >= 0 && in_girder_span(p.girder, cx)) {
                // Follow the slope: re-pin feet to the surface.
                p.y = girder_y_at(g_girders[p.girder], cx) - g_ph;
            } else if (p.girder >= 0) {
                // Walked off the end of the girder → fall.
                to_falling(p);
            }
        }
    }

    // ── AIRBORNE (JUMPING / FALLING) — RIGID: no left/right read ──
    if (airborne(p)) {
        p.vy += g_gravity * dt;
        if (p.vy > g_max_fall) p.vy = g_max_fall;
        if (p.state == JM_JUMPING && p.vy > 0) p.state = JM_FALLING;  // past apex

        float prev_y = p.y;
        p.x += p.vx * dt;        // keep launch momentum (no steering)
        clamp_to_board(p);       // can't drift off the screen sides mid-air
        p.y += p.vy * dt;
        // fall_origin_y stays at the takeoff girder: the fall-damage
        // rule measures NET descent (levels dropped), so a hop that
        // returns to the same height counts as zero and never hurts,
        // while a clean one-level drop stays under the threshold and a
        // two-level drop exceeds it.

        float cx = p.x + g_pw * 0.5f;
        if (p.vy >= 0) {
            int gi = landing_girder(prev_y, p.y, cx);
            if (gi >= 0) land_on_girder(gs, gi, cx);
        }
    }

    // Off-bottom safety net.
    if (p.y > (float)VIEW_H + g_ph) {
        to_dying(gs);
    }
}

// ──────────────────────────────────────────
//  SPAWN HELPERS
// ──────────────────────────────────────────
static int find_free_barrel(GameState& gs) {
    for (int i = 0; i < MAX_BARRELS; i++) if (!gs.barrels[i].active) return i;
    return -1;
}
static int find_free_fireball(GameState& gs) {
    for (int i = 0; i < MAX_FIREBALLS; i++) if (!gs.fireballs[i].active) return i;
    return -1;
}

static void spawn_barrel(GameState& gs) {
    int slot = find_free_barrel(gs);
    if (slot < 0) return;
    Barrel& b = gs.barrels[slot];
    const Girder& g5 = g_girders[5];
    // Spawn at DK's right side, on the G5 surface.
    float spawn_x = g_dk_rect.x + g_dk_rect.w + 2.0f;
    if (spawn_x + g_barrel_w > g5.x_right) spawn_x = g5.x_right - g_barrel_w - 1.0f;
    if (spawn_x < g5.x_left) spawn_x = g5.x_left + 1.0f;
    float surf = girder_y_at(g5, spawn_x + g_barrel_w * 0.5f);
    b.x = spawn_x;
    b.y = surf - g_barrel_h;
    b.vx = g_barrel_roll;          // rolls rightward off G5 onto G4
    b.vy = 0.0f;
    b.state = BAR_ROLLING;
    b.girder = 5;
    b.active = true;
    b.scored = false;
    gs.dk_anim_timer = DK_THROW_ANIM;
    pm_game_audio_fx_kick();
}

static void spawn_fireball(GameState& gs) {
    int slot = find_free_fireball(gs);
    if (slot < 0) return;
    Fireball& f = gs.fireballs[slot];
    const Girder& g0 = g_girders[0];
    // Emerge from the oil drum; place sprite just to the right of it.
    float spawn_x = g_oil_rect.x + g_oil_rect.w + 2.0f;
    if (spawn_x + g_fireball_w > g0.x_right) spawn_x = g0.x_left + 18.0f * g_scale;
    float surf = girder_y_at(g0, spawn_x + g_fireball_w * 0.5f);
    f.x = spawn_x;
    f.y = surf - g_fireball_h;
    f.vx = g_fireball_walk;        // wander rightward initially
    f.vy = 0.0f;
    f.girder = 0;
    f.ladder = -1;
    f.active = true;
    f.climbing = false;
    f.anim_timer = 0.0f;
}

// Generic swept landing test parameterised by entity height. Mirror of
// landing_girder() above but for any entity (barrels are taller than
// they are wide; fireballs use this on the rare descent path).
static int landing_girder_h(float prev_y, float curr_y, float cx, float h) {
    float prev_feet = prev_y + h;
    float curr_feet = curr_y + h;
    if (curr_feet < prev_feet) return -1;
    int best = -1;
    float best_surf = 1e9f;
    for (int i = 0; i < N_GIRDERS; i++) {
        if (!in_girder_span(i, cx)) continue;
        float surf = girder_y_at(g_girders[i], cx);
        if (prev_feet <= surf && curr_feet >= surf) {
            if (surf < best_surf) { best_surf = surf; best = i; }
        }
    }
    return best;
}

// ──────────────────────────────────────────
//  BARRELS
//
//  A barrel on a girder follows the slope: vx_sign tracks the
//  downhill direction, so the zigzag-stacked layout drives the
//  classic DK barrel cascade automatically. At a ladder TOP that
//  matches the current girder's surface, an RNG check (decay
//  steeper at higher levels) drops the barrel down the ladder. Off
//  the end of a girder, the barrel enters BAR_FALLING and resumes
//  normal projectile motion until the swept landing test catches
//  it on the next girder — same algorithm Jumpman uses, just
//  parameterised by g_barrel_h.
// ──────────────────────────────────────────
void update_barrel(GameState& gs, int slot, float dt) {
    Barrel& b = gs.barrels[slot];
    if (!b.active) return;

    if (b.state == BAR_ROLLING) {
        if (b.girder < 0 || b.girder >= N_GIRDERS) { b.active = false; return; }
        const Girder& g = g_girders[b.girder];

        // Roll direction is the girder's downhill (slope sign). On flat
        // girders we preserve whatever vx came in — G0 (bottom) and G5
        // (DK platform) both feed barrels off their right edges.
        float dx_span = g.x_right - g.x_left;
        float slope = (fabsf(dx_span) > 0.01f)
                      ? (g.y_right - g.y_left) / dx_span
                      : 0.0f;
        if (fabsf(slope) > 0.03f) {
            b.vx = (slope > 0 ? +1.0f : -1.0f) * g_barrel_roll;
        }
        b.x += b.vx * dt;
        float cx = b.x + g_barrel_w * 0.5f;

        // Off the girder edge → fall.
        float lo = (g.x_left < g.x_right) ? g.x_left : g.x_right;
        float hi = (g.x_left < g.x_right) ? g.x_right : g.x_left;
        if (cx < lo || cx > hi) {
            b.state  = BAR_FALLING;
            b.girder = -1;
            b.vy = 8.0f * g_scale;   // small nudge so gravity wins immediately
            return;
        }

        // Snap to surface so the roll tracks the slope.
        float surf = girder_y_at(g, cx);
        b.y = surf - g_barrel_h;

        // RNG drop check at any ladder TOP whose head sits on THIS girder.
        // Skip the broken ladders for variety — barrels usually drop only
        // through intact ladders. (Broken ladders still slow the player
        // because he can't climb them; barrels swerving past them stays
        // honest to the arcade.)
        for (int i = 0; i < N_LADDERS; i++) {
            const Ladder& L = g_ladders[i];
            if (L.type != LADDER_FULL) continue;
            if (fabsf(cx - L.x) > g_barrel_w * 0.6f) continue;
            if (fabsf(L.y_top - surf) > g_girder_t + 2.0f) continue;
            float p = BARREL_DROP_BASE + 0.04f * (float)(gs.level - 1);
            if (p > 0.55f) p = 0.55f;
            float r = (float)(esp_random() & 0xFFFF) / 65535.0f;
            if (r < p * dt * 6.0f) {
                b.x = L.x - g_barrel_w * 0.5f;
                b.state  = BAR_FALLING;
                b.girder = -1;
                b.vx = 0.0f;
                b.vy = g_barrel_roll * 0.6f;
                return;
            }
        }
        return;
    }

    if (b.state == BAR_FALLING) {
        b.vy += g_gravity * dt;
        if (b.vy > g_barrel_term) b.vy = g_barrel_term;
        float prev_y = b.y;
        b.x += b.vx * dt;
        b.y += b.vy * dt;

        float cx = b.x + g_barrel_w * 0.5f;
        if (b.vy > 0) {
            int gi = landing_girder_h(prev_y, b.y, cx, g_barrel_h);
            if (gi >= 0) {
                float surf = girder_y_at(g_girders[gi], cx);
                b.y = surf - g_barrel_h;
                b.vy = 0;
                b.state  = BAR_ROLLING;
                b.girder = gi;
                // Inherit the new girder's slope direction. Flat girders
                // (G0/G5) keep whatever sign vx had on takeoff so the
                // barrel walks off the next edge.
                float dxs = g_girders[gi].x_right - g_girders[gi].x_left;
                float sl  = (fabsf(dxs) > 0.01f)
                            ? (g_girders[gi].y_right - g_girders[gi].y_left) / dxs
                            : 0.0f;
                if (fabsf(sl) > 0.03f) b.vx = (sl > 0 ? +1.0f : -1.0f) * g_barrel_roll;
                else if (fabsf(b.vx) < 1.0f) b.vx = g_barrel_roll;
            }
        }

        // Off the bottom of the play area — retire the barrel.
        if (b.y > g_off_y + BOARD_H * g_scale + g_barrel_h) {
            b.active = false;
        }
        return;
    }

    // BAR_SPAWNED currently unused as a separate transition state —
    // spawn_barrel() places the barrel directly into BAR_ROLLING. The
    // enum slot is kept for future tuning (e.g., DK animation gate)
    // without breaking the header contract.
    (void)dt;
}

// ──────────────────────────────────────────
//  FIREBALLS
//
//  Fireballs walk a girder back and forth, bouncing off the ends.
//  Cheap tracking: if Jumpman is on the same (vertical) band, the
//  fireball aligns its vx toward him. At a ladder bottom with
//  Jumpman above, a per-second probability gate decides whether to
//  climb. While climbing, the fireball stays snapped to the rail
//  until it reaches the upper girder.
// ──────────────────────────────────────────
void update_fireball(GameState& gs, int slot, float dt) {
    Fireball& f = gs.fireballs[slot];
    if (!f.active) return;
    f.anim_timer += dt;

    if (f.climbing) {
        if (f.ladder < 0 || f.ladder >= N_LADDERS) {
            f.climbing = false; return;
        }
        const Ladder& L = g_ladders[f.ladder];
        f.y += f.vy * dt;
        f.x  = L.x - g_fireball_w * 0.5f;
        float feet = f.y + g_fireball_h;
        // Top arrival
        if (f.vy < 0 && feet <= L.y_top) {
            int gi = girder_under(L.x, L.y_top, fmaxf(4.0f, g_fireball_h * 0.6f));
            if (gi >= 0) {
                f.y = girder_y_at(g_girders[gi], L.x) - g_fireball_h;
                f.girder = gi;
                f.ladder = -1;
                f.climbing = false;
                // Aim toward Jumpman's x.
                float pcx = gs.player.x + g_pw * 0.5f;
                f.vx = (pcx > f.x + g_fireball_w * 0.5f ? +1 : -1) * g_fireball_walk;
                f.vy = 0;
            }
            return;
        }
        // Bottom arrival (shouldn't usually happen — they climb up —
        // but is safe to handle).
        if (f.vy > 0 && feet >= L.y_bottom) {
            int gi = girder_under(L.x, L.y_bottom, fmaxf(4.0f, g_fireball_h * 0.6f));
            if (gi >= 0) {
                f.y = girder_y_at(g_girders[gi], L.x) - g_fireball_h;
                f.girder = gi;
                f.ladder = -1;
                f.climbing = false;
                f.vx = g_fireball_walk;
                f.vy = 0;
            }
            return;
        }
        return;
    }

    if (f.girder < 0 || f.girder >= N_GIRDERS) { f.active = false; return; }
    const Girder& g = g_girders[f.girder];

    f.x += f.vx * dt;
    float cx = f.x + g_fireball_w * 0.5f;
    float lo = (g.x_left < g.x_right) ? g.x_left : g.x_right;
    float hi = (g.x_left < g.x_right) ? g.x_right : g.x_left;
    // Bounce off the girder ends.
    if (cx < lo + g_fireball_w * 0.5f) {
        f.vx = +g_fireball_walk;
        cx = lo + g_fireball_w * 0.5f;
        f.x = cx - g_fireball_w * 0.5f;
    } else if (cx > hi - g_fireball_w * 0.5f) {
        f.vx = -g_fireball_walk;
        cx = hi - g_fireball_w * 0.5f;
        f.x = cx - g_fireball_w * 0.5f;
    }

    // Snap to girder surface.
    f.y = girder_y_at(g, cx) - g_fireball_h;

    // Cheap tracking: if Jumpman is roughly on the same level, align
    // our walk direction toward him. Doesn't override a fresh bounce.
    const Jumpman& p = gs.player;
    float p_cx = p.x + g_pw * 0.5f;
    if (fabsf((p.y + g_ph) - (f.y + g_fireball_h)) < g_ph * 1.5f) {
        if (p_cx > f.x + g_fireball_w && f.vx < 0) f.vx = +g_fireball_walk;
        else if (p_cx + g_pw < f.x && f.vx > 0) f.vx = -g_fireball_walk;
    }

    // Ladder-climb decision: only when Jumpman is above (so the
    // fireball doesn't pointlessly climb away from him).
    if (p.y + g_ph < f.y - 4.0f) {
        for (int i = 0; i < N_LADDERS; i++) {
            const Ladder& L = g_ladders[i];
            if (L.type != LADDER_FULL) continue;
            if (fabsf(cx - L.x) > g_fireball_w * 0.6f) continue;
            if (fabsf(L.y_bottom - (f.y + g_fireball_h)) > g_fireball_h * 0.5f) continue;
            float r = (float)(esp_random() & 0xFFFF) / 65535.0f;
            if (r < FIREBALL_LADDER_RATE * dt) {
                f.climbing = true;
                f.ladder = i;
                f.girder = -1;
                f.vy = -g_fireball_climb;
                f.x  = L.x - g_fireball_w * 0.5f;
                return;
            }
        }
    }
}

// ──────────────────────────────────────────
//  COLLISIONS, PICKUPS, LEVEL CLEAR
//
//  After all entities have updated, we resolve interactions in one
//  pass: jump-over scoring (one credit per barrel), hammer pickup,
//  hammer smash, lethal contact, and Pauline touch.
// ──────────────────────────────────────────
static pmp::AABB jumpman_aabb(const Jumpman& p) {
    return { p.x + 1.0f, p.y + 1.0f, g_pw - 2.0f, g_ph - 2.0f };
}

static void try_pickup_hammer(GameState& gs) {
    Jumpman& p = gs.player;
    // Pickup is grounded-only: a mid-jump grab would transition straight
    // into JM_HAMMERING while still airborne, which to_hammering()
    // wasn't designed for. Walking over a hammer is the only legal way
    // to acquire one (matches arcade behaviour).
    if (p.state != JM_IDLE && p.state != JM_RUNNING) return;
    pmp::AABB ja = jumpman_aabb(p);
    for (int i = 0; i < 2; i++) {
        Hammer& h = gs.hammers[i];
        if (!h.active) continue;
        pmp::AABB ha = { h.x, h.y, g_hammer_w, g_hammer_h };
        if (pmp::aabb_overlap(ja, ha)) {
            h.active = false;
            to_hammering(p);
            p.hammer_timer = HAMMER_DURATION;
            pm_game_audio_fx_pow();
            return;
        }
    }
}

static void check_collisions(GameState& gs) {
    Jumpman& p = gs.player;
    if (p.state == JM_DYING || gs.level_clearing) return;

    pmp::AABB ja = jumpman_aabb(p);

    // Pauline kiss — level clear.
    pmp::AABB pa = { g_pauline_rect.x, g_pauline_rect.y,
                     g_pauline_rect.w, g_pauline_rect.h };
    if (pmp::aabb_overlap(ja, pa)) {
        gs.level_clearing    = true;
        gs.level_clear_timer = LEVEL_CLEAR_HOLD;
        gs.score += (long)gs.bonus + SCORE_PAULINE_KISS;
        gs.bonus = 0;
        p.vx = 0;
        p.vy = 0;
        p.state = JM_IDLE;
        pm_game_audio_fx_pow();
        return;
    }

    // Barrels.
    for (int i = 0; i < MAX_BARRELS; i++) {
        Barrel& b = gs.barrels[i];
        if (!b.active) continue;
        pmp::AABB ba = { b.x, b.y, g_barrel_w, g_barrel_h };

        // Jump-over: when airborne with the barrel passing under us,
        // credit one (and only one) point award per barrel.
        if (!b.scored && (p.state == JM_JUMPING || p.state == JM_FALLING)) {
            float bcx = b.x + g_barrel_w * 0.5f;
            float pcx = p.x + g_pw * 0.5f;
            if (fabsf(bcx - pcx) < g_pw * 0.85f &&
                b.y > p.y + g_ph * 0.55f) {
                b.scored = true;
                gs.score += SCORE_BARREL_JUMP;
                pm_game_audio_fx_flip();
            }
        }

        if (pmp::aabb_overlap(ja, ba)) {
            if (p.state == JM_HAMMERING) {
                b.active = false;
                gs.score += SCORE_BARREL_SMASH;
                pm_game_audio_fx_explode();
            } else {
                to_dying(gs);
                return;
            }
        }
    }

    // Fireballs.
    for (int i = 0; i < MAX_FIREBALLS; i++) {
        Fireball& f = gs.fireballs[i];
        if (!f.active) continue;
        pmp::AABB fa = { f.x, f.y, g_fireball_w, g_fireball_h };
        if (pmp::aabb_overlap(ja, fa)) {
            if (p.state == JM_HAMMERING) {
                f.active = false;
                gs.score += SCORE_FIREBALL_SMASH;
                pm_game_audio_fx_explode();
            } else {
                to_dying(gs);
                return;
            }
        }
    }
}

// Per-level fireball cap escalates with progress (1→3 over 3 levels).
static int fireball_cap_for_level(int level) {
    int cap = level;
    if (cap < 1) cap = 1;
    if (cap > MAX_FIREBALLS) cap = MAX_FIREBALLS;
    return cap;
}

static int active_fireball_count(const GameState& gs) {
    int n = 0;
    for (int i = 0; i < MAX_FIREBALLS; i++) if (gs.fireballs[i].active) n++;
    return n;
}

// Place the two hammer pickups on their canonical board spots, on the
// girder surface. Called by reset_game at level start.
static void place_hammers(GameState& gs) {
    for (int i = 0; i < 2; i++) {
        const HammerSpotDef& s = HAMMER_SPOTS[i];
        const Girder& g = g_girders[s.girder];
        float x_mid = pmp::lerpf(g.x_left, g.x_right, s.frac);
        float surf  = girder_y_at(g, x_mid);
        gs.hammers[i].x = x_mid - g_hammer_w * 0.5f;
        gs.hammers[i].y = surf - g_hammer_h;
        gs.hammers[i].active = true;
    }
}

void step_world(GameState& gs, const PMNesInput& in, float dt) {
    // Level-clear flourish: hold the screen for a beat before resetting
    // the board for the next level.
    if (gs.level_clearing) {
        gs.level_clear_timer -= dt;
        if (gs.dk_anim_timer > 0) gs.dk_anim_timer -= dt;
        return;
    }

    update_jumpman(gs, in, dt);
    if (gs.player.state == JM_DYING) {
        // Pause the rest of the world during the death animation. Barrels
        // and fireballs already on screen freeze — standard arcade feel.
        if (gs.dk_anim_timer > 0) gs.dk_anim_timer -= dt;
        return;
    }

    try_pickup_hammer(gs);

    // Hammer auto-expires.
    if (gs.player.state == JM_HAMMERING && gs.player.hammer_timer <= 0.0f) {
        gs.player.state = JM_IDLE;
        gs.player.vx = 0;
    }

    for (int i = 0; i < MAX_BARRELS;   i++) update_barrel  (gs, i, dt);
    for (int i = 0; i < MAX_FIREBALLS; i++) update_fireball(gs, i, dt);

    check_collisions(gs);

    // Bonus drain (visible counter; awarded to score on Pauline touch).
    if (gs.bonus > 0) {
        gs.bonus -= BONUS_RATE * dt;
        if (gs.bonus < 0) gs.bonus = 0;
    }

    // Barrel spawn cadence. Faster every level, capped at BARREL_SPAWN_MIN.
    gs.barrel_spawn_timer -= dt;
    if (gs.barrel_spawn_timer <= 0.0f) {
        spawn_barrel(gs);
        float interval = BARREL_SPAWN_BASE - BARREL_SPAWN_DECAY * (gs.level - 1);
        if (interval < BARREL_SPAWN_MIN) interval = BARREL_SPAWN_MIN;
        gs.barrel_spawn_timer = interval;
    }

    // Fireball spawn cadence — only while under the per-level cap.
    gs.fireball_spawn_timer -= dt;
    if (gs.fireball_spawn_timer <= 0.0f) {
        if (active_fireball_count(gs) < fireball_cap_for_level(gs.level)) {
            spawn_fireball(gs);
        }
        float interval = FIREBALL_SPAWN_BASE - 1.5f * (gs.level - 1);
        if (interval < FIREBALL_SPAWN_MIN) interval = FIREBALL_SPAWN_MIN;
        gs.fireball_spawn_timer = interval;
    }

    // DK animation timer.
    if (gs.dk_anim_timer > 0) gs.dk_anim_timer -= dt;
}

// ═════════════════════════════════════════════
//  DRAW SECTION — reads GameState, owns all gfx-> calls
// ═════════════════════════════════════════════

static void draw_girder(const Girder& g) {
    int x0 = (int)g.x_left, x1 = (int)g.x_right;
    int t  = (int)g_girder_t;
    for (int x = x0; x <= x1; x++) {
        float tt = (g.x_right == g.x_left) ? 0.0f
                 : (float)(x - g.x_left) / (g.x_right - g.x_left);
        int y = (int)(g.y_left + (g.y_right - g.y_left) * tt);
        gfx->drawFastVLine(scr_x((float)x), scr_y((float)y), t, COL_GIRDER);
    }
    // Rivets every ~14px along the span.
    for (int x = x0 + 6; x <= x1 - 4; x += 14) {
        float tt = (g.x_right == g.x_left) ? 0.0f
                 : (float)(x - g.x_left) / (g.x_right - g.x_left);
        int y = (int)(g.y_left + (g.y_right - g.y_left) * tt);
        gfx->fillRect(scr_x((float)x), scr_y((float)(y + t / 2 - 1)), 2, 2, COL_GIRDER_RIV);
    }
}

static void draw_ladder(const Ladder& L) {
    bool full = (L.type == LADDER_FULL);
    uint16_t col = full ? COL_LADDER : COL_LADDER_BRK;
    int x  = (int)L.x;
    int yt = (int)L.y_top, yb = (int)L.y_bottom;
    int railw = (g_pw >= 12) ? 3 : 2;
    int half  = (int)(g_pw * 0.45f); if (half < 4) half = 4;
    // Rails
    gfx->fillRect(scr_x((float)(x - half)), scr_y((float)yt), railw, yb - yt, col);
    gfx->fillRect(scr_x((float)(x + half - railw + 1)), scr_y((float)yt), railw, yb - yt, col);
    // Rungs — broken ladders skip a band in the middle.
    int brk_lo = yt + (yb - yt) / 3, brk_hi = yt + 2 * (yb - yt) / 3;
    for (int yy = yt + 3; yy < yb; yy += 6) {
        if (!full && yy > brk_lo && yy < brk_hi) continue;   // missing rungs
        gfx->drawFastHLine(scr_x((float)(x - half)), scr_y((float)yy), half * 2, col);
    }
}

static void draw_jumpman(const Jumpman& p) {
    int x = scr_x(p.x), y = scr_y(p.y);
    int w = (int)g_pw, h = (int)g_ph;
    if (w < 6) w = 6; if (h < 8) h = 8;

    if (p.state == JM_DYING) {
        // Flash + a stunned "dizzy" box while the death timer runs.
        if (((millis() / 90) & 1)) {
            gfx->fillRect(x, y, w, h, COL_JM_DYING);
            gfx->drawRect(x, y, w, h, 0xF800);
        }
        return;
    }

    int head_h = h / 3;
    // Overalls (lower body)
    gfx->fillRect(x, y + head_h, w, h - head_h, COL_JM_OVERALL);
    // Cap + shirt band (upper)
    gfx->fillRect(x, y, w, head_h, COL_JM_CAP);
    // Face
    gfx->fillRect(x + 1, y + head_h - 2, w - 2, 3, COL_JM_FACE);
    // Facing eye
    int ex = (p.facing > 0) ? x + w - 3 : x + 1;
    gfx->fillRect(ex, y + 1, 2, 2, 0x0000);
    // Climbing: draw a centred ladder-grip pose hint (arms up)
    if (p.state == JM_CLIMBING) {
        gfx->drawFastVLine(x, y, head_h, COL_JM_CAP);
        gfx->drawFastVLine(x + w - 1, y, head_h, COL_JM_CAP);
    }
    // Hammer overhead during JM_HAMMERING. Two-frame swing animation
    // (overhead ↔ down-and-to-the-facing-side) so it reads as actively
    // smashing rather than a held trophy.
    if (p.state == JM_HAMMERING) {
        int hw = (int)g_hammer_w; if (hw < 4) hw = 4;
        int hh = (int)(g_hammer_h * 0.6f); if (hh < 4) hh = 4;
        bool swing_down = ((millis() / 200) & 1);
        int hx, hy;
        if (swing_down) {
            hx = (p.facing > 0) ? x + w : x - hw;
            hy = y + head_h;
        } else {
            hx = x + w / 2 - hw / 2;
            hy = y - hh - 1;
        }
        gfx->fillRect(hx, hy, hw, hh, COL_HAMMER);
        gfx->drawRect(hx, hy, hw, hh, COL_HAMMER_GRIP);
    }
}

// ──────────────────────────────────────────
//  Barrels, fireballs, hammers, oil drum — the arcade props that
//  separate "Phase 1 skeleton" from "actual Donkey Kong."
// ──────────────────────────────────────────
static void draw_barrel(const Barrel& b) {
    int x = scr_x(b.x), y = scr_y(b.y);
    int w = (int)g_barrel_w, h = (int)g_barrel_h;
    if (w < 4) w = 4; if (h < 4) h = 4;
    int cx = x + w / 2, cy = y + h / 2;
    int r  = (w < h ? w : h) / 2;
    gfx->fillCircle(cx, cy, r, COL_BARREL);
    // Single horizontal stripe — reads as a barrel band, also rotates
    // visually as the barrel rolls (no real rotation, but the band
    // breaks up the silhouette).
    int band_y = cy + ((millis() / 100) & 1 ? -1 : 0);
    gfx->drawFastHLine(x, band_y, w, COL_BARREL_BAND);
}

static void draw_fireball(const Fireball& f) {
    int x = scr_x(f.x), y = scr_y(f.y);
    int w = (int)g_fireball_w, h = (int)g_fireball_h;
    if (w < 5) w = 5; if (h < 5) h = 5;
    int cx = x + w / 2, cy = y + h / 2;
    int r  = (w < h ? w : h) / 2;
    bool flick = ((millis() / 90) & 1);
    int  r_rim = flick ? r : r - 1; if (r_rim < 2) r_rim = 2;
    gfx->fillCircle(cx, cy,     r_rim,     COL_FIRE_RIM);
    gfx->fillCircle(cx, cy,     r_rim - 1, COL_FIRE_MID);
    gfx->fillCircle(cx, cy - 1, r_rim / 2, COL_FIRE_HOT);
}

static void draw_hammer(const Hammer& h) {
    if (!h.active) return;
    int x = scr_x(h.x), y = scr_y(h.y);
    int w = (int)g_hammer_w, hh = (int)g_hammer_h;
    if (w < 4) w = 4; if (hh < 5) hh = 5;
    int head_h = hh / 2;
    // Head
    gfx->fillRect(x, y, w, head_h, COL_HAMMER);
    gfx->drawRect(x, y, w, head_h, COL_HAMMER_GRIP);
    // Grip (centered shaft down to the girder surface)
    int gx = x + w / 2 - 1;
    gfx->fillRect(gx, y + head_h, 2, hh - head_h, COL_HAMMER_GRIP);
}

static void draw_oil_drum() {
    int x = scr_x(g_oil_rect.x), y = scr_y(g_oil_rect.y);
    int w = (int)g_oil_rect.w, h = (int)g_oil_rect.h;
    gfx->fillRect(x, y, w, h, COL_OIL_DRUM);
    gfx->drawFastHLine(x, y + h / 3,     w, COL_GIRDER_RIV);
    gfx->drawFastHLine(x, y + 2 * h / 3, w, COL_GIRDER_RIV);
    gfx->drawRect(x, y, w, h, COL_GIRDER_RIV);
    // Flame on top
    int fcx = x + w / 2;
    int fcy = y - 3;
    bool flick = ((millis() / 160) & 1);
    int fr = flick ? 4 : 3;
    if (fr > w / 2) fr = w / 2;
    if (fr < 2) fr = 2;
    gfx->fillCircle(fcx, fcy,     fr,     COL_FIRE_RIM);
    gfx->fillCircle(fcx, fcy,     fr - 1, COL_FIRE_MID);
    gfx->fillCircle(fcx, fcy - 1, fr / 2, COL_FIRE_HOT);
}

static void draw_dk_animated(const GameState& gs) {
    int x = scr_x(g_dk_rect.x), y = scr_y(g_dk_rect.y);
    int w = (int)g_dk_rect.w, h = (int)g_dk_rect.h;
    // Body
    gfx->fillRect(x, y, w, h, COL_DK_BODY);
    // Muzzle/face block
    gfx->fillRect(x + 3, y + 4, w - 6, h / 3, COL_DK_FACE);
    // Eyes
    gfx->fillRect(x + 4,     y + 6, 2, 2, 0x0000);
    gfx->fillRect(x + w - 6, y + 6, 2, 2, 0x0000);
    // Arms — raised during the throw window, resting otherwise.
    bool throwing = (gs.dk_anim_timer > 0);
    int arm_w = (w >= 16) ? 3 : 2;
    int arm_h = h / 3;
    int arm_y = throwing ? y - arm_h / 2 : y + h / 3;
    gfx->fillRect(x - arm_w, arm_y, arm_w, arm_h, COL_DK_BODY);
    gfx->fillRect(x + w,     arm_y, arm_w, arm_h, COL_DK_BODY);
    // Tiny barrel-in-hand cue while throwing.
    if (throwing) {
        int bx = x + w + arm_w;
        int by = arm_y - 2;
        int bs = arm_w + 2;
        gfx->fillRect(bx, by, bs, bs, COL_BARREL);
        gfx->drawFastHLine(bx, by + bs / 2, bs, COL_BARREL_BAND);
    }
}

static void draw_pauline_animated() {
    int x = scr_x(g_pauline_rect.x), y = scr_y(g_pauline_rect.y);
    int w = (int)g_pauline_rect.w, h = (int)g_pauline_rect.h;
    // Hair (top quarter)
    int hair_h = h / 4;
    gfx->fillRect(x, y, w, hair_h, COL_PAULINE_HAIR);
    // Face
    gfx->fillRect(x, y + hair_h, w, h / 6, COL_JM_FACE);
    // Dress
    gfx->fillRect(x, y + hair_h + h / 6, w, h - hair_h - h / 6, COL_PAULINE);
    // Optional "!" cue — a single yellow dot above her head, blinking.
    if (((millis() / 600) & 1)) {
        gfx->fillRect(x + w / 2 - 1, y - 4, 2, 3, COL_TITLE);
    }
}

static void draw_hud(const GameState& gs) {
    int hy = vy();
    gfx->fillRect(vx(), hy, VIEW_W, HUD_H, COL_BG);
    gfx->setTextSize(1);

    // Left: score
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(vx() + 4, hy + 2);
    gfx->printf("SC%ld", gs.score);

    // Center-left: level + bonus
    gfx->setTextColor(COL_FIRE_HOT);
    int cx = vx() + VIEW_W / 2 - 36;
    gfx->setCursor(cx, hy + 2);
    gfx->printf("L%d", gs.level);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(cx + 18, hy + 2);
    gfx->printf("B%d", (int)gs.bonus);

    // Center-right: high score
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor(vx() + VIEW_W - 76, hy + 2);
    gfx->printf("HI%ld", gs.high_score);

    // Right: lives count
    gfx->setTextColor(COL_JM_CAP);
    gfx->setCursor(vx() + VIEW_W - 22, hy + 2);
    gfx->printf("x%d", gs.lives);
}

void render(const GameState& gs) {
    // Clear the play area only (touch kiosks own the strip below VIEW).
    gfx->fillRect(vx(), vy() + HUD_H, VIEW_W, VIEW_H - HUD_H, COL_BG);
    for (int i = 0; i < N_LADDERS; i++) draw_ladder(g_ladders[i]);   // behind girders
    for (int i = 0; i < N_GIRDERS; i++) draw_girder(g_girders[i]);
    draw_oil_drum();
    draw_dk_animated(gs);
    draw_pauline_animated();
    // Pickups behind dynamic entities so a barrel passing over a hammer
    // is still readable.
    for (int i = 0; i < 2; i++) draw_hammer(gs.hammers[i]);
    for (int i = 0; i < MAX_BARRELS; i++) if (gs.barrels[i].active)   draw_barrel(gs.barrels[i]);
    for (int i = 0; i < MAX_FIREBALLS; i++) if (gs.fireballs[i].active) draw_fireball(gs.fireballs[i]);
    draw_jumpman(gs.player);
    draw_hud(gs);

    // Level-clear flourish overlays on top of the static board.
    if (gs.level_clearing) {
        gfx->setTextSize(2);
        gfx->setTextColor(COL_TITLE);
        char buf[24];
        snprintf(buf, sizeof(buf), "LEVEL %d CLEAR!", gs.level);
        int tw = (int)strlen(buf) * 12;
        int ox = vx() + (VIEW_W - tw) / 2;
        int oy = vy() + VIEW_H / 2 - 8;
        // Drop-shadow for legibility against the busy board.
        gfx->setTextColor(0x0000); gfx->setCursor(ox + 1, oy + 1); gfx->print(buf);
        gfx->setTextColor(COL_TITLE); gfx->setCursor(ox, oy);     gfx->print(buf);
    }
}

// ─────────────────────────────────────────────
//  TITLE / GAME OVER (static screens)
// ─────────────────────────────────────────────
static bool wait_for_start() {
    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, COL_BG);
    for (int i = 0; i < N_LADDERS; i++) draw_ladder(g_ladders[i]);
    for (int i = 0; i < N_GIRDERS; i++) draw_girder(g_girders[i]);
    draw_oil_drum();
    // Build a one-off zeroed gs purely so the title screen can use
    // draw_dk_animated() in its "resting" pose without exposing the
    // internal helpers to anything outside this TU.
    GameState title_gs{};
    draw_dk_animated(title_gs);
    draw_pauline_animated();

    gfx->setTextSize(2);
    gfx->setTextColor(COL_TITLE);
    const char* title = "DONKEY KONG";
    int tw = (int)strlen(title) * 12;
    gfx->setCursor(vx() + (VIEW_W - tw) / 2, vy() + VIEW_H / 2 - 18);
    gfx->print(title);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    const char* sub = "TAP A TO START";
#else
    const char* sub = "PRESS A / SPACE TO START";
#endif
    int sw = (int)strlen(sub) * 6;
    gfx->setCursor(vx() + (VIEW_W - sw) / 2, vy() + VIEW_H / 2 + 4);
    gfx->print(sub);

    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return false;
        if (in.a || in.b || in.start) return true;
        delay(24);
        yield();
    }
}

static void show_game_over(GameState& gs) {
    int boxW = (VIEW_W - 16 < 180) ? VIEW_W - 16 : 180;
    int boxH = 60;
    int boxX = vx() + (VIEW_W - boxW) / 2;
    int boxY = vy() + (VIEW_H - boxH) / 2;
    gfx->fillRect(boxX, boxY, boxW, boxH, 0x0000);
    gfx->drawRect(boxX, boxY, boxW, boxH, COL_TITLE);
    gfx->setTextSize(2);
    gfx->setTextColor(0xF800);
    gfx->setCursor(boxX + (boxW - 108) / 2, boxY + 10);
    gfx->print("GAME OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(boxX + 12, boxY + 34);
    gfx->printf("SCORE: %ld", gs.score);
    if (gs.score > gs.high_score && gs.score > 0) {
        gs.high_score = gs.score;
        save_high_score(gs.high_score);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(boxX + 12, boxY + 46);
        gfx->print("NEW HIGH SCORE!");
    }
    delay(2800);
}

// Reset state for the NEXT level. Preserves score, lives, high score;
// regenerates barrels/fireballs/hammers; freshens the bonus timer.
static void next_level_reset(GameState& gs) {
    gs.bonus               = BONUS_START;
    gs.barrel_spawn_timer  = 1.4f;   // grace period before the first throw
    gs.fireball_spawn_timer = 5.0f;
    gs.death_timer         = 0;
    gs.dk_anim_timer       = 0;
    gs.level_clear_timer   = 0;
    gs.level_clearing      = false;
    for (int i = 0; i < MAX_BARRELS; i++)   gs.barrels[i].active = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) gs.fireballs[i].active = false;
    place_jumpman_start(gs);
    place_hammers(gs);
}

static void reset_game(GameState& gs) {
    gs.level = 1;
    gs.lives = 3;
    gs.score = 0;
    gs.quit  = false;
    next_level_reset(gs);
}

}  // namespace dk

// ─────────────────────────────────────────────
//  PUBLIC ENTRY
// ─────────────────────────────────────────────
void run_donkey_kong() {
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_C5
    c5_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif

    // Project the canonical board into this device's pixels FIRST —
    // everything (title screen included) reads the projected layout.
    dk::project_board(dk::VIEW_W, dk::VIEW_H, dk::HUD_H);

    dk::GameState gs;
    gs.high_score = dk::load_high_score();

    if (!dk::wait_for_start()) {
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
        gfx->fillRect(0, 0, dk::VIEW_W, dk::VIEW_H, 0);
#else
        gfx->fillScreen(0);
#endif
        return;
    }

    pm_game_audio_begin();   // SFX HAL (jump/death); DK has no bg music
    dk::reset_game(gs);

    pmp::FixedTimestep ts;
    ts.reset(millis());

    while (gs.lives > 0 && !gs.quit) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) { gs.quit = true; break; }

        int n = ts.steps_due(millis());
        for (int i = 0; i < n; i++)
            dk::step_world(gs, in, pmp::FixedTimestep::FIXED_DT);

        // Level advance: once the level-clear flourish has finished
        // playing, escalate to the next level while preserving score,
        // lives, and high score. The fixed-timestep accumulator is
        // realigned so the dt = sim-time invariant holds across the
        // visible board reset.
        if (gs.level_clearing && gs.level_clear_timer <= 0.0f) {
            gs.level++;
            dk::next_level_reset(gs);
            ts.reset(millis());
        }

        dk::render(gs);

        delay(dk::FRAME_DELAY_MS);
        yield();
    }

    pm_game_audio_stop();
    dk::show_game_over(gs);

#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    gfx->fillRect(0, 0, dk::VIEW_W, dk::VIEW_H, 0);
#else
    gfx->fillScreen(0);
#endif
}
