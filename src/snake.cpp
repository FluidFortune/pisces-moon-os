/**
 * PISCES MOON OS — snake.cpp
 *
 * Full Snake: growing tail, wraparound-free walls, score, high-score
 * persistence. Rewritten from the ring-buffer model to a simple
 * shift-down body array: body[0] = head, body[len-1] = tail. Each
 * step we shift the array down by one and write the new head at
 * body[0]. No modular indexing, no head/tail wraparound math, no
 * int8_t coordinates. Easier to reason about; same gameplay.
 *
 * Per-device layout:
 *   T-Deck Plus   320x240  8px cells   40x24 grid
 *   T-LoRa Pager  480x222  8px cells   40x24 grid (320x240 centered)
 *   Cardputer ADV 240x135  5px cells   48x22 grid
 *   C28P          240x320  8px cells   30x23 grid (top 200px, dpad below)
 *   C5            240x320  8px cells   30x23 grid (top 200px, dpad below)
 *   Maxine        480x800  16px cells  30x32 grid (top 520px, dpad below)
 *
 * Input mapping (universal — pm_read_nes_input handles the mapping):
 *   D-pad / trackball / arrow keys / WASD = directions
 *   A / B / quit                          = end game
 *   START / R                             = retry on game-over
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
extern SdFat sd;

// ─────────────────────────────────────────────
//  GAME GEOMETRY — per device
// ─────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
  #define GRID_X      0
  #define GRID_Y      12
  #define GRID_W      240
  #define GRID_H      110
  #define CELL        5
  #define HEADER_H    12
  #define SCREEN_W    240
  #define SCREEN_H    135
  #define OVER_X      30
  #define OVER_Y      30
  #define OVER_W      180
  #define OVER_H      75
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
  // C28P + C5 share the same 240x320 portrait panel and the same
  // 240x200 game viewport above the virtual dpad strip. Geometry is
  // identical; only the touch driver underneath differs.
  #define GRID_X      0
  #define GRID_Y      16
  #define GRID_W      240
  #define GRID_H      184
  #define CELL        8
  #define HEADER_H    14
  #define SCREEN_W    240
  #define SCREEN_H    200
  #define OVER_X      20
  #define OVER_Y      60
  #define OVER_W      200
  #define OVER_H      80
#elif defined(DEVICE_MAXINE)
  // Maxine portrait: top 520px is the game viewport (MAXINE_GAME_VIEW_H),
  // bottom 280px is the virtual D-pad. 16px cells × 30 cols × 32 rows
  // fills the viewport cleanly with a 24px header at the top.
  #define GRID_X      0
  #define GRID_Y      24
  #define GRID_W      480
  #define GRID_H      512
  #define CELL        16
  #define HEADER_H    24
  #define SCREEN_W    480
  #define SCREEN_H    520
  #define OVER_X      80
  #define OVER_Y      160
  #define OVER_W      320
  #define OVER_H      160
#else
  // T-Deck Plus / T-LoRa Pager: 320x240 viewport, centered on wider panels.
  #define GRID_X      0
  #define GRID_Y      24
  #define GRID_W      320
  #define GRID_H      192
  #define CELL        8
  #define HEADER_H    24
  #define SCREEN_W    320
  #define SCREEN_H    240
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

// ─────────────────────────────────────────────
//  Types
// ─────────────────────────────────────────────
enum Dir { D_UP, D_DOWN, D_LEFT, D_RIGHT };
enum OverAction { OVER_EXIT, OVER_RETRY };

struct Cell { int16_t col; int16_t row; };

// ─────────────────────────────────────────────
//  Game state — one struct, lives in run_snake().
//  Simpler than passing 8 parameters around.
// ─────────────────────────────────────────────
struct SnakeGame {
    Cell body[MAX_LEN];    // body[0] = head, body[len-1] = tail
    int  len;
    Dir  dir;              // current direction (committed on each step)
    Dir  next_dir;         // direction queued by input (commits next step)
    int  food_col;
    int  food_row;
    int  score;
    bool alive;
};

// ─────────────────────────────────────────────
//  Viewport helpers
// ─────────────────────────────────────────────
static int view_x() {
    int w = gfx->width();
    return (w > SCREEN_W) ? (w - SCREEN_W) / 2 : 0;
}

static int view_y() {
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    return 0;   // top-anchored (dpad owns the bottom strip)
#else
    int h = gfx->height();
    return (h > SCREEN_H) ? (h - SCREEN_H) / 2 : 0;
#endif
}

// ─────────────────────────────────────────────
//  High-score persistence
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
//  Drawing — one cell, the header, the border, the playfield
// ─────────────────────────────────────────────
static inline void draw_cell(int col, int row, uint16_t color) {
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) return;
    gfx->fillRect(view_x() + GRID_X + col * CELL,
                  view_y() + GRID_Y + row * CELL,
                  CELL, CELL, color);
}

static void draw_header(int score, int hi) {
#if defined(DEVICE_C28P) || defined(DEVICE_C5)
    // Score line painted into the right side of the exit bar.
    // Same layout on C28P + C5 since both have the same 240x14
    // header strip above the game viewport.
    gfx->fillRect(80, 0, 240 - 80, 14, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(90, 4);
    gfx->printf("SNAKE  S:%d  H:%d", score, hi);
    return;
#endif
    int vx = view_x();
    int vy = view_y();
    gfx->fillRect(vx, vy, SCREEN_W, HEADER_H, BORDER_COLOR);
    gfx->setTextColor(C_GREEN);
#ifdef DEVICE_CARDPUTER_ADV
    gfx->setTextSize(1);
    gfx->setCursor(vx + 4, vy + 3);
    gfx->printf("SNAKE  S:%d  H:%d  Q=QUIT", score, hi);
#elif defined(DEVICE_MAXINE)
    gfx->setTextSize(2);
    gfx->setCursor(vx + 12, vy + 4);
    gfx->printf("SNAKE   SCORE:%d   HI:%d", score, hi);
#else
    gfx->setTextSize(1);
    gfx->setCursor(vx + 8, vy + 8);
    gfx->printf("SNAKE  SCORE:%d  HI:%d  Z=DOWN  Q=QUIT", score, hi);
#endif
}

static void draw_border() {
    gfx->drawRect(view_x() + GRID_X, view_y() + GRID_Y,
                  GRID_W, GRID_H, BORDER_COLOR);
}

// Full repaint — no delta tricks. Easier to reason about; only
// COLS*ROWS cells max, well within the fill budget on every target.
static void draw_playfield(const SnakeGame &g) {
    // Background — exactly the grid rect, not the whole screen.
    gfx->fillRect(view_x() + GRID_X, view_y() + GRID_Y,
                  GRID_W, GRID_H, BG_COLOR);

    // Body: tail first, head last (so a body cell on top of the head
    // doesn't hide the head color on an overlap).
    for (int i = g.len - 1; i >= 1; i--) {
        draw_cell(g.body[i].col, g.body[i].row, SNAKE_COLOR);
    }
    if (g.len > 0) {
        draw_cell(g.body[0].col, g.body[0].row, HEAD_COLOR);
    }

    // Food.
    if (g.food_col >= 0 && g.food_row >= 0) {
        draw_cell(g.food_col, g.food_row, FOOD_COLOR);
    }

    // Border last so cells never chew into it.
    draw_border();
}

// ─────────────────────────────────────────────
//  Game logic — keep one source of truth, do not duplicate
//  collision tests between place_food and the step path.
// ─────────────────────────────────────────────
static bool cell_in_snake(const SnakeGame &g, int col, int row) {
    for (int i = 0; i < g.len; i++) {
        if (g.body[i].col == col && g.body[i].row == row) return true;
    }
    return false;
}

static void place_food(SnakeGame &g) {
    int free_cells = 0;
    for (int r = 1; r < ROWS - 1; r++) {
        for (int c = 1; c < COLS - 1; c++) {
            if (!cell_in_snake(g, c, r)) free_cells++;
        }
    }
    if (free_cells <= 0) {
        g.food_col = -1;
        g.food_row = -1;
        return;
    }

    int choice = random(free_cells);
    for (int r = 1; r < ROWS - 1; r++) {
        for (int c = 1; c < COLS - 1; c++) {
            if (cell_in_snake(g, c, r)) continue;
            if (choice-- == 0) {
                g.food_col = c;
                g.food_row = r;
                return;
            }
        }
    }
}

static void init_snake(SnakeGame &g) {
    g.len      = 3;
    g.dir      = D_RIGHT;
    g.next_dir = D_RIGHT;
    g.score    = 0;
    g.alive    = true;

    // Head at the center; tail extends to the left.
    int cx = COLS / 2;
    int cy = ROWS / 2;
    g.body[0] = { (int16_t)cx,       (int16_t)cy };  // head
    g.body[1] = { (int16_t)(cx - 1), (int16_t)cy };
    g.body[2] = { (int16_t)(cx - 2), (int16_t)cy };  // tail

    g.food_col = -1;
    g.food_row = -1;
    place_food(g);
}

// Commit one step. Reads g.next_dir, advances, handles collisions,
// growth, and food respawn. Returns true if the game is still alive.
static bool step_snake(SnakeGame &g) {
    // Commit queued direction, rejecting illegal 180° reversals.
    Dir d = g.next_dir;
    if ((d == D_UP    && g.dir == D_DOWN)  ||
        (d == D_DOWN  && g.dir == D_UP)    ||
        (d == D_LEFT  && g.dir == D_RIGHT) ||
        (d == D_RIGHT && g.dir == D_LEFT)) {
        d = g.dir;
    }
    g.dir = d;

    // Compute new head.
    Cell new_head = g.body[0];
    switch (d) {
        case D_UP:    new_head.row--; break;
        case D_DOWN:  new_head.row++; break;
        case D_LEFT:  new_head.col--; break;
        case D_RIGHT: new_head.col++; break;
    }

    // Wall collision.
    if (new_head.col < 0 || new_head.col >= COLS ||
        new_head.row < 0 || new_head.row >= ROWS) {
        return false;
    }

    bool ate = (new_head.col == g.food_col && new_head.row == g.food_row);

    // Self-collision. Skip the tail cell if we are NOT eating, because
    // the tail is about to vacate. If we ARE eating, the tail stays
    // (snake grows), so check it too.
    int check_end = ate ? g.len : (g.len - 1);
    for (int i = 0; i < check_end; i++) {
        if (g.body[i].col == new_head.col && g.body[i].row == new_head.row) {
            return false;
        }
    }

    // Apply the move: shift body down by one, write new head at [0].
    // If growing (ate), bump len BEFORE the shift so the old tail
    // sticks around.
    if (ate) {
        if (g.len < MAX_LEN) g.len++;
        g.score += 10;
    }
    // Shift: body[1..len-1] = body[0..len-2]. Bytes-aligned move is fine.
    memmove(&g.body[1], &g.body[0], sizeof(Cell) * (g.len - 1));
    g.body[0] = new_head;

    if (ate) place_food(g);

    return true;
}

// ─────────────────────────────────────────────
//  Game-over modal (per-device layout)
// ─────────────────────────────────────────────
static OverAction show_game_over(int score, int hi, bool new_hi) {
    int ox = view_x() + OVER_X;
    int oy = view_y() + OVER_Y;
    gfx->fillRect(ox, oy, OVER_W, OVER_H, 0x18C3);
    gfx->drawRect(ox, oy, OVER_W, OVER_H, C_GREEN);

#if defined(DEVICE_CARDPUTER_ADV)
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(ox + (OVER_W - 108) / 2, oy + 6);
    gfx->print("GAME OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    char buf[24];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    gfx->setCursor(ox + (OVER_W - (int)strlen(buf) * 6) / 2, oy + 28);
    gfx->print(buf);
    if (new_hi) {
        gfx->setTextColor(0xFFE0);
        const char *m = "** NEW HIGH SCORE **";
        gfx->setCursor(ox + (OVER_W - (int)strlen(m) * 6) / 2, oy + 40);
        gfx->print(m);
    } else {
        gfx->setTextColor(C_GREY);
        snprintf(buf, sizeof(buf), "Best: %d", hi);
        gfx->setCursor(ox + (OVER_W - (int)strlen(buf) * 6) / 2, oy + 40);
        gfx->print(buf);
    }
    gfx->setTextColor(C_GREEN);
    const char *p = "R=RETRY  Q=QUIT";
    gfx->setCursor(ox + (OVER_W - (int)strlen(p) * 6) / 2, oy + 58);
    gfx->print(p);
#elif defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    int title_x = ox + (OVER_W - 108) / 2;
    int line_y  = oy + (OVER_H >= 120 ? 40 : 30);
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(title_x, oy + 12);
    gfx->print("GAME OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    char buf[24];
    snprintf(buf, sizeof(buf), "Score: %d", score);
    gfx->setCursor(ox + (OVER_W - (int)strlen(buf) * 6) / 2, line_y);
    gfx->print(buf);
    if (new_hi) {
        gfx->setTextColor(0xFFE0);
        const char *m = "** NEW HIGH SCORE **";
        gfx->setCursor(ox + (OVER_W - (int)strlen(m) * 6) / 2, line_y + 14);
        gfx->print(m);
    } else {
        gfx->setTextColor(C_GREY);
        snprintf(buf, sizeof(buf), "Best: %d", hi);
        gfx->setCursor(ox + (OVER_W - (int)strlen(buf) * 6) / 2, line_y + 14);
        gfx->print(buf);
    }
    gfx->setTextColor(C_GREEN);
    const char *p = "A=RETRY  B=QUIT";
    gfx->setCursor(ox + (OVER_W - (int)strlen(p) * 6) / 2, oy + OVER_H - 24);
    gfx->print(p);
#else
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

    // ── Wait for the user to release any input that's currently
    //    held BEFORE accepting a new press. Otherwise the same press
    //    that ended the game instantly chooses retry/exit.
    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (!in.a && !in.b && !in.start && in.key == 0 &&
            !in.quit && !in.trackball.clicked) break;
        delay(20);
        yield();
    }

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
        if (input.a)               return OVER_RETRY;
        if (input.b || input.quit) return OVER_EXIT;
#else
        if (input.key == 'r' || input.key == 'R' || input.start)
            return OVER_RETRY;
        if (input.quit || input.a || input.b || input.key == PM_KEY_ENTER)
            return OVER_EXIT;
#endif
        delay(30);
        yield();
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_snake() {
#if !defined(DEVICE_CARDPUTER_ADV) && !defined(DEVICE_C28P) && !defined(DEVICE_C5) && !defined(DEVICE_MAXINE)
    init_trackball();
#endif
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_C5
    c5_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif

    int hi = load_high_score();

    // Static so the array doesn't blow the local stack on the bigger grids.
    static SnakeGame g;

    while (true) {
        init_snake(g);

#if defined(DEVICE_C28P) || defined(DEVICE_C5)
        // C28P + C5: clear only the game viewport (top 200px). The
        // dpad chrome below must persist across game-over and retry.
        gfx->fillRect(0, 0, 240, 200, BG_COLOR);
#elif defined(DEVICE_MAXINE)
        // Maxine: clear only the game viewport. The dpad chrome below
        // (y >= 520) must persist across game-over and retry.
        gfx->fillRect(0, 0, 480, SCREEN_H, BG_COLOR);
#else
        gfx->fillScreen(BG_COLOR);
#endif
        draw_header(g.score, hi);
        draw_playfield(g);

        // ── On entry, drain any input that was used to launch the
        //    game so the first frame doesn't see a held key as a
        //    direction press.
        while (true) {
            PMNesInput in = pm_read_nes_input(true);
            if (!in.a && !in.b && !in.start && in.key == 0 &&
                !in.up && !in.down && !in.left && !in.right &&
                !in.quit && !in.trackball.clicked) break;
            delay(15);
            yield();
        }

        // ── Speed scales with score; floor at 80ms for playability.
        auto compute_step_ms = [](int score) -> unsigned long {
            unsigned long base = 200UL;
            unsigned long speedup = (unsigned long)(score / 10) * 8UL;
            if (speedup > base - 80UL) return 80UL;
            return base - speedup;
        };

        unsigned long step_ms   = compute_step_ms(0);
        unsigned long last_step = millis();

        while (g.alive) {
            PMNesInput input = pm_read_nes_input(true);

            // Quit explicitly via the universal quit signal. A/B no
            // longer end a live game by themselves — they were
            // killing the snake on stray trackball clicks. To exit
            // mid-game now: hold quit, or press the device's quit
            // key / dpad exit zone (input.quit).
            if (input.quit) { g.alive = false; break; }

            // Queue direction. The 180° guard is enforced at commit
            // time in step_snake(), so we don't need it here too.
            if      (input.up)    g.next_dir = D_UP;
            else if (input.down)  g.next_dir = D_DOWN;
            else if (input.left)  g.next_dir = D_LEFT;
            else if (input.right) g.next_dir = D_RIGHT;

            unsigned long now = millis();
            if (now - last_step < step_ms) {
                delay(10);
                yield();
                continue;
            }
            last_step = now;

            int prev_score = g.score;
            if (!step_snake(g)) {
                g.alive = false;
                break;
            }
            if (g.score != prev_score) {
                step_ms = compute_step_ms(g.score);
                draw_header(g.score, hi);
            }
            draw_playfield(g);
            yield();
        }

        // Game over.
        bool new_hi = false;
        if (g.score > hi) {
            hi = g.score;
            new_hi = true;
            save_high_score(hi);
        }
        if (show_game_over(g.score, hi, new_hi) != OVER_RETRY) break;
    }
}
