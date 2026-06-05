// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  mario_bros.cpp — original-arcade-style platform-flipping game
//
//  Inspired by the 1983 Mario Bros. arcade cabinet (NOT the 1985
//  NES side-scroller, which is a different genre entirely). This
//  implementation is from-scratch original code: our own engine,
//  our own physics, our own art, our own enemy AI and phase data.
//
//  GAMEPLAY
//
//  Fixed single-screen play field. Five horizontal platforms with
//  side wraparound (walk off left edge → reappear at right edge of
//  the same row). Two pipes at the top spawn enemies that fall
//  onto the highest platform and walk down through pipe holes at
//  the floor level. Player punches the platform from BELOW to flip
//  the enemy on the surface ABOVE that punch point; flipped
//  enemies can be kicked off the screen for points. Touching an
//  upright enemy costs a life.
//
//  POW BLOCK
//
//  Centered POW block has three uses. Punching it from below
//  stuns every grounded enemy at once. The block cracks visually
//  with each hit and disappears when all three uses are spent.
//  POW does not regenerate within a life — phase clear restores
//  it to fresh state.
//
//  ENEMIES
//
//    SHELLCREEPER (turtle) — slow walk, single flip to defeat.
//    SIDESTEPPER  (crab)   — two hits required; first hit angers
//                             it (color shifts, speeds up).
//    FIGHTER FLY  (insect) — moves in hops; only flippable mid-
//                             ground-contact between hops.
//
//  PHASES
//
//  Each phase has a target number of enemies and an enemy-mix.
//  Clear the phase by flipping-and-kicking every enemy. Speed and
//  spawn rate scale with phase. Surviving enemies that escape
//  flipping for too long turn red and accelerate. Every fourth
//  phase is an ICE phase: horizontal friction drops dramatically,
//  player slides on stop.
//
//  CONTROLS
//
//    Keyboard (T-Deck, Pager, Cardputer):
//      A / Left          move left
//      D / Right         move right
//      Space / W / B     jump
//      Q                 quit
//
//    Touch kiosks (C28P, Maxine):
//      Virtual D-pad left/right  move
//      Virtual D-pad A           jump
//      Top exit strip            quit
//
//  DEVICES
//
//  All five Pisces Moon devices. Per-device viewport scaling
//  picks a tile size and platform layout that keeps the play
//  field readable on each display.
//
//  HIGH SCORE
//
//  Saved to /mario_bros_hs.txt via SdFat (or SD_MMC on C28P).
//  Wrapped in spi_mutex on devices that share the SPI bus with
//  the Ghost Engine, matching the SPI Bus Treaty used by every
//  other game in the suite.
// ─────────────────────────────────────────────

#include <Arduino.h>
#include <math.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_input.h"
#include "game_audio.h"
#include "theme.h"
#include "mario_bros.h"

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

// ─────────────────────────────────────────────
//  PER-DEVICE GEOMETRY
//
//  Five-platform layout. PLAT_W is the full play-field width;
//  each platform spans the whole width minus two PIPE_W gaps
//  near the edges where pipes/wraparound openings live.
//  PLAT_YS is the y-coordinate of the TOP edge of each platform,
//  ordered top-to-bottom (PLAT_YS[0] is highest, PLAT_YS[4] is
//  the floor). Player walks ON TOP of each y in PLAT_YS, i.e.
//  their feet sit at PLAT_YS[n], head at PLAT_YS[n] - PLAYER_H.
//
//  POW_X/POW_Y is the top-left of the POW block. SPAWN_LX/RX are
//  the x-positions of the two pipes that drop enemies onto the
//  TOP platform.
// ─────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
static constexpr int VIEW_W   = 240;
static constexpr int VIEW_H   = 135;
static constexpr int HUD_H    = 10;
static constexpr int PLAT_T   = 3;     // platform tile thickness (pixels)
static constexpr int PLAT_W   = 240;
static constexpr int PIPE_W   = 14;    // wraparound gap each side
static constexpr int PLAYER_W = 9;
static constexpr int PLAYER_H = 12;
static constexpr int ENEMY_W  = 9;
static constexpr int ENEMY_H  = 9;
static constexpr int POW_W    = 18;
static constexpr int POW_H    = 9;
// Floor is the device viewport bottom; four floating platforms above.
// Four platforms fit cleanly in 125px of play area; the arcade's five
// would be too cramped on Cardputer.
static constexpr int N_PLATS  = 4;
static constexpr int PLAT_YS[N_PLATS] = { 38, 66, 94, 122 };
static constexpr int POW_X    = (VIEW_W - POW_W) / 2;
// POW sits low & centered, punched from the bottom floor (arcade).
static constexpr int POW_Y    = PLAT_YS[N_PLATS - 1] - PLAYER_H - POW_H - 6;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W   = 320;
static constexpr int VIEW_H   = 222;
static constexpr int HUD_H    = 12;
static constexpr int PLAT_T   = 4;
static constexpr int PLAT_W   = 320;
static constexpr int PIPE_W   = 22;
static constexpr int PLAYER_W = 14;
static constexpr int PLAYER_H = 18;
static constexpr int ENEMY_W  = 14;
static constexpr int ENEMY_H  = 14;
static constexpr int POW_W    = 26;
static constexpr int POW_H    = 12;
static constexpr int N_PLATS  = 5;
static constexpr int PLAT_YS[N_PLATS] = { 50, 88, 126, 164, 210 };
static constexpr int POW_X    = (VIEW_W - POW_W) / 2;
// POW sits low & centered, punched from the bottom floor (arcade).
static constexpr int POW_Y    = PLAT_YS[N_PLATS - 1] - PLAYER_H - POW_H - 6;
#elif defined(DEVICE_C28P)
// C28P 240×320 portrait. Game viewport is the top 200px (the bottom
// 120px holds the virtual D-pad chrome). HUD lives in a 14px strip
// inside the dpad's exit bar at y=0..13; the play field starts at
// y=14. Five platforms fit comfortably in 186px of play area.
static constexpr int VIEW_W   = 240;
static constexpr int VIEW_H   = 200;
static constexpr int HUD_H    = 14;
static constexpr int PLAT_T   = 4;
static constexpr int PLAT_W   = 240;
static constexpr int PIPE_W   = 18;
static constexpr int PLAYER_W = 12;
static constexpr int PLAYER_H = 16;
static constexpr int ENEMY_W  = 12;
static constexpr int ENEMY_H  = 12;
static constexpr int POW_W    = 22;
static constexpr int POW_H    = 10;
static constexpr int N_PLATS  = 5;
static constexpr int PLAT_YS[N_PLATS] = { 46, 78, 110, 142, 190 };
static constexpr int POW_X    = (VIEW_W - POW_W) / 2;
// POW sits low & centered, punched from the bottom floor (arcade).
static constexpr int POW_Y    = PLAT_YS[N_PLATS - 1] - PLAYER_H - POW_H - 6;
#elif defined(DEVICE_MAXINE)
// Maxine 480×800 portrait. Game viewport top 520px above the
// virtual D-pad chrome at y >= 520. Spacious — platforms get the
// full visual treatment the arcade original had room for.
static constexpr int VIEW_W   = 480;
static constexpr int VIEW_H   = 520;
static constexpr int HUD_H    = 22;
static constexpr int PLAT_T   = 7;
static constexpr int PLAT_W   = 480;
static constexpr int PIPE_W   = 38;
static constexpr int PLAYER_W = 26;
static constexpr int PLAYER_H = 34;
static constexpr int ENEMY_W  = 26;
static constexpr int ENEMY_H  = 26;
static constexpr int POW_W    = 48;
static constexpr int POW_H    = 22;
static constexpr int N_PLATS  = 5;
static constexpr int PLAT_YS[N_PLATS] = { 110, 196, 282, 368, 500 };
static constexpr int POW_X    = (VIEW_W - POW_W) / 2;
// POW sits low & centered, punched from the bottom floor (arcade).
static constexpr int POW_Y    = PLAT_YS[N_PLATS - 1] - PLAYER_H - POW_H - 6;
#else
// T-Deck Plus 320×240. Five platforms in the standard arcade
// vertical proportions. HUD strip at the very top of the viewport.
static constexpr int VIEW_W   = 320;
static constexpr int VIEW_H   = 240;
static constexpr int HUD_H    = 14;
static constexpr int PLAT_T   = 5;
static constexpr int PLAT_W   = 320;
static constexpr int PIPE_W   = 24;
static constexpr int PLAYER_W = 16;
static constexpr int PLAYER_H = 20;
static constexpr int ENEMY_W  = 16;
static constexpr int ENEMY_H  = 16;
static constexpr int POW_W    = 30;
static constexpr int POW_H    = 14;
static constexpr int N_PLATS  = 5;
static constexpr int PLAT_YS[N_PLATS] = { 54, 96, 138, 180, 230 };
static constexpr int POW_X    = (VIEW_W - POW_W) / 2;
// POW sits low & centered, punched from the bottom floor (arcade).
static constexpr int POW_Y    = PLAT_YS[N_PLATS - 1] - PLAYER_H - POW_H - 6;
#endif

static constexpr int SPAWN_LX = PIPE_W + 4;                 // left pipe column
static constexpr int SPAWN_RX = VIEW_W - PIPE_W - 4;        // right pipe column

// ─────────────────────────────────────────────
//  PLATFORM GAPS (arcade-style descent)
//
//  Each floating platform has a hole. An entity whose CENTER passes
//  over the hole falls through to the platform below. This is the
//  defining 1983-arcade behavior: enemies emerge from the top pipes
//  and snake DOWN through the structure, giving the player a chance
//  to flip them from below at every level — and the player can drop
//  through gaps to chase them. The bottom floor has NO gap.
//
//  Gaps alternate sides by platform parity so a walker descends in a
//  zig-zag instead of straight down one column (even rows: hole to
//  the right of center; odd rows: hole to the left).
// ─────────────────────────────────────────────
static constexpr int PLAT_GAP_W = ENEMY_W * 3;

static inline int plat_gap_left(int i) {
    int left_pos  = VIEW_W / 4 - PLAT_GAP_W / 2;          // ~25% across
    int right_pos = (VIEW_W * 3) / 4 - PLAT_GAP_W / 2;    // ~75% across
    return (i & 1) ? left_pos : right_pos;
}

// True if a given center-x sits over the hole in floating platform i.
// The bottom floor (i == N_PLATS-1) is solid and always returns false.
static inline bool over_gap(float center_x, int plat_i) {
    if (plat_i < 0 || plat_i >= N_PLATS - 1) return false;
    float gl = (float)plat_gap_left(plat_i);
    return center_x >= gl && center_x < gl + (float)PLAT_GAP_W;
}

// ─────────────────────────────────────────────
//  VIEWPORT HELPERS
//
//  vx()/vy() compute the top-left of our centered play area on
//  devices whose physical display is larger than VIEW_W/VIEW_H.
//  Touch kiosks anchor the play area to (0, 0) so the bottom strip
//  belongs to the virtual D-pad chrome. Everything in the game
//  computes pixel coordinates relative to VIEW_W/VIEW_H and adds
//  vx()/vy() at draw time only.
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

// ─────────────────────────────────────────────
//  COLORS (RGB565)
// ─────────────────────────────────────────────
static constexpr uint16_t COL_BG          = 0x0000;
static constexpr uint16_t COL_HUD_TEXT    = 0xFFFF;
static constexpr uint16_t COL_PLATFORM_A  = 0x9CDF;   // light blue
static constexpr uint16_t COL_PLATFORM_B  = 0x4A1F;   // darker blue trim
static constexpr uint16_t COL_FLOOR_A     = 0xC2A8;   // brown brick face
static constexpr uint16_t COL_FLOOR_B     = 0x6A40;   // brown brick mortar
static constexpr uint16_t COL_PIPE        = 0x07E0;   // green pipe
static constexpr uint16_t COL_PIPE_DARK   = 0x0320;
static constexpr uint16_t COL_PLAYER_BODY = 0xF800;   // red shirt
static constexpr uint16_t COL_PLAYER_HEAD = 0xFD60;   // tan face
static constexpr uint16_t COL_PLAYER_LEGS = 0x07FF;   // cyan overalls
static constexpr uint16_t COL_POW_NORMAL  = 0x07E0;   // green POW (3 uses)
static constexpr uint16_t COL_POW_CRACK   = 0xFFE0;   // yellow POW (2 uses)
static constexpr uint16_t COL_POW_WEAK    = 0xFD20;   // orange POW (1 use)
static constexpr uint16_t COL_POW_TEXT    = 0x0000;
static constexpr uint16_t COL_SHELL       = 0x07E0;   // green turtle shell
static constexpr uint16_t COL_SHELL_DARK  = 0x0320;
static constexpr uint16_t COL_CRAB        = 0xFD60;   // tan crab
static constexpr uint16_t COL_CRAB_ANGRY  = 0xF800;
static constexpr uint16_t COL_FLY         = 0xFBE0;   // amber fly
static constexpr uint16_t COL_FLY_DARK    = 0x8400;
static constexpr uint16_t COL_FLIPPED     = 0xFFE0;   // yellow flipped state
static constexpr uint16_t COL_ICE         = 0xCFFF;   // icy blue tint
static constexpr uint16_t COL_TEXT_TITLE  = 0xFFE0;

// ─────────────────────────────────────────────
//  PHYSICS CONSTANTS
//
//  All physics is in pixels-per-second so the gameplay reads the
//  same regardless of frame rate. dt is computed per-frame and
//  used to scale every velocity-driven update.
//
//  JUMP_VEL is device-specific because per-device PLAT_YS layouts
//  have very different floor-to-lowest-platform gaps. The math:
//
//     max jump height = JUMP_VEL² / (2 * GRAVITY)
//
//  With GRAVITY = 380, JUMP_VEL = -210 gives ~58 px of jump
//  height, which clears all the small-device gaps (max 50 px on
//  T-Deck Plus) with comfortable margin. Maxine's 800-tall layout
//  has platform gaps up to 132 px and needs a much stronger jump.
//
//  Earlier value JUMP_VEL = -180 produced only ~43 px of jump, less
//  than the C28P/T-Deck/Pager floor-to-platform gap — Mario was
//  physically incapable of reaching any floating platform and the
//  game appeared completely broken. User reported "there is no way
//  to jump levels."
// ─────────────────────────────────────────────
static constexpr float GRAVITY     = 380.0f;   // px/s²
#ifdef DEVICE_MAXINE
static constexpr float JUMP_VEL    = -340.0f;  // ~152px jump for 132px gaps
#else
static constexpr float JUMP_VEL    = -210.0f;  // ~58px jump for ≤50px gaps
#endif
static constexpr float WALK_SPEED  = 70.0f;    // base horizontal speed
static constexpr float WALK_ACCEL  = 280.0f;
static constexpr float WALK_FRIC_NORMAL = 360.0f;
static constexpr float WALK_FRIC_ICE    = 30.0f;
static constexpr float MAX_FALL    = 260.0f;
static constexpr float ENEMY_BASE_SPEED = 22.0f;
static constexpr float ENEMY_FALL_SPEED = 100.0f;
static constexpr float FLY_HOP_VEL = -110.0f;

// ─────────────────────────────────────────────
//  ENEMY KINDS / STATES
// ─────────────────────────────────────────────
enum EnemyKind : uint8_t {
    EK_SHELL = 0,    // Shellcreeper — turtle, 1 hit to flip
    EK_CRAB  = 1,    // Sidestepper — crab, 2 hits (1 angers, 2 flips)
    EK_FLY   = 2,    // Fighter Fly — hops, only flippable on ground
};

enum EnemyState : uint8_t {
    ES_SPAWNING = 0, // dropping from pipe onto top platform
    ES_WALKING  = 1, // active patrol
    ES_FLIPPED  = 2, // on its back, kickable, recover timer counting
    ES_DEAD     = 3, // off-screen / kicked, slot reusable
};

struct Enemy {
    EnemyKind  kind;
    EnemyState state;
    float x, y;          // top-left pixel position
    float vx, vy;
    int   plat;          // index into PLAT_YS we're sitting on (or -1 if airborne)
    int   dir;           // -1 = left, +1 = right
    int   hp;            // crab starts 2, others 1
    float flip_timer;    // seconds remaining on-back before recovery
    float hop_timer;     // fly: time until next hop launch
    bool  angry;         // crab: post-first-hit acceleration
};

static constexpr int MAX_ENEMIES = 6;
static Enemy enemies[MAX_ENEMIES];

// ─────────────────────────────────────────────
//  PHASE TABLE
//
//  Per-phase: which kinds spawn, total count, base speed multiplier,
//  ice flag. The first phase eases new players in with three slow
//  Shellcreepers; subsequent phases add crabs, then flies, then mix.
//  Phases loop with escalating speed after the table ends.
// ─────────────────────────────────────────────
struct PhaseDef {
    uint8_t total;      // total enemies in this phase
    uint8_t k0, k1, k2; // enemy kinds to cycle through (0..2)
    float   speed_mul;
    bool    ice;
};

static const PhaseDef PHASE_TABLE[] = {
    //  total   k0       k1       k2       speed   ice
    {   3,  EK_SHELL, EK_SHELL, EK_SHELL,  1.00f, false },  //  1
    {   4,  EK_SHELL, EK_CRAB,  EK_SHELL,  1.10f, false },  //  2
    {   5,  EK_CRAB,  EK_SHELL, EK_CRAB,   1.20f, false },  //  3
    {   5,  EK_CRAB,  EK_FLY,   EK_CRAB,   1.25f, true  },  //  4 (ice)
    {   6,  EK_SHELL, EK_FLY,   EK_CRAB,   1.35f, false },  //  5
    {   6,  EK_CRAB,  EK_FLY,   EK_CRAB,   1.45f, false },  //  6
    {   6,  EK_FLY,   EK_CRAB,  EK_FLY,    1.55f, false },  //  7
    {   7,  EK_FLY,   EK_CRAB,  EK_FLY,    1.60f, true  },  //  8 (ice)
    {   7,  EK_CRAB,  EK_FLY,   EK_CRAB,   1.75f, false },  //  9
    {   8,  EK_FLY,   EK_CRAB,  EK_FLY,    1.90f, false },  // 10
};
static constexpr int N_PHASE_DEFS = sizeof(PHASE_TABLE) / sizeof(PhaseDef);

// ─────────────────────────────────────────────
//  GAME STATE
// ─────────────────────────────────────────────
static int   phase;             // 1-indexed phase counter
static int   lives;
static long  score;
static long  high_score;
static int   enemies_spawned_this_phase;
static int   enemies_alive;
static int   spawn_index;       // which k0/k1/k2 to use next
static float spawn_cooldown;    // seconds until next pipe spawn allowed
static int   pow_uses;          // 3..0, 0 = block gone
static bool  ice_active;        // current phase is ice?
static float speed_mul;         // current phase speed multiplier

// Player
static float p_x, p_y;          // top-left
static float p_vx, p_vy;
static int   p_facing;          // -1 / +1
static bool  p_on_ground;
static int   p_plat;            // current platform index, -1 if airborne
static float p_invuln;          // post-respawn brief invulnerability (seconds)
static float p_punch_cooldown;  // prevents double-flipping in one jump

// POW block visual state (number of cracks rendered = 3 - pow_uses)
static bool  pow_visible;

// Frame timing
static uint32_t last_frame_ms;
static bool     game_quit;

// HS file path
static constexpr const char* HS_PATH = "/mario_bros_hs.txt";

// ─────────────────────────────────────────────
//  HIGH-SCORE I/O (SPI Bus Treaty)
// ─────────────────────────────────────────────
static long load_high_score() {
    long hs = 0;
#ifdef DEVICE_C28P
    // C28P uses SD_MMC, no SdFat instance. The bus is dedicated
    // (SDIO 4-bit, no SPI sharing) so no mutex is needed.
    if (SD_MMC.exists(HS_PATH)) {
        fs::File f = SD_MMC.open(HS_PATH, FILE_READ);
        if (f) {
            char buf[24] = {0};
            f.read((uint8_t*)buf, 23);
            f.close();
            hs = atol(buf);
        }
    }
#else
    if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (sd.exists(HS_PATH)) {
            FsFile f = sd.open(HS_PATH, O_READ);
            if (f) {
                char buf[24] = {0};
                f.read(buf, 23);
                f.close();
                hs = atol(buf);
            }
        }
        xSemaphoreGiveRecursive(spi_mutex);
    }
#endif
    return hs;
}

static void save_high_score(long hs) {
#ifdef DEVICE_C28P
    fs::File f = SD_MMC.open(HS_PATH, FILE_WRITE);
    if (f) {
        f.printf("%ld", hs);
        f.close();
    }
#else
    if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        FsFile f = sd.open(HS_PATH, O_WRITE | O_CREAT | O_TRUNC);
        if (f) {
            f.printf("%ld", hs);
            f.close();
        }
        xSemaphoreGiveRecursive(spi_mutex);
    }
#endif
}

// ─────────────────────────────────────────────
//  COORDINATE HELPERS
// ─────────────────────────────────────────────
static inline int scr_x(float gx) { return vx() + (int)gx; }
static inline int scr_y(float gy) { return vy() + (int)gy; }

// Convert PLAT_YS index → screen y for the top edge of that platform.
static inline int plat_screen_y(int i) { return scr_y((float)PLAT_YS[i]); }

// Is `feet_y` (a player or enemy's bottom edge in game coords) sitting
// on the top of platform `i`? Allows a tiny tolerance so floating-point
// drift doesn't unstick a walker. Returns true iff feet_y is within
// 2px of PLAT_YS[i].
static inline bool on_platform(float feet_y, int i) {
    float d = feet_y - (float)PLAT_YS[i];
    return d > -2.0f && d < 2.0f;
}

// Find the platform index whose top-edge is the highest one BELOW
// the given y (i.e. the platform we'd land on if we kept falling).
// Returns -1 if no platform below.
static int next_platform_below(float top_edge_y) {
    int best = -1;
    int best_y = INT_MAX;
    for (int i = 0; i < N_PLATS; i++) {
        if (PLAT_YS[i] > top_edge_y && PLAT_YS[i] < best_y) {
            best = i;
            best_y = PLAT_YS[i];
        }
    }
    return best;
}

// Wraparound for x: walking off the left edge re-enters at the right
// edge and vice versa. Returns x mapped into [0, VIEW_W). Operates on
// the entity's *center* x so the visual transition is symmetric.
static float wrap_x(float x, int width) {
    float cx = x + (float)width / 2;
    if (cx < 0)          x += (float)VIEW_W;
    else if (cx >= (float)VIEW_W) x -= (float)VIEW_W;
    return x;
}

// ─────────────────────────────────────────────
//  ENEMY MANAGEMENT
// ─────────────────────────────────────────────
static int find_free_enemy_slot() {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].state == ES_DEAD) return i;
    }
    return -1;
}

static void spawn_enemy_from_pipe(int slot, EnemyKind kind, bool from_right) {
    Enemy& e = enemies[slot];
    e.kind  = kind;
    e.state = ES_SPAWNING;
    e.x = from_right ? (float)(SPAWN_RX - ENEMY_W / 2) : (float)(SPAWN_LX);
    e.y = (float)(HUD_H + 2);
    e.vx = 0;
    e.vy = ENEMY_FALL_SPEED;
    e.plat = -1;
    e.dir = from_right ? -1 : +1;
    e.hp = (kind == EK_CRAB) ? 2 : 1;
    e.flip_timer = 0;
    e.hop_timer = 0.4f + (float)random(0, 800) / 1000.0f;
    e.angry = false;
    enemies_alive++;
    enemies_spawned_this_phase++;
}

static EnemyKind next_kind_for_phase() {
    const PhaseDef& pd = PHASE_TABLE[(phase - 1) % N_PHASE_DEFS];
    uint8_t k;
    switch (spawn_index % 3) {
        case 0:  k = pd.k0; break;
        case 1:  k = pd.k1; break;
        default: k = pd.k2; break;
    }
    spawn_index++;
    return (EnemyKind)k;
}

static void start_phase(int p) {
    phase = p;
    enemies_spawned_this_phase = 0;
    enemies_alive = 0;
    spawn_index = 0;
    spawn_cooldown = 0.8f;
    pow_uses = 3;
    pow_visible = true;
    for (int i = 0; i < MAX_ENEMIES; i++) enemies[i].state = ES_DEAD;
    const PhaseDef& pd = PHASE_TABLE[(p - 1) % N_PHASE_DEFS];
    ice_active = pd.ice;
    // Speed multiplier scales gently past the end of the table so
    // late-game phases stay distinctly harder rather than flat.
    int loop_count = (p - 1) / N_PHASE_DEFS;
    speed_mul = pd.speed_mul + 0.20f * (float)loop_count;
}

static int phase_total() {
    return PHASE_TABLE[(phase - 1) % N_PHASE_DEFS].total;
}

// ─────────────────────────────────────────────
//  COLLISION / GROUNDING — swept landing detection
//
//  Returns the topmost platform whose top edge an entity's feet
//  crossed going downward this frame, or -1 if no crossing.
//
//  WHY SWEPT, NOT POINT-CHECK:
//
//  An earlier version used a point-in-window check (on_platform()'s
//  ±2 px tolerance) to test "are my feet currently sitting on this
//  platform?" That works fine at low vertical velocity, but at
//  MAX_FALL = 260 px/s with the dt clamp of 0.08, a single frame
//  can move 20.8 px — completely skipping the 4 px detection
//  window. Even at the normal dt = 0.016 cap, a falling entity
//  approaching MAX_FALL moves ~4 px per frame, right at the edge.
//
//  The result was every entity (player included) periodically
//  tunneling through platforms and falling off the bottom of the
//  playfield. User reported it as "stuff just happens then runs
//  off the bottom of the playing surface like it melted."
//
//  Swept collision checks the line segment between previous and
//  current feet positions. If that segment crosses PLAT_YS[i] from
//  above going downward, we landed on platform i. Robust to any
//  velocity and any frame-time hitch.
//
//  Horizontal span check is now gap-aware: a platform is skipped if
//  the entity's center is over that platform's hole (see over_gap),
//  so entities fall straight through gaps to descend the structure.
// ─────────────────────────────────────────────
static int find_platform_crossed(float prev_y, float curr_y, int h, float center_x) {
    float prev_feet = prev_y + (float)h;
    float curr_feet = curr_y + (float)h;
    if (curr_feet < prev_feet) return -1;   // not moving down — no landing

    int   best   = -1;
    float best_py = 1e9f;
    for (int i = 0; i < N_PLATS; i++) {
        if (over_gap(center_x, i)) continue;   // fall through the hole
        float py = (float)PLAT_YS[i];
        // Feet crossed PLAT_YS[i] going down this frame iff prev
        // was at-or-above py and curr is at-or-below py.
        if (prev_feet <= py && curr_feet >= py) {
            // At very large dt the segment can cross multiple
            // platforms; the entity lands on the highest one it
            // would have hit first (smallest PLAT_YS value).
            if (py < best_py) {
                best   = i;
                best_py = py;
            }
        }
    }
    return best;
}

// Legacy point-check helper retained because next_platform_below()
// and the fly-hop on_platform() guard still use the ±2 px tolerance
// for stationary checks. Not used for landing detection — see
// find_platform_crossed() above.
static int find_ground_platform(float x, float y, int w, int h) {
    float feet = y + (float)h;
    (void)x; (void)w;
    for (int i = 0; i < N_PLATS; i++) {
        if (on_platform(feet, i)) return i;
    }
    return -1;
}

// AABB overlap between two boxes given top-left corners + sizes.
static bool aabb(float ax, float ay, int aw, int ah,
                 float bx, float by, int bw, int bh) {
    return ax < bx + (float)bw &&
           ax + (float)aw > bx &&
           ay < by + (float)bh &&
           ay + (float)ah > by;
}

// ─────────────────────────────────────────────
//  POW BLOCK
//
//  Punched when the player is jumping up (vy < 0) and their head
//  passes through the POW block's lower-edge zone. Each punch
//  decrements pow_uses and flips every grounded enemy. At 0 uses
//  the block is gone (pow_visible = false) and no further punches
//  register.
// ─────────────────────────────────────────────
static void apply_pow_hit() {
    if (!pow_visible) return;
    pow_uses--;
    if (pow_uses <= 0) pow_visible = false;
    score += 25;
    pm_game_audio_fx_pow();
    // Flip every grounded, walking enemy. Flies in their hop are
    // unaffected (consistent with the arcade rule that POW shakes
    // the floor, not the air).
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy& e = enemies[i];
        if (e.state != ES_WALKING) continue;
        if (e.plat < 0) continue;
        if (e.kind == EK_FLY && !on_platform(e.y + ENEMY_H, e.plat)) continue;
        e.state = ES_FLIPPED;
        e.flip_timer = 4.0f;
        e.vx = 0;
        if (e.kind == EK_CRAB) {
            // POW finishes the crab regardless of hp — different from
            // a punch from below, which only angers an unangered crab.
            e.hp = 0;
        }
    }
}

static bool player_punching_pow() {
    if (!pow_visible) return false;
    if (p_punch_cooldown > 0) return false;
    if (p_vy >= 0) return false;
    // Head of player overlapping bottom edge of POW block?
    float head_y = p_y;
    float pow_bottom = (float)(POW_Y + POW_H);
    if (head_y > pow_bottom + 4) return false;
    if (head_y < pow_bottom - 8) return false;
    // Horizontal overlap?
    if (p_x + (float)PLAYER_W < (float)POW_X) return false;
    if (p_x > (float)(POW_X + POW_W)) return false;
    return true;
}

// ─────────────────────────────────────────────
//  PUNCH-FROM-BELOW DETECTION
//
//  When the player's head crosses a platform's underside from
//  below (vy < 0, head_y was above the platform bottom last frame
//  and is now within reach), any enemy whose feet rest on that
//  platform's TOP within a horizontal punch-window centered on
//  the player's x gets flipped. The punch-window is half the
//  player's width on each side of the player's center.
// ─────────────────────────────────────────────
static void apply_platform_punch(int plat_idx) {
    if (p_punch_cooldown > 0) return;
    p_punch_cooldown = 0.30f;   // ~third of a second between punches

    float pcx = p_x + (float)PLAYER_W / 2;
    float window = (float)PLAYER_W * 1.5f;
    bool  flipped_any = false;

    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy& e = enemies[i];
        if (e.state != ES_WALKING) continue;
        if (e.plat != plat_idx) continue;
        if (e.kind == EK_FLY && !on_platform(e.y + ENEMY_H, e.plat)) continue;
        float ecx = e.x + (float)ENEMY_W / 2;
        if (fabsf(ecx - pcx) > window) continue;
        // Hit. Crabs absorb the first hit as anger; second hit flips.
        if (e.kind == EK_CRAB && !e.angry) {
            e.angry = true;
            e.vx *= 1.3f;
            score += 10;
            continue;
        }
        e.state = ES_FLIPPED;
        e.flip_timer = 3.5f;
        e.vx = 0;
        score += 10;
        flipped_any = true;
    }
    if (flipped_any) pm_game_audio_fx_flip();
}

// ─────────────────────────────────────────────
//  PLAYER / ENEMY COLLISION (kick or die)
//
//  Flipped enemy → kick (player gains score, enemy goes ES_DEAD).
//  Walking enemy → player loses a life and respawns at the floor.
//  Brief invulnerability after respawn prevents instant re-death.
// ─────────────────────────────────────────────
static void player_die();

static void check_player_enemy_collisions() {
    if (p_invuln > 0) return;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy& e = enemies[i];
        if (e.state == ES_DEAD) continue;
        if (e.state == ES_SPAWNING) continue;
        if (!aabb(p_x, p_y, PLAYER_W, PLAYER_H,
                  e.x, e.y, ENEMY_W, ENEMY_H)) continue;

        if (e.state == ES_FLIPPED) {
            // Kick. Direction of kick = side of player → enemy.
            float pcx = p_x + (float)PLAYER_W / 2;
            float ecx = e.x + (float)ENEMY_W / 2;
            int kick_dir = (ecx > pcx) ? +1 : -1;
            e.vx = (float)kick_dir * 220.0f;
            e.vy = -80.0f;
            e.state = ES_DEAD;     // becomes inert once airborne
            enemies_alive--;
            // Score by kind. Sidesteppers worth most because they
            // took two hits to flip.
            int pts = (e.kind == EK_SHELL) ? 800
                    : (e.kind == EK_CRAB)  ? 1200
                    :                        1500;  // EK_FLY
            score += pts;
            pm_game_audio_fx_kick();
        } else {
            player_die();
            return;
        }
    }
}

// ─────────────────────────────────────────────
//  PLAYER RESPAWN / DEATH
// ─────────────────────────────────────────────
static void place_player_at_floor() {
    p_x  = (float)(VIEW_W / 2 - PLAYER_W / 2);
    p_y  = (float)(PLAT_YS[N_PLATS - 1] - PLAYER_H);
    p_vx = 0;
    p_vy = 0;
    p_facing = +1;
    p_on_ground = true;
    p_plat = N_PLATS - 1;
    p_invuln = 1.5f;
    p_punch_cooldown = 0;
}

static void player_die() {
    lives--;
    pm_game_audio_fx_die();
    place_player_at_floor();
}

// ─────────────────────────────────────────────
//  UPDATE LOOPS
// ─────────────────────────────────────────────
static void update_player(float dt, const PMNesInput& in) {
    p_invuln = fmaxf(0.0f, p_invuln - dt);
    p_punch_cooldown = fmaxf(0.0f, p_punch_cooldown - dt);

    // Horizontal input
    float target_vx = 0;
    if (in.left)  { target_vx -= WALK_SPEED; p_facing = -1; }
    if (in.right) { target_vx += WALK_SPEED; p_facing = +1; }

    // Acceleration / friction (slower friction on ice)
    if (target_vx != 0) {
        if (p_vx < target_vx)      p_vx = fminf(target_vx, p_vx + WALK_ACCEL * dt);
        else if (p_vx > target_vx) p_vx = fmaxf(target_vx, p_vx - WALK_ACCEL * dt);
    } else {
        float fric = ice_active ? WALK_FRIC_ICE : WALK_FRIC_NORMAL;
        if (p_vx > 0)      p_vx = fmaxf(0.0f, p_vx - fric * dt);
        else if (p_vx < 0) p_vx = fminf(0.0f, p_vx + fric * dt);
    }

    // Jump (only when grounded)
    bool jump_pressed = in.a || in.b || in.up;
    if (jump_pressed && p_on_ground) {
        p_vy = JUMP_VEL;
        p_on_ground = false;
        p_plat = -1;
        pm_game_audio_fx_jump();   // arcade jump blip
    }

    // Gravity
    p_vy = fminf(MAX_FALL, p_vy + GRAVITY * dt);

    // Save previous head_y for platform-underside crossing detection.
    float prev_head_y = p_y;

    // Integrate
    p_x += p_vx * dt;
    p_y += p_vy * dt;

    // Horizontal wrap
    p_x = wrap_x(p_x, PLAYER_W);

    // Platform landing / punching
    if (p_vy >= 0) {
        // Falling/standing: swept landing check. Did our feet cross
        // any platform top going down this frame? Sweep (not point)
        // collision so high vy or a hitched frame can't tunnel us.
        // Gap-aware: if our center is over a platform's hole we fall
        // straight through it (this also handles walking off a gap
        // edge — a grounded player over the hole stops matching its
        // platform and starts falling).
        int gp = find_platform_crossed(prev_head_y, p_y, PLAYER_H,
                                       p_x + (float)PLAYER_W / 2);
        if (gp >= 0) {
            p_y = (float)(PLAT_YS[gp] - PLAYER_H);
            p_vy = 0;
            p_on_ground = true;
            p_plat = gp;
        } else {
            p_on_ground = false;
            p_plat = -1;
        }
    } else {
        // Rising: did our head pass through a platform's bottom from
        // below? Check each platform; if head_y crosses (PLAT_Y - 1)
        // → that's a punch on the platform above. A platform whose
        // hole we're under is skipped — the head passes through it.
        float pcx = p_x + (float)PLAYER_W / 2;
        for (int i = 0; i < N_PLATS - 1; i++) {  // exclude floor
            if (over_gap(pcx, i)) continue;
            float plat_bottom = (float)PLAT_YS[i] + (float)PLAT_T;
            if (prev_head_y >= plat_bottom && p_y < plat_bottom) {
                apply_platform_punch(i);
                // Bonk: reset vy so player doesn't fly up through.
                p_vy = 40.0f;
                p_y = plat_bottom;
                break;
            }
        }
        // POW block punch (lives on no platform — separate check)
        if (player_punching_pow()) {
            apply_pow_hit();
            p_vy = 60.0f;
            p_punch_cooldown = 0.35f;
        }
    }

    // Safety net: if the player ended up below the playfield, the
    // swept landing check should have caught the floor — but if a
    // pathological frame somehow slips past it, respawn rather than
    // letting Mario fall forever off-screen.
    if (p_y > (float)VIEW_H) {
        player_die();
    }
}

static void update_enemy(int slot, float dt) {
    Enemy& e = enemies[slot];
    if (e.state == ES_DEAD) {
        // If still on-screen and airborne (kicked), fly off.
        if (e.x < -ENEMY_W * 2 || e.x > VIEW_W + ENEMY_W) return;
        e.vy = fminf(MAX_FALL, e.vy + GRAVITY * dt);
        e.x += e.vx * dt;
        e.y += e.vy * dt;
        return;
    }

    if (e.state == ES_FLIPPED) {
        e.flip_timer -= dt;
        if (e.flip_timer <= 0) {
            // Recovers — back to walking, faster + reversed direction.
            e.state = ES_WALKING;
            e.angry = true;
            e.dir = -e.dir;
            e.vx = (float)e.dir * ENEMY_BASE_SPEED * speed_mul * 1.4f;
            e.vy = 0;
        }
        return;
    }

    if (e.state == ES_SPAWNING) {
        // Falling from pipe; settle on top platform. Swept landing
        // check (capture prev_y BEFORE integrating) so the enemy
        // can't tunnel through the top platform at high vy.
        float prev_y = e.y;
        e.vy = fminf(MAX_FALL, e.vy + GRAVITY * dt);
        e.y += e.vy * dt;
        int gp = find_platform_crossed(prev_y, e.y, ENEMY_H, e.x + (float)ENEMY_W / 2);
        if (gp >= 0) {
            e.y = (float)(PLAT_YS[gp] - ENEMY_H);
            e.plat = gp;
            e.state = ES_WALKING;
            float base = ENEMY_BASE_SPEED * speed_mul;
            if (e.kind == EK_CRAB && e.angry) base *= 1.3f;
            e.vx = (float)e.dir * base;
            e.vy = 0;
            return;
        }
        // Off-bottom safety: ES_SPAWNING enemy that missed every
        // platform (shouldn't happen with sweep, but defense-in-depth)
        // gets reaped so the phase-clear counter can advance.
        if (e.y > (float)VIEW_H + (float)ENEMY_H) {
            e.state = ES_DEAD;
            enemies_alive--;
        }
        return;
    }

    // ES_WALKING
    // Fighter Flies periodically hop off their platform. Leaving the
    // surface makes them airborne; gravity (below) brings them back.
    if (e.kind == EK_FLY) {
        e.hop_timer -= dt;
        if (e.hop_timer <= 0 && e.plat >= 0 &&
            on_platform(e.y + ENEMY_H, e.plat)) {
            e.vy = FLY_HOP_VEL;
            e.plat = -1;               // leaves the surface
            e.hop_timer = 0.7f + (float)random(0, 400) / 1000.0f;
        }
    }

    // Walk-off-gap: a grounded walker whose center is over the hole in
    // its platform drops through it, descending the structure exactly
    // like the arcade. The bottom floor has no hole (over_gap false).
    if (e.plat >= 0 && over_gap(e.x + (float)ENEMY_W / 2, e.plat)) {
        e.plat = -1;
    }

    // Gravity applies whenever airborne (mid-hop, or having just
    // dropped through a gap). Grounded walkers keep vy = 0 and patrol.
    if (e.plat < 0) {
        e.vy = fminf(MAX_FALL, e.vy + GRAVITY * dt);
    }

    float prev_y = e.y;
    e.x += e.vx * dt;
    e.y += e.vy * dt;

    // Bottom-floor walkers that reach a screen edge re-circulate to a
    // top pipe (arcade side-pipe behavior) instead of wrapping across
    // the floor. This keeps every enemy reachable from below so the
    // phase can always be cleared, and matches the arcade's loop where
    // an enemy that escapes down the structure comes back around.
    if (e.plat == N_PLATS - 1) {
        float ecx = e.x + (float)ENEMY_W / 2;
        if (ecx < 0.0f || ecx >= (float)VIEW_W) {
            bool from_right = (ecx < 0.0f);
            e.x  = from_right ? (float)(SPAWN_RX - ENEMY_W / 2) : (float)SPAWN_LX;
            e.y  = (float)(HUD_H + 2);
            e.vx = 0;
            e.vy = ENEMY_FALL_SPEED;
            e.plat = -1;
            e.dir = from_right ? -1 : +1;
            e.state = ES_SPAWNING;
            return;
        }
    }

    e.x = wrap_x(e.x, ENEMY_W);

    // Land on the next solid platform below (gap-aware sweep). Over a
    // hole, find_platform_crossed returns -1 and the enemy keeps
    // falling to the level beneath — because gaps alternate sides,
    // it won't pass through two consecutive platforms.
    if (e.vy > 0) {
        int gp = find_platform_crossed(prev_y, e.y, ENEMY_H,
                                       e.x + (float)ENEMY_W / 2);
        if (gp >= 0) {
            e.y = (float)(PLAT_YS[gp] - ENEMY_H);
            e.plat = gp;
            e.vy = 0;
        }
    }

    // Off-bottom safety: any walking enemy that somehow ended up
    // well below the playfield gets reaped. Without this, an
    // escaped enemy stays "alive" in the counter and the phase
    // can never clear — the entire game stalls on phase 1.
    if (e.y > (float)VIEW_H + (float)ENEMY_H) {
        e.state = ES_DEAD;
        enemies_alive--;
    }
}

// ─────────────────────────────────────────────
//  RENDERING
// ─────────────────────────────────────────────
static void draw_background_static() {
    // Clear viewport only — on touch kiosks the bottom strip is
    // owned by the virtual D-pad chrome and must not be wiped.
    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, COL_BG);
    // Subtle floor brick texture on the bottom platform only.
    int fy = scr_y((float)PLAT_YS[N_PLATS - 1]);
    int fh = scr_y((float)VIEW_H) - fy;
    if (fh > 0) {
        gfx->fillRect(vx(), fy, VIEW_W, fh, COL_FLOOR_A);
        // Mortar lines
        for (int by = fy + 4; by < fy + fh; by += 6) {
            gfx->drawFastHLine(vx(), by, VIEW_W, COL_FLOOR_B);
        }
        // Vertical bricks (offset every other row)
        for (int by = fy; by < fy + fh; by += 6) {
            int row = (by - fy) / 6;
            int offset = (row & 1) ? 8 : 0;
            for (int bx = vx() + offset; bx < vx() + VIEW_W; bx += 16) {
                gfx->drawFastVLine(bx, by, 6, COL_FLOOR_B);
            }
        }
    }
    // Pipes top-left and top-right
    int pipe_h = PLAT_YS[0] - HUD_H;
    if (pipe_h > 0) {
        // Left pipe
        gfx->fillRect(vx(), scr_y((float)HUD_H), PIPE_W, pipe_h, COL_PIPE);
        gfx->fillRect(vx(), scr_y((float)HUD_H), 3, pipe_h, COL_PIPE_DARK);
        gfx->fillRect(vx() + PIPE_W - 3, scr_y((float)HUD_H), 3, pipe_h, COL_PIPE_DARK);
        // Lip
        gfx->fillRect(vx(), scr_y((float)HUD_H), PIPE_W, 4, COL_PIPE_DARK);
        // Right pipe
        gfx->fillRect(vx() + VIEW_W - PIPE_W, scr_y((float)HUD_H), PIPE_W, pipe_h, COL_PIPE);
        gfx->fillRect(vx() + VIEW_W - PIPE_W, scr_y((float)HUD_H), 3, pipe_h, COL_PIPE_DARK);
        gfx->fillRect(vx() + VIEW_W - 3, scr_y((float)HUD_H), 3, pipe_h, COL_PIPE_DARK);
        gfx->fillRect(vx() + VIEW_W - PIPE_W, scr_y((float)HUD_H), PIPE_W, 4, COL_PIPE_DARK);
    }
    // Floating platforms (not the floor). Each has a hole (see
    // plat_gap_left / over_gap) so enemies snake down the structure
    // and the player can drop through to chase them — so each platform
    // is drawn as TWO segments with the gap left empty between them.
    uint16_t pa = ice_active ? COL_ICE : COL_PLATFORM_A;
    uint16_t pb = ice_active ? 0x65BF : COL_PLATFORM_B;
    for (int i = 0; i < N_PLATS - 1; i++) {
        int y  = scr_y((float)PLAT_YS[i]);
        int gl = plat_gap_left(i);          // gap left edge (game x)
        int gr = gl + PLAT_GAP_W;           // gap right edge (game x)
        // Left segment: [0, gl)
        if (gl > 0) {
            gfx->fillRect(vx(), y, gl, PLAT_T, pa);
            gfx->drawFastHLine(vx(), y + PLAT_T - 1, gl, pb);
        }
        // Right segment: [gr, VIEW_W)
        if (gr < VIEW_W) {
            gfx->fillRect(vx() + gr, y, VIEW_W - gr, PLAT_T, pa);
            gfx->drawFastHLine(vx() + gr, y + PLAT_T - 1, VIEW_W - gr, pb);
        }
    }
}

static void draw_pow_block() {
    if (!pow_visible) return;
    uint16_t col = (pow_uses == 3) ? COL_POW_NORMAL
                 : (pow_uses == 2) ? COL_POW_CRACK
                 :                   COL_POW_WEAK;
    int x = scr_x((float)POW_X);
    int y = scr_y((float)POW_Y);
    gfx->fillRect(x, y, POW_W, POW_H, col);
    gfx->drawRect(x, y, POW_W, POW_H, COL_POW_TEXT);
    // Cracks: render (3 - pow_uses) diagonal scratches across the face.
    int cracks = 3 - pow_uses;
    for (int c = 0; c < cracks; c++) {
        int cy = y + 2 + c * (POW_H / 3);
        gfx->drawLine(x + 2, cy, x + POW_W - 3, cy + 2, COL_POW_TEXT);
    }
    // "POW" label (size-1 if small device, size-2 if Maxine-class)
#ifdef DEVICE_MAXINE
    gfx->setTextSize(2);
    gfx->setTextColor(COL_POW_TEXT);
    gfx->setCursor(x + (POW_W - 36) / 2, y + (POW_H - 14) / 2);
    gfx->print("POW");
#else
    if (POW_W >= 22) {
        gfx->setTextSize(1);
        gfx->setTextColor(COL_POW_TEXT);
        gfx->setCursor(x + (POW_W - 18) / 2, y + (POW_H - 7) / 2);
        gfx->print("POW");
    }
#endif
}

static void draw_player() {
    // Flicker during invulnerability.
    if (p_invuln > 0 && ((int)(p_invuln * 10.0f) & 1)) return;
    int x = scr_x(p_x);
    int y = scr_y(p_y);
    // Body
    gfx->fillRect(x, y + PLAYER_H / 2, PLAYER_W, PLAYER_H / 2, COL_PLAYER_BODY);
    // Head
    int hh = max(3, PLAYER_H / 3);
    gfx->fillRect(x + 1, y, PLAYER_W - 2, hh, COL_PLAYER_HEAD);
    // Legs/overalls
    gfx->fillRect(x, y + PLAYER_H - PLAYER_H / 4, PLAYER_W, PLAYER_H / 4, COL_PLAYER_LEGS);
    // Eye dot (face direction)
    int eye_x = (p_facing > 0) ? x + PLAYER_W - 3 : x + 2;
    gfx->fillRect(eye_x, y + 2, 2, 2, 0x0000);
}

static void draw_enemy(const Enemy& e) {
    if (e.state == ES_DEAD &&
        (e.x < -ENEMY_W * 2 || e.x > VIEW_W + ENEMY_W)) return;
    int x = scr_x(e.x);
    int y = scr_y(e.y);
    uint16_t main_col, dark_col;
    switch (e.kind) {
        case EK_SHELL: main_col = COL_SHELL; dark_col = COL_SHELL_DARK; break;
        case EK_CRAB:  main_col = e.angry ? COL_CRAB_ANGRY : COL_CRAB;
                       dark_col = 0x6200; break;
        default:       main_col = COL_FLY;   dark_col = COL_FLY_DARK; break;
    }
    if (e.state == ES_FLIPPED) {
        // On-back rendering: flat oval, yellow tint, legs up.
        gfx->fillRect(x, y + ENEMY_H - 3, ENEMY_W, 3, COL_FLIPPED);
        gfx->fillRect(x + 1, y + ENEMY_H / 2, ENEMY_W - 2, ENEMY_H / 2 - 3, COL_FLIPPED);
        // Twitching legs near recovery
        if (e.flip_timer < 1.0f && ((int)(e.flip_timer * 8) & 1)) {
            gfx->fillRect(x + 1, y, 2, ENEMY_H / 3, dark_col);
            gfx->fillRect(x + ENEMY_W - 3, y, 2, ENEMY_H / 3, dark_col);
        }
        return;
    }
    // Walking body
    gfx->fillRect(x, y, ENEMY_W, ENEMY_H, main_col);
    // Dark belt across the middle
    gfx->fillRect(x, y + ENEMY_H / 2 - 1, ENEMY_W, 2, dark_col);
    // Eyes
    int eye_y = y + 2;
    gfx->fillRect(x + 2, eye_y, 2, 2, 0x0000);
    gfx->fillRect(x + ENEMY_W - 4, eye_y, 2, 2, 0x0000);
    // Kind-specific overlay
    if (e.kind == EK_FLY) {
        // Wing tips when mid-hop
        if (e.vy < 0) {
            gfx->drawFastHLine(x - 2, y + ENEMY_H / 3, 3, dark_col);
            gfx->drawFastHLine(x + ENEMY_W - 1, y + ENEMY_H / 3, 3, dark_col);
        }
    }
}

static void draw_hud() {
    int hy = vy();
    // Background strip
    gfx->fillRect(vx(), hy, VIEW_W, HUD_H, COL_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_HUD_TEXT);
    gfx->setCursor(vx() + 4, hy + 2);
    gfx->printf("SC %ld", score);
    gfx->setCursor(vx() + VIEW_W / 2 - 24, hy + 2);
    gfx->printf("HI %ld", high_score);
    gfx->setCursor(vx() + VIEW_W - 76, hy + 2);
    gfx->printf("P%d  L%d", phase, lives);
}

static void draw_frame() {
    draw_background_static();
    draw_pow_block();
    for (int i = 0; i < MAX_ENEMIES; i++) draw_enemy(enemies[i]);
    draw_player();
    draw_hud();
}

// ─────────────────────────────────────────────
//  TITLE / GAME OVER
// ─────────────────────────────────────────────
static bool wait_for_start() {
    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, COL_BG);
    draw_background_static();
    gfx->setTextSize(2);
    gfx->setTextColor(COL_TEXT_TITLE);
    const char* title = "MARIO BROS";
    int tw = (int)strlen(title) * 12;
    gfx->setCursor(vx() + (VIEW_W - tw) / 2, vy() + VIEW_H / 2 - 30);
    gfx->print(title);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
    const char* sub = "TAP A TO START";
#else
    const char* sub = "PRESS A / SPACE TO START";
#endif
    int sw = (int)strlen(sub) * 6;
    gfx->setCursor(vx() + (VIEW_W - sw) / 2, vy() + VIEW_H / 2);
    gfx->print(sub);
    gfx->setTextColor(0xAD55);
    const char* hint = "Punch from below to flip!";
    int hw = (int)strlen(hint) * 6;
    gfx->setCursor(vx() + (VIEW_W - hw) / 2, vy() + VIEW_H / 2 + 20);
    gfx->print(hint);
    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return false;
        if (in.a || in.b || in.start) return true;
        delay(24);
        yield();
    }
}

static void show_game_over() {
    int boxW = min(180, VIEW_W - 16);
    int boxH = 60;
    int boxX = vx() + (VIEW_W - boxW) / 2;
    int boxY = vy() + (VIEW_H - boxH) / 2;
    gfx->fillRect(boxX, boxY, boxW, boxH, 0x0000);
    gfx->drawRect(boxX, boxY, boxW, boxH, COL_TEXT_TITLE);
    gfx->setTextSize(2);
    gfx->setTextColor(0xF800);
    gfx->setCursor(boxX + (boxW - 108) / 2, boxY + 10);
    gfx->print("GAME OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_HUD_TEXT);
    gfx->setCursor(boxX + 12, boxY + 34);
    gfx->printf("SCORE: %ld", score);
    if (score > high_score && score > 0) {
        high_score = score;
        save_high_score(high_score);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(boxX + 12, boxY + 46);
        gfx->print("NEW HIGH SCORE!");
    }
    delay(2800);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_mario_bros() {
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif

    high_score = load_high_score();

    // Title screen — bail out cleanly if the player quits before
    // starting the run.
    if (!wait_for_start()) {
#if defined(DEVICE_C28P)
        gfx->fillRect(0, 0, VIEW_W, VIEW_H, 0);   // game viewport only
#elif defined(DEVICE_MAXINE)
        gfx->fillRect(0, 0, VIEW_W, VIEW_H, 0);
#else
        gfx->fillScreen(0);
#endif
        return;
    }

    // Bring up the audio HAL so the arcade SFX (jump / flip / kick /
    // POW / death / phase-clear) are audible. Mario Bros has no
    // continuous background music — like the arcade, it's SFX-driven.
    pm_game_audio_begin();

    score = 0;
    lives = 3;
    start_phase(1);
    place_player_at_floor();
    last_frame_ms = millis();
    game_quit = false;

    while (lives > 0 && !game_quit) {
        // Frame timing.
        uint32_t now = millis();
        float dt = (float)(now - last_frame_ms) / 1000.0f;
        if (dt <= 0 || dt > 0.08f) dt = 0.016f;
        last_frame_ms = now;

        // Input.
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) { game_quit = true; break; }

        // Update.
        update_player(dt, in);
        for (int i = 0; i < MAX_ENEMIES; i++) update_enemy(i, dt);
        check_player_enemy_collisions();

        // Spawn from pipes if phase has more to send.
        spawn_cooldown -= dt;
        if (spawn_cooldown <= 0 &&
            enemies_spawned_this_phase < phase_total()) {
            int slot = find_free_enemy_slot();
            if (slot >= 0) {
                EnemyKind k = next_kind_for_phase();
                bool from_right = (spawn_index & 1) == 0;
                spawn_enemy_from_pipe(slot, k, from_right);
                spawn_cooldown = 2.4f / speed_mul;
            }
        }

        // Phase clear?
        if (enemies_alive == 0 &&
            enemies_spawned_this_phase >= phase_total()) {
            pm_game_audio_fx_phase();
            // Brief celebratory flash, then start the next phase.
            for (int f = 0; f < 4; f++) {
                gfx->fillRect(vx(), vy() + HUD_H, VIEW_W,
                              VIEW_H - HUD_H,
                              (f & 1) ? 0xFFE0 : COL_BG);
                delay(120);
            }
            start_phase(phase + 1);
            place_player_at_floor();
        }

        // Render.
        draw_frame();

        // Frame pacing — target ~60 Hz, except Cardputer ST7789
        // which reads cleaner at 30 fps on this kind of motion.
#ifdef DEVICE_CARDPUTER_ADV
        delay(33);
#else
        delay(16);
#endif
        yield();
    }

    pm_game_audio_stop();
    show_game_over();

#if defined(DEVICE_C28P)
    gfx->fillRect(0, 0, VIEW_W, VIEW_H, 0);
#elif defined(DEVICE_MAXINE)
    gfx->fillRect(0, 0, VIEW_W, VIEW_H, 0);
#else
    gfx->fillScreen(0);
#endif
}
