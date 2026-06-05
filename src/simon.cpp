// Pisces Moon OS — Simon
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Classic memory game. Four colored panels light up in a sequence;
// the player must reproduce the sequence by tapping the panels
// back. Each round adds one more step. Game over when the player
// makes a mistake.

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_input.h"
#include "game_audio.h"
#include "theme.h"
#include "simon.h"
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

static constexpr int MAX_SEQ = 60;
static uint8_t sequence[MAX_SEQ];
static int seq_len;
static int high_score;

struct Panel { int x, y, w, h; uint16_t bright; uint16_t dim; uint16_t tone; };

static Panel panels[4];

static void compute_panels() {
    int margin = 8;
    int pad = 6;
    int panel_w = (VIEW_W - margin * 2 - pad) / 2;
    int panel_h = (VIEW_H - 40 - pad) / 2;
    int x0 = vx() + margin;
    int y0 = vy() + 28;
    panels[0] = { x0,                     y0,                     panel_w, panel_h, 0x07E0, 0x0300, 392 };
    panels[1] = { x0 + panel_w + pad,     y0,                     panel_w, panel_h, 0xF800, 0x6000, 196 };
    panels[2] = { x0,                     y0 + panel_h + pad,     panel_w, panel_h, 0xFFE0, 0x6300, 523 };
    panels[3] = { x0 + panel_w + pad,     y0 + panel_h + pad,     panel_w, panel_h, 0x07FF, 0x0317, 659 };
}

static void draw_panel(int idx, bool bright) {
    const Panel& p = panels[idx];
    gfx->fillRect(p.x, p.y, p.w, p.h, bright ? p.bright : p.dim);
    gfx->drawRect(p.x, p.y, p.w, p.h, 0xFFFF);
}

static void flash_panel(int idx, int duration_ms) {
    draw_panel(idx, true);
    pm_game_audio_fx_line();
    delay(duration_ms);
    draw_panel(idx, false);
    delay(80);
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, 22, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 6);
    gfx->printf("ROUND %d   BEST %d", seq_len, high_score);
}

static int hit_panel(int x, int y) {
    for (int i = 0; i < 4; i++) {
        const Panel& p = panels[i];
        if (x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h) return i;
    }
    return -1;
}

static int wait_for_press() {
    bool was_a = false, was_b = false, was_l = false, was_r = false, was_u = false, was_d = false;
    bool was_touched = false;
    int cursor = 0;
    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return -2;
        // Direct touch
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
        int16_t tx, ty;
#ifdef DEVICE_C28P
        bool t = c28p_touch_read(&tx, &ty);
#else
        bool t = maxine_touch_read(&tx, &ty);
#endif
        if (t && !was_touched) {
            int p = hit_panel(tx, ty);
            if (p >= 0) return p;
        }
        was_touched = t;
#endif
        // Map directional input to panels:
        // Up=0(green), Right=1(red), Down=2(yellow), Left=3(blue)
        if (in.up && !was_u) return 0;
        if (in.right && !was_r) return 1;
        if (in.down && !was_d) return 2;
        if (in.left && !was_l) return 3;
        was_u = in.up; was_d = in.down; was_l = in.left; was_r = in.right;
        delay(20);
        yield();
    }
}

static void load_best() {
    high_score = 0;
    nosql_init("settings");
    int total = nosql_get_count("settings");
    String t, c;
    for (int i = 0; i < total; i++) {
        if (!nosql_get_entry("settings", i, t, c)) continue;
        if (t == "simon_best") { high_score = c.toInt(); return; }
    }
}
static void save_best() {
    nosql_init("settings");
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", high_score);
    nosql_save_entry("settings", "simon_best", buf);
}

void run_simon() {
    pm_game_audio_begin();
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif
    load_best();
    compute_panels();
    seq_len = 0;

    gfx->fillRect(vx(), vy(), VIEW_W, VIEW_H, 0x0000);
    draw_hud();
    for (int i = 0; i < 4; i++) draw_panel(i, false);

    delay(800);

    while (true) {
        // Add a step
        if (seq_len < MAX_SEQ) {
            sequence[seq_len++] = random(4);
        }
        draw_hud();

        // Play sequence
        int duration = max(180, 480 - seq_len * 6);
        for (int i = 0; i < seq_len; i++) {
            flash_panel(sequence[i], duration);
        }

        // Player input
        for (int i = 0; i < seq_len; i++) {
            int p = wait_for_press();
            if (p == -2) {
                if (seq_len - 1 > high_score) { high_score = seq_len - 1; save_best(); }
                return;
            }
            flash_panel(p, 160);
            if (p != sequence[i]) {
                if (seq_len - 1 > high_score) { high_score = seq_len - 1; save_best(); }
                gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                gfx->setTextSize(2);
                gfx->setTextColor(0xF800);
                gfx->setCursor(vx() + VIEW_W/2 - 36, vy() + VIEW_H/2 - 8);
                gfx->print("WRONG");
                delay(2400);
                return;
            }
        }
        delay(500);
    }
}
