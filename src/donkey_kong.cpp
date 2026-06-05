// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  donkey_kong.cpp — original arcade-style climbing game
//
//  CHUNK 1 — THE PROVING SKELETON.
//
//  This is the first of four build chunks (see donkey_kong.h). It
//  exists to prove three risky things on real hardware BEFORE any
//  barrels/fireballs are added:
//    1. The pmp::FixedTimestep accumulator runs the sim at a steady
//       60 Hz without stutter on the ESP32-S3.
//    2. Sloped-girder collision (height-function y=f(x)) feels right
//       underfoot — Jumpman rises/falls with the incline as he walks.
//    3. The fall-damage rule is tuned so a one-level drop is safe and
//       a two-level drop is fatal.
//
//  What's HERE: the canonical 25m board (girders + full/broken
//  ladders) in board-units, per-device projection, Jumpman walking
//  sloped girders, climbing FULL ladders, falling, fall damage, and
//  rendering. DK + Pauline are drawn as static decoration so the
//  board reads correctly.
//
//  What's NOT here yet: barrels (chunk 2), fireballs/hammers/scoring/
//  level-clear (chunk 3), five-device tuning + audio polish (chunk 4).
//  Launcher entry is C28P-only for now.
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
#elif defined(DEVICE_C28P)
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
static constexpr uint16_t COL_TITLE      = 0xFFE0;

// ─────────────────────────────────────────────
//  THE 25m BOARD (board-units; one source of truth)
//
//  Six girders bottom→top, 32-BU spacing, gentle ±6 BU alternating
//  slope (zig-zag). G5 is the flat top platform DK stands on. Ladder
//  endpoints (yt/yb) are pre-computed to sit on the girder surfaces
//  at their x. A full-ladder climb path exists: L0→L1→L2→L3→L4.
//  Broken ladders (for chunk-2 barrels) can't be climbed.
// ─────────────────────────────────────────────
static const GirderDef BOARD_GIRDERS[N_GIRDERS] = {
    //  xl   yl    xr   yr
    {   0, 244,  224, 244 },   // G0 bottom — FLAT, full width (safe floor, no walk-off)
    {  16, 202,  208, 214 },   // G1        — down-right
    {  16, 182,  208, 170 },   // G2        — down-left
    {  16, 138,  208, 150 },   // G3        — down-right
    {  16, 118,  208, 106 },   // G4        — down-left
    {  60,  80,  164,  80 },   // G5 top    — flat (DK platform)
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
// drift off the left/right screen edges (DK has no horizontal wrap).
// This does NOT prevent falling off the interior ENDS of the upper
// girders (those ends sit inside the board), so the drop-to-the-level-
// below mechanic still works; it only stops him leaving the world.
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

    // ── GROUNDED (IDLE / RUNNING) ──
    if (!airborne(p)) {
        // Climb intent takes priority over walking.
        if (in.up   && try_start_climb(gs, true,  false)) return;
        if (in.down && try_start_climb(gs, false, true )) return;

        // Horizontal walk (full ground control).
        if (in.left)       { p.vx = -g_walk; p.facing = -1; p.state = JM_RUNNING; }
        else if (in.right) { p.vx =  g_walk; p.facing = +1; p.state = JM_RUNNING; }
        else               { p.vx = 0;       p.state = JM_IDLE; }

        // Jump (rigid). HAMMERING (chunk 3) will disable this.
        if (in.a) { to_jumping(p); pm_game_audio_fx_jump(); }

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

// Barrels / fireballs arrive in chunks 2 and 3 — no-ops for now so the
// header contract is satisfied and step_world can call them uniformly.
void update_barrel(GameState& gs, int slot, float dt)   { (void)gs; (void)slot; (void)dt; }
void update_fireball(GameState& gs, int slot, float dt) { (void)gs; (void)slot; (void)dt; }

void step_world(GameState& gs, const PMNesInput& in, float dt) {
    update_jumpman(gs, in, dt);
    // (chunk 2) for each active barrel: update_barrel(gs, i, dt);
    // (chunk 3) for each active fireball: update_fireball(gs, i, dt);
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
}

// Static decoration so the board reads as DK (animated in chunk 3).
static void draw_dk_and_pauline() {
    // DK on the top-left of the upper platform (G5).
    const Girder& g5 = g_girders[5];
    int gx = (int)g5.x_left + 2;
    int gy = (int)girder_y_at(g5, g5.x_left + 2);
    int s  = (int)(20 * g_scale); if (s < 12) s = 12;
    gfx->fillRect(scr_x((float)gx), scr_y((float)(gy - s)), s, s, COL_DK_BODY);
    gfx->fillRect(scr_x((float)(gx + 3)), scr_y((float)(gy - s + 4)), s - 6, s / 3, COL_DK_FACE);
    // Pauline above DK.
    int px = gx + s + 4;
    int ph = (int)(14 * g_scale); if (ph < 9) ph = 9;
    gfx->fillRect(scr_x((float)px), scr_y((float)(gy - s - ph)), ph / 2 + 3, ph, COL_PAULINE);
    gfx->fillRect(scr_x((float)px), scr_y((float)(gy - s - ph)), ph / 2 + 3, 3, COL_JM_FACE);
}

static void draw_hud(const GameState& gs) {
    int hy = vy();
    gfx->fillRect(vx(), hy, VIEW_W, HUD_H, COL_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(vx() + 4, hy + 2);
    gfx->printf("SC %ld", gs.score);
    gfx->setCursor(vx() + VIEW_W / 2 - 24, hy + 2);
    gfx->printf("HI %ld", gs.high_score);
    gfx->setCursor(vx() + VIEW_W - 64, hy + 2);
    gfx->printf("L%d", gs.lives);
}

void render(const GameState& gs) {
    // Clear the play area only (touch kiosks own the strip below VIEW).
    gfx->fillRect(vx(), vy() + HUD_H, VIEW_W, VIEW_H - HUD_H, COL_BG);
    for (int i = 0; i < N_LADDERS; i++) draw_ladder(g_ladders[i]);   // behind girders
    for (int i = 0; i < N_GIRDERS; i++) draw_girder(g_girders[i]);
    draw_dk_and_pauline();
    draw_jumpman(gs.player);
    draw_hud(gs);
}

// ─────────────────────────────────────────────
//  TITLE / GAME OVER (static screens)
// ─────────────────────────────────────────────
static bool wait_for_start() {
    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, COL_BG);
    for (int i = 0; i < N_LADDERS; i++) draw_ladder(g_ladders[i]);
    for (int i = 0; i < N_GIRDERS; i++) draw_girder(g_girders[i]);
    draw_dk_and_pauline();

    gfx->setTextSize(2);
    gfx->setTextColor(COL_TITLE);
    const char* title = "DONKEY KONG";
    int tw = (int)strlen(title) * 12;
    gfx->setCursor(vx() + (VIEW_W - tw) / 2, vy() + VIEW_H / 2 - 18);
    gfx->print(title);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
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

static void reset_game(GameState& gs) {
    gs.level = 1;
    gs.lives = 3;
    gs.score = 0;
    gs.bonus = 5000;
    gs.barrel_spawn_timer = 0;
    gs.death_timer = 0;
    gs.quit = false;
    for (int i = 0; i < MAX_BARRELS; i++)   gs.barrels[i].active = false;
    for (int i = 0; i < MAX_FIREBALLS; i++) gs.fireballs[i].active = false;
    place_jumpman_start(gs);
}

}  // namespace dk

// ─────────────────────────────────────────────
//  PUBLIC ENTRY
// ─────────────────────────────────────────────
void run_donkey_kong() {
#ifdef DEVICE_C28P
    c28p_dpad_render();
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
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
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

        dk::render(gs);

        delay(dk::FRAME_DELAY_MS);
        yield();
    }

    pm_game_audio_stop();
    dk::show_game_over(gs);

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
    gfx->fillRect(0, 0, dk::VIEW_W, dk::VIEW_H, 0);
#else
    gfx->fillScreen(0);
#endif
}
