// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This program is free software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or any later version.
//
// fluidfortune.com

/**
 * PISCES MOON OS — snake.cpp
 * Full Snake: growing tail, score, high score saved to /snake_hs.txt on SD.
 * Input: Trackball (primary) + WASD keyboard (fallback).
 * Exit: Q key or trackball click on game-over screen.
 */

#include <Arduino_GFX_Library.h>
#include "keyboard.h"
#include "trackball.h"
#include "theme.h"
#include "SdFat.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  GAME CONSTANTS
// ─────────────────────────────────────────────
#define GRID_X      0       // Grid left edge (pixels)
#define GRID_Y      24      // Grid top edge (below header)
#define GRID_W      320
#define GRID_H      196     // Leaves room for header + score bar
#define CELL        8       // Pixel size of each grid cell
#define COLS        (GRID_W / CELL)   // 40
#define ROWS        (GRID_H / CELL)   // 24
#define MAX_LEN     (COLS * ROWS)     // 960 — absolute max snake length

#define SNAKE_COLOR     C_GREEN
#define FOOD_COLOR      C_RED
#define HEAD_COLOR      0x07FF   // Cyan head so it's easy to spot
#define BG_COLOR        C_BLACK
#define BORDER_COLOR    0x18C3   // C_DARK

#define HS_FILE         "/snake_hs.txt"

// ─────────────────────────────────────────────
//  DIRECTION ENUM
// ─────────────────────────────────────────────
enum Dir { UP, DOWN, LEFT, RIGHT };

// ─────────────────────────────────────────────
//  HIGH SCORE HELPERS
// ─────────────────────────────────────────────
static int load_high_score() {
    if (!sd.exists(HS_FILE)) return 0;
    FsFile f = sd.open(HS_FILE, O_READ);
    if (!f) return 0;
    char buf[16] = {0};
    f.read(buf, sizeof(buf) - 1);
    f.close();
    return atoi(buf);
}

static void save_high_score(int score) {
    FsFile f = sd.open(HS_FILE, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return;
    f.printf("%d", score);
    f.close();
}

// ─────────────────────────────────────────────
//  DRAW HELPERS
// ─────────────────────────────────────────────
static void draw_cell(int col, int row, uint16_t color) {
    gfx->fillRect(GRID_X + col * CELL, GRID_Y + row * CELL, CELL, CELL, color);
}

static void draw_header(int score, int hi) {
    gfx->fillRect(0, 0, 320, 24, BORDER_COLOR);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(8, 8);
    gfx->printf("SNAKE  SCORE: %d  HI: %d  Q/CLICK: QUIT", score, hi);
}

static void draw_border() {
    gfx->drawRect(GRID_X, GRID_Y, GRID_W, GRID_H, BORDER_COLOR);
}

// ─────────────────────────────────────────────
//  GAME OVER SCREEN
// ─────────────────────────────────────────────
static void show_game_over(int score, int hi, bool new_hi) {
    gfx->fillRect(60, 80, 200, 90, 0x18C3);
    gfx->drawRect(60, 80, 200, 90, C_GREEN);

    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(90, 92);
    gfx->print("GAME OVER");

    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(90, 118);
    gfx->printf("Score: %d", score);

    if (new_hi) {
        gfx->setTextColor(0xFFE0); // Yellow
        gfx->setCursor(72, 134);
        gfx->print("** NEW HIGH SCORE! **");
    } else {
        gfx->setTextColor(C_GREY);
        gfx->setCursor(85, 134);
        gfx->printf("Best: %d", hi);
    }

    gfx->setTextColor(C_GREEN);
    gfx->setCursor(68, 154);
    gfx->print("CLICK / Q = QUIT  R = RETRY");

    // Wait for input
    while (true) {
        TrackballState tb = update_trackball();
        if (tb.clicked) return;
        char c = get_keypress();
        if (c == 'q' || c == 'Q' || c == 'r' || c == 'R') return;
        delay(30);
        yield();
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_snake() {
    // Make sure trackball pins are ready
    init_trackball();

    int hi = load_high_score();

    // ── Snake state ──
    // Ring-buffer approach: body[i] = {col, row}
    struct Pt { int8_t col, row; };
    static Pt body[MAX_LEN];

    // ── Outer game loop (supports retry) ──
    while (true) {

        // ── Init game state ──
        int len    = 3;
        int head   = 0;           // Index of head in ring buffer
        int score  = 0;
        Dir dir    = RIGHT;
        Dir next_dir = RIGHT;
        bool alive = true;

        // Place snake horizontally in the middle
        for (int i = 0; i < len; i++) {
            body[i].col = COLS / 2 - i;
            body[i].row = ROWS / 2;
        }

        // Place first food
        int food_col = random(1, COLS - 1);
        int food_row = random(1, ROWS - 1);

        // ── Initial draw ──
        gfx->fillScreen(BG_COLOR);
        draw_border();
        draw_header(score, hi);

        // Draw initial snake
        for (int i = 0; i < len; i++) {
            draw_cell(body[i].col, body[i].row, (i == 0) ? HEAD_COLOR : SNAKE_COLOR);
        }
        draw_cell(food_col, food_row, FOOD_COLOR);

        // ── Speed schedule: starts at 200ms, floors at 80ms ──
        unsigned long step_ms    = 200;
        unsigned long last_step  = millis();

        // ── Game loop ──
        while (alive) {

            // ── Input: trackball takes priority, WASD as fallback ──
            TrackballState tb = update_trackball();

            if (tb.clicked) { alive = false; break; } // Click = instant quit

            // Trackball direction (prevent 180° reversal)
            if      (tb.y == -1 && dir != DOWN)  next_dir = UP;
            else if (tb.y ==  1 && dir != UP)    next_dir = DOWN;
            else if (tb.x == -1 && dir != RIGHT) next_dir = LEFT;
            else if (tb.x ==  1 && dir != LEFT)  next_dir = RIGHT;

            // Keyboard fallback (also prevent 180° reversal)
            char c = get_keypress();
            if      ((c == 'w' || c == 'W') && dir != DOWN)  next_dir = UP;
            else if ((c == 's' || c == 'S') && dir != UP)    next_dir = DOWN;
            else if ((c == 'a' || c == 'A') && dir != RIGHT) next_dir = LEFT;
            else if ((c == 'd' || c == 'D') && dir != LEFT)  next_dir = RIGHT;
            else if  (c == 'q' || c == 'Q') { alive = false; break; }

            // ── Step the snake on timer ──
            unsigned long now = millis();
            if (now - last_step < step_ms) {
                delay(10);
                yield();
                continue;
            }
            last_step = now;
            dir = next_dir;

            // Calculate new head position
            Pt new_head = body[head];
            switch (dir) {
                case UP:    new_head.row--; break;
                case DOWN:  new_head.row++; break;
                case LEFT:  new_head.col--; break;
                case RIGHT: new_head.col++; break;
            }

            // ── Wall collision ──
            if (new_head.col < 0 || new_head.col >= COLS ||
                new_head.row < 0 || new_head.row >= ROWS) {
                alive = false;
                break;
            }

            // ── Self collision (skip tail tip — it's about to move) ──
            int tail_idx = (head - len + 1 + MAX_LEN) % MAX_LEN;
            for (int i = 0; i < len - 1; i++) {
                int idx = (tail_idx + i) % MAX_LEN;
                if (body[idx].col == new_head.col &&
                    body[idx].row == new_head.row) {
                    alive = false;
                    break;
                }
            }
            if (!alive) break;

            // ── Ate food? ──
            bool ate = (new_head.col == food_col && new_head.row == food_row);

            if (!ate) {
                // Erase the tail tip
                int tail = (head - len + 1 + MAX_LEN) % MAX_LEN;
                draw_cell(body[tail].col, body[tail].row, BG_COLOR);
            } else {
                // Grow
                len++;
                score += 10;

                // Regenerate food (not on snake body)
                bool valid = false;
                while (!valid) {
                    food_col = random(1, COLS - 1);
                    food_row = random(1, ROWS - 1);
                    valid = true;
                    for (int i = 0; i < len - 1; i++) {
                        int idx = (head - i + MAX_LEN) % MAX_LEN;
                        if (body[idx].col == food_col &&
                            body[idx].row == food_row) {
                            valid = false;
                            break;
                        }
                    }
                }
                draw_cell(food_col, food_row, FOOD_COLOR);

                // Update speed
                step_ms = max((unsigned long)80, 200UL - (unsigned long)(score / 10) * 8);

                // Refresh score in header
                draw_header(score, hi);
            }

            // Repaint old head as body colour
            draw_cell(body[head].col, body[head].row, SNAKE_COLOR);

            // Advance ring buffer
            head = (head + 1) % MAX_LEN;
            body[head] = new_head;

            // Draw new head in cyan
            draw_cell(new_head.col, new_head.row, HEAD_COLOR);

            yield();
        }

        // ── Game over ──
        bool new_hi = false;
        if (score > hi) {
            hi = score;
            new_hi = true;
            save_high_score(hi);
        }

        show_game_over(score, hi, new_hi);

        // Check if player pressed R for retry vs anything else
        // (show_game_over already consumed the keypress — we re-check with a quick poll)
        // The game_over screen exits for any key; we detect retry via fresh keypress here.
        // Simple approach: show_game_over blocks; after it returns just ask again.
        gfx->fillRect(0, 215, 320, 25, 0x18C3);
        gfx->setTextColor(C_GREEN);
        gfx->setCursor(70, 222);
        gfx->print("R: PLAY AGAIN   ANY: EXIT");

        unsigned long wait_start = millis();
        bool retry = false;
        while (millis() - wait_start < 5000) {
            char c2 = get_keypress();
            if (c2 == 'r' || c2 == 'R') { retry = true; break; }
            if (c2 != 0) break;
            TrackballState tb2 = update_trackball();
            if (tb2.clicked) break; // click = exit
            delay(30);
            yield();
        }

        if (!retry) break; // Exit run_snake() back to launcher
        // Otherwise loop back to "Init game state"
    }
}