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
 * PISCES MOON OS — CHESS v1.0
 * Full chess engine with AI opponent for LilyGO T-Deck Plus (320x240)
 *
 * Engine: Minimax with alpha-beta pruning, iterative deepening
 * Personalities: Beginner / Club / Expert / Kasparov / Fischer / Random
 *
 * Board: 8x8, each square 26px. Offset: left=16, top=8 (fits 208px board)
 * Right panel: 80px wide — captured pieces, status, difficulty
 *
 * Controls:
 *   Trackball      = move cursor
 *   Click          = select piece / move to square
 *   A key          = cycle AI personality
 *   Q / header tap = quit
 *
 * Piece representation: positive = white, negative = black
 *   1=Pawn 2=Knight 3=Bishop 4=Rook 5=Queen 6=King
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "gamepad.h"
#include "chess.h"
#if defined(DEVICE_MAXINE) || defined(DEVICE_C28P) || defined(DEVICE_C5)
// ──────────────────────────────────────────────────────────────
//  Touch-only kiosk stubs for symbols chess.cpp calls directly
//  but which aren't linked into the Maxine / C28P builds.
//
//  WHY chess needs stubs at all: the T-Deck input loop polls
//  get_touch(), get_keypress(), update_trackball{,_game}(), and
//  gamepad_*() unconditionally each frame. We don't want to
//  scatter #ifdef branches through that loop, so we provide
//  no-op definitions that satisfy the linker and read as "no
//  input" at runtime. Chess's device-specific touch path then
//  injects the real input via c28p_touch_read / maxine_touch_read.
//
//  Split between the two devices:
//    Maxine — needs ALL the stubs (no keyboard/trackball/gamepad
//             .cpp files in maxine src_filter).
//    C28P   — c28p_boot.cpp already provides get_keypress,
//             init_trackball, update_trackball_game, g_gamepad,
//             and the gamepad_* functions. Redefining them
//             here would multiply-define. We only add the two
//             chess specifically uses that c28p_boot doesn't:
//             get_touch and update_trackball (non-game variant).
// ──────────────────────────────────────────────────────────────
bool get_touch(int16_t* x, int16_t* y) { (void)x; (void)y; return false; }
TrackballState update_trackball() { return TrackballState{0, 0, false}; }
#endif

#ifdef DEVICE_MAXINE
// Note: maxine_dpad.h is intentionally NOT included. Chess on Maxine
// is a full-screen touch-native app — every input is a tap on the
// board or a tap in the header / panel strip, never a directional
// press. The maxine dpad chrome that pac-man, galaga, and pole
// position paint at y >= 520 is deliberately absent here, which
// frees the full 800px of vertical real estate for the chess UI.
extern bool maxine_touch_read(int16_t* x, int16_t* y);

// Maxine-only stubs (C28P provides these in c28p_boot.cpp).
char get_keypress() { return 0; }
void init_trackball() {}
TrackballState update_trackball_game() { return TrackballState{0, 0, false}; }
GamepadState g_gamepad = {};
bool gamepad_poll() { return false; }
#endif

#ifdef DEVICE_C28P
// C28P single-touch driver, defined non-static in c28p_boot.cpp.
// Returns true if a finger is currently down, writing display
// coords (0..239, 0..319) into *x and *y.
extern bool c28p_touch_read(int16_t* x, int16_t* y);
#endif

#ifdef DEVICE_C5
// C5 single-touch driver (XPT2046 resistive), defined non-static in
// c5_boot.cpp. Same shape as c28p_touch_read — the kiosk_touch()
// helper below dispatches to whichever the build target provides.
extern bool c5_touch_read(int16_t* x, int16_t* y);
#endif

#if defined(DEVICE_C28P) || defined(DEVICE_C5)
// Single entry point for touch on both portrait kiosk targets. The
// touch-handling blocks below are written against this name and the
// preprocessor picks the right physical driver per build. Avoids
// duplicating ~120 lines of board-tap / button-tap logic between
// C28P and C5 branches when the only difference is which extern
// gets called.
static inline bool kiosk_touch(int16_t* x, int16_t* y) {
#ifdef DEVICE_C28P
    return c28p_touch_read(x, y);
#else
    return c5_touch_read(x, y);
#endif
}
#endif

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  BOARD GEOMETRY
// ─────────────────────────────────────────────
#ifdef DEVICE_MAXINE
// Maxine 480x800 portrait, full-screen touch-native app (no virtual
// dpad — chess is all taps). Layout:
//    y=0..29     30-px header strip (tap-to-quit hot zone with
//                "< EXIT" / "CHESS" affordance painted by drawFull).
//    y=30..445   416-px board (SQ=52 * 8), x=32..448 centered.
//    y=446..461  16-px gap holds the a..h file labels (size-2).
//    y=462..799  338-px panel filling the rest of the screen: turn
//                indicator, AI mode, check status, controls hints.
//
// Earlier versions reserved y >= 520 for maxine_dpad chrome. That
// reservation is gone — without a dpad to host, the bottom 280px
// becomes additional panel real estate instead of being painted
// over by the dpad layer.
#define SQ          52
#define BOARD_X     32
#define BOARD_Y     30
#define PANEL_X     0
#define PANEL_W     480
#define PANEL_Y     (BOARD_Y + SQ*8 + 16)
#define PANEL_H     (800 - PANEL_Y)
#define SCREEN_W_C  480
#define SCREEN_H_C  800
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
// C28P + C5 share the same 240x320 portrait panel + touch-only
// kiosk shape, so they share the same chess board geometry.
// Layout (both):
//    y=0..13     14-px header strip (tap-to-quit hot zone)
//    y=14..237   224-px board (8 * SQ=28), x=14..238 centered
//    y=238..247  10-px gap holds the a..h file labels (size-1)
//    y=248..319  72-px panel: turn indicator, AI cycle button,
//                check status, control hints
#define SQ          28
#define BOARD_X     14
#define BOARD_Y     14
#define PANEL_X     0
#define PANEL_W     240
#define PANEL_Y     248
#define PANEL_H     72
#define SCREEN_W_C  240
#define SCREEN_H_C  320
#else
#define SQ          26          // Square size px
#define BOARD_X     16          // Board left edge
#define BOARD_Y      8          // Board top edge
#define PANEL_X    (BOARD_X + SQ*8 + 4)  // Right panel x = 228
#define PANEL_W    (320 - PANEL_X - 2)   // ~90px
#define SCREEN_W_C  320
#define SCREEN_H_C  240
#endif

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_LIGHT   0xF79E   // Light square (cream)
#define COL_DARK    0x6269   // Dark square (brown)
#define COL_SEL     0x07E0   // Selected square (green)
#define COL_MOVE    0xFFE0   // Valid move highlight (yellow)
#define COL_CURSOR  0xF800   // Cursor (red outline)
#define COL_CHECK   0xF800   // King in check
#define COL_WHITE_P 0xFFFF   // White pieces
#define COL_BLACK_P 0x0000   // Black pieces
#define COL_OUTLINE 0x4208   // Piece outline
#define COL_BG      0x18C3   // Panel background
#define COL_TEXT    0xFFFF
#define COL_COORD   0xAD55   // Board coordinates

// ─────────────────────────────────────────────
//  PIECE CONSTANTS
// ─────────────────────────────────────────────
#define EMPTY    0
#define PAWN     1
#define KNIGHT   2
#define BISHOP   3
#define ROOK     4
#define QUEEN    5
#define KING     6

#define WHITE    1
#define BLACK   -1

// ─────────────────────────────────────────────
//  AI PERSONALITIES
// ─────────────────────────────────────────────
#define AI_RANDOM    0  // Pure random legal moves
#define AI_BEGINNER  1  // Depth 1 — captures and basic eval only
#define AI_CLUB      2  // Depth 2 — looks ahead one full exchange
#define AI_EXPERT    3  // Depth 3 — solid positional play
#define AI_FISCHER   4  // Depth 4 — aggressive, open games
#define AI_KASPAROV  5  // Depth 5 — deep calculation, punishing

static const char* AI_NAMES[] = {
    "RANDOM", "BEGINNER", "CLUB", "EXPERT", "FISCHER", "KASPAROV"
};
static const int AI_DEPTHS[] = { 0, 1, 2, 3, 4, 5 };

// ─────────────────────────────────────────────
//  GAME STATE
// ─────────────────────────────────────────────
static int8_t board[8][8];     // board[row][col], row 0 = rank 8 (black's back rank)
static bool   whiteToMove;
static bool   gameOver;
static bool   playerIsWhite;   // Player always plays white in current version

// Castling rights
static bool wKCastle, wQCastle, bKCastle, bQCastle;

// En passant target square (-1 if none)
static int8_t epRow, epCol;

// Move count for 50-move rule
static int halfMoveClock;
static int fullMoveNum;

// Selected square (cursor & selection)
static int8_t cursorRow, cursorCol;
static int8_t selRow, selCol;  // -1 = nothing selected
static bool   pieceSelected;

// AI
static int aiPersonality;
static bool aiThinking;

// Check state
static bool whiteInCheck, blackInCheck;

// Captured piece counters (for display)
static int whiteCaptured[7]; // index = piece type, value = count
static int blackCaptured[7];

// ─────────────────────────────────────────────
//  MOVE STRUCTURE
// ─────────────────────────────────────────────
struct Move {
    int8_t fromRow, fromCol;
    int8_t toRow,   toCol;
    int8_t captured;        // piece captured (0 = none)
    int8_t promotion;       // promotion piece (0 = none)
    bool   isCastle;
    bool   isEnPassant;
};

// ─────────────────────────────────────────────
//  PIECE VALUES (centipawns)
// ─────────────────────────────────────────────
static const int PIECE_VALUE[] = { 0, 100, 320, 330, 500, 900, 20000 };

// Piece-square tables for positional bonus (white's perspective, row 7 = rank 1)
// Pawn table
static const int8_t PST_PAWN[8][8] = {
    { 0,  0,  0,  0,  0,  0,  0,  0},
    {50, 50, 50, 50, 50, 50, 50, 50},
    {10, 10, 20, 30, 30, 20, 10, 10},
    { 5,  5, 10, 25, 25, 10,  5,  5},
    { 0,  0,  0, 20, 20,  0,  0,  0},
    { 5, -5,-10,  0,  0,-10, -5,  5},
    { 5, 10, 10,-20,-20, 10, 10,  5},
    { 0,  0,  0,  0,  0,  0,  0,  0}
};
// Knight table
static const int8_t PST_KNIGHT[8][8] = {
    {-50,-40,-30,-30,-30,-30,-40,-50},
    {-40,-20,  0,  0,  0,  0,-20,-40},
    {-30,  0, 10, 15, 15, 10,  0,-30},
    {-30,  5, 15, 20, 20, 15,  5,-30},
    {-30,  0, 15, 20, 20, 15,  0,-30},
    {-30,  5, 10, 15, 15, 10,  5,-30},
    {-40,-20,  0,  5,  5,  0,-20,-40},
    {-50,-40,-30,-30,-30,-30,-40,-50}
};
// Bishop table
static const int8_t PST_BISHOP[8][8] = {
    {-20,-10,-10,-10,-10,-10,-10,-20},
    {-10,  0,  0,  0,  0,  0,  0,-10},
    {-10,  0,  5, 10, 10,  5,  0,-10},
    {-10,  5,  5, 10, 10,  5,  5,-10},
    {-10,  0, 10, 10, 10, 10,  0,-10},
    {-10, 10, 10, 10, 10, 10, 10,-10},
    {-10,  5,  0,  0,  0,  0,  5,-10},
    {-20,-10,-10,-10,-10,-10,-10,-20}
};
// Rook table
static const int8_t PST_ROOK[8][8] = {
    { 0,  0,  0,  0,  0,  0,  0,  0},
    { 5, 10, 10, 10, 10, 10, 10,  5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    { 0,  0,  0,  5,  5,  0,  0,  0}
};
// Queen table
static const int8_t PST_QUEEN[8][8] = {
    {-20,-10,-10, -5, -5,-10,-10,-20},
    {-10,  0,  0,  0,  0,  0,  0,-10},
    {-10,  0,  5,  5,  5,  5,  0,-10},
    { -5,  0,  5,  5,  5,  5,  0, -5},
    {  0,  0,  5,  5,  5,  5,  0, -5},
    {-10,  5,  5,  5,  5,  5,  0,-10},
    {-10,  0,  5,  0,  0,  0,  0,-10},
    {-20,-10,-10, -5, -5,-10,-10,-20}
};
// King middlegame table (penalize exposure)
static const int8_t PST_KING_MID[8][8] = {
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-20,-30,-30,-40,-40,-30,-30,-20},
    {-10,-20,-20,-20,-20,-20,-20,-10},
    { 20, 20,  0,  0,  0,  0, 20, 20},
    { 20, 30, 10,  0,  0, 10, 30, 20}
};

// ─────────────────────────────────────────────
//  BOARD INIT
// ─────────────────────────────────────────────
static void initBoard() {
    memset(board, 0, sizeof(board));
    // Black pieces (row 0 = rank 8)
    board[0][0] = -ROOK;   board[0][1] = -KNIGHT; board[0][2] = -BISHOP;
    board[0][3] = -QUEEN;  board[0][4] = -KING;   board[0][5] = -BISHOP;
    board[0][6] = -KNIGHT; board[0][7] = -ROOK;
    for (int c = 0; c < 8; c++) board[1][c] = -PAWN;
    // White pieces (row 7 = rank 1)
    board[7][0] = ROOK;    board[7][1] = KNIGHT;  board[7][2] = BISHOP;
    board[7][3] = QUEEN;   board[7][4] = KING;    board[7][5] = BISHOP;
    board[7][6] = KNIGHT;  board[7][7] = ROOK;
    for (int c = 0; c < 8; c++) board[6][c] = PAWN;

    whiteToMove = true;
    gameOver    = false;
    wKCastle = wQCastle = bKCastle = bQCastle = true;
    epRow = epCol = -1;
    halfMoveClock = 0; fullMoveNum = 1;
    cursorRow = 7; cursorCol = 4;
    selRow = selCol = -1;
    pieceSelected = false;
    whiteInCheck = blackInCheck = false;
    memset(whiteCaptured, 0, sizeof(whiteCaptured));
    memset(blackCaptured, 0, sizeof(blackCaptured));
}

// ─────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────
static inline bool inBounds(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}
static inline int colorOf(int piece) {
    if (piece > 0) return WHITE;
    if (piece < 0) return BLACK;
    return 0;
}
static inline int absP(int piece) { return piece < 0 ? -piece : piece; }

static void findKing(int color, int& kr, int& kc) {
    int king = color * KING;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            if (board[r][c] == king) { kr = r; kc = c; return; }
    kr = kc = -1; // shouldn't happen
}

// ─────────────────────────────────────────────
//  ATTACK DETECTION
// ─────────────────────────────────────────────
static bool squareAttackedBy(int r, int c, int attacker) {
    // attacker = WHITE or BLACK
    int dir = (attacker == WHITE) ? 1 : -1; // white pawns move up (negative row dir from row 6)

    // Pawn attacks
    int pawnRow = r + dir; // row where attacking pawn sits (if attacker attacks r,c)
    // Actually: white pawn on (r+1) attacks (r,c-1) and (r,c+1)
    // Reconsider: attacker pawn attacks diagonally forward
    // White pawn at (pr,pc) attacks (pr-1, pc±1) — moving up (decreasing row)
    int pRow = r - (-dir); // = r + dir — the row the pawn would be ON to attack r,c
    // White pawn attacks forward-diagonally: from row pRow = r+1 (one row below r in array)
    int pawnSrcRow = r + (attacker == WHITE ? 1 : -1);
    if (inBounds(pawnSrcRow, c-1) && board[pawnSrcRow][c-1] == attacker * PAWN) return true;
    if (inBounds(pawnSrcRow, c+1) && board[pawnSrcRow][c+1] == attacker * PAWN) return true;

    // Knight attacks
    int knightMoves[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    for (auto& m : knightMoves) {
        int nr = r + m[0], nc = c + m[1];
        if (inBounds(nr,nc) && board[nr][nc] == attacker * KNIGHT) return true;
    }

    // Sliding pieces: rook/queen (straight lines)
    int lines[4][2] = {{0,1},{0,-1},{1,0},{-1,0}};
    for (auto& d : lines) {
        for (int i = 1; i < 8; i++) {
            int nr = r + d[0]*i, nc = c + d[1]*i;
            if (!inBounds(nr,nc)) break;
            if (board[nr][nc] != EMPTY) {
                int p = board[nr][nc];
                if (colorOf(p) == attacker && (absP(p) == ROOK || absP(p) == QUEEN)) return true;
                break;
            }
        }
    }

    // Sliding: bishop/queen (diagonals)
    int diags[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
    for (auto& d : diags) {
        for (int i = 1; i < 8; i++) {
            int nr = r + d[0]*i, nc = c + d[1]*i;
            if (!inBounds(nr,nc)) break;
            if (board[nr][nc] != EMPTY) {
                int p = board[nr][nc];
                if (colorOf(p) == attacker && (absP(p) == BISHOP || absP(p) == QUEEN)) return true;
                break;
            }
        }
    }

    // King attacks
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r+dr, nc = c+dc;
            if (inBounds(nr,nc) && board[nr][nc] == attacker * KING) return true;
        }

    return false;
}

static bool inCheck(int color) {
    int kr, kc;
    findKing(color, kr, kc);
    if (kr < 0) return false;
    return squareAttackedBy(kr, kc, -color);
}

// ─────────────────────────────────────────────
//  MOVE GENERATION
// ─────────────────────────────────────────────
static int generateMoves(int color, Move moves[], int maxMoves) {
    int count = 0;
    int dir = (color == WHITE) ? -1 : 1; // pawns move in decreasing or increasing row

    for (int r = 0; r < 8 && count < maxMoves - 20; r++) {
        for (int c = 0; c < 8 && count < maxMoves - 20; c++) {
            if (colorOf(board[r][c]) != color) continue;
            int pt = absP(board[r][c]);

            auto addMove = [&](int tr, int tc, bool castle = false, bool ep = false, int promo = 0) {
                if (!inBounds(tr,tc)) return;
                if (colorOf(board[tr][tc]) == color) return;
                Move& m = moves[count++];
                m.fromRow = r; m.fromCol = c;
                m.toRow = tr; m.toCol = tc;
                m.captured = ep ? (color == WHITE ? -PAWN : PAWN) : board[tr][tc];
                m.promotion = promo;
                m.isCastle = castle;
                m.isEnPassant = ep;
            };

            if (pt == PAWN) {
                // Forward
                int nr = r + dir;
                if (inBounds(nr,c) && board[nr][c] == EMPTY) {
                    bool promo = (nr == 0 || nr == 7);
                    if (promo) {
                        for (int p : {QUEEN, ROOK, BISHOP, KNIGHT}) addMove(nr,c,false,false,p);
                    } else {
                        addMove(nr,c);
                        // Double push from starting rank
                        int startRank = (color == WHITE) ? 6 : 1;
                        if (r == startRank && board[nr+dir][c] == EMPTY)
                            addMove(nr+dir, c);
                    }
                }
                // Captures
                for (int dc : {-1, 1}) {
                    int nc2 = c + dc;
                    if (!inBounds(nr, nc2)) continue;
                    bool promo = (nr == 0 || nr == 7);
                    // Normal capture
                    if (colorOf(board[nr][nc2]) == -color) {
                        if (promo) for (int p : {QUEEN,ROOK,BISHOP,KNIGHT}) addMove(nr,nc2,false,false,p);
                        else addMove(nr,nc2);
                    }
                    // En passant
                    if (nr == epRow && nc2 == epCol) addMove(nr,nc2,false,true);
                }
            }
            else if (pt == KNIGHT) {
                int km[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
                for (auto& mv : km) addMove(r+mv[0], c+mv[1]);
            }
            else if (pt == BISHOP || pt == QUEEN) {
                int dd[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
                for (auto& d : dd)
                    for (int i = 1; i < 8; i++) {
                        int nr=r+d[0]*i, nc=c+d[1]*i;
                        if (!inBounds(nr,nc)) break;
                        addMove(nr,nc);
                        if (board[nr][nc] != EMPTY) break;
                    }
                if (pt == BISHOP) continue; // don't fall through to straight lines
            }
            if (pt == ROOK || pt == QUEEN) {
                int dl[4][2] = {{0,1},{0,-1},{1,0},{-1,0}};
                for (auto& d : dl)
                    for (int i = 1; i < 8; i++) {
                        int nr=r+d[0]*i, nc=c+d[1]*i;
                        if (!inBounds(nr,nc)) break;
                        addMove(nr,nc);
                        if (board[nr][nc] != EMPTY) break;
                    }
            }
            else if (pt == KING) {
                for (int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++) {
                    if (dr==0&&dc==0) continue;
                    addMove(r+dr,c+dc);
                }
                // Castling
                int backRow = (color == WHITE) ? 7 : 0;
                if (r == backRow && c == 4 && !inCheck(color)) {
                    // Kingside
                    bool kCastle = (color == WHITE) ? wKCastle : bKCastle;
                    if (kCastle && board[backRow][5]==EMPTY && board[backRow][6]==EMPTY &&
                        !squareAttackedBy(backRow,5,-color) && !squareAttackedBy(backRow,6,-color))
                        addMove(backRow,6,true);
                    // Queenside
                    bool qCastle = (color == WHITE) ? wQCastle : bQCastle;
                    if (qCastle && board[backRow][3]==EMPTY && board[backRow][2]==EMPTY && board[backRow][1]==EMPTY &&
                        !squareAttackedBy(backRow,3,-color) && !squareAttackedBy(backRow,2,-color))
                        addMove(backRow,2,true);
                }
            }
        }
    }
    return count;
}

// ─────────────────────────────────────────────
//  MAKE / UNMAKE MOVE
// ─────────────────────────────────────────────
struct BoardState {
    int8_t board[8][8];
    bool wKC, wQC, bKC, bQC;
    int8_t epRow, epCol;
    int halfClock;
};

static void saveState(BoardState& s) {
    memcpy(s.board, board, sizeof(board));
    s.wKC=wKCastle; s.wQC=wQCastle; s.bKC=bKCastle; s.bQC=bQCastle;
    s.epRow=epRow; s.epCol=epCol; s.halfClock=halfMoveClock;
}
static void restoreState(const BoardState& s) {
    memcpy(board, s.board, sizeof(board));
    wKCastle=s.wKC; wQCastle=s.wQC; bKCastle=s.bKC; bQCastle=s.bQC;
    epRow=s.epRow; epCol=s.epCol; halfMoveClock=s.halfClock;
}

static void applyMove(const Move& m, int color) {
    int piece = board[m.fromRow][m.fromCol];
    int pt    = absP(piece);

    // En passant reset
    epRow = epCol = -1;

    // Handle castling
    if (m.isCastle) {
        int row = m.fromRow;
        if (m.toCol == 6) { // Kingside
            board[row][6] = board[row][4];
            board[row][5] = board[row][7];
            board[row][4] = board[row][7] = EMPTY;
        } else { // Queenside
            board[row][2] = board[row][4];
            board[row][3] = board[row][0];
            board[row][4] = board[row][0] = EMPTY;
        }
        if (color == WHITE) wKCastle = wQCastle = false;
        else                bKCastle = bQCastle = false;
        return;
    }

    // En passant capture
    if (m.isEnPassant) {
        board[m.fromRow][m.toCol] = EMPTY; // Remove captured pawn
    }

    // Move piece
    board[m.toRow][m.toCol] = m.promotion ? (color * m.promotion) : piece;
    board[m.fromRow][m.fromCol] = EMPTY;

    // Set en passant square for double pawn push
    if (pt == PAWN && abs(m.toRow - m.fromRow) == 2) {
        epRow = (m.fromRow + m.toRow) / 2;
        epCol = m.fromCol;
    }

    // Update castling rights
    if (pt == KING) {
        if (color == WHITE) wKCastle = wQCastle = false;
        else                bKCastle = bQCastle = false;
    }
    if (pt == ROOK) {
        if (color == WHITE) {
            if (m.fromRow==7 && m.fromCol==7) wKCastle = false;
            if (m.fromRow==7 && m.fromCol==0) wQCastle = false;
        } else {
            if (m.fromRow==0 && m.fromCol==7) bKCastle = false;
            if (m.fromRow==0 && m.fromCol==0) bQCastle = false;
        }
    }

    // Half-move clock
    if (pt == PAWN || m.captured) halfMoveClock = 0;
    else halfMoveClock++;
}

// ─────────────────────────────────────────────
//  LEGAL MOVE FILTER
// ─────────────────────────────────────────────
static int legalMoves(int color, Move out[], int maxOut) {
    Move pseudo[256];
    int n = generateMoves(color, pseudo, 256);
    int count = 0;
    BoardState saved;
    for (int i = 0; i < n && count < maxOut; i++) {
        saveState(saved);
        applyMove(pseudo[i], color);
        if (!inCheck(color)) out[count++] = pseudo[i];
        restoreState(saved);
    }
    return count;
}

// ─────────────────────────────────────────────
//  STATIC EVALUATION
// ─────────────────────────────────────────────
static int evaluate() {
    int score = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int p = board[r][c];
            if (p == EMPTY) continue;
            int color = colorOf(p);
            int pt = absP(p);
            int val = PIECE_VALUE[pt];

            // PST bonus (white tables are from white's perspective — row 7 = rank 1)
            int pr = (color == WHITE) ? r : (7 - r);
            int pc = c;
            int pst = 0;
            switch (pt) {
                case PAWN:   pst = PST_PAWN[pr][pc];   break;
                case KNIGHT: pst = PST_KNIGHT[pr][pc];  break;
                case BISHOP: pst = PST_BISHOP[pr][pc];  break;
                case ROOK:   pst = PST_ROOK[pr][pc];    break;
                case QUEEN:  pst = PST_QUEEN[pr][pc];   break;
                case KING:   pst = PST_KING_MID[pr][pc]; break;
            }

            score += color * (val + pst);
        }
    }
    return score; // positive = white advantage
}

// ─────────────────────────────────────────────
//  MINIMAX WITH ALPHA-BETA
// ─────────────────────────────────────────────
static int minimax(int depth, int alpha, int beta, int color, int maxDepth) {
    if (depth == 0) return evaluate();

    Move moves[128];
    int n = legalMoves(color, moves, 128);

    if (n == 0) {
        if (inCheck(color)) {
            // Checkmate — penalize by depth (faster mate = worse)
            return color * -20000 - depth * color;
        }
        return 0; // Stalemate
    }

    // Move ordering: captures first (improves alpha-beta pruning)
    // Simple: sort captures before quiet moves
    for (int i = 0; i < n-1; i++)
        for (int j = i+1; j < n; j++)
            if (moves[j].captured && !moves[i].captured)
                { Move tmp = moves[i]; moves[i] = moves[j]; moves[j] = tmp; }

    BoardState saved;
    int best = (color == WHITE) ? -99999 : 99999;

    for (int i = 0; i < n; i++) {
        saveState(saved);
        applyMove(moves[i], color);
        int score = minimax(depth - 1, alpha, beta, -color, maxDepth);
        restoreState(saved);

        if (color == WHITE) {
            if (score > best) best = score;
            if (score > alpha) alpha = score;
        } else {
            if (score < best) best = score;
            if (score < beta) beta = score;
        }
        if (beta <= alpha) break; // Alpha-beta cutoff
    }
    return best;
}

// ─────────────────────────────────────────────
//  AI MOVE SELECTION
// ─────────────────────────────────────────────
static Move selectAIMove(int color) {
    Move moves[128];
    int n = legalMoves(color, moves, 128);
    if (n == 0) { Move m; m.fromRow=-1; return m; }

    if (aiPersonality == AI_RANDOM) {
        return moves[random(n)];
    }

    int depth = AI_DEPTHS[aiPersonality];
    int bestScore = (color == WHITE) ? -99999 : 99999;
    int bestIdx = 0;

    // Fischer/Kasparov opening bias: prefer center + aggressive openings
    // Implemented as a small bonus to certain moves at shallow search
    BoardState saved;
    for (int i = 0; i < n; i++) {
        saveState(saved);
        applyMove(moves[i], color);
        int score = minimax(depth - 1, -99999, 99999, -color, depth);

        // Personality-specific adjustments
        if (aiPersonality >= AI_FISCHER) {
            // Aggression bonus: bonus for checks and captures at top level
            if (moves[i].captured) score += color * 15;
            if (inCheck(-color))   score += color * 20;
        }
        // Add tiny random noise to prevent repetition at lower levels
        if (depth <= 2) score += random(-10, 10);

        restoreState(saved);

        if (color == WHITE && score > bestScore) { bestScore = score; bestIdx = i; }
        if (color == BLACK && score < bestScore) { bestScore = score; bestIdx = i; }
    }
    return moves[bestIdx];
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawSquare(int row, int col, bool highlight = false, bool cursor = false, bool validMove = false) {
    int x = BOARD_X + col * SQ;
    int y = BOARD_Y + row * SQ;
    uint16_t bg;
    if (highlight)  bg = COL_SEL;
    else if (validMove) bg = COL_MOVE;
    else            bg = ((row + col) % 2 == 0) ? COL_LIGHT : COL_DARK;
    gfx->fillRect(x, y, SQ, SQ, bg);

    // Cursor outline
    if (cursor) {
        gfx->drawRect(x, y, SQ, SQ, COL_CURSOR);
        gfx->drawRect(x+1, y+1, SQ-2, SQ-2, COL_CURSOR);
    }
}

// Draw a piece glyph at screen position
//
// Pieces were authored for SQ=26 with literal pixel offsets. On
// Maxine SQ is 52 (exactly 2x), so we scale every offset by CPS
// so the pieces occupy the same fraction of each square on both
// targets without any artwork rework. CPS = SQ/26 = 1 on T-Deck
// and C28P, 2 on Maxine.
//
// NOTE — the identifier is CPS (Chess Piece Scale), not PS, because
// xtensa/config/specreg.h #define's PS as 230 (the Processor State
// register address). That macro is included transitively via
// Arduino.h -> FreeRTOS.h -> portable.h -> portmacro.h, and a bare
// `PS` token would be substituted to `230` before the compiler
// sees this declaration. Two-letter prefix on the chess-specific
// constant avoids the collision.
static void drawPiece(int row, int col, int piece) {
    if (piece == EMPTY) return;
    int x = BOARD_X + col * SQ + SQ/2;
    int y = BOARD_Y + row * SQ + SQ/2;
    int pt = absP(piece);
    int clr = (piece > 0) ? COL_WHITE_P : COL_BLACK_P;
    int out = (piece > 0) ? COL_OUTLINE : 0x632C;
    constexpr int CPS = SQ / 26;

    switch (pt) {
        case PAWN:
            gfx->fillCircle(x, y-4*CPS, 4*CPS, clr);
            gfx->drawCircle(x, y-4*CPS, 4*CPS, out);
            gfx->fillRect(x-3*CPS, y-1*CPS, 6*CPS, 6*CPS, clr);
            gfx->fillRect(x-5*CPS, y+4*CPS, 10*CPS, 3*CPS, clr);
            gfx->drawRect(x-5*CPS, y+4*CPS, 10*CPS, 3*CPS, out);
            break;
        case KNIGHT:
            // Horse-head stylized
            gfx->fillRect(x-5*CPS, y-2*CPS, 10*CPS, 10*CPS, clr);
            gfx->fillRect(x-3*CPS, y-7*CPS, 7*CPS, 7*CPS, clr);
            gfx->fillRect(x-6*CPS, y-4*CPS, 4*CPS, 5*CPS, clr);
            gfx->drawRect(x-6*CPS, y-7*CPS, 11*CPS, 19*CPS, out);
            gfx->fillRect(x-5*CPS, y+7*CPS, 10*CPS, 2*CPS, clr);
            gfx->drawRect(x-5*CPS, y+7*CPS, 10*CPS, 2*CPS, out);
            // Eye
            gfx->fillCircle(x+1*CPS, y-4*CPS, 1*CPS, (piece>0)?0x0000:0xFFFF);
            break;
        case BISHOP:
            gfx->fillTriangle(x, y-9*CPS, x-4*CPS, y+7*CPS, x+4*CPS, y+7*CPS, clr);
            gfx->drawTriangle(x, y-9*CPS, x-4*CPS, y+7*CPS, x+4*CPS, y+7*CPS, out);
            gfx->fillCircle(x, y-9*CPS, 2*CPS, clr);
            gfx->drawCircle(x, y-9*CPS, 2*CPS, out);
            gfx->fillRect(x-5*CPS, y+7*CPS, 10*CPS, 2*CPS, clr);
            gfx->drawRect(x-5*CPS, y+7*CPS, 10*CPS, 2*CPS, out);
            break;
        case ROOK:
            gfx->fillRect(x-5*CPS, y-8*CPS, 10*CPS, 16*CPS, clr);
            gfx->drawRect(x-5*CPS, y-8*CPS, 10*CPS, 16*CPS, out);
            // Battlements
            gfx->fillRect(x-5*CPS, y-10*CPS, 3*CPS, 4*CPS, clr);
            gfx->fillRect(x,       y-10*CPS, 3*CPS, 4*CPS, clr);
            gfx->fillRect(x+2*CPS, y-10*CPS, 3*CPS, 4*CPS, clr);
            gfx->fillRect(x-5*CPS, y+7*CPS, 10*CPS, 3*CPS, clr);
            gfx->drawRect(x-5*CPS, y+7*CPS, 10*CPS, 3*CPS, out);
            break;
        case QUEEN:
            gfx->fillCircle(x, y-6*CPS, 5*CPS, clr);
            gfx->drawCircle(x, y-6*CPS, 5*CPS, out);
            gfx->fillRect(x-5*CPS, y-2*CPS, 10*CPS, 10*CPS, clr);
            gfx->drawRect(x-5*CPS, y-2*CPS, 10*CPS, 10*CPS, out);
            gfx->fillRect(x-6*CPS, y+7*CPS, 12*CPS, 3*CPS, clr);
            gfx->drawRect(x-6*CPS, y+7*CPS, 12*CPS, 3*CPS, out);
            // Crown points
            gfx->fillCircle(x-4*CPS, y-9*CPS, 2*CPS, clr); gfx->drawCircle(x-4*CPS, y-9*CPS, 2*CPS, out);
            gfx->fillCircle(x+4*CPS, y-9*CPS, 2*CPS, clr); gfx->drawCircle(x+4*CPS, y-9*CPS, 2*CPS, out);
            gfx->fillCircle(x,       y-11*CPS, 2*CPS, clr); gfx->drawCircle(x,       y-11*CPS, 2*CPS, out);
            break;
        case KING:
            gfx->fillRect(x-5*CPS, y-2*CPS, 10*CPS, 10*CPS, clr);
            gfx->drawRect(x-5*CPS, y-2*CPS, 10*CPS, 10*CPS, out);
            // Cross
            gfx->fillRect(x-1*CPS, y-9*CPS, 3*CPS, 8*CPS, clr);
            gfx->fillRect(x-4*CPS, y-7*CPS, 9*CPS, 3*CPS, clr);
            gfx->drawRect(x-1*CPS, y-9*CPS, 3*CPS, 8*CPS, out);
            gfx->drawRect(x-4*CPS, y-7*CPS, 9*CPS, 3*CPS, out);
            gfx->fillRect(x-6*CPS, y+7*CPS, 12*CPS, 3*CPS, clr);
            gfx->drawRect(x-6*CPS, y+7*CPS, 12*CPS, 3*CPS, out);
            break;
    }
}

static bool validMoveTargets[8][8]; // precomputed valid move squares for selected piece

static void computeValidMoves(int row, int col) {
    memset(validMoveTargets, 0, sizeof(validMoveTargets));
    int color = colorOf(board[row][col]);
    Move moves[64];
    int n = legalMoves(color, moves, 64);
    for (int i = 0; i < n; i++)
        if (moves[i].fromRow == row && moves[i].fromCol == col)
            validMoveTargets[moves[i].toRow][moves[i].toCol] = true;
}

static void drawBoard() {
    // Check highlighting for kings
    int wkr=-1, wkc=-1, bkr=-1, bkc=-1;
    findKing(WHITE, wkr, wkc);
    findKing(BLACK, bkr, bkc);

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            bool sel  = pieceSelected && r == selRow && c == selCol;
            bool vm   = pieceSelected && validMoveTargets[r][c];
            bool cur  = (r == cursorRow && c == cursorCol);
            bool chk  = (whiteInCheck && r==wkr && c==wkc) ||
                        (blackInCheck && r==bkr && c==bkc);
            
            // Check overrides other colors
            if (chk) {
                int x = BOARD_X + c * SQ, y = BOARD_Y + r * SQ;
                gfx->fillRect(x, y, SQ, SQ, COL_CHECK);
            } else {
                drawSquare(r, c, sel, cur, vm);
            }
            drawPiece(r, c, board[r][c]);
        }
    }

    // Rank / file coords
#ifdef DEVICE_MAXINE
    // Maxine: size-2 text for the a-h / 8-1 labels so they read on
    // the larger panel. Centered against the larger SQ/2 mid-points.
    gfx->setTextSize(2);
    gfx->setTextColor(COL_COORD);
    for (int i = 0; i < 8; i++) {
        // Files a-h along bottom
        gfx->setCursor(BOARD_X + i*SQ + SQ/2 - 6, BOARD_Y + 8*SQ + 2);
        gfx->print((char)('a' + i));
        // Ranks 8-1 along left
        gfx->setCursor(BOARD_X - 20, BOARD_Y + i*SQ + SQ/2 - 8);
        gfx->print(8 - i);
    }
#else
    gfx->setTextSize(1);
    gfx->setTextColor(COL_COORD);
    for (int i = 0; i < 8; i++) {
        // Files a-h along bottom
        gfx->setCursor(BOARD_X + i*SQ + SQ/2 - 3, BOARD_Y + 8*SQ + 1);
        gfx->print((char)('a' + i));
        // Ranks 8-1 along left
        gfx->setCursor(BOARD_X - 10, BOARD_Y + i*SQ + SQ/2 - 4);
        gfx->print(8 - i);
    }
#endif
}

static void drawPanel() {
#ifdef DEVICE_MAXINE
    // Maxine: panel is a horizontal strip BELOW the board, not a
    // vertical strip beside it. Lay out the same fields left-to-right
    // across the 480px width: turn indicator | AI | check status |
    // captured pieces | controls hint.
    gfx->fillRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, COL_BG);
    gfx->drawFastHLine(0, PANEL_Y, 480, 0x4208);

    gfx->setTextSize(2);

    // Turn indicator (leftmost)
    gfx->setCursor(8, PANEL_Y + 6);
    gfx->setTextColor(whiteToMove ? COL_WHITE_P : 0xAD55);
    gfx->print(whiteToMove ? "WHITE" : "BLACK");
    gfx->setTextColor(COL_TEXT);
    gfx->print(" TURN");

    // AI personality
    gfx->setCursor(8, PANEL_Y + 30);
    gfx->setTextColor(0x07FF);
    gfx->print("AI:");
    gfx->setTextColor(0xFD20);
    gfx->setCursor(54, PANEL_Y + 30);
    gfx->print(AI_NAMES[aiPersonality]);

    // Check status (middle)
    gfx->setTextSize(2);
    if (whiteInCheck) {
        gfx->setTextColor(COL_CHECK);
        gfx->setCursor(200, PANEL_Y + 6);
        gfx->print("W IN CHECK");
    } else if (blackInCheck) {
        gfx->setTextColor(COL_CHECK);
        gfx->setCursor(200, PANEL_Y + 6);
        gfx->print("B IN CHECK");
    }

    // Controls hint (right)
    gfx->setTextSize(1);
    gfx->setTextColor(0xAD55);
    gfx->setCursor(340, PANEL_Y + 8);
    gfx->print("TAP A PIECE THEN");
    gfx->setCursor(340, PANEL_Y + 18);
    gfx->print("TAP A SQUARE");
    gfx->setCursor(340, PANEL_Y + 32);
    gfx->print("A KEY = AI MODE");
    gfx->setCursor(340, PANEL_Y + 42);
    gfx->print("Q / EXIT = QUIT");
    return;
#endif

#if defined(DEVICE_C28P) || defined(DEVICE_C5)
    // C28P + C5: 240x72 panel below the board.
    // zones top-to-bottom:
    //    row 1 (y+4) : "WHITE TURN" / "BLACK TURN" (size-2 left)
    //                  + check badge (size-2 right, red, if any)
    //    row 2 (y+26): "AI: <name>" rendered as a TAPPABLE button.
    //                  Tap zone = the full 240px row, ~22px tall.
    //                  Tapping it cycles aiPersonality.
    //    row 3 (y+52): "TAP PIECE > SQUARE" hint (size-1)
    //    row 4 (y+62): "TAP TOP TO EXIT" hint (size-1)
    gfx->fillRect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, COL_BG);
    gfx->drawFastHLine(0, PANEL_Y, 240, 0x4208);

    // Row 1 — turn indicator + check status
    gfx->setTextSize(2);
    gfx->setCursor(4, PANEL_Y + 4);
    gfx->setTextColor(whiteToMove ? COL_WHITE_P : 0xAD55);
    gfx->print(whiteToMove ? "WHITE" : "BLACK");
    gfx->setTextColor(COL_TEXT);
    gfx->print(" TURN");
    if (whiteInCheck) {
        gfx->setTextColor(COL_CHECK);
        gfx->setCursor(150, PANEL_Y + 4);
        gfx->print("CHECK!");
    } else if (blackInCheck) {
        gfx->setTextColor(COL_CHECK);
        gfx->setCursor(150, PANEL_Y + 4);
        gfx->print("CHECK!");
    }

    // Row 2 — AI personality button (tappable, cycles on tap).
    // Draw it as a button (filled rect with border) so the user
    // realizes it's interactive.
    gfx->fillRect(2, PANEL_Y + 24, 236, 22, 0x2104);
    gfx->drawRect(2, PANEL_Y + 24, 236, 22, 0x07FF);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, PANEL_Y + 28);
    gfx->print("AI:");
    gfx->setTextColor(0xFD20);
    gfx->setCursor(46, PANEL_Y + 28);
    gfx->print(AI_NAMES[aiPersonality]);
    // Cycle hint (right side of button)
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(200, PANEL_Y + 32);
    gfx->print("TAP");

    // Row 3 + 4 — controls
    gfx->setTextSize(1);
    gfx->setTextColor(0xAD55);
    gfx->setCursor(4, PANEL_Y + 52);
    gfx->print("TAP PIECE > TAP SQUARE");
    gfx->setCursor(4, PANEL_Y + 62);
    gfx->print("TAP TOP STRIP TO EXIT");
    return;
#endif

    gfx->fillRect(PANEL_X, 0, PANEL_W, 240, COL_BG);
    gfx->drawFastVLine(PANEL_X-1, 0, 240, 0x4208);

    gfx->setTextSize(1);
    gfx->setTextColor(COL_TEXT);

    // Turn indicator
    gfx->setCursor(PANEL_X+2, 4);
    gfx->setTextColor(whiteToMove ? COL_WHITE_P : 0xAD55);
    gfx->print(whiteToMove ? "WHITE" : "BLACK");
    gfx->setTextColor(COL_TEXT);
    gfx->print(" TO");
    gfx->setCursor(PANEL_X+2, 14);
    gfx->print("MOVE");

    // AI personality
    gfx->setCursor(PANEL_X+2, 30);
    gfx->setTextColor(0x07FF);
    gfx->print("AI:");
    gfx->setCursor(PANEL_X+2, 40);
    gfx->setTextColor(0xFD20);
    gfx->print(AI_NAMES[aiPersonality]);

    // Check/status
    gfx->setCursor(PANEL_X+2, 56);
    if (whiteInCheck) {
        gfx->setTextColor(COL_CHECK); gfx->print("WHITE");
        gfx->setCursor(PANEL_X+2, 66);
        gfx->print("IN CHECK");
    } else if (blackInCheck) {
        gfx->setTextColor(COL_CHECK); gfx->print("BLACK");
        gfx->setCursor(PANEL_X+2, 66);
        gfx->print("IN CHECK");
    } else {
        gfx->setTextColor(0x4208); gfx->print("--------");
    }

    // Captured pieces (simplified counts)
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(PANEL_X+2, 84);
    gfx->print("CAPTURED:");
    
    // White captured (shown in white)
    gfx->setTextColor(COL_WHITE_P);
    int y = 94;
    for (int pt = PAWN; pt <= QUEEN; pt++) {
        if (whiteCaptured[pt] > 0) {
            gfx->setCursor(PANEL_X+2, y);
            const char* names[] = {"","P","N","B","R","Q","K"};
            gfx->printf("%sx%d", names[pt], whiteCaptured[pt]);
            y += 10;
        }
    }
    // Black captured (shown in yellow)
    gfx->setTextColor(0xFFE0);
    for (int pt = PAWN; pt <= QUEEN; pt++) {
        if (blackCaptured[pt] > 0) {
            gfx->setCursor(PANEL_X+2, y);
            const char* names[] = {"","P","N","B","R","Q","K"};
            gfx->printf("%sx%d", names[pt], blackCaptured[pt]);
            y += 10;
        }
    }

    // Controls hint at bottom
    gfx->setTextColor(0x4208);
    gfx->setCursor(PANEL_X+2, 200);
    gfx->print("A=AI MODE");
    gfx->setCursor(PANEL_X+2, 210);
    gfx->print("Q=QUIT");
    gfx->setCursor(PANEL_X+2, 220);
    gfx->print("CLK=SELECT");
}

static void drawFull() {
    // Chess is a full-screen app on every device — no chrome that
    // any other layer is responsible for preserving — so a plain
    // fillScreen is safe regardless of target.
    gfx->fillScreen(0x0000);
    drawBoard();
    drawPanel();
#ifdef DEVICE_MAXINE
    // Paint the tap-to-quit affordance in the top 30px header strip.
    // Without this hint, users on Maxine have no visible way to leave
    // chess before game-over (no physical keyboard, no dpad, no
    // gamepad — only the touchscreen). Left-aligned "< EXIT" with a
    // centered "CHESS" title makes the header read as intentional UI
    // rather than dead space.
    gfx->setTextSize(2);
    gfx->setTextColor(0xAD55);
    gfx->setCursor(8, 8);
    gfx->print("< EXIT");
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(200, 8);
    gfx->print("CHESS");
#endif
}

static void showMessage(const char* line1, const char* line2 = nullptr) {
#ifdef DEVICE_MAXINE
    // Maxine: scale the popup to be legible against the much larger
    // board. Center horizontally inside the board area; size-3 title,
    // size-2 subtitle. Wrapped in a block so the bx/by/bw/bh locals
    // don't collide with the T-Deck path's same-named variables
    // below — even though only one path runs at a time, the compiler
    // sees both declarations in the same function scope.
    {
        const int bw = 360, bh = 130;
        const int bx = (480 - bw) / 2;
        const int by = (BOARD_Y + SQ*8 / 2) - bh/2;
        gfx->fillRect(bx, by, bw, bh, 0x18C3);
        gfx->drawRect(bx, by, bw, bh, 0xFFE0);
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFE0);
        int title_w = (int)strlen(line1) * 18;   // size-3 ~18px/char
        gfx->setCursor(bx + (bw - title_w) / 2, by + 22);
        gfx->print(line1);
        if (line2) {
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFFF);
            int sub_w = (int)strlen(line2) * 12;
            gfx->setCursor(bx + (bw - sub_w) / 2, by + 70);
            gfx->print(line2);
        }
        delay(2500);
    }
    return;
#endif
    int bx = BOARD_X + 10, by = BOARD_Y + 80;
    gfx->fillRect(bx, by, 190, 60, 0x18C3);
    gfx->drawRect(bx, by, 190, 60, 0xFFE0);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(bx+10, by+12);
    gfx->print(line1);
    if (line2) {
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(bx+10, by+36);
        gfx->print(line2);
    }
    delay(2500);
}

// ─────────────────────────────────────────────
//  PLAYER MOVE HANDLING
// ─────────────────────────────────────────────
static bool tryPlayerMove(int toRow, int toCol) {
    if (!pieceSelected) return false;
    
    // Find matching legal move
    Move moves[64];
    int n = legalMoves(WHITE, moves, 64);
    for (int i = 0; i < n; i++) {
        if (moves[i].fromRow == selRow && moves[i].fromCol == selCol &&
            moves[i].toRow  == toRow  && moves[i].toCol  == toCol) {
            
            // Execute move
            int cap = moves[i].captured;
            applyMove(moves[i], WHITE);
            
            // Track captures
            if (cap && absP(cap) < 7) blackCaptured[absP(cap)]++;
            
            whiteToMove = false;
            whiteInCheck = inCheck(WHITE);
            blackInCheck  = inCheck(BLACK);
            pieceSelected = false;
            selRow = selCol = -1;
            memset(validMoveTargets, 0, sizeof(validMoveTargets));
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_chess() {
    aiPersonality = AI_CLUB; // Default: Club player

    // Chess on every device is full-screen: no maxine_dpad_render()
    // or c28p_dpad_render() call here. Every input is a tap, so
    // there's no virtual dpad chrome to reserve a strip for. Compare
    // pacman / galaga / pole_position which paint dpad chrome before
    // entering their game loop because they need real-time
    // directional input.

    while (true) { // New game loop
        initBoard();
        playerIsWhite = true;
        drawFull();

        bool quit = false;

        while (!gameOver && !quit) {
            // ── PLAYER TURN (White) ──
            if (whiteToMove) {
                // Check for stalemate/checkmate
                Move lm[128];
                int n = legalMoves(WHITE, lm, 128);
                if (n == 0) {
                    if (whiteInCheck) showMessage("CHECKMATE!", "Black wins.");
                    else              showMessage("STALEMATE!", "Draw.");
                    gameOver = true; break;
                }

                bool moved = false;
                while (!moved && !quit) {
                    // Input
                    char k = get_keypress();
                    TrackballState tb = update_trackball_game();

                    // Header tap = quit. Each device has its own hot
                    // zone: T-Deck top 40px, C28P top 14px, Maxine
                    // top 30px. Below that is either the board, the
                    // gap, or the panel — taps there mean something
                    // else (board = move, panel = AI cycle, etc).
                    int16_t tx, ty;
#if defined(DEVICE_C28P) || defined(DEVICE_C5)
                    // C28P + C5: top 14px strip is the quit hot zone.
                    // Anything below is either the board or the panel.
                    if (kiosk_touch(&tx, &ty) && ty < 14) {
                        // Debounce: wait for release before quitting.
                        int16_t rx, ry;
                        while (kiosk_touch(&rx, &ry)) { delay(10); yield(); }
                        quit = true; break;
                    }
#elif defined(DEVICE_MAXINE)
                    // Maxine: top 30px strip is the quit hot zone.
                    // The "< EXIT" hint painted by drawFull() lives
                    // in this strip so the affordance is visible.
                    // Using a fresh pair of locals (mqx/mqy) rather
                    // than reusing tx/ty so this branch is self
                    // contained and the board-tap branch below can
                    // continue to use tx/ty for the T-Deck path.
                    int16_t mqx, mqy;
                    if (maxine_touch_read(&mqx, &mqy) && mqy < 30) {
                        // Debounce: wait for release before quitting.
                        int16_t rx, ry;
                        while (maxine_touch_read(&rx, &ry)) { delay(10); yield(); }
                        quit = true; break;
                    }
#else
                    if (get_touch(&tx, &ty) && ty < 40) {
                        while(get_touch(&tx,&ty)){delay(10);}
                        quit = true; break;
                    }
#endif
                    if (k == 'q' || k == 'Q') { quit = true; break; }

                    // Gamepad HOME/START = quit
                    if (gamepad_poll()) { quit = true; break; }
                    if (gamepad_pressed(GP_START)) { quit = true; break; }

                    // Cycle AI personality
                    if (k == 'a' || k == 'A') {
                        aiPersonality = (aiPersonality + 1) % 6;
                        drawPanel();
                        continue;
                    }

                    // Cursor movement — trackball and D-pad
                    if (tb.y == -1 || gamepad_pressed(GP_UP))    { if (cursorRow > 0) cursorRow--; }
                    if (tb.y ==  1 || gamepad_pressed(GP_DOWN))  { if (cursorRow < 7) cursorRow++; }
                    if (tb.x == -1 || gamepad_pressed(GP_LEFT))  { if (cursorCol > 0) cursorCol--; }
                    if (tb.x ==  1 || gamepad_pressed(GP_RIGHT)) { if (cursorCol < 7) cursorCol++; }

                    // Touch on board = move cursor to tapped square and confirm
                    bool touchConfirm = false;
#ifdef DEVICE_MAXINE
                    // Maxine has no T-Deck-style touch driver; route through
                    // maxine_touch_read instead. Tap inside the board area
                    // moves the cursor to that square and confirms.
                    int16_t mtx, mty;
                    if (maxine_touch_read(&mtx, &mty) &&
                        mtx >= BOARD_X && mty >= BOARD_Y &&
                        mtx < BOARD_X + SQ*8 && mty < BOARD_Y + SQ*8) {
                        // Debounce: wait for release
                        int16_t dx, dy;
                        while (maxine_touch_read(&dx, &dy)) { delay(5); yield(); }
                        cursorCol = (mtx - BOARD_X) / SQ;
                        cursorRow = (mty - BOARD_Y) / SQ;
                        touchConfirm = true;
                    }
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
                    // C28P + C5: tap inside the board area moves cursor and confirms.
                    // Tap on the AI button row (PANEL_Y+24..PANEL_Y+45) cycles
                    // the AI personality — since there's no 'A' key on this
                    // device, the button is the only way to change difficulty.
                    int16_t ctx, cty;
                    if (kiosk_touch(&ctx, &cty)) {
                        // Board tap
                        if (ctx >= BOARD_X && cty >= BOARD_Y &&
                            ctx < BOARD_X + SQ*8 && cty < BOARD_Y + SQ*8) {
                            int16_t rx, ry;
                            while (kiosk_touch(&rx, &ry)) { delay(5); yield(); }
                            cursorCol = (ctx - BOARD_X) / SQ;
                            cursorRow = (cty - BOARD_Y) / SQ;
                            touchConfirm = true;
                        }
                        // AI cycle button tap
                        else if (cty >= PANEL_Y + 24 && cty < PANEL_Y + 46) {
                            int16_t rx, ry;
                            while (kiosk_touch(&rx, &ry)) { delay(5); yield(); }
                            aiPersonality = (aiPersonality + 1) % 6;
                            drawPanel();
                            continue;
                        }
                    }
#else
                    if (get_touch(&tx, &ty) && tx >= BOARD_X && ty >= BOARD_Y &&
                        tx < BOARD_X + SQ*8 && ty < BOARD_Y + SQ*8) {
                        while(get_touch(&tx,&ty)){delay(5);}
                        cursorCol = (tx - BOARD_X) / SQ;
                        cursorRow = (ty - BOARD_Y) / SQ;
                        touchConfirm = true;
                    }
#endif

                    // Select / move — trackball click, SPACE, ENTER, or touch on board
                    bool confirm = tb.clicked || gamepad_pressed(GP_A) ||
                                   k == ' ' || k == '\n' || k == '\r' ||
                                   touchConfirm;

                    // Deselect — GP_B or ESC
                    if ((gamepad_pressed(GP_B) || k == 27) && pieceSelected) {
                        pieceSelected = false;
                        memset(validMoveTargets, 0, sizeof(validMoveTargets));
                        drawBoard(); drawPanel();
                    }

                    if (confirm) {
                        if (!pieceSelected) {
                            // Select a white piece
                            if (colorOf(board[cursorRow][cursorCol]) == WHITE) {
                                selRow = cursorRow; selCol = cursorCol;
                                pieceSelected = true;
                                computeValidMoves(selRow, selCol);
                            }
                        } else {
                            // Try to move to cursor, or reselect
                            if (cursorRow == selRow && cursorCol == selCol) {
                                // Deselect
                                pieceSelected = false;
                                memset(validMoveTargets, 0, sizeof(validMoveTargets));
                            } else if (!tryPlayerMove(cursorRow, cursorCol)) {
                                // Couldn't move — try to select new piece
                                if (colorOf(board[cursorRow][cursorCol]) == WHITE) {
                                    selRow = cursorRow; selCol = cursorCol;
                                    computeValidMoves(selRow, selCol);
                                } else {
                                    pieceSelected = false;
                                    memset(validMoveTargets, 0, sizeof(validMoveTargets));
                                }
                            } else {
                                moved = true;
                            }
                        }
                        drawBoard();
                        drawPanel();
                    } else if (tb.x != 0 || tb.y != 0 ||
                               gamepad_pressed(GP_UP | GP_DOWN | GP_LEFT | GP_RIGHT)) {
                        // Redraw board to show new cursor position
                        drawBoard();
                    }
                    delay(30);
                    yield();
                }
                if (quit) break;

            } else {
                // ── AI TURN (Black) ──
                // Check for AI checkmate/stalemate
                Move lm[128];
                int n = legalMoves(BLACK, lm, 128);
                if (n == 0) {
                    if (blackInCheck) showMessage("CHECKMATE!", "You win!");
                    else              showMessage("STALEMATE!", "Draw.");
                    gameOver = true; break;
                }

                // Show "thinking" indicator
#ifdef DEVICE_MAXINE
                // Maxine: panel sits below the board. Write thinking into
                // the right portion of the panel where the controls hint
                // lives (overwrite hint temporarily during AI think).
                gfx->fillRect(340, PANEL_Y + 8, 140, 40, COL_BG);
                gfx->setCursor(340, PANEL_Y + 14);
                gfx->setTextColor(0xFD20);
                gfx->setTextSize(2);
                gfx->print("THINKING");
                for (int d = 0; d < 3; d++) {
                    gfx->print(".");
                    delay(200);
                }
#else
                gfx->setCursor(PANEL_X+2, 170);
                gfx->setTextColor(0xFD20);
                gfx->setTextSize(1);
                gfx->print("THINKING");
                for (int d = 0; d < 3; d++) {
                    gfx->print(".");
                    delay(200);
                }
#endif

                Move ai = selectAIMove(BLACK);
                if (ai.fromRow < 0) { gameOver = true; break; }

                int cap = ai.captured;
                applyMove(ai, BLACK);
                if (cap && absP(cap) < 7) whiteCaptured[absP(cap)]++;

                whiteToMove = true;
                whiteInCheck = inCheck(WHITE);
                blackInCheck  = inCheck(BLACK);

                if (whiteInCheck) {
                    // Brief flash
#ifdef DEVICE_MAXINE
                    // No discrete CHECK badge on Maxine; the next drawPanel()
                    // call paints "W IN CHECK" in red across the middle of
                    // the panel strip. Just pause briefly so the player
                    // notices the state change.
                    delay(500);
#else
                    gfx->setCursor(PANEL_X+2, 170);
                    gfx->fillRect(PANEL_X+2, 168, PANEL_W-4, 12, COL_BG);
                    gfx->setTextColor(COL_CHECK);
                    gfx->print("CHECK!");
                    delay(500);
#endif
                }

                // Clear thinking indicator
#ifdef DEVICE_MAXINE
                // Restore the controls hint area
                gfx->fillRect(340, PANEL_Y + 6, 140, 44, COL_BG);
#else
                gfx->fillRect(PANEL_X+2, 168, PANEL_W-4, 20, COL_BG);
#endif
                drawBoard();
                drawPanel();
            }
        }

        if (quit) break;

        // Offer new game
        showMessage("GAME OVER", "Any key=new game");
        bool newGame = false;
        unsigned long t = millis();
        while (millis() - t < 10000) {
            char k = get_keypress();
            TrackballState tb = update_trackball();
            int16_t tx, ty;
#if defined(DEVICE_MAXINE)
            // Maxine: any touch anywhere starts a new game.
            if (k || tb.clicked || maxine_touch_read(&tx, &ty)) {
                newGame = true; break;
            }
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
            // C28P + C5: any tap restarts the game (matching T-Deck's
            // behavior). The 10-second timeout is the only way out
            // — if the player wants to leave chess after game-over,
            // they just don't tap.
            if (k || tb.clicked || kiosk_touch(&tx, &ty)) {
                newGame = true; break;
            }
#else
            if (k || tb.clicked || (get_touch(&tx,&ty) && ty < 40)) {
                newGame = true; break;
            }
#endif
            delay(50);
        }
        if (!newGame) break;
        gameOver = false;
    }

    // Full-screen clear on exit on every device — chess never had
    // any chrome below (Maxine) or beside (T-Deck) the board that
    // the caller is responsible for preserving, so the cleanup is
    // identical everywhere.
    gfx->fillScreen(0x0000);
}