// Pisces Moon OS — Frogger
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Cross a busy road and a river to reach safe pads at the top.
// Road lanes have cars; getting hit by one kills the frog. River
// has logs/turtles drifting; you must land ON them to cross (water
// kills you). Reach an unfilled goal pad to score and respawn.
// Fill all goal pads to advance to next level.

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_audio.h"
#include "game_input.h"
#include "theme.h"
#include "frogger.h"
#ifdef DEVICE_C28P
#include "c28p_dpad.h"
#endif
#ifdef DEVICE_C5
#include "c5_dpad.h"
#endif
#ifdef DEVICE_MAXINE
#include "maxine_dpad.h"
#endif

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

#ifdef DEVICE_CARDPUTER_ADV
static constexpr int VIEW_W = 240, VIEW_H = 135;
static constexpr int LANE_H = 12;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 480, VIEW_H = 222;
static constexpr int LANE_H = 20;
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
static constexpr int VIEW_W = 240, VIEW_H = 200;
static constexpr int LANE_H = 18;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480, VIEW_H = 520;
static constexpr int LANE_H = 44;
#else
static constexpr int VIEW_W = 320, VIEW_H = 240;
static constexpr int LANE_H = 20;
#endif

static int vx() { int w = gfx->width(); return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0; }
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height(); return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}

static constexpr int HUD_H = 14;

// Lane layout (top to bottom):
//   0 : goal row
//   1..4: river (logs/turtles)
//   5 : median
//   6..9: road (cars)
//   10 : start row
static constexpr int NUM_LANES = 11;

struct Obstacle { int x; bool right; bool is_log; };
static Obstacle lane_obs[NUM_LANES][6];   // up to 6 per lane
static int lane_speeds[NUM_LANES];
static int frog_x, frog_y;
static int frog_lane;
static int level, lives, score;
static int goal_filled[5];
static int total_goals;

static int lane_y(int l) { return HUD_H + l * LANE_H; }

static void seed_level(int lv) {
    for (int i = 0; i < 5; i++) goal_filled[i] = 0;
    total_goals = 0;
    // Lane 0 = goals, lane 5 = median, lane 10 = start. No obstacles.
    // River (1..4) lanes carry logs
    // Road (6..9) lanes carry cars
    for (int l = 0; l < NUM_LANES; l++) {
        bool right = (l & 1);
        bool is_log = (l >= 1 && l <= 4);
        bool empty  = (l == 0 || l == 5 || l == 10);
        for (int i = 0; i < 6; i++) lane_obs[l][i] = { -100, right, is_log };
        if (empty) { lane_speeds[l] = 0; continue; }
        int spacing = VIEW_W / 3;
        int speed = (l >= 6 ? 2 : 1) + lv / 2;
        if (l == 6 || l == 4) speed += 1;
        if (l == 8) speed += 1;
        lane_speeds[l] = right ? speed : -speed;
        for (int i = 0; i < 3; i++) {
            lane_obs[l][i].x = i * spacing + random(spacing);
        }
    }
    frog_x = VIEW_W / 2 - 6;
    frog_lane = 10;
}

static int obstacle_w(int l, int i) {
    if (l >= 1 && l <= 4) {
        // Logs: longer
        return (l == 2 ? 60 : 40);
    }
    return 30;
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, HUD_H, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 4);
    gfx->printf("LV%d  SC %d  L%d  G%d/5", level + 1, score, lives, total_goals);
}

static void draw_terrain() {
    int ox = vx(), oy = vy();
    // Goal row
    gfx->fillRect(ox, oy + lane_y(0), VIEW_W, LANE_H, 0x0420);
    for (int i = 0; i < 5; i++) {
        int gx = ox + 8 + i * (VIEW_W / 5);
        gfx->fillRect(gx, oy + lane_y(0) + 2, VIEW_W / 5 - 8, LANE_H - 4,
                      goal_filled[i] ? 0xFFE0 : 0x0260);
    }
    // River
    for (int l = 1; l <= 4; l++) {
        gfx->fillRect(ox, oy + lane_y(l), VIEW_W, LANE_H, 0x001F);
    }
    // Median
    gfx->fillRect(ox, oy + lane_y(5), VIEW_W, LANE_H, 0x4208);
    // Road
    for (int l = 6; l <= 9; l++) {
        gfx->fillRect(ox, oy + lane_y(l), VIEW_W, LANE_H, 0x18C3);
        // Dashed line
        for (int x = 0; x < VIEW_W; x += 16) {
            gfx->fillRect(ox + x, oy + lane_y(l) + LANE_H / 2 - 1, 8, 2, 0xFFE0);
        }
    }
    // Start
    gfx->fillRect(ox, oy + lane_y(10), VIEW_W, LANE_H, 0x4208);
}

static void draw_obstacles() {
    int ox = vx(), oy = vy();
    for (int l = 0; l < NUM_LANES; l++) {
        for (int i = 0; i < 6; i++) {
            if (lane_obs[l][i].x < -80) continue;
            int w = obstacle_w(l, i);
            int x = ox + lane_obs[l][i].x;
            int y = oy + lane_y(l) + 2;
            uint16_t col = lane_obs[l][i].is_log ? 0x8208 : (l == 7 || l == 9 ? 0xFD20 : 0xF800);
            gfx->fillRect(x, y, w, LANE_H - 4, col);
            gfx->drawRect(x, y, w, LANE_H - 4, 0x0000);
        }
    }
}

static void draw_frog() {
    int ox = vx(), oy = vy();
    int y = oy + lane_y(frog_lane) + 2;
    gfx->fillRect(ox + frog_x, y, 12, LANE_H - 4, 0x07E0);
    gfx->fillRect(ox + frog_x + 3, y + 3, 2, 2, 0x0000);
    gfx->fillRect(ox + frog_x + 7, y + 3, 2, 2, 0x0000);
}

static void draw_play_clear() {
    int ox = vx(), oy = vy();
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    gfx->fillRect(ox, oy + HUD_H, VIEW_W, VIEW_H - HUD_H, 0x0000);
#else
    gfx->fillRect(ox, oy, VIEW_W, VIEW_H, 0x0000);
#endif
}

static int find_log_under_frog() {
    // If frog is on a river lane, find a log it stands on. Returns
    // log index or -1.
    if (frog_lane < 1 || frog_lane > 4) return -1;
    for (int i = 0; i < 6; i++) {
        int x = lane_obs[frog_lane][i].x;
        int w = obstacle_w(frog_lane, i);
        if (frog_x + 6 >= x && frog_x + 6 < x + w) return i;
    }
    return -1;
}

static bool car_hits_frog() {
    if (frog_lane < 6 || frog_lane > 9) return false;
    for (int i = 0; i < 6; i++) {
        int x = lane_obs[frog_lane][i].x;
        int w = obstacle_w(frog_lane, i);
        if (frog_x + 12 > x && frog_x < x + w) return true;
    }
    return false;
}

void run_frogger() {
    pm_game_audio_begin();
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_C5
    c5_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif
    level = 0;
    lives = 3;
    score = 0;
    seed_level(level);
    draw_play_clear();
    draw_hud();

    bool was_up = false, was_dn = false, was_l = false, was_r = false;
    uint32_t last_frame = millis();

    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return;

        // Move frog (one tile per discrete press)
        if (in.up && !was_up && frog_lane > 0) { frog_lane--; pm_game_audio_fx_drop(); }
        if (in.down && !was_dn && frog_lane < 10) { frog_lane++; pm_game_audio_fx_drop(); }
        if (in.left && !was_l) frog_x -= 12;
        if (in.right && !was_r) frog_x += 12;
        was_up = in.up; was_dn = in.down; was_l = in.left; was_r = in.right;
        if (frog_x < 0) frog_x = 0;
        if (frog_x > VIEW_W - 12) frog_x = VIEW_W - 12;

        // Move obstacles
        for (int l = 0; l < NUM_LANES; l++) {
            for (int i = 0; i < 6; i++) {
                if (lane_obs[l][i].x < -80) continue;
                lane_obs[l][i].x += lane_speeds[l];
                if (lane_speeds[l] > 0 && lane_obs[l][i].x > VIEW_W + 20) {
                    lane_obs[l][i].x = -obstacle_w(l, i) - random(40);
                }
                if (lane_speeds[l] < 0 && lane_obs[l][i].x + obstacle_w(l, i) < -20) {
                    lane_obs[l][i].x = VIEW_W + random(40);
                }
            }
        }

        // If frog is on river, ride the log it's on
        if (frog_lane >= 1 && frog_lane <= 4) {
            int li = find_log_under_frog();
            if (li >= 0) {
                frog_x += lane_speeds[frog_lane];
            } else {
                // Drowned
                lives--;
                if (lives <= 0) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xF800);
                    gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
                    gfx->print("GAME OVER");
                    delay(2200);
                    return;
                }
                frog_x = VIEW_W / 2 - 6;
                frog_lane = 10;
                pm_game_audio_fx_drop();
            }
        }

        // Car collisions
        if (car_hits_frog()) {
            lives--;
            if (lives <= 0) {
                gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                gfx->setTextSize(2);
                gfx->setTextColor(0xF800);
                gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
                gfx->print("SPLAT!");
                delay(2200);
                return;
            }
            frog_x = VIEW_W / 2 - 6;
            frog_lane = 10;
        }

        // Goal row?
        if (frog_lane == 0) {
            int slot = (frog_x + 6) / (VIEW_W / 5);
            if (slot >= 0 && slot < 5 && !goal_filled[slot]) {
                goal_filled[slot] = 1;
                total_goals++;
                score += 500;
                pm_game_audio_fx_line();
                if (total_goals >= 5) {
                    level++;
                    score += 2000;
                    delay(700);
                    seed_level(level);
                } else {
                    frog_x = VIEW_W / 2 - 6;
                    frog_lane = 10;
                }
            } else {
                // Hit a barrier between goals
                lives--;
                if (lives <= 0) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xF800);
                    gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
                    gfx->print("GAME OVER");
                    delay(2200);
                    return;
                }
                frog_x = VIEW_W / 2 - 6;
                frog_lane = 10;
            }
        }

        draw_play_clear();
        draw_terrain();
        draw_obstacles();
        draw_frog();
        draw_hud();

        uint32_t now = millis();
        if (now - last_frame < 50) delay(50 - (now - last_frame));
        last_frame = millis();
        yield();
    }
}
