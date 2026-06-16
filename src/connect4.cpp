// Pisces Moon OS — Connect Four
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// 7-column, 6-row Connect Four against an AI. Drop discs into
// columns; first to align 4 in a row wins.
//
// AI: minimax with alpha-beta, depth 5. Light position scoring
// (count 3-in-a-rows that can be extended, center-column bias).

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_input.h"
#include "game_audio.h"
#include "theme.h"
#include "connect4.h"
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

#define COLS 7
#define ROWS 6

static int board[ROWS][COLS];   // 0 empty, 1 player, 2 AI
static int cursor_col;
static int turn;                // 1 player, 2 AI

static int cell_size() {
#ifdef DEVICE_CARDPUTER_ADV
    return 16;
#elif defined(DEVICE_MAXINE)
    return 56;
#elif defined(DEVICE_TLORAPAGER)
    return 26;
#else
    return 22;
#endif
}
static int board_x() { return (VIEW_W - COLS * cell_size()) / 2; }
static int board_y() { return 28; }

static int drop_row(int c) {
    if (c < 0 || c >= COLS) return -1;
    for (int r = ROWS - 1; r >= 0; r--)
        if (board[r][c] == 0) return r;
    return -1;
}

static int check_win_at(int r, int c, int player) {
    static const int dirs[4][2] = {{0,1},{1,0},{1,1},{1,-1}};
    for (int d = 0; d < 4; d++) {
        int dr = dirs[d][0], dc = dirs[d][1];
        int count = 1;
        for (int i = 1; i < 4; i++) {
            int rr = r + dr * i, cc = c + dc * i;
            if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) break;
            if (board[rr][cc] != player) break;
            count++;
        }
        for (int i = 1; i < 4; i++) {
            int rr = r - dr * i, cc = c - dc * i;
            if (rr < 0 || rr >= ROWS || cc < 0 || cc >= COLS) break;
            if (board[rr][cc] != player) break;
            count++;
        }
        if (count >= 4) return 1;
    }
    return 0;
}

static int score_window(int a, int b, int c, int d, int player) {
    int ours = 0, theirs = 0;
    int vals[4] = {a, b, c, d};
    for (int i = 0; i < 4; i++) {
        if (vals[i] == player) ours++;
        else if (vals[i] != 0) theirs++;
    }
    if (ours == 4) return 100000;
    if (theirs == 4) return -100000;
    if (ours == 3 && theirs == 0) return 50;
    if (ours == 2 && theirs == 0) return 5;
    if (theirs == 3 && ours == 0) return -60;
    return 0;
}

static int evaluate(int player) {
    int s = 0;
    // Center column bias
    for (int r = 0; r < ROWS; r++) if (board[r][COLS / 2] == player) s += 3;
    // All 4-windows
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c <= COLS - 4; c++)
            s += score_window(board[r][c], board[r][c+1], board[r][c+2], board[r][c+3], player);
    for (int c = 0; c < COLS; c++)
        for (int r = 0; r <= ROWS - 4; r++)
            s += score_window(board[r][c], board[r+1][c], board[r+2][c], board[r+3][c], player);
    for (int r = 0; r <= ROWS - 4; r++)
        for (int c = 0; c <= COLS - 4; c++)
            s += score_window(board[r][c], board[r+1][c+1], board[r+2][c+2], board[r+3][c+3], player);
    for (int r = 3; r < ROWS; r++)
        for (int c = 0; c <= COLS - 4; c++)
            s += score_window(board[r][c], board[r-1][c+1], board[r-2][c+2], board[r-3][c+3], player);
    return s;
}

static bool board_full() {
    for (int c = 0; c < COLS; c++) if (board[0][c] == 0) return false;
    return true;
}

static int minimax(int depth, int alpha, int beta, bool maximizing,
                   int* best_col_out, int last_r, int last_c, int last_player) {
    if (last_r >= 0 && check_win_at(last_r, last_c, last_player)) {
        return (last_player == 2) ? 100000 + depth : -100000 - depth;
    }
    if (depth == 0 || board_full()) {
        return evaluate(2) - evaluate(1);
    }
    int player = maximizing ? 2 : 1;
    int best = maximizing ? -1000000 : 1000000;
    int best_col = COLS / 2;
    // Try center-first move ordering for better pruning
    static const int order[COLS] = {3, 2, 4, 1, 5, 0, 6};
    for (int oi = 0; oi < COLS; oi++) {
        int c = order[oi];
        int r = drop_row(c);
        if (r < 0) continue;
        board[r][c] = player;
        int v = minimax(depth - 1, alpha, beta, !maximizing, nullptr, r, c, player);
        board[r][c] = 0;
        if (maximizing) {
            if (v > best) { best = v; best_col = c; }
            if (best > alpha) alpha = best;
        } else {
            if (v < best) { best = v; best_col = c; }
            if (best < beta) beta = best;
        }
        if (alpha >= beta) break;
    }
    if (best_col_out) *best_col_out = best_col;
    return best;
}

static int ai_move() {
    int col = COLS / 2;
    minimax(5, -1000000, 1000000, true, &col, -1, -1, 0);
    return col;
}

static void draw_cell(int r, int c) {
    int cs = cell_size();
    int x = vx() + board_x() + c * cs;
    int y = vy() + board_y() + r * cs;
    gfx->fillRect(x, y, cs, cs, 0x001F);
    gfx->drawRect(x, y, cs, cs, 0x000F);
    uint16_t fill = 0x0000;
    if (board[r][c] == 1) fill = 0xF800;
    if (board[r][c] == 2) fill = 0xFFE0;
    gfx->fillCircle(x + cs / 2, y + cs / 2, cs / 2 - 3, fill);
}

static void draw_board() {
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            draw_cell(r, c);
}

static void draw_cursor() {
    int cs = cell_size();
    int by = vy() + board_y() - cs / 2 - 4;
    if (by < vy() + 18) by = vy() + 18;   // don't overdraw HUD
    int bx_full = vx() + board_x();
    // Clear cursor row
    gfx->fillRect(bx_full, by, COLS * cs, cs / 2 + 4, 0x0000);
    int cx = bx_full + cursor_col * cs + cs / 2;
    gfx->fillCircle(cx, by + cs / 4, cs / 4, turn == 1 ? 0xF800 : 0xFFE0);
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, 18, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 5);
    gfx->print(turn == 1 ? "YOUR TURN (RED)" : "AI THINKING (YELLOW)...");
}

void run_connect4() {
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
    memset(board, 0, sizeof(board));
    cursor_col = 3;
    turn = 1;
    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, 0x0000);
    draw_hud();
    draw_board();
    draw_cursor();

    bool was_a = false;
    bool was_touched = false;
    int16_t touch_x_start = -1;
    int last_cursor = cursor_col;

    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return;

        if (turn == 1) {
            int oc = cursor_col;
            if (in.left  && cursor_col > 0)         cursor_col--;
            if (in.right && cursor_col < COLS - 1)  cursor_col++;
            if (oc != cursor_col) {
                draw_cursor();
                delay(120);
            }

#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
            int16_t tx, ty;
#ifdef DEVICE_C28P
            bool t = c28p_touch_read(&tx, &ty);
#elif defined(DEVICE_C5)
            bool t = c5_touch_read(&tx, &ty);
#else
            bool t = maxine_touch_read(&tx, &ty);
#endif
            if (t && ty < VIEW_H) {
                int gc = (tx - vx() - board_x()) / cell_size();
                if (gc >= 0 && gc < COLS && gc != cursor_col) {
                    cursor_col = gc;
                    draw_cursor();
                }
                if (t && !was_touched) touch_x_start = tx;
            }
            if (!t && was_touched && touch_x_start >= 0) {
                int gc = (touch_x_start - vx() - board_x()) / cell_size();
                if (gc >= 0 && gc < COLS) {
                    cursor_col = gc;
                    in.a = true;
                }
                touch_x_start = -1;
            }
            was_touched = t;
#endif

            if (in.a && !was_a) {
                int r = drop_row(cursor_col);
                if (r >= 0) {
                    board[r][cursor_col] = 1;
                    draw_cell(r, cursor_col);
                    pm_game_audio_fx_drop();
                    if (check_win_at(r, cursor_col, 1)) {
                        draw_hud();
                        gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                        gfx->setTextSize(2);
                        gfx->setTextColor(0xF800);
                        gfx->setCursor(vx() + VIEW_W/2 - 48, vy() + VIEW_H/2 - 8);
                        gfx->print("YOU WIN!");
                        delay(2400);
                        return;
                    }
                    if (board_full()) {
                        gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                        gfx->setTextSize(2);
                        gfx->setTextColor(0xFFFF);
                        gfx->setCursor(vx() + VIEW_W/2 - 24, vy() + VIEW_H/2 - 8);
                        gfx->print("DRAW");
                        delay(2400);
                        return;
                    }
                    turn = 2;
                    draw_hud();
                }
            }
            was_a = in.a;
        } else {
            // AI turn
            int c = ai_move();
            int r = drop_row(c);
            if (r >= 0) {
                board[r][c] = 2;
                draw_cell(r, c);
                pm_game_audio_fx_drop();
                if (check_win_at(r, c, 2)) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xFFE0);
                    gfx->setCursor(vx() + VIEW_W/2 - 36, vy() + VIEW_H/2 - 8);
                    gfx->print("AI WINS");
                    delay(2400);
                    return;
                }
                if (board_full()) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xFFFF);
                    gfx->setCursor(vx() + VIEW_W/2 - 24, vy() + VIEW_H/2 - 8);
                    gfx->print("DRAW");
                    delay(2400);
                    return;
                }
                turn = 1;
                draw_hud();
                draw_cursor();
            }
        }
        delay(30);
        yield();
    }
}
