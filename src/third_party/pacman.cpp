/**
 * PISCES MOON OS — PAC-MAN
 * Faithful arcade clone for LilyGO T-Deck Plus (320x240)
 *
 * Layout: 8px tiles, 28x31 maze = 224x248px
 * Centered horizontally (48px left margin), 8px top for score strip
 *
 * Ghost AI (faithful to original):
 * BLINKY  (red)   — targets Pac-Man directly
 * PINKY   (pink)  — targets 4 tiles ahead of Pac-Man's direction
 * INKY    (cyan)  — targets using Blinky's position as mirror point
 * CLYDE   (orange)— targets directly if >8 tiles away, else scatter
 *
 * Controls: Trackball direction = next turn intent, WASD fallback, Q=quit
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SdFat.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "pacman.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  CONSTANTS
// ─────────────────────────────────────────────
#define TS          8           // Tile size in pixels
#define MAZE_W      28
#define MAZE_H      31
#define MARGIN_X    ((320 - MAZE_W * TS) / 2)   // 48
#define MARGIN_Y    10          // Score strip height
#define HS_PATH     "/pacman_hs.txt"

// Tile types
#define T_EMPTY     0
#define T_WALL      1
#define T_DOT       2
#define T_ENERGIZER 3
#define T_DOOR      4   // Ghost house door

// Directions
#define DIR_NONE    0
#define DIR_LEFT    1
#define DIR_RIGHT   2
#define DIR_UP      3
#define DIR_DOWN    4

// Ghost states
#define GS_SCATTER    0
#define GS_CHASE      1
#define GS_FRIGHTENED 2
#define GS_EATEN      3
#define GS_HOUSE      4   // Waiting in ghost house

// Colors (RGB565)
#define COL_WALL        0x0410   // Dark blue
#define COL_DOT         0xFFDF   // Cream
#define COL_ENERGIZER   0xFFDF
#define COL_PACMAN      0xFFE0   // Yellow
#define COL_BLINKY      0xF800   // Red
#define COL_PINKY       0xFBB7   // Pink
#define COL_INKY        0x07FF   // Cyan
#define COL_CLYDE       0xFD20   // Orange
#define COL_FRIGHTENED  0x001F   // Blue
#define COL_EATEN       0xFFFF   // White (eyes only)
#define COL_BLACK       0x0000
#define COL_SCORE       0xFFFF

// ─────────────────────────────────────────────
//  MAZE DATA
//  Classic 28x31 Pac-Man maze
//  W=wall, .=dot, o=energizer, ' '=empty, D=door
// ─────────────────────────────────────────────
static const char MAZE_TEMPLATE[MAZE_H][MAZE_W + 2] = { // FIX: Changed from MAZE_W + 1 to MAZE_W + 2
    "WWWWWWWWWWWWWWWWWWWWWWWWWWWW",
    "W............WW............W",
    "W.WWWW.WWWWW.WW.WWWWW.WWWW.W",
    "WoWWWW.WWWWW.WW.WWWWW.WWWWoW",
    "W.WWWW.WWWWW.WW.WWWWW.WWWW.W",
    "W..........................W",  // row 5
    "W.WWWW.WW.WWWWWWWW.WW.WWWW.W",
    "W.WWWW.WW.WWWWWWWW.WW.WWWW.W",
    "W......WW....WW....WW......W",
    "WWWWWW.WWWWW WW WWWWW.WWWWWW",
    "WWWWWW.WWWWW WW WWWWW.WWWWWW",
    "WWWWWW.WW          WW.WWWWWW",
    "WWWWWW.WW WWWDWWWW WW.WWWWWW",
    "WWWWWW.WW W      W WW.WWWWWW",
    "      .   W      W   .      ",
    "WWWWWW.WW W      W WW.WWWWWW",
    "WWWWWW.WW WWWWWWWW WW.WWWWWW",
    "WWWWWW.WW          WW.WWWWWW",
    "WWWWWW.WW WWWWWWWW WW.WWWWWW",
    "WWWWWW.WW WWWWWWWW WW.WWWWWW",
    "W............WW............W",
    "W.WWWW.WWWWW.WW.WWWWW.WWWW.W",
    "W.WWWW.WWWWW.WW.WWWWW.WWWW.W",
    "Wo..WW.................WW..oW",
    "WWW.WW.WW.WWWWWWWW.WW.WW.WWW",
    "WWW.WW.WW.WWWWWWWW.WW.WW.WWW",
    "W......WW....WW....WW......W",
    "W.WWWWWWWWWW.WW.WWWWWWWWWW.W",
    "W.WWWWWWWWWW.WW.WWWWWWWWWW.W",
    "W..........................W",
    "WWWWWWWWWWWWWWWWWWWWWWWWWWWW",
};

// Runtime maze (mutable — dots get eaten)
static uint8_t maze[MAZE_H][MAZE_W];
static int dotsRemaining = 0;
static int totalDots     = 0;

// ─────────────────────────────────────────────
//  GAME STATE
// ─────────────────────────────────────────────
struct Entity {
    int   tx, ty;       // Tile position (whole number)
    float px, py;       // Pixel position (sub-tile for smooth movement)
    int   dir;          // Current direction
    int   nextDir;      // Queued direction
    float speed;        // Tiles per tick
};

struct Ghost {
    Entity e;
    int    state;
    int    color;
    int    scatterTx, scatterTy;  // Scatter corner target
    int    frightenedTimer;       // Ticks remaining
    int    houseTimer;            // Ticks before leaving house
    bool   wasFrightened;         // For score doubling
};

static Entity pac;
static Ghost  ghosts[4];   // 0=Blinky, 1=Pinky, 2=Inky, 3=Clyde

static int  score       = 0;
static int  highScore   = 0;
static int  lives       = 3;
static int  stage       = 1;
static int  ghostEatMultiplier = 1;  // Doubles each ghost eaten in one power

// Global mode timer (scatter/chase alternation)
static int  modeTimer   = 0;
static int  modePhase   = 0;  // 0,2,4,6 = scatter; 1,3,5 = chase
// Phase durations in ticks (at ~10 ticks/sec): 7s, 20s, 7s, 20s, 5s, 20s, 5s, infinite
static const int MODE_DURATIONS[] = {70,200,70,200,50,200,50,9999};

// Frightened duration ticks per stage
static const int FRIGHTENED_TICKS[] = {60,50,40,30,20,15,10,5,0,0,0,0,0,0,0,0,0,0,0};

// ─────────────────────────────────────────────
//  HELPER: tile pixel coords
// ─────────────────────────────────────────────
static inline int tileX(int tx) { return MARGIN_X + tx * TS; }
static inline int tileY(int ty) { return MARGIN_Y + ty * TS; }

// ─────────────────────────────────────────────
//  HELPER: direction vectors
// ─────────────────────────────────────────────
static void dirVec(int dir, int& dx, int& dy) {
    dx = dy = 0;
    if      (dir == DIR_LEFT)  dx = -1;
    else if (dir == DIR_RIGHT) dx =  1;
    else if (dir == DIR_UP)    dy = -1;
    else if (dir == DIR_DOWN)  dy =  1;
}

// ─────────────────────────────────────────────
//  HELPER: can move to tile?
// ─────────────────────────────────────────────
static bool canEnter(int tx, int ty, bool isGhost = false) {
    // Wrap tunnel (rows 14)
    if (tx < 0)       tx = MAZE_W - 1;
    if (tx >= MAZE_W) tx = 0;
    if (ty < 0 || ty >= MAZE_H) return false;
    uint8_t t = maze[ty][tx];
    if (t == T_WALL) return false;
    if (t == T_DOOR && !isGhost) return false;
    return true;
}

static int wrapTx(int tx) {
    if (tx < 0)       return MAZE_W - 1;
    if (tx >= MAZE_W) return 0;
    return tx;
}

// ─────────────────────────────────────────────
//  MAZE INIT
// ─────────────────────────────────────────────
static void initMaze() {
    dotsRemaining = 0;
    for (int y = 0; y < MAZE_H; y++) {
        for (int x = 0; x < MAZE_W; x++) {
            char c = MAZE_TEMPLATE[y][x];
            if      (c == 'W') maze[y][x] = T_WALL;
            else if (c == '.') maze[y][x] = T_DOT;
            else if (c == 'o') maze[y][x] = T_ENERGIZER;
            else if (c == 'D') maze[y][x] = T_DOOR;
            else               maze[y][x] = T_EMPTY;
            if (maze[y][x] == T_DOT || maze[y][x] == T_ENERGIZER)
                dotsRemaining++;
        }
    }
    totalDots = dotsRemaining;
}

// ─────────────────────────────────────────────
//  DRAW MAZE
// ─────────────────────────────────────────────
static void drawMazeFull() {
    gfx->fillRect(MARGIN_X, MARGIN_Y, MAZE_W * TS, MAZE_H * TS, COL_BLACK);
    for (int y = 0; y < MAZE_H; y++) {
        for (int x = 0; x < MAZE_W; x++) {
            int px = tileX(x), py = tileY(y);
            switch (maze[y][x]) {
                case T_WALL:
                    gfx->fillRect(px, py, TS, TS, COL_WALL);
                    break;
                case T_DOT:
                    gfx->fillRect(px + 3, py + 3, 2, 2, COL_DOT);
                    break;
                case T_ENERGIZER:
                    gfx->fillCircle(px + 4, py + 4, 3, COL_ENERGIZER);
                    break;
                case T_DOOR:
                    gfx->fillRect(px, py + 3, TS, 2, 0xFBB7); // Pink door
                    break;
                default: break;
            }
        }
    }
}

static void drawTile(int tx, int ty) {
    int px = tileX(tx), py = tileY(ty);
    gfx->fillRect(px, py, TS, TS, COL_BLACK);
    switch (maze[ty][tx]) {
        case T_WALL:
            gfx->fillRect(px, py, TS, TS, COL_WALL);
            break;
        case T_DOT:
            gfx->fillRect(px + 3, py + 3, 2, 2, COL_DOT);
            break;
        case T_ENERGIZER:
            gfx->fillCircle(px + 4, py + 4, 3, COL_ENERGIZER);
            break;
        case T_DOOR:
            gfx->fillRect(px, py + 3, TS, 2, 0xFBB7);
            break;
        default: break;
    }
}

// ─────────────────────────────────────────────
//  SCORE / HUD
// ─────────────────────────────────────────────
static int loadHS() {
    if (!sd.exists(HS_PATH)) return 0;
    FsFile f = sd.open(HS_PATH, O_READ);
    if (!f) return 0;
    char buf[16] = {0};
    f.read(buf, 15);
    f.close();
    return atoi(buf);
}
static void saveHS(int hs) {
    FsFile f = sd.open(HS_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return;
    f.printf("%d", hs);
    f.close();
}

static void drawHUD() {
    gfx->fillRect(0, 0, 320, MARGIN_Y, COL_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_SCORE);
    gfx->setCursor(MARGIN_X, 1);
    gfx->printf("SC:%d", score);
    gfx->setCursor(MARGIN_X + 80, 1);
    gfx->printf("HI:%d", highScore);
    gfx->setCursor(MARGIN_X + 160, 1);
    gfx->printf("STG:%d", stage);
    // Lives as dots
    for (int i = 0; i < lives; i++) {
        gfx->fillCircle(MARGIN_X + 220 + i * 10, 5, 3, COL_PACMAN);
    }
}

// ─────────────────────────────────────────────
//  ENTITY DRAW
// ─────────────────────────────────────────────
static void erasePac() {
    // pac.px/py are tile-space coords; screen coords need MARGIN added
    int sx = (int)pac.px + MARGIN_X;
    int sy = (int)pac.py + MARGIN_Y;
    gfx->fillRect(sx - 1, sy - 1, TS + 2, TS + 2, COL_BLACK);
    // Redraw any maze tiles we covered — use tile-space px/py directly
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int tx = (int)(pac.px / TS) + dx;
            int ty = (int)(pac.py / TS) + dy;
            if (tx >= 0 && tx < MAZE_W && ty >= 0 && ty < MAZE_H)
                drawTile(tx, ty);
        }
    }
}

static void drawPac() {
    int px = (int)pac.px + MARGIN_X;
    int py = (int)pac.py + MARGIN_Y;
    gfx->fillCircle(px + TS/2, py + TS/2, TS/2 - 1, COL_PACMAN);
}

static void eraseGhost(int g) {
    // e.px/py are tile-space; screen coords need MARGIN added
    int sx = (int)ghosts[g].e.px + MARGIN_X;
    int sy = (int)ghosts[g].e.py + MARGIN_Y;
    gfx->fillRect(sx - 1, sy - 1, TS + 2, TS + 2, COL_BLACK);
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int tx = (int)(ghosts[g].e.px / TS) + dx;
            int ty = (int)(ghosts[g].e.py / TS) + dy;
            if (tx >= 0 && tx < MAZE_W && ty >= 0 && ty < MAZE_H)
                drawTile(tx, ty);
        }
    }
}

static void drawGhost(int g) {
    Ghost& gh = ghosts[g];
    int px = (int)gh.e.px + MARGIN_X + TS/2;
    int py = (int)gh.e.py + MARGIN_Y + TS/2;
    uint16_t color;
    if      (gh.state == GS_FRIGHTENED) color = COL_FRIGHTENED;
    else if (gh.state == GS_EATEN)      color = COL_BLACK; // Eyes only
    else                                 color = gh.color;

    if (gh.state != GS_EATEN) {
        // Ghost body: rounded top, skirt bottom
        gfx->fillCircle(px, py - 1, TS/2 - 1, color);
        gfx->fillRect(px - TS/2 + 1, py - 1, TS - 2, TS/2, color);
        // Skirt bumps
        for (int b = 0; b < 3; b++)
            gfx->fillCircle(px - 3 + b * 3, py + TS/2 - 3, 2, color);
    }
    // Eyes (always visible)
    gfx->fillCircle(px - 2, py - 1, 2, COL_SCORE);
    gfx->fillCircle(px + 2, py - 1, 2, COL_SCORE);
    // Pupils (direction)
    int ex = 0, ey = 0;
    dirVec(gh.e.dir, ex, ey);
    gfx->fillCircle(px - 2 + ex, py - 1 + ey, 1, 0x001F);
    gfx->fillCircle(px + 2 + ex, py - 1 + ey, 1, 0x001F);
}

// ─────────────────────────────────────────────
//  ENTITY INIT
// ─────────────────────────────────────────────
static void initEntities() {
    // Pac-Man starts at tile (13,23) facing left
    pac.tx = 13; pac.ty = 23;
    pac.px = pac.tx * TS; pac.py = pac.ty * TS;
    pac.dir = DIR_LEFT; pac.nextDir = DIR_LEFT;
    pac.speed = 0.15f;

    // Ghost starts and scatter corners
    // Blinky starts outside house, others inside
    const int startX[4] = {13, 13, 11, 15};
    const int startY[4] = {11, 14, 14, 14};
    const int scatX[4]  = {25,  2, 27,  0};
    const int scatY[4]  = { 0,  0, 31, 31};
    const uint16_t colors[4] = {COL_BLINKY, COL_PINKY, COL_INKY, COL_CLYDE};
    const int houseTimers[4] = {0, 30, 80, 160};

    for (int i = 0; i < 4; i++) {
        ghosts[i].e.tx = startX[i]; ghosts[i].e.ty = startY[i];
        ghosts[i].e.px = startX[i] * TS;
        ghosts[i].e.py = startY[i] * TS;
        ghosts[i].e.dir  = DIR_LEFT;
        ghosts[i].e.nextDir = DIR_LEFT;
        ghosts[i].e.speed = 0.12f;
        ghosts[i].state   = (i == 0) ? GS_SCATTER : GS_HOUSE;
        ghosts[i].color   = colors[i];
        ghosts[i].scatterTx  = scatX[i];
        ghosts[i].scatterTy  = scatY[i];
        ghosts[i].frightenedTimer = 0;
        ghosts[i].houseTimer      = houseTimers[i];
        ghosts[i].wasFrightened   = false;
    }

    modeTimer = 0;
    modePhase = 0;
    ghostEatMultiplier = 1;
}

// ─────────────────────────────────────────────
//  GHOST AI — TARGET TILE CALCULATION
// ─────────────────────────────────────────────
static void getGhostTarget(int g, int& targetX, int& targetY) {
    Ghost& gh = ghosts[g];

    if (gh.state == GS_SCATTER) {
        targetX = gh.scatterTx;
        targetY = gh.scatterTy;
        return;
    }

    // Chase targets
    int pdx, pdy;
    dirVec(pac.dir, pdx, pdy);

    if (g == 0) {
        // Blinky: target Pac directly
        targetX = pac.tx;
        targetY = pac.ty;
    } else if (g == 1) {
        // Pinky: 4 tiles ahead of Pac
        targetX = pac.tx + pdx * 4;
        targetY = pac.ty + pdy * 4;
    } else if (g == 2) {
        // Inky: vector from Blinky through 2 tiles ahead of Pac
        int pivotX = pac.tx + pdx * 2;
        int pivotY = pac.ty + pdy * 2;
        targetX = pivotX + (pivotX - ghosts[0].e.tx);
        targetY = pivotY + (pivotY - ghosts[0].e.ty);
    } else {
        // Clyde: chase if >8 tiles away, else scatter
        int dx = ghosts[3].e.tx - pac.tx;
        int dy = ghosts[3].e.ty - pac.ty;
        if ((dx*dx + dy*dy) > 64) {
            targetX = pac.tx;
            targetY = pac.ty;
        } else {
            targetX = gh.scatterTx;
            targetY = gh.scatterTy;
        }
    }
}

// ─────────────────────────────────────────────
//  GHOST MOVEMENT
// ─────────────────────────────────────────────
static int opposite(int dir) {
    if (dir == DIR_LEFT)  return DIR_RIGHT;
    if (dir == DIR_RIGHT) return DIR_LEFT;
    if (dir == DIR_UP)    return DIR_DOWN;
    if (dir == DIR_DOWN)  return DIR_UP;
    return DIR_NONE;
}

static int dist2(int ax, int ay, int bx, int by) {
    return (ax-bx)*(ax-bx) + (ay-by)*(ay-by);
}

static void chooseGhostDir(int g) {
    Ghost& gh = ghosts[g];
    int tx = gh.e.tx, ty = gh.e.ty;
    int opp = opposite(gh.e.dir);

    int targetX, targetY;
    if (gh.state == GS_FRIGHTENED) {
        // Random direction
        int dirs[4] = {DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN};
        for (int i = 3; i > 0; i--) {
            int j = random(i + 1);
            int t = dirs[i]; dirs[i] = dirs[j]; dirs[j] = t;
        }
        for (int i = 0; i < 4; i++) {
            if (dirs[i] == opp) continue;
            int dx, dy;
            dirVec(dirs[i], dx, dy);
            int nx = wrapTx(tx + dx), ny = ty + dy;
            if (canEnter(nx, ny, true)) {
                gh.e.nextDir = dirs[i];
                return;
            }
        }
    } else {
        getGhostTarget(g, targetX, targetY);
        // Pick direction (not reverse) that minimizes distance to target
        int bestDir = DIR_NONE, bestDist = 999999;
        int tryDirs[4] = {DIR_UP, DIR_LEFT, DIR_DOWN, DIR_RIGHT}; // Priority order
        for (int i = 0; i < 4; i++) {
            int d = tryDirs[i];
            if (d == opp) continue;
            int ddx, ddy;
            dirVec(d, ddx, ddy);
            int nx = wrapTx(tx + ddx), ny = ty + ddy;
            if (!canEnter(nx, ny, true)) continue;
            int dist = dist2(nx, ny, targetX, targetY);
            if (dist < bestDist) { bestDist = dist; bestDir = d; }
        }
        if (bestDir != DIR_NONE) gh.e.nextDir = bestDir;
    }
}

static void moveGhost(int g) {
    Ghost& gh = ghosts[g];

    if (gh.state == GS_HOUSE) {
        gh.houseTimer--;
        if (gh.houseTimer <= 0) {
            gh.state = GS_SCATTER;
            gh.e.tx = 13; gh.e.ty = 11; // Exit position
            gh.e.px = gh.e.tx * TS;
            gh.e.py = gh.e.ty * TS;
        }
        return;
    }

    if (gh.state == GS_FRIGHTENED) {
        gh.frightenedTimer--;
        if (gh.frightenedTimer <= 0) {
            gh.state = (modePhase % 2 == 0) ? GS_SCATTER : GS_CHASE;
        }
        gh.e.speed = 0.07f;
    } else if (gh.state == GS_EATEN) {
        gh.e.speed = 0.25f;
        // Bee-line to ghost house
        if (gh.e.tx == 13 && gh.e.ty == 14) {
            gh.state = GS_SCATTER;
            gh.e.speed = 0.12f;
        }
    } else {
        gh.e.speed = 0.12f + (stage - 1) * 0.005f;
    }

    // At tile center: choose new direction
    float cx = gh.e.tx * TS, cy = gh.e.ty * TS;
    float distToCenter = abs(gh.e.px - cx) + abs(gh.e.py - cy);
    if (distToCenter < gh.e.speed * 1.5f) {
        gh.e.px = cx; gh.e.py = cy;
        chooseGhostDir(g);
        gh.e.dir = gh.e.nextDir;
    }

    // Move
    int dx, dy;
    dirVec(gh.e.dir, dx, dy);
    int ntx = wrapTx(gh.e.tx + dx);
    int nty = gh.e.ty + dy;
    if (canEnter(ntx, nty, true)) {
        gh.e.px += dx * gh.e.speed * TS;
        gh.e.py += dy * gh.e.speed * TS;
        // Update tile position
        gh.e.tx = (int)(gh.e.px / TS + 0.5f);
        gh.e.ty = (int)(gh.e.py / TS + 0.5f);
        gh.e.tx = wrapTx(gh.e.tx);
    }
}

// ─────────────────────────────────────────────
//  PAC-MAN MOVEMENT
// ─────────────────────────────────────────────
static void movePac() {
    float cx = pac.tx * TS, cy = pac.ty * TS;
    float distToCenter = abs(pac.px - cx) + abs(pac.py - cy);

    if (distToCenter < pac.speed * 1.5f) {
        pac.px = cx; pac.py = cy;

        // Try to turn if next direction is queued
        int ndx, ndy;
        dirVec(pac.nextDir, ndx, ndy);
        int ntx = wrapTx(pac.tx + ndx), nty = pac.ty + ndy;
        if (canEnter(ntx, nty)) {
            pac.dir = pac.nextDir;
        }
        // If current dir blocked, stop
        int cdx, cdy;
        dirVec(pac.dir, cdx, cdy);
        ntx = wrapTx(pac.tx + cdx); nty = pac.ty + cdy;
        if (!canEnter(ntx, nty)) return;
    }

    int dx, dy;
    dirVec(pac.dir, dx, dy);
    int ntx = wrapTx(pac.tx + dx), nty = pac.ty + dy;
    if (!canEnter(ntx, nty)) return;

    pac.px += dx * pac.speed * TS;
    pac.py += dy * pac.speed * TS;
    pac.tx = (int)(pac.px / TS + 0.5f);
    pac.ty = (int)(pac.py / TS + 0.5f);
    pac.tx = wrapTx(pac.tx);

    // Eat dot or energizer
    uint8_t& tile = maze[pac.ty][pac.tx];
    if (tile == T_DOT) {
        tile = T_EMPTY;
        score += 10;
        dotsRemaining--;
        drawTile(pac.tx, pac.ty);
    } else if (tile == T_ENERGIZER) {
        tile = T_EMPTY;
        score += 50;
        dotsRemaining--;
        drawTile(pac.tx, pac.ty);
        // Frighten all active ghosts
        int fTicks = FRIGHTENED_TICKS[min(stage - 1, 18)];
        ghostEatMultiplier = 1;
        for (int g = 0; g < 4; g++) {
            if (ghosts[g].state != GS_HOUSE && ghosts[g].state != GS_EATEN) {
                ghosts[g].state = GS_FRIGHTENED;
                ghosts[g].frightenedTimer = fTicks;
                ghosts[g].e.dir = opposite(ghosts[g].e.dir); // Reverse
            }
        }
    }
}

// ─────────────────────────────────────────────
//  COLLISION DETECTION
// ─────────────────────────────────────────────
static bool checkGhostCollision(int g) {
    int dx = abs((int)pac.px - (int)ghosts[g].e.px);
    int dy = abs((int)pac.py - (int)ghosts[g].e.py);
    return (dx < TS - 2 && dy < TS - 2);
}

// ─────────────────────────────────────────────
//  DEATH ANIMATION
// ─────────────────────────────────────────────
static void deathAnimation() {
    int cx = (int)pac.px + MARGIN_X + TS/2;
    int cy = (int)pac.py + MARGIN_Y + TS/2;
    for (int r = TS/2; r >= 0; r--) {
        gfx->fillCircle(cx, cy, r, COL_PACMAN);
        delay(60);
        gfx->fillCircle(cx, cy, r + 1, COL_BLACK);
    }
    delay(500);
}

// ─────────────────────────────────────────────
//  STAGE COMPLETE FLASH
// ─────────────────────────────────────────────
static void stageCompleteFlash() {
    for (int i = 0; i < 4; i++) {
        // Flash maze blue/white
        for (int y = 0; y < MAZE_H; y++)
            for (int x = 0; x < MAZE_W; x++)
                if (maze[y][x] == T_WALL)
                    gfx->fillRect(tileX(x), tileY(y), TS, TS,
                                  (i % 2 == 0) ? 0xFFFF : COL_WALL);
        delay(200);
    }
}

// ─────────────────────────────────────────────
//  GAME OVER SCREEN
// ─────────────────────────────────────────────
static void showGameOver() {
    gfx->fillRect(MARGIN_X + 20, MARGIN_Y + 80, 190, 80, C_DARK);
    gfx->drawRect(MARGIN_X + 20, MARGIN_Y + 80, 190, 80, COL_PACMAN);
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(MARGIN_X + 45, MARGIN_Y + 95);
    gfx->print("GAME OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_SCORE);
    gfx->setCursor(MARGIN_X + 50, MARGIN_Y + 122);
    gfx->printf("SCORE: %d", score);
    if (score >= highScore && score > 0) {
        gfx->setTextColor(COL_PACMAN);
        gfx->setCursor(MARGIN_X + 35, MARGIN_Y + 138);
        gfx->print("** NEW HIGH SCORE! **");
    }
    delay(3000);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_pacman() {
    highScore = loadHS();
    score     = 0;
    lives     = 3;
    stage     = 1;

    while (lives > 0) {
        // Stage setup
        initMaze();
        initEntities();
        gfx->fillScreen(COL_BLACK);
        drawMazeFull();
        drawHUD();
        drawPac();
        for (int g = 0; g < 4; g++) drawGhost(g);

        // Ready screen
        gfx->setTextSize(1);
        gfx->setTextColor(COL_PACMAN);
        gfx->setCursor(MARGIN_X + 70, MARGIN_Y + 140);
        gfx->print("READY!");
        delay(2000);
        gfx->fillRect(MARGIN_X + 70, MARGIN_Y + 138, 50, 10, COL_BLACK);

        bool stageDone = false;
        bool died      = false;

        while (!stageDone && !died) {
            // Input
            // We poll input every loop iteration but only act on it.
            // nextDir persists until Pac-Man actually turns — don't
            // overwrite it with DIR_NONE if no key is pressed this frame.
            char k = get_keypress();
            if (k == 'q' || k == 'Q') {
                lives = 0; stageDone = true; break;
            }
            if (k == 'a' || k == 'A') pac.nextDir = DIR_LEFT;
            if (k == 'd' || k == 'D') pac.nextDir = DIR_RIGHT;
            if (k == 'w' || k == 'W') pac.nextDir = DIR_UP;
            if (k == 's' || k == 'S') pac.nextDir = DIR_DOWN;

            // Arrow keys (ASCII sequences) — some T-Deck keyboard firmwares
            // emit these for the directional cluster
            if (k == 0x1B) {
                // ESC prefix — peek for bracket
                char k2 = get_keypress();
                if (k2 == '[') {
                    char k3 = get_keypress();
                    if (k3 == 'A') pac.nextDir = DIR_UP;
                    if (k3 == 'B') pac.nextDir = DIR_DOWN;
                    if (k3 == 'C') pac.nextDir = DIR_RIGHT;
                    if (k3 == 'D') pac.nextDir = DIR_LEFT;
                }
            }

            TrackballState tb = update_trackball_game();
            if (tb.x == -1) pac.nextDir = DIR_LEFT;
            if (tb.x ==  1) pac.nextDir = DIR_RIGHT;
            // Y axis is physically inverted on T-Deck Plus trackball —
            // rolling "up" fires the DOWN pin and vice versa.
            // tb.y == 1 means TRK_DOWN fired, which is physical upward roll.
            if (tb.y ==  1) pac.nextDir = DIR_UP;
            if (tb.y == -1) pac.nextDir = DIR_DOWN;

            // Note: trackball.cpp has a 250ms lockout between events to
            // prevent multiple fires per roll in UI contexts. For games this
            // means we only pick up a new direction every ~250ms from the
            // trackball — which is fine since nextDir persists until the
            // next valid junction anyway. WASD has no such lockout and
            // responds every frame, making it the most responsive input.

            int16_t tx, ty;
            if (get_touch(&tx, &ty) && ty < MARGIN_Y) {
                while (get_touch(&tx, &ty)) { delay(10); }
                lives = 0; stageDone = true; break;
            }

            // Erase
            erasePac();
            for (int g = 0; g < 4; g++) eraseGhost(g);

            // Update global mode (scatter/chase timer)
            modeTimer++;
            if (modeTimer >= MODE_DURATIONS[modePhase]) {
                modeTimer = 0;
                modePhase = min(modePhase + 1, 7);
                // Switch non-frightened ghosts to new mode
                int newState = (modePhase % 2 == 0) ? GS_SCATTER : GS_CHASE;
                for (int g = 0; g < 4; g++) {
                    if (ghosts[g].state != GS_FRIGHTENED &&
                        ghosts[g].state != GS_EATEN &&
                        ghosts[g].state != GS_HOUSE) {
                        ghosts[g].state = newState;
                        ghosts[g].e.dir = opposite(ghosts[g].e.dir);
                    }
                }
            }

            // Move
            movePac();
            for (int g = 0; g < 4; g++) moveGhost(g);

            // Collision
            for (int g = 0; g < 4; g++) {
                if (ghosts[g].state == GS_EATEN) continue;
                if (ghosts[g].state == GS_HOUSE) continue;
                if (checkGhostCollision(g)) {
                    if (ghosts[g].state == GS_FRIGHTENED) {
                        // Eat ghost
                        int pts = 200 * ghostEatMultiplier;
                        ghostEatMultiplier *= 2;
                        score += pts;
                        ghosts[g].state = GS_EATEN;
                        // Flash score
                        gfx->setCursor(tileX(ghosts[g].e.tx), tileY(ghosts[g].e.ty));
                        gfx->setTextColor(COL_SCORE);
                        gfx->setTextSize(1);
                        gfx->printf("%d", pts);
                        delay(300);
                    } else {
                        // Pac dies
                        died = true;
                        deathAnimation();
                        lives--;
                        break;
                    }
                }
            }

            // Draw
            drawPac();
            for (int g = 0; g < 4; g++) drawGhost(g);
            drawHUD();

            // Check stage complete
            if (dotsRemaining <= 0) {
                stageCompleteFlash();
                stage++;
                // Speed up pac and ghosts per stage
                pac.speed = min(0.20f, 0.15f + (stage - 1) * 0.003f);
                stageDone = true;
            }

            delay(33); // ~30fps cap
            yield();
        }
    }

    // Game over
    if (score > highScore) {
        highScore = score;
        saveHS(highScore);
    }
    showGameOver();
}