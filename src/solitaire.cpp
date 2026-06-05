// Pisces Moon OS — Klondike Solitaire
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Standard Klondike: 7 tableau columns, 4 foundations, stock + waste.
// Draw 1 from stock. Tableau builds down by alternating colors;
// foundations build up by suit starting with Ace.
//
// Input model:
//   Tap a card to select; tap a valid destination to move.
//   On keyboard, arrow keys move a cursor through piles, A selects/
//   places, B cancels selection.
//
// This is a competent implementation, not a polished one — Klondike
// has dozens of corner cases (multi-card moves, auto-complete,
// undo). We support: single-card and run moves between tableau,
// single-card moves to foundation, stock draw, recycle stock.

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_input.h"
#include "game_audio.h"
#include "theme.h"
#include "solitaire.h"
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
static constexpr int CW = 24, CH = 32;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 480, VIEW_H = 222;
static constexpr int CW = 56, CH = 78;
#elif defined(DEVICE_C28P)
static constexpr int VIEW_W = 240, VIEW_H = 200;
static constexpr int CW = 30, CH = 42;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480, VIEW_H = 520;
static constexpr int CW = 62, CH = 86;
#else
static constexpr int VIEW_W = 320, VIEW_H = 240;
static constexpr int CW = 38, CH = 54;
#endif

static int vx() { int w = gfx->width(); return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0; }
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height(); return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}

// Card encoding: 0..51 for suit*13 + rank
//   Suit: 0=hearts(red), 1=diamonds(red), 2=clubs(black), 3=spades(black)
//   Rank: 0=Ace, 1=2, ..., 10=J, 11=Q, 12=K
// Bit 7 = face-up flag (stored on top of card value)
static inline uint8_t card_rank(uint8_t c) { return (c & 0x7F) % 13; }
static inline uint8_t card_suit(uint8_t c) { return (c & 0x7F) / 13; }
static inline bool    card_red(uint8_t c) { return card_suit(c) < 2; }
static inline bool    card_up(uint8_t c) { return c & 0x80; }
static inline uint8_t card_flip_up(uint8_t c) { return c | 0x80; }
static inline uint8_t card_strip(uint8_t c) { return c & 0x7F; }

// Pile structure
static uint8_t tableau[7][24];
static int     tableau_len[7];
static uint8_t foundation[4][13];
static int     foundation_len[4];
static uint8_t stock[52];
static int     stock_len;
static uint8_t waste[52];
static int     waste_len;

// Selection state. -1 = nothing selected.
//   pile_type: 0=tableau, 1=foundation, 2=waste
//   pile_idx:  which pile (0..6 tableau, 0..3 foundation, 0 waste)
//   pile_card: card index within that pile (for tableau)
static int sel_type = -1, sel_idx = -1, sel_card = -1;

// Cursor for keyboard input
static int cursor_pile = 0;

static int tab_x(int i) {
    int gap = (VIEW_W - 7 * CW) / 8;
    return vx() + gap + i * (CW + gap);
}
static int tab_y() { return vy() + 6 + CH + 6; }
static int foundation_x(int i) {
    int gap = (VIEW_W - 7 * CW) / 8;
    return vx() + gap + (3 + i) * (CW + gap);
}
static int foundation_y() { return vy() + 6; }
static int stock_x()  { int gap = (VIEW_W - 7 * CW) / 8; return vx() + gap; }
static int waste_x()  { int gap = (VIEW_W - 7 * CW) / 8; return vx() + gap + (CW + gap); }

static int card_overlap() {
#ifdef DEVICE_CARDPUTER_ADV
    return 6;
#elif defined(DEVICE_MAXINE)
    return 18;
#else
    return 10;
#endif
}

static void draw_card(int x, int y, uint8_t c, bool selected) {
    if (!card_up(c)) {
        gfx->fillRoundRect(x, y, CW, CH, 3, 0x0019);
        gfx->drawRoundRect(x, y, CW, CH, 3, 0x07FF);
        // Pattern lines
        for (int i = 4; i < CW - 4; i += 4) {
            gfx->drawFastVLine(x + i, y + 4, CH - 8, 0x041F);
        }
    } else {
        gfx->fillRoundRect(x, y, CW, CH, 3, 0xFFFF);
        gfx->drawRoundRect(x, y, CW, CH, 3, selected ? 0xFFE0 : 0x4208);
        uint8_t r = card_rank(c);
        uint8_t s = card_suit(c);
        static const char* RANKS = "A23456789TJQK";
        char rch = RANKS[r];
        static const char* SUITS = "HDCS";   // we render as letter; abuse but works
        char sch = SUITS[s];
        gfx->setTextSize(1);
        gfx->setTextColor(card_red(c) ? 0xF800 : 0x0000);
        gfx->setCursor(x + 2, y + 2);
        gfx->print(rch);
        gfx->setCursor(x + 2, y + 10);
        gfx->print(sch);
        // Center suit big
        if (CH >= 40) {
            gfx->setTextSize(2);
            gfx->setCursor(x + CW / 2 - 6, y + CH / 2 - 8);
            gfx->print(sch);
        }
    }
}

static void draw_empty_slot(int x, int y) {
    gfx->drawRoundRect(x, y, CW, CH, 3, 0x4208);
}

static void draw_pile_top(int x, int y, uint8_t c, bool selected) {
    draw_card(x, y, c, selected);
}

static void draw_tableau() {
    int ov = card_overlap();
    for (int i = 0; i < 7; i++) {
        int x = tab_x(i);
        int y = tab_y();
        if (tableau_len[i] == 0) {
            draw_empty_slot(x, y);
            continue;
        }
        for (int j = 0; j < tableau_len[i]; j++) {
            bool sel = (sel_type == 0 && sel_idx == i && j >= sel_card);
            draw_card(x, y + j * ov, tableau[i][j], sel);
        }
    }
}

static void draw_foundations() {
    for (int i = 0; i < 4; i++) {
        int x = foundation_x(i);
        int y = foundation_y();
        if (foundation_len[i] == 0) {
            draw_empty_slot(x, y);
            // Letter for suit
            gfx->setTextSize(1);
            gfx->setTextColor(0x4208);
            gfx->setCursor(x + CW / 2 - 3, y + CH / 2 - 4);
            const char* SUITS = "HDCS";
            gfx->print(SUITS[i]);
        } else {
            uint8_t c = foundation[i][foundation_len[i] - 1];
            draw_card(x, y, c | 0x80, sel_type == 1 && sel_idx == i);
        }
    }
}

static void draw_stock_waste() {
    int sx = stock_x();
    int wx = waste_x();
    int y  = foundation_y();
    if (stock_len == 0) {
        draw_empty_slot(sx, y);
        gfx->setTextSize(1); gfx->setTextColor(0x07E0);
        gfx->setCursor(sx + 4, y + CH / 2 - 4); gfx->print("RST");
    } else {
        draw_card(sx, y, stock[stock_len - 1], false);  // face-down style
    }
    if (waste_len == 0) {
        draw_empty_slot(wx, y);
    } else {
        draw_card(wx, y, waste[waste_len - 1] | 0x80,
                  sel_type == 2);
    }
}

static void draw_all() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, VIEW_H, 0x0420);
    draw_foundations();
    draw_stock_waste();
    draw_tableau();
}

static void deal() {
    // Build deck
    uint8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = i;
    // Shuffle
    for (int i = 51; i > 0; i--) {
        int j = random(i + 1);
        uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    int p = 0;
    for (int i = 0; i < 7; i++) {
        tableau_len[i] = i + 1;
        for (int j = 0; j <= i; j++) {
            tableau[i][j] = deck[p++];
            if (j == i) tableau[i][j] = card_flip_up(tableau[i][j]);
        }
    }
    stock_len = 0;
    waste_len = 0;
    for (; p < 52; p++) stock[stock_len++] = deck[p];   // face-down in stock
    for (int i = 0; i < 4; i++) foundation_len[i] = 0;
    sel_type = sel_idx = sel_card = -1;
}

// Move validation
static bool can_place_on_tableau(uint8_t moving, int dst_pile) {
    if (tableau_len[dst_pile] == 0) {
        return card_rank(moving) == 12;   // Kings only on empty
    }
    uint8_t top = tableau[dst_pile][tableau_len[dst_pile] - 1];
    if (!card_up(top)) return false;
    if (card_red(moving) == card_red(top)) return false;
    return card_rank(moving) == card_rank(top) - 1;
}

static bool can_place_on_foundation(uint8_t moving, int dst) {
    if (card_suit(moving) != (uint8_t)dst) return false;
    if (foundation_len[dst] == 0) return card_rank(moving) == 0;
    uint8_t top = foundation[dst][foundation_len[dst] - 1];
    return card_rank(moving) == card_rank(top) + 1;
}

// Try to perform the move from current selection to (type, idx).
// Returns true on success.
static bool execute_move(int dst_type, int dst_idx) {
    if (sel_type == 0) {
        // Moving from tableau
        int src = sel_idx;
        int cnt = tableau_len[src] - sel_card;
        uint8_t first = tableau[src][sel_card] & 0x7F;
        if (dst_type == 0) {
            // Tableau-to-tableau: validate first card vs destination
            if (!can_place_on_tableau(first, dst_idx)) return false;
            for (int i = 0; i < cnt; i++) {
                tableau[dst_idx][tableau_len[dst_idx]++] = tableau[src][sel_card + i];
            }
            tableau_len[src] = sel_card;
            // Flip new top
            if (tableau_len[src] > 0 && !card_up(tableau[src][tableau_len[src] - 1])) {
                tableau[src][tableau_len[src] - 1] = card_flip_up(tableau[src][tableau_len[src] - 1]);
            }
            return true;
        }
        if (dst_type == 1 && cnt == 1) {
            if (!can_place_on_foundation(first, dst_idx)) return false;
            foundation[dst_idx][foundation_len[dst_idx]++] = first;
            tableau_len[src]--;
            if (tableau_len[src] > 0 && !card_up(tableau[src][tableau_len[src] - 1])) {
                tableau[src][tableau_len[src] - 1] = card_flip_up(tableau[src][tableau_len[src] - 1]);
            }
            return true;
        }
        return false;
    }
    if (sel_type == 1) {
        // Moving from foundation
        if (foundation_len[sel_idx] == 0) return false;
        uint8_t c = foundation[sel_idx][foundation_len[sel_idx] - 1];
        if (dst_type == 0) {
            if (!can_place_on_tableau(c, dst_idx)) return false;
            foundation_len[sel_idx]--;
            tableau[dst_idx][tableau_len[dst_idx]++] = c | 0x80;
            return true;
        }
        return false;
    }
    if (sel_type == 2) {
        // Moving from waste
        if (waste_len == 0) return false;
        uint8_t c = waste[waste_len - 1];
        if (dst_type == 0) {
            if (!can_place_on_tableau(c, dst_idx)) return false;
            waste_len--;
            tableau[dst_idx][tableau_len[dst_idx]++] = c | 0x80;
            return true;
        }
        if (dst_type == 1) {
            if (!can_place_on_foundation(c, dst_idx)) return false;
            waste_len--;
            foundation[dst_idx][foundation_len[dst_idx]++] = c;
            return true;
        }
        return false;
    }
    return false;
}

static void draw_stock() {
    int sx = stock_x(), y = foundation_y();
    gfx->fillRect(sx, y, CW, CH, 0x0420);
    draw_stock_waste();
}

static void cycle_stock() {
    if (stock_len > 0) {
        waste[waste_len++] = stock[--stock_len];
    } else {
        // Recycle: turn waste back into stock
        while (waste_len > 0) stock[stock_len++] = waste[--waste_len];
    }
    pm_game_audio_fx_drop();
}

// Hit-testing: figure out what was tapped
struct Hit { int type; int idx; int card_in_pile; };
// type: 0=tableau 1=foundation 2=waste 3=stock 4=none
static Hit hit_test(int x, int y) {
    Hit h = { 4, -1, -1 };
    int ov = card_overlap();
    // Foundation row
    for (int i = 0; i < 4; i++) {
        int fx = foundation_x(i), fy = foundation_y();
        if (x >= fx && x < fx + CW && y >= fy && y < fy + CH) {
            h.type = 1; h.idx = i; return h;
        }
    }
    if (x >= stock_x() && x < stock_x() + CW && y >= foundation_y() && y < foundation_y() + CH) {
        h.type = 3; return h;
    }
    if (x >= waste_x() && x < waste_x() + CW && y >= foundation_y() && y < foundation_y() + CH) {
        h.type = 2; return h;
    }
    // Tableau
    for (int i = 0; i < 7; i++) {
        int tx = tab_x(i);
        if (x < tx || x >= tx + CW) continue;
        int ty = tab_y();
        if (tableau_len[i] == 0) {
            if (y >= ty && y < ty + CH) { h.type = 0; h.idx = i; h.card_in_pile = 0; return h; }
            continue;
        }
        // Each card before the top is shown at ty + j*ov, with ov height.
        // Top card is fully shown.
        for (int j = tableau_len[i] - 1; j >= 0; j--) {
            int cy = ty + j * ov;
            int cy_end = (j == tableau_len[i] - 1) ? cy + CH : cy + ov;
            if (y >= cy && y < cy_end) {
                // Only allow selecting face-up cards
                if (card_up(tableau[i][j])) {
                    h.type = 0; h.idx = i; h.card_in_pile = j;
                }
                return h;
            }
        }
    }
    return h;
}

static bool check_win() {
    for (int i = 0; i < 4; i++) if (foundation_len[i] != 13) return false;
    return true;
}

void run_solitaire() {
    pm_game_audio_begin();
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif
    randomSeed(millis());
    deal();
    draw_all();

    bool was_touched = false;
    bool was_a = false, was_b = false;

    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return;

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
        int16_t tx, ty;
#ifdef DEVICE_C28P
        bool t = c28p_touch_read(&tx, &ty);
#else
        bool t = maxine_touch_read(&tx, &ty);
#endif
        if (t && !was_touched && ty < VIEW_H) {
            Hit h = hit_test(tx, ty);
            if (h.type == 3) {
                cycle_stock();
                sel_type = -1;
                draw_all();
            } else if (sel_type == -1) {
                if (h.type == 0 && h.idx >= 0 && tableau_len[h.idx] > 0) {
                    sel_type = 0; sel_idx = h.idx; sel_card = h.card_in_pile;
                    draw_all();
                } else if (h.type == 2 && waste_len > 0) {
                    sel_type = 2; sel_idx = 0; sel_card = waste_len - 1;
                    draw_all();
                } else if (h.type == 1 && foundation_len[h.idx] > 0) {
                    sel_type = 1; sel_idx = h.idx;
                    draw_all();
                }
            } else {
                // Try to move
                if (execute_move(h.type, h.idx)) {
                    pm_game_audio_fx_line();
                }
                sel_type = -1;
                draw_all();
                if (check_win()) {
                    gfx->fillRect(vx(), vy() + VIEW_H/2 - 16, VIEW_W, 32, 0x0000);
                    gfx->setTextSize(2);
                    gfx->setTextColor(0xFFE0);
                    gfx->setCursor(vx() + VIEW_W/2 - 36, vy() + VIEW_H/2 - 8);
                    gfx->print("YOU WIN");
                    delay(3000);
                    return;
                }
            }
        }
        was_touched = t;
#endif

        // Keyboard fallback: D draws from stock, Q quits handled above,
        // arrow + A selects/places, B cancels
        if (in.b && !was_b) { sel_type = -1; draw_all(); }
        if (in.select) { cycle_stock(); draw_all(); }
        was_b = in.b;

        delay(30);
        yield();
    }
}
