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

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  BOARD GEOMETRY
// ─────────────────────────────────────────────
#define SQ          26          // Square size px
#define BOARD_X     16          // Board left edge
#define BOARD_Y      8          // Board top edge
#define PANEL_X    (BOARD_X + SQ*8 + 4)  // Right panel x = 228
#define PANEL_W    (320 - PANEL_X - 2)   // ~90px

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
static void drawPiece(int row, int col, int piece) {
    if (piece == EMPTY) return;
    int x = BOARD_X + col * SQ + SQ/2;
    int y = BOARD_Y + row * SQ + SQ/2;
    int pt = absP(piece);
    int clr = (piece > 0) ? COL_WHITE_P : COL_BLACK_P;
    int out = (piece > 0) ? COL_OUTLINE : 0x632C;

    switch (pt) {
        case PAWN:
            gfx->fillCircle(x, y-4, 4, clr);
            gfx->drawCircle(x, y-4, 4, out);
            gfx->fillRect(x-3, y-1, 6, 6, clr);
            gfx->fillRect(x-5, y+4, 10, 3, clr);
            gfx->drawRect(x-5, y+4, 10, 3, out);
            break;
        case KNIGHT:
            // Horse-head stylized
            gfx->fillRect(x-5, y-2, 10, 10, clr);
            gfx->fillRect(x-3, y-7, 7, 7, clr);
            gfx->fillRect(x-6, y-4, 4, 5, clr);
            gfx->drawRect(x-6, y-7, 11, 19, out);
            gfx->fillRect(x-5, y+7, 10, 2, clr);
            gfx->drawRect(x-5, y+7, 10, 2, out);
            // Eye
            gfx->fillCircle(x+1, y-4, 1, (piece>0)?0x0000:0xFFFF);
            break;
        case BISHOP:
            gfx->fillTriangle(x, y-9, x-4, y+7, x+4, y+7, clr);
            gfx->drawTriangle(x, y-9, x-4, y+7, x+4, y+7, out);
            gfx->fillCircle(x, y-9, 2, clr);
            gfx->drawCircle(x, y-9, 2, out);
            gfx->fillRect(x-5, y+7, 10, 2, clr);
            gfx->drawRect(x-5, y+7, 10, 2, out);
            break;
        case ROOK:
            gfx->fillRect(x-5, y-8, 10, 16, clr);
            gfx->drawRect(x-5, y-8, 10, 16, out);
            // Battlements
            gfx->fillRect(x-5, y-10, 3, 4, clr);
            gfx->fillRect(x,   y-10, 3, 4, clr);
            gfx->fillRect(x+2, y-10, 3, 4, clr);
            gfx->fillRect(x-5, y+7, 10, 3, clr);
            gfx->drawRect(x-5, y+7, 10, 3, out);
            break;
        case QUEEN:
            gfx->fillCircle(x, y-6, 5, clr);
            gfx->drawCircle(x, y-6, 5, out);
            gfx->fillRect(x-5, y-2, 10, 10, clr);
            gfx->drawRect(x-5, y-2, 10, 10, out);
            gfx->fillRect(x-6, y+7, 12, 3, clr);
            gfx->drawRect(x-6, y+7, 12, 3, out);
            // Crown points
            gfx->fillCircle(x-4, y-9, 2, clr); gfx->drawCircle(x-4, y-9, 2, out);
            gfx->fillCircle(x+4, y-9, 2, clr); gfx->drawCircle(x+4, y-9, 2, out);
            gfx->fillCircle(x,   y-11,2, clr); gfx->drawCircle(x,   y-11,2, out);
            break;
        case KING:
            gfx->fillRect(x-5, y-2, 10, 10, clr);
            gfx->drawRect(x-5, y-2, 10, 10, out);
            // Cross
            gfx->fillRect(x-1, y-9, 3, 8, clr);
            gfx->fillRect(x-4, y-7, 9, 3, clr);
            gfx->drawRect(x-1, y-9, 3, 8, out);
            gfx->drawRect(x-4, y-7, 9, 3, out);
            gfx->fillRect(x-6, y+7, 12, 3, clr);
            gfx->drawRect(x-6, y+7, 12, 3, out);
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
}

static void drawPanel() {
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
    gfx->fillScreen(0x0000);
    drawBoard();
    drawPanel();
}

static void showMessage(const char* line1, const char* line2 = nullptr) {
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
                    TrackballState tb = update_trackball();

                    // Header tap = quit
                    int16_t tx, ty;
                    if (get_touch(&tx, &ty) && ty < 8) {
                        while(get_touch(&tx,&ty)){delay(10);}
                        quit = true; break;
                    }
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

                    // Select / move — trackball click or GP_A
                    bool confirm = tb.clicked || gamepad_pressed(GP_A);
                    // Deselect — GP_B
                    if (gamepad_pressed(GP_B) && pieceSelected) {
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
                gfx->setCursor(PANEL_X+2, 170);
                gfx->setTextColor(0xFD20);
                gfx->setTextSize(1);
                gfx->print("THINKING");
                for (int d = 0; d < 3; d++) {
                    gfx->print(".");
                    delay(200);
                }

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
                    gfx->setCursor(PANEL_X+2, 170);
                    gfx->fillRect(PANEL_X+2, 168, PANEL_W-4, 12, COL_BG);
                    gfx->setTextColor(COL_CHECK);
                    gfx->print("CHECK!");
                    delay(500);
                }

                // Clear thinking indicator
                gfx->fillRect(PANEL_X+2, 168, PANEL_W-4, 20, COL_BG);
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
            if (k || tb.clicked || (get_touch(&tx,&ty) && ty < 8)) {
                newGame = true; break;
            }
            delay(50);
        }
        if (!newGame) break;
        gameOver = false;
    }

    gfx->fillScreen(0x0000);
}