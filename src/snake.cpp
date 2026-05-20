/**
 * PISCES MOON OS — snake.cpp
 * Full Snake: growing tail, score, high score saved to /snake_hs.txt on SD.
 *
 * Multi-device layouts:
 *   T-Deck Plus  (320×240): 8px cells, 40×24 grid, trackball + NES keys
 *   T-LoRa Pager (480×222): 8px cells, 40×24 grid centered in 320px viewport
 *   Cardputer ADV (240×135): 5px cells, 48×22 grid, Fn-arrows + WASD
 *
 * Cardputer has no trackball; navigation uses Fn-layer arrows plus
 * WASD. "Click to quit" affordance is replaced with Enter or Q.
 *
 * Input mapping (universal):
 *   W / Fn+;        → UP
 *   Z / Fn+.        → DOWN
 *   A / Fn+,        → LEFT
 *   D / Fn+/        → RIGHT
 *   Q               → QUIT
 *   R               → RETRY (game-over screen only)
 *   Enter / click   → confirm / quit (game-over screen)
 */

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "keyboard.h"
#include "trackball.h"
#include "game_input.h"
#include "theme.h"
#include "pm_input.h"
#include "SdFat.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif
extern SdFat sd;

// ─────────────────────────────────────────────
//  GAME GEOMETRY — per device
// ─────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
  // Cardputer ADV: 240×135
  #define GRID_X      0
  #define GRID_Y      12
  #define GRID_W      240
  #define GRID_H      110
  #define CELL        5
  #define HEADER_H    12
  #define SCREEN_W    240
  #define SCREEN_H    135
  #define HEADER_TXT_SIZE   1
  // Game-over box: centered, 180×75
  #define OVER_X      30
  #define OVER_Y      30
  #define OVER_W      180
  #define OVER_H      75
#else
  // T-Deck Plus / T-LoRa Pager: 320×240 viewport centered on wider displays
  #define GRID_X      0
  #define GRID_Y      24
  #define GRID_W      320
  #define GRID_H      196
  #define CELL        8
  #define HEADER_H    24
  #define SCREEN_W    320
  #define SCREEN_H    240
  #define HEADER_TXT_SIZE   1
  // Game-over box: centered, 200×90
  #define OVER_X      60
  #define OVER_Y      80
  #define OVER_W      200
  #define OVER_H      90
#endif

#define COLS        (GRID_W / CELL)
#define ROWS        (GRID_H / CELL)
#define MAX_LEN     (COLS * ROWS)

#define SNAKE_COLOR     C_GREEN
#define FOOD_COLOR      C_RED
#define HEAD_COLOR      0x07FF
#define BG_COLOR        C_BLACK
#define BORDER_COLOR    0x18C3

#define HS_FILE         "/snake_hs.txt"

enum Dir { UP, DOWN, LEFT, RIGHT };
enum OverAction { OVER_EXIT, OVER_RETRY };

struct SnakePt {
    int8_t col;
    int8_t row;
};

static int view_x() {
    int w = gfx->width();
    return (w > SCREEN_W) ? (w - SCREEN_W) / 2 : 0;
}

static int view_y() {
    int h = gfx->height();
    return (h > SCREEN_H) ? (h - SCREEN_H) / 2 : 0;
}

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
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
    gfx->fillRect(view_x() + GRID_X + col * CELL,
                  view_y() + GRID_Y + row * CELL,
                  CELL, CELL, color);
}

static void draw_header(int score, int hi) {
    int vx = view_x();
    int vy = view_y();
    gfx->fillRect(vx, vy, SCREEN_W, HEADER_H, BORDER_COLOR);
    gfx->setTextSize(HEADER_TXT_SIZE);
    gfx->setTextColor(C_GREEN);
#ifdef DEVICE_CARDPUTER_ADV
    // 240px / 6px per char = 40 chars max at size 1
    // Compact: "S:NN  H:NN  Q=QUIT" (~22 chars)
    gfx->setCursor(vx + 4, vy + 3);
    gfx->printf("SNAKE  S:%d  H:%d  Q=QUIT", score, hi);
#else
    gfx->setCursor(vx + 8, vy + 8);
    gfx->printf("SNAKE  SCORE:%d  HI:%d  Z=DOWN  Q=QUIT", score, hi);
#endif
}

static void draw_border() {
    gfx->drawRect(view_x() + GRID_X, view_y() + GRID_Y, GRID_W, GRID_H, BORDER_COLOR);
}

static bool snake_has_cell(const SnakePt body[], int len, int head, int col, int row) {
    for (int i = 0; i < len; i++) {
        int idx = (head - i + MAX_LEN) % MAX_LEN;
        if (body[idx].col == col && body[idx].row == row) return true;
    }
    return false;
}

static void place_food(const SnakePt body[], int len, int head, int &food_col, int &food_row) {
    int free_cells = 0;
    for (int row = 1; row < ROWS - 1; row++) {
        for (int col = 1; col < COLS - 1; col++) {
            if (!snake_has_cell(body, len, head, col, row)) free_cells++;
        }
    }

    if (free_cells <= 0) {
        food_col = -1;
        food_row = -1;
        return;
    }

    int choice = random(free_cells);
    for (int row = 1; row < ROWS - 1; row++) {
        for (int col = 1; col < COLS - 1; col++) {
            if (snake_has_cell(body, len, head, col, row)) continue;
            if (choice-- == 0) {
                food_col = col;
                food_row = row;
                return;
            }
        }
    }
}

static void draw_playfield(const SnakePt body[], int len, int head, int food_col, int food_row) {
    gfx->fillRect(view_x() + GRID_X, view_y() + GRID_Y, GRID_W, GRID_H, BG_COLOR);

    for (int i = 0; i < len; i++) {
        int idx = (head - len + 1 + i + MAX_LEN) % MAX_LEN;
        draw_cell(body[idx].col, body[idx].row, (idx == head) ? HEAD_COLOR : SNAKE_COLOR);
    }
    if (food_col >= 0 && food_row >= 0) {
        draw_cell(food_col, food_row, FOOD_COLOR);
    }

    // Restore the frame last so edge cells cannot chew holes in it.
    draw_border();
}

// ─────────────────────────────────────────────
//  GAME OVER SCREEN
// ─────────────────────────────────────────────
static OverAction show_game_over(int score, int hi, bool new_hi) {
    int ox = view_x() + OVER_X;
    int oy = view_y() + OVER_Y;
    gfx->fillRect(ox, oy, OVER_W, OVER_H, 0x18C3);
    gfx->drawRect(ox, oy, OVER_W, OVER_H, C_GREEN);

#ifdef DEVICE_CARDPUTER_ADV
    // Cardputer compact layout — everything fits in 180×75
    //   GAME OVER (size 2)  → y=OVER_Y+6 = 36
    //   Score: N            → y=OVER_Y+28 = 58
    //   NEW HIGH / Best: N  → y=OVER_Y+40 = 70
    //   R=RETRY  Q=QUIT     → y=OVER_Y+58 = 88
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    // "GAME OVER" at size 2 is 9*12 = 108px wide; box is 180px so center
    gfx->setCursor(ox + (OVER_W - 108) / 2, oy + 6);
    gfx->print("GAME OVER");

    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    char buf[24];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    int tw = strlen(buf) * 6;
    gfx->setCursor(ox + (OVER_W - tw) / 2, oy + 28);
    gfx->print(buf);

    if (new_hi) {
        gfx->setTextColor(0xFFE0);
        const char *msg = "** NEW HIGH SCORE **";
        gfx->setCursor(ox + (OVER_W - (int)strlen(msg) * 6) / 2, oy + 40);
        gfx->print(msg);
    } else {
        gfx->setTextColor(C_GREY);
        snprintf(buf, sizeof(buf), "Best: %d", hi);
        tw = strlen(buf) * 6;
        gfx->setCursor(ox + (OVER_W - tw) / 2, oy + 40);
        gfx->print(buf);
    }

    gfx->setTextColor(C_GREEN);
    const char *prompt = "R=RETRY  Q=QUIT";
    gfx->setCursor(ox + (OVER_W - (int)strlen(prompt) * 6) / 2, oy + 58);
    gfx->print(prompt);
#else
    // T-Deck / Pager original layout
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(ox + 30, oy + 12);
    gfx->print("GAME OVER");

    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(ox + 30, oy + 38);
    gfx->printf("Score: %d", score);

    if (new_hi) {
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(ox + 12, oy + 54);
        gfx->print("** NEW HIGH SCORE! **");
    } else {
        gfx->setTextColor(C_GREY);
        gfx->setCursor(ox + 25, oy + 54);
        gfx->printf("Best: %d", hi);
    }

    gfx->setTextColor(C_GREEN);
    gfx->setCursor(ox + 20, oy + 74);
    gfx->print("R = RETRY   Q = QUIT");
#endif

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.key == 'r' || input.key == 'R' || input.start) return OVER_RETRY;
        if (input.quit || input.a || input.b || input.key == PM_KEY_ENTER) return OVER_EXIT;
        delay(30);
        yield();
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_snake() {
#ifndef DEVICE_CARDPUTER_ADV
    init_trackball();
#endif

    int hi = load_high_score();

    static SnakePt body[MAX_LEN];

    while (true) {

        int len    = 3;
        int head   = len - 1;
        int score  = 0;
        Dir dir    = RIGHT;
        Dir next_dir = RIGHT;
        bool alive = true;

        for (int i = 0; i < len; i++) {
            body[i].col = COLS / 2 - (len - 1 - i);
            body[i].row = ROWS / 2;
        }

        int food_col = -1;
        int food_row = -1;
        place_food(body, len, head, food_col, food_row);

        gfx->fillScreen(BG_COLOR);
        draw_header(score, hi);
        draw_playfield(body, len, head, food_col, food_row);

        unsigned long step_ms    = 200;
        unsigned long last_step  = millis();

        while (alive) {
            PMNesInput input = pm_read_nes_input(true);
            if (input.quit || input.a || input.b) { alive = false; break; }
            if      (input.up    && dir != DOWN)  next_dir = UP;
            else if (input.down  && dir != UP)    next_dir = DOWN;
            else if (input.left  && dir != RIGHT) next_dir = LEFT;
            else if (input.right && dir != LEFT)  next_dir = RIGHT;

            unsigned long now = millis();
            if (now - last_step < step_ms) {
                delay(10);
                yield();
                continue;
            }
            last_step = now;
            dir = next_dir;

            SnakePt new_head = body[head];
            switch (dir) {
                case UP:    new_head.row--; break;
                case DOWN:  new_head.row++; break;
                case LEFT:  new_head.col--; break;
                case RIGHT: new_head.col++; break;
            }

            // Wall collision
            if (new_head.col < 0 || new_head.col >= COLS ||
                new_head.row < 0 || new_head.row >= ROWS) {
                alive = false;
                break;
            }

            // Self collision (skip tail tip — about to move)
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

            bool ate = (new_head.col == food_col && new_head.row == food_row);

            if (ate) {
                if (len < MAX_LEN) len++;
                score += 10;
                step_ms = max((unsigned long)80, 200UL - (unsigned long)(score / 10) * 8);
            }

            head = (head + 1) % MAX_LEN;
            body[head] = new_head;

            if (ate) {
                place_food(body, len, head, food_col, food_row);
                draw_header(score, hi);
            }
            draw_playfield(body, len, head, food_col, food_row);

            yield();
        }

        // ── Game over ──
        bool new_hi = false;
        if (score > hi) {
            hi = score;
            new_hi = true;
            save_high_score(hi);
        }

        if (show_game_over(score, hi, new_hi) != OVER_RETRY) break;
    }
}
