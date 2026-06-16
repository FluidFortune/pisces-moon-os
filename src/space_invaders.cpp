// Pisces Moon OS — Space Invaders
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Classic 1978 model. Grid of aliens marches left/right and steps
// down on edge contact. Player ship at the bottom shoots up. Aliens
// occasionally drop bombs. Clear all aliens to advance to a faster
// wave. Aliens reaching the player's row = game over.
//
// Input: LEFT/RIGHT moves, A/B fires. Touch kiosks can use the
// virtual D-pad.

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_audio.h"
#include "game_input.h"
#include "theme.h"
#include "space_invaders.h"
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
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 480, VIEW_H = 222;
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
static constexpr int VIEW_W = 240, VIEW_H = 200;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480, VIEW_H = 520;
#else
static constexpr int VIEW_W = 320, VIEW_H = 240;
#endif

static int vx() { int w = gfx->width(); return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0; }
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height(); return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}

static constexpr int HUD_H = 12;
static constexpr int A_ROWS = 5;
static constexpr int A_COLS = 10;
static constexpr int ALIEN_W = 14;
static constexpr int ALIEN_H = 10;

static bool aliens[A_ROWS][A_COLS];
static int  a_offset_x, a_offset_y;
static int  a_dir;             // +1 or -1
static int  a_step_timer;
static int  a_step_interval;   // frames between marches
static int  ship_x;
static int  ship_w = 18, ship_h = 8;
static int32_t bullet_x = -1, bullet_y;
struct Bomb { int x, y; bool alive; };
static Bomb bombs[6];
static int score, lives, wave;
static int alive_count;

static int alien_x(int r, int c) {
    int spacing_x = (VIEW_W - 20) / A_COLS;
    return 10 + c * spacing_x + a_offset_x;
}
static int alien_y(int r, int c) {
    int spacing_y = ALIEN_H + 6;
    return HUD_H + 16 + r * spacing_y + a_offset_y;
}

static void seed_aliens() {
    for (int r = 0; r < A_ROWS; r++)
        for (int c = 0; c < A_COLS; c++)
            aliens[r][c] = true;
    a_offset_x = 0;
    a_offset_y = 0;
    a_dir = 1;
    a_step_timer = 0;
    a_step_interval = max(8, 30 - wave * 2);
    for (int i = 0; i < (int)(sizeof(bombs)/sizeof(bombs[0])); i++) bombs[i].alive = false;
    bullet_x = -1;
    alive_count = A_ROWS * A_COLS;
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, HUD_H, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 3);
    gfx->printf("WAVE %d  SC %05d  L%d", wave + 1, score, lives);
}

static uint16_t alien_color(int r) {
    static const uint16_t COL[5] = { 0xF800, 0xFD20, 0xFFE0, 0x07FF, 0xF81F };
    return COL[r];
}

static void draw_alien(int r, int c) {
    int x = vx() + alien_x(r, c);
    int y = vy() + alien_y(r, c);
    if (!aliens[r][c]) {
        gfx->fillRect(x, y, ALIEN_W, ALIEN_H, 0x0000);
        return;
    }
    uint16_t col = alien_color(r);
    bool flip = (a_step_timer >> 3) & 1;
    gfx->fillRect(x + 2, y, ALIEN_W - 4, ALIEN_H, col);
    gfx->fillRect(x, y + 2, ALIEN_W, ALIEN_H - 4, col);
    gfx->fillRect(x + 3, y + 3, 2, 2, 0x0000);
    gfx->fillRect(x + ALIEN_W - 5, y + 3, 2, 2, 0x0000);
    if (flip) {
        gfx->fillRect(x, y + ALIEN_H - 2, 2, 2, 0x0000);
        gfx->fillRect(x + ALIEN_W - 2, y + ALIEN_H - 2, 2, 2, 0x0000);
    }
}

static void draw_ship() {
    int x = vx() + ship_x, y = vy() + VIEW_H - ship_h - 4;
    gfx->fillRect(x, y, ship_w, ship_h, 0x07E0);
    gfx->fillRect(x + ship_w / 2 - 1, y - 3, 2, 3, 0x07E0);
}

static void draw_play_clear() {
    int ox = vx(), oy = vy();
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    gfx->fillRect(ox, oy + HUD_H, VIEW_W, VIEW_H - HUD_H, 0x0000);
#else
    gfx->fillRect(ox, oy, VIEW_W, VIEW_H, 0x0000);
#endif
}

static void march() {
    bool hit_edge = false;
    // Find horizontal extent of alive aliens
    int min_x = VIEW_W, max_x = 0;
    for (int r = 0; r < A_ROWS; r++) {
        for (int c = 0; c < A_COLS; c++) {
            if (!aliens[r][c]) continue;
            int x = alien_x(r, c);
            if (x < min_x) min_x = x;
            if (x + ALIEN_W > max_x) max_x = x + ALIEN_W;
        }
    }
    if (a_dir > 0 && max_x + 4 >= VIEW_W) hit_edge = true;
    if (a_dir < 0 && min_x <= 4)          hit_edge = true;
    if (hit_edge) {
        a_offset_y += 8;
        a_dir = -a_dir;
    } else {
        a_offset_x += a_dir * 4;
    }
}

static void maybe_drop_bomb() {
    if (random(20) != 0) return;
    // Pick a random alive alien column and find its lowest occupied row
    int tries = 8;
    while (tries-- > 0) {
        int c = random(A_COLS);
        for (int r = A_ROWS - 1; r >= 0; r--) {
            if (!aliens[r][c]) continue;
            for (int i = 0; i < (int)(sizeof(bombs)/sizeof(bombs[0])); i++) {
                if (!bombs[i].alive) {
                    bombs[i].alive = true;
                    bombs[i].x = alien_x(r, c) + ALIEN_W / 2;
                    bombs[i].y = alien_y(r, c) + ALIEN_H;
                    return;
                }
            }
            return;
        }
    }
}

void run_space_invaders() {
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
    wave = 0;
    ship_x = (VIEW_W - ship_w) / 2;
    seed_aliens();
    draw_play_clear();
    draw_hud();

    bool was_a = false, was_b = false;
    uint32_t last_frame = millis();
    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return;

        if (in.left)  ship_x -= 3;
        if (in.right) ship_x += 3;
        if (ship_x < 2) ship_x = 2;
        if (ship_x > VIEW_W - ship_w - 2) ship_x = VIEW_W - ship_w - 2;

        if ((in.a || in.b) && !(was_a || was_b) && bullet_x < 0) {
            bullet_x = ship_x + ship_w / 2;
            bullet_y = VIEW_H - ship_h - 8;
            pm_game_audio_fx_drop();
        }
        was_a = in.a; was_b = in.b;

        // Bullet
        if (bullet_x >= 0) {
            bullet_y -= 5;
            if (bullet_y < HUD_H) bullet_x = -1;
        }
        // Bomb fall
        for (int i = 0; i < (int)(sizeof(bombs)/sizeof(bombs[0])); i++) {
            if (!bombs[i].alive) continue;
            bombs[i].y += 2;
            if (bombs[i].y > VIEW_H) bombs[i].alive = false;
        }

        // Alien march
        if (++a_step_timer >= a_step_interval) {
            a_step_timer = 0;
            march();
            maybe_drop_bomb();
        }

        // Bullet vs alien
        if (bullet_x >= 0) {
            for (int r = 0; r < A_ROWS; r++) {
                for (int c = 0; c < A_COLS; c++) {
                    if (!aliens[r][c]) continue;
                    int ax = alien_x(r, c), ay = alien_y(r, c);
                    if (bullet_x >= ax && bullet_x < ax + ALIEN_W &&
                        bullet_y >= ay && bullet_y < ay + ALIEN_H) {
                        aliens[r][c] = false;
                        alive_count--;
                        score += (A_ROWS - r) * 10;
                        bullet_x = -1;
                        pm_game_audio_fx_line();
                        // Speed up as aliens die
                        a_step_interval = max(4, 30 - wave * 2 - (A_ROWS * A_COLS - alive_count) / 4);
                        goto bullet_done;
                    }
                }
            }
        }
        bullet_done:;

        // Bomb vs ship
        for (int i = 0; i < (int)(sizeof(bombs)/sizeof(bombs[0])); i++) {
            if (!bombs[i].alive) continue;
            if (bombs[i].x >= ship_x && bombs[i].x < ship_x + ship_w &&
                bombs[i].y >= VIEW_H - ship_h - 4 && bombs[i].y < VIEW_H) {
                bombs[i].alive = false;
                lives--;
                if (lives <= 0) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xF800);
                    gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
                    gfx->print("GAME OVER");
                    delay(2400);
                    return;
                }
            }
        }

        // Aliens reach the bottom?
        for (int r = A_ROWS - 1; r >= 0; r--) {
            for (int c = 0; c < A_COLS; c++) {
                if (aliens[r][c] && alien_y(r, c) + ALIEN_H >= VIEW_H - ship_h - 4) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xF800);
                    gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
                    gfx->print("INVADED!");
                    delay(2400);
                    return;
                }
            }
        }

        // Next wave
        if (alive_count == 0) {
            wave++;
            score += 500;
            seed_aliens();
        }

        // Render
        draw_play_clear();
        draw_hud();
        for (int r = 0; r < A_ROWS; r++)
            for (int c = 0; c < A_COLS; c++)
                draw_alien(r, c);
        draw_ship();
        if (bullet_x >= 0) {
            gfx->fillRect(vx() + (int)bullet_x - 1, vy() + (int)bullet_y, 2, 6, 0xFFE0);
        }
        for (int i = 0; i < (int)(sizeof(bombs)/sizeof(bombs[0])); i++) {
            if (!bombs[i].alive) continue;
            gfx->fillRect(vx() + bombs[i].x - 1, vy() + bombs[i].y, 2, 6, 0xF800);
        }

        uint32_t now = millis();
        if (now - last_frame < 33) delay(33 - (now - last_frame));
        last_frame = millis();
        yield();
    }
}
