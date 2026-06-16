// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_audio.h"
#include "game_input.h"
#include "theme.h"
#include "tetris.h"
#ifdef DEVICE_C28P
#include "c28p_dpad.h"
#endif
#ifdef DEVICE_MAXINE
#include "maxine_dpad.h"
#endif
#ifdef DEVICE_C5
#include "c5_dpad.h"
#endif

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

#ifdef DEVICE_CARDPUTER_ADV
static constexpr int VIEW_W = 240;
static constexpr int VIEW_H = 135;
static constexpr int CELL = 5;
static constexpr int BOARD_X = 42;
static constexpr int BOARD_Y = 18;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 320;
static constexpr int VIEW_H = 222;
static constexpr int CELL = 9;
static constexpr int BOARD_X = 74;
static constexpr int BOARD_Y = 24;
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
// C28P + C5 portrait — game lives in the top 200px above the virtual
// D-pad. Playfield: 80x160 (BW=10, BH=20, CELL=8). Side panel on the
// right holds NEXT preview + score/level/lines display. The view
// origin is the top-left of the screen; the D-pad layer owns y >= 200.
// BOARD_Y is set below the universal exit bar (C28P_EXIT_BAR_H=14)
// to keep the playfield from overlapping the "tap to quit" zone.
//
// The C5 has identical 240×320 portrait geometry and a parallel
// c5_dpad.cpp implementation (single-touch instead of multi), so it
// shares this branch byte-for-byte.
static constexpr int VIEW_W = 240;
static constexpr int VIEW_H = 200;
static constexpr int CELL = 8;
static constexpr int BOARD_X = 16;    // 16px left margin
static constexpr int BOARD_Y = 22;    // 22px top margin — clears exit bar + breathing room
#elif defined(DEVICE_MAXINE)
// Maxine portrait (480x800) — game lives in the top 520px above the
// virtual D-pad (MAXINE_GAME_VIEW_H). Same software model as the
// C28P, scaled up. Playfield 240x480 (BW=10, BH=20, CELL=24), with
// the side panel (NEXT + score/level/lines) to the right. The D-pad
// layer owns y >= 520.
static constexpr int VIEW_W = 480;
static constexpr int VIEW_H = 520;
static constexpr int CELL = 24;
static constexpr int BOARD_X = 36;    // left margin
static constexpr int BOARD_Y = 28;    // top margin
#else
static constexpr int VIEW_W = 320;
static constexpr int VIEW_H = 240;
static constexpr int CELL = 9;
static constexpr int BOARD_X = 74;
static constexpr int BOARD_Y = 28;
#endif

static constexpr int BW = 10;
static constexpr int BH = 20;

static const uint16_t SHAPES[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444}, // I
    {0x8E00, 0x6440, 0x0E20, 0x44C0}, // J
    {0x2E00, 0x4460, 0x0E80, 0xC440}, // L
    {0x6600, 0x6600, 0x6600, 0x6600}, // O
    {0x6C00, 0x4620, 0x06C0, 0x8C40}, // S
    {0x4E00, 0x4640, 0x0E40, 0x4C40}, // T
    {0xC600, 0x2640, 0x0C60, 0x4C80}, // Z
};

static const uint16_t PIECE_COL[7] = {
    0x07FF, 0x001F, 0xFD20, 0xFFE0, 0x07E0, 0xF81F, 0xF800
};

static uint8_t board[BH][BW];
static int curType, curRot, curX, curY;
static int nextType;
static int score, lines, level;
static bool gameOver;
static PMGameSong song = PM_SONG_KOROBEINIKI;

static int vx() {
    int w = gfx->width();
    return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0;
}

static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)
    // Touch kiosk (C28P / Maxine / C5): a virtual D-pad occupies the
    // bottom of the screen. The game viewport MUST start at the top
    // (y=0) rather than being vertically centered — otherwise the
    // centered viewport overlaps the D-pad chrome and hides its
    // controls. VIEW_H is set to exactly the top game region for
    // these boards.
    return 0;
#else
    int h = gfx->height();
    return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}

static void clearPhysicalScreen() {
#ifdef DEVICE_C28P
    // C28P: only clear the game viewport (top 200px). The bottom 120px
    // holds the virtual D-pad chrome which must persist across game
    // events like game-over and launch-scene transitions.
    gfx->fillRect(0, 0, gfx->width(), 200, C_BLACK);
#elif defined(DEVICE_C5)
    // C5: same layout as C28P. Preserve the dpad chrome below y=200.
    gfx->fillRect(0, 0, gfx->width(), 200, C_BLACK);
#elif defined(DEVICE_MAXINE)
    // Maxine: same idea, clear only the top game viewport so the
    // virtual D-pad chrome below (y >= VIEW_H) survives.
    gfx->fillRect(0, 0, gfx->width(), VIEW_H, C_BLACK);
#else
    gfx->fillRect(0, 0, gfx->width(), gfx->height(), C_BLACK);
#endif
}

static void clearView() {
    int ox = vx();
    int oy = vy();
    gfx->fillRect(ox, oy, min(VIEW_W, gfx->width() - ox), min(VIEW_H, gfx->height() - oy), C_BLACK);
}

static bool shapeCell(int type, int rot, int x, int y) {
    return (SHAPES[type][rot & 3] & (0x8000 >> (y * 4 + x))) != 0;
}

static bool collides(int nx, int ny, int nr) {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (!shapeCell(curType, nr, x, y)) continue;
            int bx = nx + x;
            int by = ny + y;
            if (bx < 0 || bx >= BW || by >= BH) return true;
            if (by >= 0 && board[by][bx]) return true;
        }
    }
    return false;
}

static void drawBlock(int bx, int by, uint16_t color) {
    int x = vx() + BOARD_X + bx * CELL;
    int y = vy() + BOARD_Y + by * CELL;
    gfx->fillRect(x, y, CELL, CELL, color);
    if (CELL >= 5) {
        gfx->drawRect(x, y, CELL, CELL, 0xFFFF);
        gfx->drawFastHLine(x + 1, y + 1, CELL - 2, 0xFFFF);
    }
}

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)
// Touch-kiosk no-flicker support: paint an empty cell as black with no
// outline. Used by the no-flicker redraw path — every cell of the
// board gets painted every frame (occupied or not), so we never need
// to clear the playfield to black first. Eliminates the
// flash-to-black gap that causes visible refresh bounce on the RGB
// panels (240x200 C28P, 480x520 Maxine, 240x200 C5). Same code, all
// three boards.
static void drawBlockEmpty(int bx, int by) {
    int x = vx() + BOARD_X + bx * CELL;
    int y = vy() + BOARD_Y + by * CELL;
    gfx->fillRect(x, y, CELL, CELL, C_BLACK);
}
#endif

static void drawMiniPiece(int type, int x0, int y0) {
    uint16_t col = PIECE_COL[type];
    int s = (CELL >= 9) ? 6 : 4;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (shapeCell(type, 0, x, y)) {
                gfx->fillRect(x0 + x * s, y0 + y * s, s - 1, s - 1, col);
            }
        }
    }
}

static void drawFrame() {
    int ox = vx();
    int oy = vy();
    clearPhysicalScreen();
    gfx->drawRect(ox + BOARD_X - 2, oy + BOARD_Y - 2, BW * CELL + 4, BH * CELL + 4, C_GREEN);
#if !defined(DEVICE_C28P) && !defined(DEVICE_C5)
    // C28P / C5: the top 14px is the dpad's exit bar — don't clobber it.
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(ox + 6, oy + 5);
    gfx->print("TETRIS");
#endif
}

static void drawLaunchScene() {
    int ox = vx();
    int oy = vy();
    clearPhysicalScreen();
    for (int i = 0; i < 30; i++) {
        gfx->drawPixel(ox + random(VIEW_W), oy + random(max(20, VIEW_H - 40)), 0x7BEF);
    }

    int padY = oy + VIEW_H - 18;
    int rocketX = ox + VIEW_W / 2;
    for (int f = 0; f < 14; f++) {
        gfx->fillRect(ox, oy + 20, VIEW_W, VIEW_H - 20, C_BLACK);
        for (int sx = 0; sx < VIEW_W; sx += 18) {
            gfx->drawFastVLine(ox + sx, padY - 34, 34, 0x4208);
            gfx->drawLine(ox + sx, padY - 34, ox + sx + 14, padY, 0x2104);
        }
        gfx->drawFastHLine(ox, padY, VIEW_W, 0x8410);

        int ry = padY - 44 - f * 3;
        gfx->fillTriangle(rocketX, ry - 14, rocketX - 7, ry, rocketX + 7, ry, C_RED);
        gfx->fillRect(rocketX - 6, ry, 12, 30, C_WHITE);
        gfx->fillRect(rocketX - 4, ry + 8, 8, 8, 0x07FF);
        gfx->fillTriangle(rocketX - 6, ry + 22, rocketX - 13, ry + 33, rocketX - 6, ry + 30, 0x001F);
        gfx->fillTriangle(rocketX + 6, ry + 22, rocketX + 13, ry + 33, rocketX + 6, ry + 30, 0x001F);
        gfx->fillTriangle(rocketX, ry + 40, rocketX - 5, ry + 30, rocketX + 5, ry + 30, (f & 1) ? 0xFFE0 : 0xFD20);
        gfx->fillCircle(rocketX - 10 - f, padY - 2, 4 + f / 3, 0x8410);
        gfx->fillCircle(rocketX + 12 + f, padY - 1, 5 + f / 4, 0x8410);

        gfx->setTextSize(1);
        gfx->setTextColor(C_GREEN);
        gfx->setCursor(ox + 6, oy + 5);
        gfx->print("BAIKONUR BLOCK STACK");
        pm_game_audio_tick();
        delay(55);
        yield();
    }
}

static void drawGame() {
    int ox = vx();
    int oy = vy();
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)
    // No-flicker redraw path (touch-kiosk boards): do NOT clear
    // the viewport or playfield before drawing. Every cell of the
    // board gets painted every frame (occupied cells as their piece
    // color, empty cells as black), and the falling piece overlays on
    // top. Borders and the TETRIS label get painted unconditionally
    // too — they're idempotent and cheap.
    // This eliminates the flash-to-black gap that causes visible
    // refresh bounce on the RGB panels.
#else
    clearView();
#endif
    gfx->drawRect(ox + BOARD_X - 2, oy + BOARD_Y - 2, BW * CELL + 4, BH * CELL + 4, C_GREEN);
#if !defined(DEVICE_C28P) && !defined(DEVICE_MAXINE) && !defined(DEVICE_C5)
    // C28P / Maxine / C5: the top of the screen belongs to the dpad's
    // exit bar / header strip — don't clobber it with a TETRIS label.
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(ox + 6, oy + 5);
    gfx->print("TETRIS");
#endif

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)
    // Paint every cell of the board explicitly — no playfield clear.
    for (int y = 0; y < BH; y++) {
        for (int x = 0; x < BW; x++) {
            if (board[y][x]) {
                drawBlock(x, y, PIECE_COL[board[y][x] - 1]);
            } else {
                drawBlockEmpty(x, y);
            }
        }
    }
#else
    gfx->fillRect(ox + BOARD_X, oy + BOARD_Y, BW * CELL, BH * CELL, C_BLACK);
    for (int y = 0; y < BH; y++) {
        for (int x = 0; x < BW; x++) {
            if (board[y][x]) drawBlock(x, y, PIECE_COL[board[y][x] - 1]);
        }
    }
#endif
    if (!gameOver) {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (shapeCell(curType, curRot, x, y)) {
                    int bx = curX + x;
                    int by = curY + y;
                    if (by >= 0) drawBlock(bx, by, PIECE_COL[curType]);
                }
            }
        }
    }

    int panelX = ox + BOARD_X + BW * CELL + 14;
    gfx->fillRect(panelX, oy + BOARD_Y, max(1, VIEW_W - (panelX - ox) - 4), 96, C_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(panelX, oy + BOARD_Y);
    gfx->printf("SC %d", score);
    gfx->setCursor(panelX, oy + BOARD_Y + 14);
    gfx->printf("LN %d", lines);
    gfx->setCursor(panelX, oy + BOARD_Y + 28);
    gfx->printf("LV %d", level);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(panelX, oy + BOARD_Y + 46);
    gfx->print("NEXT");
    drawMiniPiece(nextType, panelX, oy + BOARD_Y + 60);
#ifndef DEVICE_CARDPUTER_ADV
    gfx->setTextColor(C_GREY);
    gfx->setCursor(panelX, oy + BOARD_Y + 88);
    gfx->printf("V %s", pm_game_audio_song_name());
#endif
}

static void spawnPiece() {
    curType = nextType;
    nextType = random(7);
    curRot = 0;
    curX = 3;
    curY = -1;
    if (collides(curX, curY, curRot)) gameOver = true;
}

static void lockPiece() {
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (!shapeCell(curType, curRot, x, y)) continue;
            int bx = curX + x;
            int by = curY + y;
            if (by < 0) {
                gameOver = true;
            } else if (bx >= 0 && bx < BW && by < BH) {
                board[by][bx] = curType + 1;
            }
        }
    }

    int cleared = 0;
    for (int y = BH - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < BW; x++) {
            if (!board[y][x]) { full = false; break; }
        }
        if (!full) continue;
        cleared++;
        for (int yy = y; yy > 0; yy--) {
            memcpy(board[yy], board[yy - 1], BW);
        }
        memset(board[0], 0, BW);
        y++;
    }

    if (cleared) {
        static const int pts[5] = {0, 100, 300, 500, 800};
        score += pts[cleared] * level;
        lines += cleared;
        level = 1 + lines / 10;
        pm_game_audio_fx_line();
    }
    spawnPiece();
}

static void tryRotate() {
    int nr = (curRot + 1) & 3;
    if (!collides(curX, curY, nr)) {
        curRot = nr;
    } else if (!collides(curX - 1, curY, nr)) {
        curX--;
        curRot = nr;
    } else if (!collides(curX + 1, curY, nr)) {
        curX++;
        curRot = nr;
    }
}

static bool waitPaused() {
    int ox = vx(), oy = vy();
    int boxW = min(140, VIEW_W - 20);
    int boxX = ox + (VIEW_W - boxW) / 2;
    int boxY = oy + VIEW_H / 2 - 16;
    gfx->fillRect(boxX, boxY, boxW, 32, C_DARK);
    gfx->drawRect(boxX, boxY, boxW, 32, C_GREEN);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(boxX + (boxW - 36) / 2, boxY + 12);
    gfx->print("PAUSED");
    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return false;
        if (input.start) return true;
        delay(30);
        yield();
    }
}

static bool showGameOver() {
    int ox = vx(), oy = vy();
    int boxW = min(168, VIEW_W - 16);
    int boxX = ox + (VIEW_W - boxW) / 2;
    int boxY = oy + VIEW_H / 2 - 28;
    gfx->fillRect(boxX, boxY, boxW, 58, C_DARK);
    gfx->drawRect(boxX, boxY, boxW, 58, C_GREEN);
    gfx->setTextColor(C_RED);
    gfx->setTextSize(2);
    gfx->setCursor(boxX + (boxW - 48) / 2, boxY + 8);
    gfx->print("GAME");
    gfx->setCursor(boxX + (boxW - 48) / 2, boxY + 28);
    gfx->print("OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
#ifdef DEVICE_CARDPUTER_ADV
    gfx->setCursor(boxX + 16, boxY + 48);
    gfx->print("R/START RETRY");
#elif defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)
    // Touch-only kiosk (C28P / Maxine / C5) — map game-over actions to
    // the on-screen D-pad's A and B buttons. A=retry (affirmative), B=exit.
    gfx->setCursor(boxX + 18, boxY + 48);
    gfx->print("A RETRY    B EXIT");
#else
    gfx->setCursor(boxX + 22, boxY + 48);
    gfx->print("R/START RETRY  Q EXIT");
#endif
    while (true) {
        PMNesInput input = pm_read_nes_input(true);
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)
        // Touch-only: A=retry, B=exit. Ignore key/start (no keyboard).
        if (input.b) return false;
        if (input.a) return true;
#else
        if (input.quit) return false;
        if (input.start || input.key == 'r' || input.key == 'R') return true;
#endif
        delay(30);
        yield();
    }
}

void run_tetris() {
    pm_game_audio_begin();
    pm_game_audio_start(song);
#ifdef DEVICE_C28P
    // Paint the virtual D-pad chrome below the game viewport.
    // pm_read_nes_input() poll calls handle press-state redraw
    // selectively from here on.
    c28p_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    // Maxine: paint the (scaled) virtual D-pad chrome below the
    // game viewport, same as the C28P.
    maxine_dpad_render();
#endif
#ifdef DEVICE_C5
    // C5: paint the virtual D-pad chrome below the game viewport.
    // Same layout as the C28P, single-touch only (XPT2046 resistive).
    c5_dpad_render();
#endif
    drawLaunchScene();

    while (true) {
        memset(board, 0, sizeof(board));
        score = 0;
        lines = 0;
        level = 1;
        gameOver = false;
        nextType = random(7);
        spawnPiece();
        drawFrame();

        uint32_t lastFall = millis();
        uint32_t lastMove = 0;
        bool redraw = true;

        while (!gameOver) {
            pm_game_audio_tick();
            PMNesInput input = pm_read_nes_input(true);
            if (input.quit) {
                pm_game_audio_stop();
                return;
            }
            if (input.start) {
                if (!waitPaused()) {
                    pm_game_audio_stop();
                    return;
                }
                drawFrame();
                redraw = true;
            }

            uint32_t now = millis();
            if (input.left && now - lastMove > 110 && !collides(curX - 1, curY, curRot)) {
                curX--;
                lastMove = now;
                redraw = true;
            }
            if (input.right && now - lastMove > 110 && !collides(curX + 1, curY, curRot)) {
                curX++;
                lastMove = now;
                redraw = true;
            }
            if ((input.a || input.b || input.up) && now - lastMove > 150) {
                tryRotate();
                lastMove = now;
                redraw = true;
            }

            int fallMs = max(90, 650 - (level - 1) * 45);
            if (input.down) fallMs = 45;
            if (now - lastFall >= (uint32_t)fallMs) {
                if (!collides(curX, curY + 1, curRot)) {
                    curY++;
                } else {
                    pm_game_audio_fx_drop();
                    lockPiece();
                }
                lastFall = now;
                redraw = true;
            }

            if (redraw) {
                drawGame();
                redraw = false;
            }
            delay(16);
            yield();
        }

        drawGame();
        if (!showGameOver()) {
            pm_game_audio_stop();
            return;
        }
        pm_game_audio_start(song);
        drawLaunchScene();
    }
}