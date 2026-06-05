// Pisces Moon OS — 2048
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Slide tiles in 4 directions to merge equal values. Goal: reach
// the 2048 tile. Game over when no moves remain. Tracks high score
// in NoSQL.
//
// Input: arrows (or W/A/S/D) on keyboard. On touch kiosks, swipe
// detection: track start position of touch, on release direction
// of motion picks the slide axis.

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_audio.h"
#include "game_input.h"
#include "theme.h"
#include "game_2048.h"
#include "nosql_store.h"
#ifdef DEVICE_C28P
#include "c28p_dpad.h"
extern bool c28p_touch_read(int16_t* x, int16_t* y);
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
static constexpr int VIEW_W = 240, VIEW_H = 135;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 480, VIEW_H = 222;
#elif defined(DEVICE_C28P)
static constexpr int VIEW_W = 240, VIEW_H = 200;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480, VIEW_H = 520;
#else
static constexpr int VIEW_W = 320, VIEW_H = 240;
#endif

static int vx() { int w = gfx->width(); return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0; }
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height(); return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}

static constexpr int N = 4;
static int board[N][N];
static int score, best;

static uint16_t tile_color(int v) {
    switch (v) {
        case 0:    return 0x4208;
        case 2:    return 0xEF7D;
        case 4:    return 0xEF1A;
        case 8:    return 0xFD20;
        case 16:   return 0xFC00;
        case 32:   return 0xF800;
        case 64:   return 0xF801;
        case 128:  return 0xFFE0;
        case 256:  return 0xFFC0;
        case 512:  return 0x07E0;
        case 1024: return 0x07FF;
        case 2048: return 0xF81F;
        default:   return 0xFFFF;
    }
}

static int board_w() {
#ifdef DEVICE_TLORAPAGER
    return 200;
#elif defined(DEVICE_MAXINE)
    return 420;
#elif defined(DEVICE_CARDPUTER_ADV)
    return 110;
#else
    return 180;
#endif
}
static int board_x() { return (VIEW_W - board_w()) / 2; }
static int board_y() { return 20; }
static int cell_size() { return (board_w() - 5 * 4) / N; }

static void spawn_tile() {
    int empty_count = 0;
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (board[r][c] == 0) empty_count++;
    if (empty_count == 0) return;
    int target = random(empty_count);
    int i = 0;
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            if (board[r][c] == 0) {
                if (i == target) { board[r][c] = (random(10) == 0) ? 4 : 2; return; }
                i++;
            }
        }
    }
}

static void draw_board() {
    int ox = vx(), oy = vy();
    int bx = ox + board_x(), by = oy + board_y();
    gfx->fillRect(bx - 4, by - 4, board_w() + 8, board_w() + 8, 0x2104);
    gfx->drawRect(bx - 4, by - 4, board_w() + 8, board_w() + 8, 0x8410);
    int cs = cell_size();
    for (int r = 0; r < N; r++) {
        for (int c = 0; c < N; c++) {
            int x = bx + 4 + c * (cs + 4);
            int y = by + 4 + r * (cs + 4);
            int v = board[r][c];
            gfx->fillRect(x, y, cs, cs, tile_color(v));
            if (v > 0) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%d", v);
                int len = strlen(buf);
                int ts = (v >= 1000) ? 1 : (v >= 100) ? 2 : (cs > 50) ? 3 : 2;
                int cw = 6 * ts, ch = 8 * ts;
                gfx->setTextSize(ts);
                gfx->setTextColor(v <= 4 ? 0x0000 : 0xFFFF);
                gfx->setCursor(x + (cs - len * cw) / 2, y + (cs - ch) / 2);
                gfx->print(buf);
            }
        }
    }
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, 18, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 5);
    gfx->printf("SC %d   BEST %d", score, best);
}

// Slide one row left in-place. Returns true if anything changed.
static bool slide_row_left(int row[N]) {
    bool changed = false;
    int out[N] = {0};
    int outi = 0;
    for (int i = 0; i < N; i++) {
        if (row[i] == 0) continue;
        if (outi > 0 && out[outi - 1] == row[i]) {
            out[outi - 1] *= 2;
            score += out[outi - 1];
            changed = true;
        } else {
            out[outi++] = row[i];
        }
    }
    for (int i = 0; i < N; i++) {
        if (out[i] != row[i]) changed = true;
        row[i] = out[i];
    }
    return changed;
}

static bool move_left() {
    bool any = false;
    for (int r = 0; r < N; r++) if (slide_row_left(board[r])) any = true;
    return any;
}
static bool move_right() {
    bool any = false;
    for (int r = 0; r < N; r++) {
        int tmp[N]; for (int i = 0; i < N; i++) tmp[i] = board[r][N - 1 - i];
        if (slide_row_left(tmp)) any = true;
        for (int i = 0; i < N; i++) board[r][N - 1 - i] = tmp[i];
    }
    return any;
}
static bool move_up() {
    bool any = false;
    for (int c = 0; c < N; c++) {
        int tmp[N]; for (int i = 0; i < N; i++) tmp[i] = board[i][c];
        if (slide_row_left(tmp)) any = true;
        for (int i = 0; i < N; i++) board[i][c] = tmp[i];
    }
    return any;
}
static bool move_down() {
    bool any = false;
    for (int c = 0; c < N; c++) {
        int tmp[N]; for (int i = 0; i < N; i++) tmp[i] = board[N - 1 - i][c];
        if (slide_row_left(tmp)) any = true;
        for (int i = 0; i < N; i++) board[N - 1 - i][c] = tmp[i];
    }
    return any;
}

static bool has_moves() {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (board[r][c] == 0) return true;
            if (c < N - 1 && board[r][c] == board[r][c + 1]) return true;
            if (r < N - 1 && board[r][c] == board[r + 1][c]) return true;
        }
    return false;
}

static void load_best() {
    best = 0;
    nosql_init("settings");
    int total = nosql_get_count("settings");
    String t, c;
    for (int i = 0; i < total; i++) {
        if (!nosql_get_entry("settings", i, t, c)) continue;
        if (t == "2048_best") { best = c.toInt(); return; }
    }
}
static void save_best() {
    nosql_init("settings");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", best);
    nosql_save_entry("settings", "2048_best", buf);
}

void run_2048() {
    pm_game_audio_begin();
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif
    load_best();
    score = 0;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) board[r][c] = 0;
    spawn_tile();
    spawn_tile();
    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, 0x0000);
    draw_hud();
    draw_board();

    int16_t swipe_sx = -1, swipe_sy = -1;
    bool was_touched = false;

    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) {
            if (score > best) { best = score; save_best(); }
            return;
        }
        bool moved = false;
        if (in.left)  moved = move_left();
        if (in.right) moved = move_right();
        if (in.up)    moved = move_up();
        if (in.down)  moved = move_down();

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
        int16_t tx, ty;
#ifdef DEVICE_C28P
        bool t = c28p_touch_read(&tx, &ty);
#else
        bool t = maxine_touch_read(&tx, &ty);
#endif
        if (t && !was_touched) { swipe_sx = tx; swipe_sy = ty; }
        if (!t && was_touched && swipe_sx >= 0) {
            int dx = tx - swipe_sx;
            int dy = ty - swipe_sy;
            if (abs(dx) > 20 || abs(dy) > 20) {
                if (abs(dx) > abs(dy)) {
                    if (dx > 0) moved = move_right(); else moved = move_left();
                } else {
                    if (dy > 0) moved = move_down(); else moved = move_up();
                }
            }
            swipe_sx = swipe_sy = -1;
        }
        was_touched = t;
#endif

        if (moved) {
            spawn_tile();
            if (score > best) best = score;
            draw_board();
            draw_hud();
            pm_game_audio_fx_drop();
            if (!has_moves()) {
                gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                gfx->setTextSize(2);
                gfx->setTextColor(0xF800);
                gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
                gfx->print("GAME OVER");
                save_best();
                delay(2400);
                return;
            }
        }
        delay(40);
        yield();
    }
}
