/**
 * PISCES MOON OS — PAC-MAN
 * Faithful arcade clone for LilyGO T-Deck Plus (320x240)
 *
 * Layout: scaled per target so the full 28x31 maze fits on screen.
 * T-Deck uses 7px tiles, Pager uses 6px tiles, Cardputer uses 4px.
 *
 * Ghost AI (faithful to original):
 * BLINKY  (red)   — targets Pac-Man directly
 * PINKY   (pink)  — targets 4 tiles ahead of Pac-Man's direction
 * INKY    (cyan)  — targets using Blinky's position as mirror point
 * CLYDE   (orange)— targets directly if >8 tiles away, else scatter
 *
 * Controls: Trackball direction = next turn intent.
 * NES keys: A-left, W-up, D-right, Z-down, B-Start/pause, Q-quit.
 */

#include <Arduino.h>
#include <math.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "trackball.h"
#include "game_input.h"
#include "theme.h"
#include "pacman.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif
extern SdFat sd;
extern SemaphoreHandle_t spi_mutex;

// ─────────────────────────────────────────────
//  CONSTANTS
// ─────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
#define SCREEN_W    240
#define SCREEN_H    135
#define TS          4
#define HUD_H       8
#define MAZE_TOP    9
#elif defined(DEVICE_TLORAPAGER)
#define SCREEN_W    320
#define SCREEN_H    222
#define TS          6
#define HUD_H       12
#define MAZE_TOP    20
#else
#define SCREEN_W    320
#define SCREEN_H    240
#define TS          7
#define HUD_H       14
#define MAZE_TOP    18
#endif
#define MAZE_W      28
#define MAZE_H      31
#define MARGIN_X    ((SCREEN_W - MAZE_W * TS) / 2)
#define MARGIN_Y    MAZE_TOP
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
#define COL_WALL_HI     0x05FF   // Electric blue edge
#define COL_WALL_DIM    0x0018   // Deep blue body
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
    "Wo..WW................WW..oW",
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
    float px, py;       // Maze-local pixel position, snapped at tile centers
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

static int viewX() {
    int w = gfx->width();
    return (w > SCREEN_W) ? (w - SCREEN_W) / 2 : 0;
}

static int viewY() {
    int h = gfx->height();
    return (h > SCREEN_H) ? (h - SCREEN_H) / 2 : 0;
}

// ─────────────────────────────────────────────
//  HELPER: tile pixel coords
// ─────────────────────────────────────────────
static inline int tileX(int tx) { return viewX() + MARGIN_X + tx * TS; }
static inline int tileY(int ty) { return viewY() + MARGIN_Y + ty * TS; }

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

static bool isTunnelRow(int ty) {
    return ty == 14;
}

static int wrapTx(int tx, int ty) {
    if (!isTunnelRow(ty)) return tx;
    if (tx < 0)       return MAZE_W - 1;
    if (tx >= MAZE_W) return 0;
    return tx;
}

// ─────────────────────────────────────────────
//  HELPER: can move to tile?
// ─────────────────────────────────────────────
static bool canEnter(int tx, int ty, bool isGhost = false) {
    if (ty < 0 || ty >= MAZE_H) return false;
    tx = wrapTx(tx, ty);
    if (tx < 0 || tx >= MAZE_W) return false;
    uint8_t t = maze[ty][tx];
    if (t == T_WALL) return false;
    if (t == T_DOOR && !isGhost) return false;
    return true;
}

static void setEntityTile(Entity &e, int tx, int ty) {
    e.ty = ty;
    e.tx = wrapTx(tx, ty);
    e.px = e.tx * TS;
    e.py = e.ty * TS;
}

static bool isEntityCentered(const Entity &e) {
    return fabsf(e.px - e.tx * TS) < 0.01f &&
           fabsf(e.py - e.ty * TS) < 0.01f;
}

static bool nextTileFor(const Entity &e, int dir, bool isGhost,
                        int &ntx, int &nty, float &targetPx, float &targetPy) {
    int dx, dy;
    dirVec(dir, dx, dy);
    if (dx == 0 && dy == 0) return false;

    int rawTx = e.tx + dx;
    nty = e.ty + dy;
    if (!canEnter(rawTx, nty, isGhost)) return false;

    ntx = wrapTx(rawTx, nty);
    if (dx < 0 && isTunnelRow(e.ty) && rawTx < 0) {
        targetPx = -TS;
    } else if (dx > 0 && isTunnelRow(e.ty) && rawTx >= MAZE_W) {
        targetPx = MAZE_W * TS;
    } else {
        targetPx = ntx * TS;
    }
    targetPy = nty * TS;
    return true;
}

static bool canMoveDir(const Entity &e, int dir, bool isGhost = false) {
    int ntx, nty;
    float targetPx, targetPy;
    return nextTileFor(e, dir, isGhost, ntx, nty, targetPx, targetPy);
}

static bool advanceEntity(Entity &e, bool isGhost = false) {
    int dx, dy;
    dirVec(e.dir, dx, dy);
    if (dx == 0 && dy == 0) return false;

    int ntx, nty;
    float targetPx, targetPy;
    if (!nextTileFor(e, e.dir, isGhost, ntx, nty, targetPx, targetPy)) return false;

    float step = max(0.1f, e.speed * TS);
    bool reached = false;

    if (dx < 0) {
        e.px -= step;
        if (e.px <= targetPx) { e.px = targetPx; reached = true; }
    } else if (dx > 0) {
        e.px += step;
        if (e.px >= targetPx) { e.px = targetPx; reached = true; }
    } else if (dy < 0) {
        e.py -= step;
        if (e.py <= targetPy) { e.py = targetPy; reached = true; }
    } else if (dy > 0) {
        e.py += step;
        if (e.py >= targetPy) { e.py = targetPy; reached = true; }
    }

    if (reached) {
        setEntityTile(e, ntx, nty);
    }
    return reached;
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
static bool isWallAt(int tx, int ty) {
    if (tx < 0 || tx >= MAZE_W || ty < 0 || ty >= MAZE_H) return false;
    return maze[ty][tx] == T_WALL;
}

static void drawWallTile(int tx, int ty) {
    int px = tileX(tx), py = tileY(ty);
    gfx->fillRect(px, py, TS, TS, COL_BLACK);

    bool l = isWallAt(tx - 1, ty);
    bool r = isWallAt(tx + 1, ty);
    bool u = isWallAt(tx, ty - 1);
    bool d = isWallAt(tx, ty + 1);

    int cx = px + TS / 2;
    int cy = py + TS / 2;
    int thick = (TS >= 7) ? 3 : 2;
    int half = thick / 2;

    if (!l && !r && !u && !d) {
        gfx->fillRect(px + 1, py + 1, max(1, TS - 2), max(1, TS - 2), COL_WALL_DIM);
        gfx->drawRect(px + 1, py + 1, max(1, TS - 2), max(1, TS - 2), COL_WALL_HI);
        return;
    }

    if (l) gfx->fillRect(px, cy - half, TS / 2 + 1, thick, COL_WALL_HI);
    if (r) gfx->fillRect(cx, cy - half, TS - TS / 2, thick, COL_WALL_HI);
    if (u) gfx->fillRect(cx - half, py, thick, TS / 2 + 1, COL_WALL_HI);
    if (d) gfx->fillRect(cx - half, cy, thick, TS - TS / 2, COL_WALL_HI);
    gfx->fillCircle(cx, cy, half + 1, COL_WALL_HI);
    if (TS >= 6) gfx->drawPixel(cx, cy, 0xFFFF);
}

static void drawMazeFull() {
    gfx->fillRect(tileX(0), tileY(0), MAZE_W * TS, MAZE_H * TS, COL_BLACK);
    for (int y = 0; y < MAZE_H; y++) {
        for (int x = 0; x < MAZE_W; x++) {
            int px = tileX(x), py = tileY(y);
            switch (maze[y][x]) {
                case T_WALL:
                    drawWallTile(x, y);
                    break;
                case T_DOT:
                    gfx->fillCircle(px + TS / 2, py + TS / 2, (TS >= 7) ? 1 : 0, COL_DOT);
                    break;
                case T_ENERGIZER:
                    gfx->fillCircle(px + TS / 2, py + TS / 2, max(2, TS / 3), COL_ENERGIZER);
                    break;
                case T_DOOR:
                    gfx->fillRect(px, py + TS / 2, TS, 2, 0xFBB7); // Pink door
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
            drawWallTile(tx, ty);
            break;
        case T_DOT:
            gfx->fillCircle(px + TS / 2, py + TS / 2, (TS >= 7) ? 1 : 0, COL_DOT);
            break;
        case T_ENERGIZER:
            gfx->fillCircle(px + TS / 2, py + TS / 2, max(2, TS / 3), COL_ENERGIZER);
            break;
        case T_DOOR:
            gfx->fillRect(px, py + TS / 2, TS, 2, 0xFBB7);
            break;
        default: break;
    }
}

// ─────────────────────────────────────────────
//  SCORE / HUD
//  SPI Bus Treaty: HS load/save wrapped in spi_mutex
//  to prevent collision with Ghost Engine on Core 0.
// ─────────────────────────────────────────────
static int loadHS() {
    int score = 0;
    if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (sd.exists(HS_PATH)) {
            FsFile f = sd.open(HS_PATH, O_READ);
            if (f) {
                char buf[16] = {0};
                f.read(buf, 15);
                f.close();
                score = atoi(buf);
            }
        }
        xSemaphoreGiveRecursive(spi_mutex);
    }
    return score;
}
static void saveHS(int hs) {
    if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        FsFile f = sd.open(HS_PATH, O_WRITE | O_CREAT | O_TRUNC);
        if (f) {
            f.printf("%d", hs);
            f.close();
        }
        xSemaphoreGiveRecursive(spi_mutex);
    }
}

static void drawHUD() {
    int vx = viewX();
    int vy = viewY();
    gfx->fillRect(vx, vy, SCREEN_W, HUD_H, COL_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_SCORE);
#ifdef DEVICE_CARDPUTER_ADV
    gfx->setCursor(vx + 2, vy);
    gfx->printf("S:%d", score);
    gfx->setCursor(vx + 58, vy);
    gfx->printf("H:%d", highScore);
    gfx->setCursor(vx + 112, vy);
    gfx->printf("L:%d", lives);
#else
    gfx->setCursor(vx + 6, vy + 2);
    gfx->printf("SC:%d", score);
    gfx->setCursor(vx + 88, vy + 2);
    gfx->printf("HI:%d", highScore);
    gfx->setCursor(vx + 170, vy + 2);
    gfx->printf("STG:%d", stage);
    // Lives as dots
    for (int i = 0; i < lives; i++) {
        gfx->fillCircle(vx + 250 + i * 10, vy + 6, 3, COL_PACMAN);
    }
#endif
}

// ─────────────────────────────────────────────
//  ENTITY DRAW
// ─────────────────────────────────────────────
static void erasePac() {
    // pac.px/py are tile-space coords; screen coords need MARGIN added
    int sx = (int)pac.px + viewX() + MARGIN_X;
    int sy = (int)pac.py + viewY() + MARGIN_Y;
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
    int px = (int)pac.px + viewX() + MARGIN_X;
    int py = (int)pac.py + viewY() + MARGIN_Y;
    int cx = px + TS / 2;
    int cy = py + TS / 2;
    int r = max(2, TS / 2 - 1);
    gfx->fillCircle(cx, cy, r, COL_PACMAN);
    if (TS >= 5) {
        if (pac.dir == DIR_RIGHT) {
            gfx->fillTriangle(cx, cy, cx + r + 1, cy - r, cx + r + 1, cy + r, COL_BLACK);
        } else if (pac.dir == DIR_LEFT) {
            gfx->fillTriangle(cx, cy, cx - r - 1, cy - r, cx - r - 1, cy + r, COL_BLACK);
        } else if (pac.dir == DIR_UP) {
            gfx->fillTriangle(cx, cy, cx - r, cy - r - 1, cx + r, cy - r - 1, COL_BLACK);
        } else if (pac.dir == DIR_DOWN) {
            gfx->fillTriangle(cx, cy, cx - r, cy + r + 1, cx + r, cy + r + 1, COL_BLACK);
        }
    }
}

static void eraseGhost(int g) {
    // e.px/py are tile-space; screen coords need MARGIN added
    int sx = (int)ghosts[g].e.px + viewX() + MARGIN_X;
    int sy = (int)ghosts[g].e.py + viewY() + MARGIN_Y;
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
    int px = (int)gh.e.px + viewX() + MARGIN_X + TS/2;
    int py = (int)gh.e.py + viewY() + MARGIN_Y + TS/2;
    uint16_t color;
    if      (gh.state == GS_FRIGHTENED) color = COL_FRIGHTENED;
    else if (gh.state == GS_EATEN)      color = COL_BLACK; // Eyes only
    else                                 color = gh.color;

    if (gh.state != GS_EATEN) {
        // Ghost body: rounded top, skirt bottom
        int r = max(2, TS / 2 - 1);
        gfx->fillCircle(px, py - 1, r, color);
        gfx->fillRect(px - r, py - 1, r * 2 + 1, max(2, TS / 2), color);
        // Skirt bumps
        if (TS >= 5) {
            for (int b = 0; b < 3; b++)
                gfx->fillCircle(px - r + b * r, py + TS/2 - 2, 1, color);
        }
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

static bool waitForPacmanResume() {
    const int boxW = 132;
    const int boxH = 44;
    const int boxX = viewX() + (SCREEN_W - boxW) / 2;
    const int boxY = viewY() + (SCREEN_H - boxH) / 2;
    gfx->fillRect(boxX, boxY, boxW, boxH, COL_BLACK);
    gfx->drawRect(boxX, boxY, boxW, boxH, COL_SCORE);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_PACMAN);
    gfx->setCursor(boxX + 46, boxY + 10);
    gfx->print("PAUSED");
    gfx->setTextColor(COL_SCORE);
    gfx->setCursor(boxX + 10, boxY + 28);
    gfx->print("B START / Q QUIT");

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return false;
        if (input.start) {
            drawMazeFull();
            drawHUD();
            drawPac();
            for (int g = 0; g < 4; g++) drawGhost(g);
            return true;
        }
        delay(30);
        yield();
    }
}

// ─────────────────────────────────────────────
//  ENTITY INIT
// ─────────────────────────────────────────────
static void initEntities() {
    // Pac-Man starts at tile (13,23) facing left
    setEntityTile(pac, 13, 23);
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
        setEntityTile(ghosts[i].e, startX[i], startY[i]);
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
    if (gh.state == GS_EATEN) {
        targetX = 13;
        targetY = 14;
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
            int ny = ty + dy;
            int nx = wrapTx(tx + dx, ny);
            if (canEnter(nx, ny, true)) {
                gh.e.nextDir = dirs[i];
                return;
            }
        }
        for (int i = 0; i < 4; i++) {
            int dx, dy;
            dirVec(dirs[i], dx, dy);
            int ny = ty + dy;
            int nx = wrapTx(tx + dx, ny);
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
            int ny = ty + ddy;
            int nx = wrapTx(tx + ddx, ny);
            if (!canEnter(nx, ny, true)) continue;
            int dist = dist2(nx, ny, targetX, targetY);
            if (dist < bestDist) { bestDist = dist; bestDir = d; }
        }
        if (bestDir == DIR_NONE) {
            for (int i = 0; i < 4; i++) {
                int d = tryDirs[i];
                int ddx, ddy;
                dirVec(d, ddx, ddy);
                int ny = ty + ddy;
                int nx = wrapTx(tx + ddx, ny);
                if (!canEnter(nx, ny, true)) continue;
                int dist = dist2(nx, ny, targetX, targetY);
                if (dist < bestDist) { bestDist = dist; bestDir = d; }
            }
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
            setEntityTile(gh.e, 13, 11); // Exit position
            gh.e.dir = DIR_LEFT;
            gh.e.nextDir = DIR_LEFT;
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

    if (isEntityCentered(gh.e)) {
        chooseGhostDir(g);
        if (canMoveDir(gh.e, gh.e.nextDir, true)) {
            gh.e.dir = gh.e.nextDir;
        }
        if (!canMoveDir(gh.e, gh.e.dir, true)) {
            gh.e.dir = DIR_NONE;
            return;
        }
    }

    bool reached = advanceEntity(gh.e, true);
    if (reached && gh.state == GS_EATEN && gh.e.tx == 13 && gh.e.ty == 14) {
        gh.state = GS_SCATTER;
        gh.e.speed = 0.12f;
        gh.e.dir = DIR_LEFT;
        gh.e.nextDir = DIR_LEFT;
    }
}

// ─────────────────────────────────────────────
//  PAC-MAN MOVEMENT
// ─────────────────────────────────────────────
static void movePac() {
    if (isEntityCentered(pac)) {
        if (canMoveDir(pac, pac.nextDir)) {
            pac.dir = pac.nextDir;
        }
        if (!canMoveDir(pac, pac.dir)) {
            pac.dir = DIR_NONE;
            return;
        }
    }

    bool reached = advanceEntity(pac);
    if (!reached && !isEntityCentered(pac)) return;

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
    int cx = (int)pac.px + viewX() + MARGIN_X + TS/2;
    int cy = (int)pac.py + viewY() + MARGIN_Y + TS/2;
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
    int boxW = min(190, SCREEN_W - 20);
    int boxH = 80;
    int boxX = viewX() + (SCREEN_W - boxW) / 2;
    int boxY = viewY() + (SCREEN_H - boxH) / 2;
    gfx->fillRect(boxX, boxY, boxW, boxH, C_DARK);
    gfx->drawRect(boxX, boxY, boxW, boxH, COL_PACMAN);
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(boxX + (boxW - 108) / 2, boxY + 15);
    gfx->print("GAME OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_SCORE);
    gfx->setCursor(boxX + 34, boxY + 42);
    gfx->printf("SCORE: %d", score);
    if (score >= highScore && score > 0) {
        gfx->setTextColor(COL_PACMAN);
        gfx->setCursor(boxX + 22, boxY + 58);
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
        gfx->setCursor(viewX() + (SCREEN_W - 42) / 2, viewY() + SCREEN_H / 2);
        gfx->print("READY!");
        delay(2000);
        gfx->fillRect(viewX() + (SCREEN_W - 54) / 2, viewY() + SCREEN_H / 2 - 2, 60, 12, COL_BLACK);

        bool stageDone = false;
        bool died      = false;

        while (!stageDone && !died) {
            // Input
            PMNesInput input = pm_read_nes_input(true);
            if (input.quit) {
                lives = 0; stageDone = true; break;
            }
            if (input.start) {
                if (!waitForPacmanResume()) {
                    lives = 0; stageDone = true; break;
                }
                continue;
            }
            if (input.left)  pac.nextDir = DIR_LEFT;
            if (input.right) pac.nextDir = DIR_RIGHT;
            if (input.up)    pac.nextDir = DIR_UP;
            if (input.down)  pac.nextDir = DIR_DOWN;

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
