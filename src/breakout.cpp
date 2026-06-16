// Pisces Moon OS — Breakout
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Classic brick-breaker. Paddle at the bottom, ball bounces off
// bricks above. Lose a life if the ball passes the paddle. Clear
// all bricks to advance to the next level (which gets faster).
//
// Input model:
//   Keyboard devices: LEFT/RIGHT to move, A/B/START to launch ball
//   Touch kiosks    : drag the paddle directly with a finger, tap
//                     the play area to launch
//
// Levels: brick layout shifts each level (more rows, more colors,
// some bricks worth more points). 10 levels then loops with speed
// increase.

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_audio.h"
#include "game_input.h"
#include "theme.h"
#include "breakout.h"
#ifdef DEVICE_C28P
#include "c28p_dpad.h"
extern bool c28p_touch_read(int16_t* x, int16_t* y);
#endif
#ifdef DEVICE_C5
#include "c5_dpad.h"
extern bool c5_touch_read(int16_t* x, int16_t* y);
#endif
#ifdef DEVICE_MAXINE
#include "maxine_dpad.h"
extern bool maxine_touch_read(int16_t* x, int16_t* y);
#endif

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

#ifdef DEVICE_CARDPUTER_ADV
static constexpr int VIEW_W = 240;
static constexpr int VIEW_H = 135;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 480;
static constexpr int VIEW_H = 222;
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
static constexpr int VIEW_W = 240;
static constexpr int VIEW_H = 200;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480;
static constexpr int VIEW_H = 520;
#else
static constexpr int VIEW_W = 320;
static constexpr int VIEW_H = 240;
#endif

static constexpr int BRICK_ROWS = 6;
static constexpr int BRICK_COLS = 10;
static constexpr int HUD_H      = 14;

static int vx() { int w = gfx->width(); return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0; }
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height(); return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}

static int brick_w() { return (VIEW_W - 20) / BRICK_COLS; }
static int brick_h() {
#ifdef DEVICE_CARDPUTER_ADV
    return 6;
#elif defined(DEVICE_MAXINE)
    return 18;
#else
    return 10;
#endif
}
static int brick_top() { return HUD_H + 12; }

static int paddle_w() { return VIEW_W / 6; }
static int paddle_h() {
#ifdef DEVICE_CARDPUTER_ADV
    return 4;
#elif defined(DEVICE_MAXINE)
    return 14;
#else
    return 6;
#endif
}
static int paddle_y() { return VIEW_H - paddle_h() - 6; }
static int ball_r() {
#ifdef DEVICE_CARDPUTER_ADV
    return 2;
#elif defined(DEVICE_MAXINE)
    return 6;
#else
    return 3;
#endif
}

static uint16_t BRICK_COLORS[BRICK_ROWS] = {
    0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0xF81F
};

static uint8_t bricks[BRICK_ROWS][BRICK_COLS];
static int paddle_x;
static int32_t ball_x_q8, ball_y_q8;
static int32_t ball_vx_q8, ball_vy_q8;
static bool ball_stuck;
static int score, lives, level, bricks_left;

static void load_level(int lv) {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            // First level: top 3 rows only. Each level adds rows.
            int active_rows = min(BRICK_ROWS, 3 + lv / 2);
            bricks[r][c] = (r < active_rows) ? 1 : 0;
        }
    }
    bricks_left = 0;
    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++)
            if (bricks[r][c]) bricks_left++;
    paddle_x = (VIEW_W - paddle_w()) / 2;
    ball_x_q8 = (int32_t)(paddle_x + paddle_w() / 2) * 256;
    ball_y_q8 = (int32_t)(paddle_y() - ball_r() - 1) * 256;
    int speed = 280 + lv * 30;
    ball_vx_q8 = (random(2) ? speed : -speed);
    ball_vy_q8 = -speed - 40;
    ball_stuck = true;
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, HUD_H, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 3);
    gfx->printf("LV%-2d  SC %05d  L%d", level + 1, score, lives);
}

static void draw_brick(int r, int c) {
    int ox = vx(), oy = vy();
    int x = ox + 10 + c * brick_w();
    int y = oy + brick_top() + r * (brick_h() + 2);
    if (bricks[r][c]) {
        gfx->fillRect(x, y, brick_w() - 2, brick_h(), BRICK_COLORS[r]);
        gfx->drawRect(x, y, brick_w() - 2, brick_h(), 0xFFFF);
    } else {
        gfx->fillRect(x, y, brick_w() - 2, brick_h(), 0x0000);
    }
}

static void draw_all_bricks() {
    for (int r = 0; r < BRICK_ROWS; r++)
        for (int c = 0; c < BRICK_COLS; c++)
            draw_brick(r, c);
}

static int last_paddle_x = -1;
static int32_t last_ball_x = -1, last_ball_y = -1;

static void draw_paddle_ball() {
    int ox = vx(), oy = vy();
    // Erase old paddle if it moved
    if (last_paddle_x >= 0 && last_paddle_x != paddle_x) {
        gfx->fillRect(ox + last_paddle_x, oy + paddle_y(), paddle_w(), paddle_h(), 0x0000);
    }
    gfx->fillRect(ox + paddle_x, oy + paddle_y(), paddle_w(), paddle_h(), 0x07FF);
    gfx->drawRect(ox + paddle_x, oy + paddle_y(), paddle_w(), paddle_h(), 0xFFFF);
    last_paddle_x = paddle_x;

    // Erase old ball
    if (last_ball_x >= 0) {
        int oldx = (int)(last_ball_x >> 8);
        int oldy = (int)(last_ball_y >> 8);
        gfx->fillRect(ox + oldx - ball_r() - 1, oy + oldy - ball_r() - 1,
                      ball_r() * 2 + 2, ball_r() * 2 + 2, 0x0000);
    }
    int bx = (int)(ball_x_q8 >> 8);
    int by = (int)(ball_y_q8 >> 8);
    gfx->fillCircle(ox + bx, oy + by, ball_r(), 0xFFE0);
    last_ball_x = ball_x_q8;
    last_ball_y = ball_y_q8;
}

static void clear_play() {
    int ox = vx(), oy = vy();
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    gfx->fillRect(ox, oy + HUD_H, VIEW_W, VIEW_H - HUD_H, 0x0000);
#else
    gfx->fillRect(ox, oy, VIEW_W, VIEW_H, 0x0000);
#endif
    last_paddle_x = -1;
    last_ball_x = -1;
}

// AABB-vs-circle simplified: treat ball as a small box and check
// against each brick cell.
static bool collide_brick(int bx, int by) {
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            if (!bricks[r][c]) continue;
            int rx = 10 + c * brick_w();
            int ry = brick_top() + r * (brick_h() + 2);
            if (bx + ball_r() >= rx && bx - ball_r() < rx + brick_w() - 2 &&
                by + ball_r() >= ry && by - ball_r() < ry + brick_h()) {
                bricks[r][c] = 0;
                bricks_left--;
                score += 10 * (BRICK_ROWS - r);
                draw_brick(r, c);
                // Decide bounce axis: based on overlap depth
                int overlap_x = min(bx + ball_r() - rx, rx + brick_w() - 2 - (bx - ball_r()));
                int overlap_y = min(by + ball_r() - ry, ry + brick_h() - (by - ball_r()));
                if (overlap_x < overlap_y) ball_vx_q8 = -ball_vx_q8;
                else                       ball_vy_q8 = -ball_vy_q8;
                pm_game_audio_fx_line();
                return true;
            }
        }
    }
    return false;
}

void run_breakout() {
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
    score = 0;
    lives = 3;
    level = 0;
    load_level(level);
    clear_play();
    draw_hud();
    draw_all_bricks();

    uint32_t last_frame = millis();
    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return;

        // Touch-drag paddle on touch kiosks
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
        int16_t tx, ty;
        bool t;
#ifdef DEVICE_C28P
        t = c28p_touch_read(&tx, &ty);
#elif defined(DEVICE_C5)
        t = c5_touch_read(&tx, &ty);
#else
        t = maxine_touch_read(&tx, &ty);
#endif
        if (t && ty < VIEW_H) {
            paddle_x = tx - paddle_w() / 2;
            if (paddle_x < 0) paddle_x = 0;
            if (paddle_x > VIEW_W - paddle_w()) paddle_x = VIEW_W - paddle_w();
            if (ball_stuck) ball_stuck = false;
        }
#endif
        if (in.left)  paddle_x -= 4;
        if (in.right) paddle_x += 4;
        if (paddle_x < 0) paddle_x = 0;
        if (paddle_x > VIEW_W - paddle_w()) paddle_x = VIEW_W - paddle_w();

        if (ball_stuck) {
            ball_x_q8 = (int32_t)(paddle_x + paddle_w() / 2) * 256;
            ball_y_q8 = (int32_t)(paddle_y() - ball_r() - 1) * 256;
            if (in.a || in.b || in.start || in.up) ball_stuck = false;
        } else {
            ball_x_q8 += ball_vx_q8 / 16;
            ball_y_q8 += ball_vy_q8 / 16;
            int bx = (int)(ball_x_q8 >> 8);
            int by = (int)(ball_y_q8 >> 8);

            // Walls
            if (bx - ball_r() <= 0)        { ball_vx_q8 = abs(ball_vx_q8);  ball_x_q8 = (ball_r() + 1) << 8; }
            if (bx + ball_r() >= VIEW_W)   { ball_vx_q8 = -abs(ball_vx_q8); ball_x_q8 = (VIEW_W - ball_r() - 1) << 8; }
            if (by - ball_r() <= HUD_H)    { ball_vy_q8 = abs(ball_vy_q8);  ball_y_q8 = (HUD_H + ball_r() + 1) << 8; }

            // Paddle
            if (by + ball_r() >= paddle_y() &&
                by - ball_r() <= paddle_y() + paddle_h() &&
                bx >= paddle_x && bx <= paddle_x + paddle_w() &&
                ball_vy_q8 > 0) {
                ball_vy_q8 = -abs(ball_vy_q8);
                // Influence X by hit position on paddle
                int rel = bx - (paddle_x + paddle_w() / 2);
                ball_vx_q8 += rel * 8;
                if (ball_vx_q8 >  600) ball_vx_q8 =  600;
                if (ball_vx_q8 < -600) ball_vx_q8 = -600;
                ball_y_q8 = (paddle_y() - ball_r() - 1) << 8;
                pm_game_audio_fx_drop();
            }

            // Bricks
            collide_brick(bx, by);

            // Fall off bottom
            if (by - ball_r() > VIEW_H) {
                lives--;
                if (lives <= 0) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 20, VIEW_W, 40, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xF800);
                    gfx->setCursor(vx() + VIEW_W/2 - 48, vy() + VIEW_H/2 - 8);
                    gfx->print("GAME OVER");
                    delay(2200);
                    return;
                }
                ball_stuck = true;
            }
        }

        // Level clear
        if (bricks_left == 0) {
            level++;
            score += 1000;
            gfx->fillRect(vx(), vy() + VIEW_H/2 - 20, VIEW_W, 40, 0x0000);
            gfx->setTextSize(2);
            gfx->setTextColor(0x07E0);
            gfx->setCursor(vx() + VIEW_W/2 - 60, vy() + VIEW_H/2 - 8);
            gfx->print("LEVEL UP!");
            delay(1400);
            load_level(level);
            clear_play();
            draw_hud();
            draw_all_bricks();
        }

        draw_paddle_ball();
        draw_hud();

        uint32_t now = millis();
        if (now - last_frame < 16) delay(16 - (now - last_frame));
        last_frame = millis();
        yield();
    }
}
