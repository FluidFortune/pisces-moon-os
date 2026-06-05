// Pisces Moon OS — Minesweeper
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Grid of hidden cells. Reveal cells to expose either a number
// (count of adjacent mines) or empty space (auto-cascades to
// reveal connected blank region). Avoid mines. Right-tap / B
// button flags a cell as a suspected mine. Reveal all non-mine
// cells to win.
//
// Grid sizes scale per device:
//   Cardputer: 12x8   (small but legible)
//   T-Deck   : 16x12  (classic)
//   Pager    : 24x10  (wide)
//   C28P     : 12x14  (portrait)
//   Maxine   : 16x22  (large portrait)
//
// Input model:
//   Keyboard: arrows move cursor, A reveals, B flags
//   Touch  : single tap reveals, long-press (> 600ms) flags

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_input.h"
#include "game_audio.h"
#include "theme.h"
#include "minesweeper.h"
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
static constexpr int GW = 16, GH = 9;
static constexpr int MINES = 14;
static constexpr int CELL = 13;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 480, VIEW_H = 222;
static constexpr int GW = 24, GH = 11;
static constexpr int MINES = 30;
static constexpr int CELL = 18;
#elif defined(DEVICE_C28P)
static constexpr int VIEW_W = 240, VIEW_H = 200;
static constexpr int GW = 12, GH = 14;
static constexpr int MINES = 25;
static constexpr int CELL = 18;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480, VIEW_H = 520;
static constexpr int GW = 16, GH = 22;
static constexpr int MINES = 60;
static constexpr int CELL = 28;
#else
static constexpr int VIEW_W = 320, VIEW_H = 240;
static constexpr int GW = 16, GH = 12;
static constexpr int MINES = 30;
static constexpr int CELL = 18;
#endif

static int vx() { int w = gfx->width(); return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0; }
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height(); return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}

// Each cell: low 4 bits = adj mine count (or 9 for mine), bit 4 = revealed, bit 5 = flagged
static uint8_t grid[24 * 22];
static int cursor_x, cursor_y;
static int revealed_count;
static bool game_over, game_won, first_click;

static int board_x() { return (VIEW_W - GW * CELL) / 2; }
static int board_y() { return 18; }

static uint8_t& cell(int x, int y) { return grid[y * GW + x]; }

static void place_mines(int safe_x, int safe_y) {
    int placed = 0;
    while (placed < MINES) {
        int rx = random(GW);
        int ry = random(GH);
        if (abs(rx - safe_x) <= 1 && abs(ry - safe_y) <= 1) continue;
        uint8_t& c = cell(rx, ry);
        if ((c & 0x0F) == 9) continue;
        c = (c & 0xF0) | 9;
        placed++;
    }
    // Compute adjacency counts
    for (int y = 0; y < GH; y++) {
        for (int x = 0; x < GW; x++) {
            if ((cell(x, y) & 0x0F) == 9) continue;
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= GW || ny < 0 || ny >= GH) continue;
                    if ((cell(nx, ny) & 0x0F) == 9) n++;
                }
            cell(x, y) = (cell(x, y) & 0xF0) | n;
        }
    }
}

static void draw_cell(int x, int y) {
    int ox = vx(), oy = vy();
    int px = ox + board_x() + x * CELL;
    int py = oy + board_y() + y * CELL;
    uint8_t c = cell(x, y);
    bool revealed = c & 0x10;
    bool flagged  = c & 0x20;
    uint8_t adj = c & 0x0F;
    bool is_cursor = (x == cursor_x && y == cursor_y);

    if (!revealed) {
        gfx->fillRect(px, py, CELL - 1, CELL - 1, is_cursor ? 0x8410 : 0xC618);
        gfx->drawRect(px, py, CELL - 1, CELL - 1, 0x4208);
        if (flagged) {
            gfx->fillTriangle(px + 3, py + 4, px + CELL - 4, py + CELL / 2,
                              px + 3, py + CELL - 4, 0xF800);
        }
    } else {
        gfx->fillRect(px, py, CELL - 1, CELL - 1, is_cursor ? 0x39E7 : 0x18C3);
        gfx->drawRect(px, py, CELL - 1, CELL - 1, 0x4208);
        if (adj == 9) {
            // Mine
            gfx->fillCircle(px + CELL / 2, py + CELL / 2, CELL / 3, 0xF800);
        } else if (adj > 0) {
            static const uint16_t NUM_COL[9] = {
                0xFFFF, 0x07FF, 0x07E0, 0xFD20, 0xF81F, 0xFFE0, 0x07FF, 0xFFFF, 0x8410
            };
            char buf[2] = { (char)('0' + adj), 0 };
            int ts = (CELL >= 18) ? 2 : 1;
            int cw = 6 * ts, ch = 8 * ts;
            gfx->setTextSize(ts);
            gfx->setTextColor(NUM_COL[adj]);
            gfx->setCursor(px + (CELL - cw) / 2, py + (CELL - ch) / 2);
            gfx->print(buf);
        }
    }
}

static void draw_all() {
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            draw_cell(x, y);
}

static void reveal(int x, int y) {
    if (x < 0 || x >= GW || y < 0 || y >= GH) return;
    uint8_t& c = cell(x, y);
    if (c & 0x10) return;
    if (c & 0x20) return;   // flagged
    c |= 0x10;
    revealed_count++;
    if ((c & 0x0F) == 9) {
        game_over = true;
        return;
    }
    if ((c & 0x0F) == 0) {
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (dx || dy) reveal(x + dx, y + dy);
    }
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, 16, 0x0000);
    int flagged = 0;
    for (int i = 0; i < GW * GH; i++) if (grid[i] & 0x20) flagged++;
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 4);
    gfx->printf("MINES %d/%d  CLEAR %d", flagged, MINES, revealed_count);
}

void run_minesweeper() {
    pm_game_audio_begin();
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif
    memset(grid, 0, sizeof(grid));
    cursor_x = GW / 2;
    cursor_y = GH / 2;
    revealed_count = 0;
    game_over = game_won = false;
    first_click = true;

    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, 0x0000);
    draw_hud();
    draw_all();

    bool was_a = false, was_b = false;
    uint32_t touch_start = 0;
    int16_t touch_x = -1, touch_y = -1;
    bool was_touched = false;

    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return;

        int old_cx = cursor_x, old_cy = cursor_y;
        if (in.left  && cursor_x > 0)      cursor_x--;
        if (in.right && cursor_x < GW - 1) cursor_x++;
        if (in.up    && cursor_y > 0)      cursor_y--;
        if (in.down  && cursor_y < GH - 1) cursor_y++;
        if (cursor_x != old_cx || cursor_y != old_cy) {
            draw_cell(old_cx, old_cy);
            draw_cell(cursor_x, cursor_y);
            delay(120);
        }

        if (in.a && !was_a) {
            if (first_click) {
                place_mines(cursor_x, cursor_y);
                first_click = false;
            }
            reveal(cursor_x, cursor_y);
            draw_all();
            draw_hud();
            pm_game_audio_fx_drop();
        }
        if (in.b && !was_b) {
            uint8_t& c = cell(cursor_x, cursor_y);
            if (!(c & 0x10)) {
                c ^= 0x20;
                draw_cell(cursor_x, cursor_y);
                draw_hud();
            }
        }
        was_a = in.a;
        was_b = in.b;

        // Touch input on kiosks
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
        int16_t tx, ty;
#ifdef DEVICE_C28P
        bool t = c28p_touch_read(&tx, &ty);
#else
        bool t = maxine_touch_read(&tx, &ty);
#endif
        if (t && !was_touched) {
            touch_start = millis();
            touch_x = tx; touch_y = ty;
        }
        if (!t && was_touched && touch_x >= 0) {
            int gx = (touch_x - vx() - board_x()) / CELL;
            int gy = (touch_y - vy() - board_y()) / CELL;
            if (gx >= 0 && gx < GW && gy >= 0 && gy < GH) {
                uint32_t hold = millis() - touch_start;
                if (hold > 600) {
                    // Long press: flag
                    uint8_t& c = cell(gx, gy);
                    if (!(c & 0x10)) {
                        c ^= 0x20;
                        draw_cell(gx, gy);
                        draw_hud();
                    }
                } else {
                    // Quick tap: reveal
                    cursor_x = gx; cursor_y = gy;
                    if (first_click) {
                        place_mines(cursor_x, cursor_y);
                        first_click = false;
                    }
                    reveal(cursor_x, cursor_y);
                    draw_all();
                    draw_hud();
                    pm_game_audio_fx_drop();
                }
            }
            touch_x = touch_y = -1;
        }
        was_touched = t;
#endif

        if (game_over) {
            // Reveal all mines
            for (int i = 0; i < GW * GH; i++) if ((grid[i] & 0x0F) == 9) grid[i] |= 0x10;
            draw_all();
            gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
            gfx->setTextSize(2);
            gfx->setTextColor(0xF800);
            gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
            gfx->print("BOOM!");
            delay(2400);
            return;
        }

        // Win check
        if (revealed_count >= GW * GH - MINES) {
            gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
            gfx->setTextSize(2);
            gfx->setTextColor(0x07E0);
            gfx->setCursor(vx() + VIEW_W/2 - 36, vy() + VIEW_H/2 - 8);
            gfx->print("CLEAR!");
            delay(2400);
            return;
        }

        delay(30);
        yield();
    }
}
