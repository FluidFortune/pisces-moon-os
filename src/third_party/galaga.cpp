/**
 * PISCES MOON OS — GALAGA
 * Arcade-style space shooter clone for T-Deck Plus (320x240)
 *
 * Features:
 *   - 40-enemy formation (4 rows × 10 cols) with entry choreography
 *   - Three enemy types: Bee (basic), Butterfly (mid), Galaga Boss (top)
 *   - Boss tractor beam capture mechanic (dual ship on rescue)
 *   - Enemy dive attacks with curved paths
 *   - Bonus "Challenging Stage" every 3 stages (no shooting, pure formation)
 *   - Multiple stages with increasing difficulty
 *   - High score saved to /galaga_hs.txt
 *
 * Controls:
 *   Trackball LEFT/RIGHT = move ship
 *   Trackball CLICK      = fire
 *   WASD A/D             = move ship
 *   SPACE                = fire
 *   Q / header tap       = quit
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SdFat.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "gamepad.h"
#include "galaga.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  CONSTANTS
// ─────────────────────────────────────────────
#define SCREEN_W        320
#define SCREEN_H        240
#define PLAY_W          280   // Play area (left margin 20, right margin 20)
#define PLAY_X          20
#define HUD_H           18
#define PLAY_BOTTOM     (SCREEN_H - 20)

#define SHIP_W          12
#define SHIP_H          10
#define SHIP_Y          (SCREEN_H - 30)
#define SHIP_SPEED      3
#define BULLET_SPEED    6
#define MAX_BULLETS     3
#define MAX_ENEMIES     40
#define MAX_ENEMY_BULLETS 4
#define MAX_DIVE        4     // Max enemies diving at once

#define HS_PATH         "/galaga_hs.txt"

// Enemy types
#define ET_BEE          0
#define ET_BUTTERFLY    1
#define ET_BOSS         2

// Enemy states
#define ES_FORMING      0   // Still entering formation
#define ES_FORMATION    1   // In formation, oscillating
#define ES_DIVING       2   // On dive attack
#define ES_CAPTURED     3   // Captured (tractor beam)
#define ES_DEAD         0xFF

// Colors
#define COL_BG          0x0000
#define COL_SHIP        0x07FF   // Cyan
#define COL_BULLET      0xFFE0   // Yellow
#define COL_BEE         0xFFE0   // Yellow
#define COL_BUTTERFLY   0x07E0   // Green
#define COL_BOSS        0xF81F   // Magenta
#define COL_BEAM        0x001F   // Blue (tractor beam)
#define COL_EXPLOSION   0xF800   // Red
#define COL_STAR        0x8410   // Grey
#define COL_HUD         0xFFFF

// ─────────────────────────────────────────────
//  STARFIELD (background)
// ─────────────────────────────────────────────
#define NUM_STARS 40
struct Star { int16_t x, y; uint8_t speed; };
static Star stars[NUM_STARS];

static void initStars() {
    for (int i = 0; i < NUM_STARS; i++) {
        stars[i].x     = random(PLAY_X, PLAY_X + PLAY_W);
        stars[i].y     = random(0, SCREEN_H);
        stars[i].speed = random(1, 4);
    }
}

static void updateStars() {
    for (int i = 0; i < NUM_STARS; i++) {
        // Erase old
        gfx->drawPixel(stars[i].x, stars[i].y, COL_BG);
        stars[i].y += stars[i].speed;
        if (stars[i].y >= SCREEN_H) {
            stars[i].y = 0;
            stars[i].x = random(PLAY_X, PLAY_X + PLAY_W);
        }
        // Draw new
        gfx->drawPixel(stars[i].x, stars[i].y, COL_STAR);
    }
}

// ─────────────────────────────────────────────
//  GAME STATE
// ─────────────────────────────────────────────
struct Bullet {
    int16_t x, y;
    bool    active;
    bool    isEnemy;
};

struct Enemy {
    float   x, y;           // Current position
    float   formX, formY;   // Formation target position
    float   entryX, entryY; // Entry path start
    uint8_t type;
    uint8_t state;
    float   diveX, diveY;   // Dive target
    float   diveAngle;      // Current angle on dive curve
    float   diveSpeed;
    int8_t  diveDir;        // 1=right, -1=left
    bool    flipX;          // Formation oscillation
    int     shootTimer;
    int     hp;             // Boss has 2hp
    bool    hasCaptured;    // Boss already has captured ship
    int     entryTimer;     // Frames until enters formation
    float   entryProgress;  // 0.0 to 1.0 along entry path
};

static struct {
    float   x;
    bool    alive;
    bool    dual;        // Dual ship (after rescue)
    float   dualOffset;  // Second ship offset
    bool    dualAlive;
} ship;

static Bullet  bullets[MAX_BULLETS + MAX_ENEMY_BULLETS];
static Enemy   enemies[MAX_ENEMIES];
static int     score, highScore, lives, stage;
static int     enemyCount;    // Alive enemies
static float   formationX;    // Formation drift X
static float   formOscDir;    // 1 or -1
static int     frameCount;
static bool    bossBeaming;   // Tractor beam active
static int     beamTimer;
static int     beamEnemy;     // Which boss is beaming
static int     capturedSlot;  // Which formation slot was captured

// HUD dirty tracking — only redraw when values change
static int     lastHudScore = -1;
static int     lastHudLives = -1;
static int     lastHudStage = -1;

// ─────────────────────────────────────────────
//  FORMATION LAYOUT
//  40 enemies: row0=4 Bosses, row1=8 Butterflies,
//              rows 2-3=14 Bees each
// ─────────────────────────────────────────────
static void buildFormation() {
    int idx = 0;
    // Row positions relative to formation anchor
    // Formation anchor: top-left at approximately (60, 40)
    const int FORM_LEFT  = 65;
    const int FORM_TOP   = 45;
    const int COL_GAP    = 20;
    const int ROW_GAP    = 18;

    // Row 0: 4 Bosses (cols 3-6 centered)
    for (int c = 0; c < 4 && idx < MAX_ENEMIES; c++, idx++) {
        enemies[idx].formX = FORM_LEFT + (c + 3) * COL_GAP;
        enemies[idx].formY = FORM_TOP;
        enemies[idx].type  = ET_BOSS;
        enemies[idx].hp    = 2;
        enemies[idx].hasCaptured = false;
    }
    // Row 1: 8 Butterflies
    for (int c = 0; c < 8 && idx < MAX_ENEMIES; c++, idx++) {
        enemies[idx].formX = FORM_LEFT + (c + 1) * COL_GAP;
        enemies[idx].formY = FORM_TOP + ROW_GAP;
        enemies[idx].type  = ET_BUTTERFLY;
        enemies[idx].hp    = 1;
    }
    // Row 2: 10 Bees
    for (int c = 0; c < 10 && idx < MAX_ENEMIES; c++, idx++) {
        enemies[idx].formX = FORM_LEFT + c * COL_GAP;
        enemies[idx].formY = FORM_TOP + ROW_GAP * 2;
        enemies[idx].type  = ET_BEE;
        enemies[idx].hp    = 1;
    }
    // Row 3: 10 Bees
    for (int c = 0; c < 10 && idx < MAX_ENEMIES; c++, idx++) {
        enemies[idx].formX = FORM_LEFT + c * COL_GAP;
        enemies[idx].formY = FORM_TOP + ROW_GAP * 3;
        enemies[idx].type  = ET_BEE;
        enemies[idx].hp    = 1;
    }
}

// ─────────────────────────────────────────────
//  ENTRY CHOREOGRAPHY
//  Enemies spiral in from top edges
// ─────────────────────────────────────────────
static void initEntry(int idx) {
    Enemy& e = enemies[idx];
    e.state         = ES_FORMING;
    e.entryProgress = 0.0f;
    e.entryTimer    = idx * 4; // Stagger entry

    // All enter from top, alternating sides
    if (idx % 2 == 0) {
        e.x = PLAY_X;
        e.y = -10;
    } else {
        e.x = PLAY_X + PLAY_W;
        e.y = -10;
    }
}

static void updateEntry(int idx) {
    Enemy& e = enemies[idx];
    if (e.entryTimer > 0) { e.entryTimer--; return; }

    e.entryProgress += 0.02f;
    if (e.entryProgress >= 1.0f) {
        e.entryProgress = 1.0f;
        e.x = e.formX + formationX;
        e.y = e.formY;
        e.state = ES_FORMATION;
        return;
    }

    // Bezier-like curve: start → overshoot → formation
    float t  = e.entryProgress;
    float t2 = t * t;
    float t3 = t2 * t;

    // Control point: center-ish arc
    float cx = (PLAY_X + PLAY_W) / 2;
    float cy = 20;

    float startX = (idx % 2 == 0) ? PLAY_X : PLAY_X + PLAY_W;
    float startY = -10;

    // Quadratic bezier
    e.x = (1-t)*(1-t)*startX + 2*(1-t)*t*cx + t2*(e.formX + formationX);
    e.y = (1-t)*(1-t)*startY + 2*(1-t)*t*cy + t2*e.formY;
    (void)t3;
}

// ─────────────────────────────────────────────
//  DIVE ATTACK
// ─────────────────────────────────────────────
static void startDive(int idx) {
    Enemy& e = enemies[idx];
    e.state     = ES_DIVING;
    e.diveAngle = 0.0f;
    e.diveSpeed = 2.5f + (stage - 1) * 0.15f;
    e.diveDir   = (e.x < ship.x) ? 1 : -1;
    e.diveX     = ship.x; // Aim at ship
    e.diveY     = SHIP_Y;
    e.shootTimer = 30 + random(40);
}

static void updateDive(int idx) {
    Enemy& e = enemies[idx];
    e.diveAngle += 0.04f;

    // Figure-8 style dive: swoop down, arc under, return up
    float progress = e.diveAngle / (PI * 2);
    if (progress >= 1.0f) {
        // Return to formation
        e.state = ES_FORMATION;
        e.x = e.formX + formationX;
        e.y = e.formY;
        e.diveAngle = 0;
        return;
    }

    // Parametric path: swoops toward ship then loops back up
    float t = progress;
    if (t < 0.5f) {
        // Swoop down to ship area
        float tt = t * 2;
        e.x = e.formX + formationX + sinf(tt * PI) * 60 * e.diveDir;
        e.y = e.formY + tt * (SHIP_Y - e.formY + 20);
    } else {
        // Loop back up from below screen
        float tt = (t - 0.5f) * 2;
        e.x = e.formX + formationX + sinf((1-tt) * PI) * 60 * e.diveDir;
        e.y = SHIP_Y + 20 - tt * (SHIP_Y + 20 - e.formY);
    }

    // Shoot at ship during downward phase
    e.shootTimer--;
    if (e.shootTimer <= 0 && progress < 0.4f) {
        e.shootTimer = 60;
        // Find free enemy bullet slot
        for (int b = MAX_BULLETS; b < MAX_BULLETS + MAX_ENEMY_BULLETS; b++) {
            if (!bullets[b].active) {
                bullets[b].x      = (int16_t)e.x;
                bullets[b].y      = (int16_t)e.y + 8;
                bullets[b].active = true;
                bullets[b].isEnemy = true;
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────
//  TRACTOR BEAM
// ─────────────────────────────────────────────
static void drawTractorBeam(float bx, float by) {
    // Draw expanding beam triangles
    for (int i = 0; i < 4; i++) {
        int w = 10 + i * 8;
        int y = (int)by + 10 + i * 12;
        gfx->drawLine((int)bx - w, y, (int)bx + w, y, COL_BEAM);
    }
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static uint16_t enemyColor(uint8_t type) {
    switch (type) {
        case ET_BEE:       return COL_BEE;
        case ET_BUTTERFLY: return COL_BUTTERFLY;
        case ET_BOSS:      return COL_BOSS;
        default:           return COL_HUD;
    }
}

static void drawEnemy(int idx) {
    Enemy& e = enemies[idx];
    if (e.state == ES_DEAD) return;
    int x = (int)e.x, y = (int)e.y;
    uint16_t col = enemyColor(e.type);

    switch (e.type) {
        case ET_BEE:
            // Simple diamond shape
            gfx->fillTriangle(x, y-4, x-4, y+3, x+4, y+3, col);
            gfx->drawPixel(x-5, y, col); gfx->drawPixel(x+5, y, col); // Wings
            break;
        case ET_BUTTERFLY:
            // Wider body with wings
            gfx->fillRect(x-3, y-3, 6, 6, col);
            gfx->fillTriangle(x-7, y-4, x-3, y, x-7, y+4, col);
            gfx->fillTriangle(x+7, y-4, x+3, y, x+7, y+4, col);
            break;
        case ET_BOSS:
            // Larger — two-part body
            gfx->fillRect(x-5, y-4, 10, 8, col);
            gfx->fillRect(x-3, y-7, 6, 4, (e.hp == 2) ? col : 0x7BEF);
            // Antennae
            gfx->drawPixel(x-4, y-8, col); gfx->drawPixel(x+4, y-8, col);
            break;
    }
}

static void eraseEnemy(int idx) {
    Enemy& e = enemies[idx];
    int x = (int)e.x, y = (int)e.y;
    gfx->fillRect(x - 8, y - 9, 18, 18, COL_BG);
    // Redraw any stars we wiped
    for (int s = 0; s < NUM_STARS; s++) {
        if (stars[s].x >= x-8 && stars[s].x <= x+10 &&
            stars[s].y >= y-9 && stars[s].y <= y+9)
            gfx->drawPixel(stars[s].x, stars[s].y, COL_STAR);
    }
}

static void drawShip() {
    int x = (int)ship.x, y = SHIP_Y;
    gfx->fillTriangle(x, y-5, x-6, y+5, x+6, y+5, COL_SHIP);
    gfx->fillRect(x-2, y-7, 4, 4, COL_HUD); // Cockpit
    if (ship.dual && ship.dualAlive) {
        int dx = (int)(ship.x + ship.dualOffset);
        gfx->fillTriangle(dx, y-5, dx-6, y+5, dx+6, y+5, 0xFD20);
        gfx->fillRect(dx-2, y-7, 4, 4, COL_HUD);
    }
}

static void eraseShip() {
    int x = (int)ship.x, y = SHIP_Y;
    gfx->fillRect(x-8, y-8, 20, 16, COL_BG);
    if (ship.dual && ship.dualAlive) {
        int dx = (int)(ship.x + ship.dualOffset);
        gfx->fillRect(dx-8, y-8, 20, 16, COL_BG);
    }
}

static void drawHUD() {
    // Only redraw if something changed — eliminates per-frame 320×18 fillRect stutter
    if (score == lastHudScore && lives == lastHudLives && stage == lastHudStage) return;
    lastHudScore = score; lastHudLives = lives; lastHudStage = stage;

    gfx->fillRect(0, 0, SCREEN_W, HUD_H, COL_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(2, 4);
    gfx->printf("SC:%d", score);
    gfx->setCursor(100, 4);
    gfx->printf("HI:%d", highScore);
    gfx->setCursor(200, 4);
    gfx->printf("STG:%d", stage);
    // Lives
    for (int i = 0; i < lives && i < 5; i++) {
        gfx->fillTriangle(260 + i*11, SHIP_Y/4 - 3,
                          256 + i*11, SHIP_Y/4 + 3,
                          264 + i*11, SHIP_Y/4 + 3, COL_SHIP);
    }
}

// ─────────────────────────────────────────────
//  EXPLOSION
// ─────────────────────────────────────────────
static void drawExplosion(int x, int y) {
    uint16_t cols[] = {COL_EXPLOSION, 0xFD20, 0xFFE0, COL_HUD};
    for (int r = 1; r <= 4; r++) {
        gfx->drawCircle(x, y, r * 2, cols[r-1]);
        delay(25);
    }
    delay(80);
    // Erase explosion with a rect (same approach as eraseEnemy — avoids full redraw flash)
    gfx->fillRect(x - 10, y - 10, 22, 22, COL_BG);
    // Restore any stars in that patch
    for (int s = 0; s < NUM_STARS; s++) {
        if (stars[s].x >= x-10 && stars[s].x <= x+12 &&
            stars[s].y >= y-10 && stars[s].y <= y+12)
            gfx->drawPixel(stars[s].x, stars[s].y, COL_STAR);
    }
}

// ─────────────────────────────────────────────
//  HIGH SCORE
// ─────────────────────────────────────────────
static int loadHS() {
    if (!sd.exists(HS_PATH)) return 0;
    FsFile f = sd.open(HS_PATH, O_READ);
    if (!f) return 0;
    char buf[16] = {0}; f.read(buf, 15); f.close();
    return atoi(buf);
}
static void saveHS(int hs) {
    FsFile f = sd.open(HS_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return;
    f.printf("%d", hs); f.close();
}

// ─────────────────────────────────────────────
//  CHALLENGING STAGE (bonus round)
//  Formation flies through — no shooting, just
//  dodge and shoot for max points
// ─────────────────────────────────────────────
static void runChallengingStage() {
    gfx->fillScreen(COL_BG);
    gfx->setTextSize(2);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(60, 100); gfx->print("CHALLENGING");
    gfx->setCursor(90, 125); gfx->print("STAGE !!");
    delay(2000);
    gfx->fillScreen(COL_BG);
    initStars();

    // 40 enemies fly through in a figure-8 pattern
    // Player can shoot them all for a big bonus
    // Enemies do NOT shoot back
    struct ChallengeEnemy {
        float x, y, angle;
        bool alive;
        uint8_t type;
    } ce[40];

    for (int i = 0; i < 40; i++) {
        ce[i].angle = -(i * 0.15f);
        ce[i].x     = PLAY_X + PLAY_W/2;
        ce[i].y     = -10 - i * 15;
        ce[i].alive = true;
        ce[i].type  = i % 3;
    }

    Bullet pb[MAX_BULLETS];
    for (int i = 0; i < MAX_BULLETS; i++) pb[i].active = false;
    float sx    = PLAY_X + PLAY_W / 2;
    int   shotCooldown = 0;
    int   hit   = 0;
    bool  done  = false;

    while (!done) {
        updateStars();

        // Move challenge enemies in swooping pattern
        bool anyAlive = false;
        for (int i = 0; i < 40; i++) {
            if (!ce[i].alive) continue;
            anyAlive = true;
            gfx->fillRect((int)ce[i].x - 6, (int)ce[i].y - 6, 14, 14, COL_BG);
            ce[i].y    += 2.0f;
            ce[i].angle += 0.06f;
            ce[i].x = PLAY_X + PLAY_W/2 + sinf(ce[i].angle) * 100;
            if (ce[i].y > SCREEN_H + 10) ce[i].alive = false;
            if (ce[i].alive) {
                uint16_t col = enemyColor(ce[i].type);
                gfx->fillRect((int)ce[i].x-4, (int)ce[i].y-4, 9, 9, col);
            }
        }
        if (!anyAlive) done = true;

        // Ship movement
        char k = get_keypress();
        TrackballState tb = update_trackball();
        if (gamepad_poll()) { done = true; break; } // HOME
        if (k == 'q' || k == 'Q') { done = true; break; }
        if ((k == 'a' || k == 'A' || tb.x == -1 || gamepad_held(GP_LEFT)) && sx > PLAY_X + 8)
            sx -= SHIP_SPEED;
        if ((k == 'd' || k == 'D' || tb.x == 1  || gamepad_held(GP_RIGHT)) && sx < PLAY_X + PLAY_W - 8)
            sx += SHIP_SPEED;

        // Fire
        if (shotCooldown > 0) shotCooldown--;
        if ((k == ' ' || tb.clicked || gamepad_pressed(GP_A)) && shotCooldown == 0) {
            for (int b = 0; b < MAX_BULLETS; b++) {
                if (!pb[b].active) {
                    pb[b].x = (int16_t)sx; pb[b].y = SHIP_Y - 8;
                    pb[b].active = true; shotCooldown = 15; break;
                }
            }
        }

        // Draw ship
        gfx->fillRect((int)sx - 8, SHIP_Y - 8, 18, 16, COL_BG);
        gfx->fillTriangle((int)sx, SHIP_Y-5, (int)sx-6, SHIP_Y+5, (int)sx+6, SHIP_Y+5, COL_SHIP);

        // Move/draw/collide bullets
        for (int b = 0; b < MAX_BULLETS; b++) {
            if (!pb[b].active) continue;
            gfx->drawPixel(pb[b].x, pb[b].y, COL_BG);
            pb[b].y -= BULLET_SPEED;
            if (pb[b].y < 0) { pb[b].active = false; continue; }
            gfx->drawLine(pb[b].x, pb[b].y, pb[b].x, pb[b].y + 4, COL_BULLET);
            for (int i = 0; i < 40; i++) {
                if (!ce[i].alive) continue;
                if (abs(pb[b].x - (int)ce[i].x) < 8 &&
                    abs(pb[b].y - (int)ce[i].y) < 8) {
                    ce[i].alive = false; pb[b].active = false;
                    gfx->fillRect((int)ce[i].x-6, (int)ce[i].y-6, 14, 14, COL_BG);
                    score += 100; hit++;
                    break;
                }
            }
        }

        drawHUD();
        delay(30); yield();
    }

    // Result
    gfx->fillRect(60, 90, 200, 60, C_DARK);
    gfx->drawRect(60, 90, 200, 60, COL_HUD);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(80, 100);
    if (hit == 40) {
        score += 10000;
        gfx->print("PERFECT!! +10000");
    } else {
        gfx->printf("HIT %d/40  +%d", hit, hit * 100);
    }
    gfx->setCursor(80, 120);
    gfx->printf("TOTAL: %d", score);
    delay(2500);
}

// ─────────────────────────────────────────────
//  STAGE INIT
// ─────────────────────────────────────────────
static void initStage() {
    memset(enemies, 0, sizeof(enemies));
    memset(bullets, 0, sizeof(bullets));

    buildFormation();
    for (int i = 0; i < MAX_ENEMIES; i++) {
        initEntry(i);
    }

    formationX  = 0;
    formOscDir  = 1;
    frameCount  = 0;
    bossBeaming = false;
    beamTimer   = 0;
    enemyCount  = MAX_ENEMIES;
    // Force HUD redraw at start of each stage
    lastHudScore = -1; lastHudLives = -1; lastHudStage = -1;

    gfx->fillScreen(COL_BG);
    initStars();
    for (int s = 0; s < NUM_STARS; s++)
        gfx->drawPixel(stars[s].x, stars[s].y, COL_STAR);
    drawHUD();
}

// ─────────────────────────────────────────────
//  COUNT ALIVE ENEMIES
// ─────────────────────────────────────────────
static int countAlive() {
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].state != ES_DEAD) n++;
    return n;
}

// ─────────────────────────────────────────────
//  MAIN GAME LOOP FOR ONE STAGE
//  Returns: true = stage clear, false = game over
// ─────────────────────────────────────────────
static bool runStage() {
    initStage();

    // Show stage banner
    gfx->setTextSize(2);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(100, 110);
    gfx->printf("STAGE %d", stage);
    delay(1500);
    gfx->fillRect(80, 105, 160, 25, COL_BG);

    int  shotCooldown  = 0;
    int  diveTimer     = 60 + random(60);
    int  divingCount   = 0;
    bool quit          = false;

    while (true) {
        frameCount++;

        // ── INPUT ──
        char k = get_keypress();
        TrackballState tb = update_trackball();

        if (gamepad_poll()) { quit = true; break; } // HOME
        if (k == 'q' || k == 'Q') { quit = true; break; }
        int16_t tx2, ty2;
        if (get_touch(&tx2, &ty2) && ty2 < HUD_H) {
            while (get_touch(&tx2, &ty2)) { delay(10); }
            quit = true; break;
        }

        // Ship movement — continuous while trackball direction is held
        // tb.x is edge-detect (fires once per roll click), so we maintain
        // a persistent direction that keeps moving until the player rolls
        // the opposite way or hits a wall.
        static int8_t shipMoveDir = 0;  // -1 = left, 0 = stop, 1 = right

        eraseShip();
        if (tb.x == -1 || gamepad_held(GP_LEFT))  shipMoveDir = -1;
        if (tb.x ==  1 || gamepad_held(GP_RIGHT))  shipMoveDir =  1;
        // Keyboard overrides
        if (k == 'a' || k == 'A') shipMoveDir = -1;
        if (k == 'd' || k == 'D') shipMoveDir =  1;
        // Gamepad stop on release
        if (!gamepad_held(GP_LEFT) && !gamepad_held(GP_RIGHT) &&
            tb.x == 0 && k != 'a' && k != 'A' && k != 'd' && k != 'D')
        { /* keep rolling */ }

        if (shipMoveDir == -1 && ship.x > PLAY_X + 8)          ship.x -= SHIP_SPEED;
        if (shipMoveDir ==  1 && ship.x < PLAY_X + PLAY_W - 8) ship.x += SHIP_SPEED;
        // Stop at walls
        if (ship.x <= PLAY_X + 8)          { ship.x = PLAY_X + 8;          shipMoveDir = 0; }
        if (ship.x >= PLAY_X + PLAY_W - 8) { ship.x = PLAY_X + PLAY_W - 8; shipMoveDir = 0; }

        // Fire — trackball click fires, keyboard space fires, GP_A fires
        if (shotCooldown > 0) shotCooldown--;
        bool fire = (k == ' ' || tb.clicked || gamepad_pressed(GP_A));
        if (fire && shotCooldown == 0) {
            for (int b = 0; b < MAX_BULLETS; b++) {
                if (!bullets[b].active) {
                    bullets[b].x       = (int16_t)ship.x;
                    bullets[b].y       = SHIP_Y - 8;
                    bullets[b].active  = true;
                    bullets[b].isEnemy = false;
                    shotCooldown       = 12;
                    // Dual ship fires both
                    if (ship.dual && ship.dualAlive) {
                        for (int b2 = b+1; b2 < MAX_BULLETS; b2++) {
                            if (!bullets[b2].active) {
                                bullets[b2].x       = (int16_t)(ship.x + ship.dualOffset);
                                bullets[b2].y       = SHIP_Y - 8;
                                bullets[b2].active  = true;
                                bullets[b2].isEnemy = false;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }

        // ── UPDATE FORMATION OSCILLATION ──
        bool allFormed = true;
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (enemies[i].state == ES_FORMING) { allFormed = false; break; }

        if (allFormed) {
            formationX += 0.4f * formOscDir;
            if (formationX > 30 || formationX < -30) formOscDir = -formOscDir;
        }

        // ── DIVE TRIGGER ──
        diveTimer--;
        divingCount = 0;
        for (int i = 0; i < MAX_ENEMIES; i++)
            if (enemies[i].state == ES_DIVING) divingCount++;

        if (diveTimer <= 0 && divingCount < MAX_DIVE && allFormed) {
            // Pick a random formation enemy to dive
            int tries = 0;
            while (tries++ < 20) {
                int idx = random(MAX_ENEMIES);
                if (enemies[idx].state == ES_FORMATION) {
                    // Boss might try tractor beam
                    if (enemies[idx].type == ET_BOSS && !bossBeaming &&
                        !enemies[idx].hasCaptured && random(3) == 0) {
                        bossBeaming = true;
                        beamTimer   = 120;
                        beamEnemy   = idx;
                        enemies[idx].state = ES_DIVING;
                        enemies[idx].diveAngle = 0;
                        enemies[idx].diveSpeed = 1.5f;
                        enemies[idx].diveDir   = (enemies[idx].x < ship.x) ? 1 : -1;
                    } else {
                        startDive(idx);
                    }
                    break;
                }
            }
            diveTimer = 45 + random(60) - (stage * 3);
        }

        // ── TRACTOR BEAM ──
        if (bossBeaming && beamTimer > 0) {
            beamTimer--;
            Enemy& boss = enemies[beamEnemy];
            // Move boss toward ship X slowly
            float targetX = ship.x;
            if (abs(boss.x - targetX) > 2) boss.x += (targetX > boss.x) ? 1.5f : -1.5f;
            boss.y += 0.5f;
            if (boss.y > 80) boss.y = 80; // Stop partway down

            drawTractorBeam(boss.x, boss.y);

            // If beam reaches ship and ship not already captured
            if (boss.y >= 70 && !enemies[beamEnemy].hasCaptured) {
                // Ship gets captured!
                enemies[beamEnemy].hasCaptured = true;
                capturedSlot = beamEnemy;
                lives--;
                if (lives <= 0) { bossBeaming = false; return false; }
                // Brief death animation then respawn
                drawExplosion((int)ship.x, SHIP_Y);
                ship.x = PLAY_X + PLAY_W / 2;
                ship.alive = true;
                bossBeaming = false;
                beamTimer = 0;
                // Boss returns to formation with captured ship sprite
                enemies[beamEnemy].state = ES_FORMATION;
                delay(500);
            }
            if (beamTimer <= 0) {
                bossBeaming = false;
                enemies[beamEnemy].state = ES_FORMATION;
            }
        }

        // ── UPDATE ENEMIES ──
        for (int i = 0; i < MAX_ENEMIES; i++) {
            if (enemies[i].state == ES_DEAD) continue;
            eraseEnemy(i);
            if      (enemies[i].state == ES_FORMING)   updateEntry(i);
            else if (enemies[i].state == ES_DIVING)    updateDive(i);
            else if (enemies[i].state == ES_FORMATION) {
                // Oscillate with formation
                enemies[i].x = enemies[i].formX + formationX;
                enemies[i].y = enemies[i].formY;
                // Random shooting from formation
                if (allFormed && random(2000) < stage + 1) {
                    for (int b = MAX_BULLETS; b < MAX_BULLETS + MAX_ENEMY_BULLETS; b++) {
                        if (!bullets[b].active) {
                            bullets[b].x       = (int16_t)enemies[i].x;
                            bullets[b].y       = (int16_t)enemies[i].y + 8;
                            bullets[b].active  = true;
                            bullets[b].isEnemy = true;
                            break;
                        }
                    }
                }
            }
        }

        // ── UPDATE BULLETS ──
        for (int b = 0; b < MAX_BULLETS + MAX_ENEMY_BULLETS; b++) {
            if (!bullets[b].active) continue;
            gfx->drawLine(bullets[b].x, bullets[b].y,
                          bullets[b].x, bullets[b].y + 4, COL_BG);

            if (bullets[b].isEnemy) {
                bullets[b].y += 4;
                if (bullets[b].y > SCREEN_H) { bullets[b].active = false; continue; }
                gfx->drawLine(bullets[b].x, bullets[b].y,
                              bullets[b].x, bullets[b].y + 4, 0xF800);

                // Hit player
                if (ship.alive &&
                    abs(bullets[b].x - (int)ship.x) < 7 &&
                    abs(bullets[b].y - SHIP_Y) < 8) {
                    bullets[b].active = false;
                    if (ship.dual && ship.dualAlive) {
                        ship.dualAlive = false; // Lose dual ship first
                    } else {
                        drawExplosion((int)ship.x, SHIP_Y);
                        lives--;
                        if (lives <= 0) return false;
                        ship.x = PLAY_X + PLAY_W / 2;
                        ship.dual = false;
                        delay(500);
                    }
                }
            } else {
                // Player bullet goes up
                bullets[b].y -= BULLET_SPEED;
                if (bullets[b].y < HUD_H) { bullets[b].active = false; continue; }
                gfx->drawLine(bullets[b].x, bullets[b].y,
                              bullets[b].x, bullets[b].y + 4, COL_BULLET);

                // Hit enemy
                for (int i = 0; i < MAX_ENEMIES; i++) {
                    if (enemies[i].state == ES_DEAD) continue;
                    if (abs(bullets[b].x - (int)enemies[i].x) < 8 &&
                        abs(bullets[b].y - (int)enemies[i].y) < 8) {
                        enemies[i].hp--;
                        bullets[b].active = false;
                        if (enemies[i].hp <= 0) {
                            // Check if this boss had a captured ship — rescue!
                            if (enemies[i].type == ET_BOSS && enemies[i].hasCaptured) {
                                ship.dual      = true;
                                ship.dualAlive = true;
                                ship.dualOffset = 20;
                                score += 1000; // Rescue bonus
                            }
                            int pts = (enemies[i].type == ET_BEE)       ? 50  :
                                      (enemies[i].type == ET_BUTTERFLY)  ? 80  :
                                      (enemies[i].state == ES_DIVING)    ? 160 : 150;
                            // Double points for diving enemies
                            if (enemies[i].state == ES_DIVING) pts *= 2;
                            score += pts;
                            drawExplosion((int)enemies[i].x, (int)enemies[i].y);
                            enemies[i].state = ES_DEAD;
                            if (bossBeaming && beamEnemy == i) bossBeaming = false;
                        }
                        break;
                    }
                }
            }
        }

        // ── DRAW EVERYTHING ──
        for (int i = 0; i < MAX_ENEMIES; i++) drawEnemy(i);
        drawShip();
        updateStars();
        drawHUD();

        // Stage clear?
        if (countAlive() == 0) return true;

        delay(25); // ~40fps
        yield();
    }

    return !quit ? true : false;
}

// ─────────────────────────────────────────────
//  GAME OVER SCREEN
// ─────────────────────────────────────────────
static void showGameOver() {
    gfx->fillRect(70, 85, 180, 70, C_DARK);
    gfx->drawRect(70, 85, 180, 70, COL_HUD);
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(95, 95);
    gfx->print("GAME OVER");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_HUD);
    gfx->setCursor(95, 118);
    gfx->printf("SCORE: %d", score);
    if (score >= highScore && score > 0) {
        gfx->setTextColor(COL_BULLET);
        gfx->setCursor(82, 133);
        gfx->print("** NEW HIGH SCORE! **");
    }
    delay(3000);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_galaga() {
    highScore = loadHS();
    score     = 0;
    lives     = 3;
    stage     = 1;

    ship.x         = PLAY_X + PLAY_W / 2;
    ship.alive     = true;
    ship.dual      = false;
    ship.dualAlive = false;
    ship.dualOffset = 20;

    while (lives > 0) {
        // Challenging stage every 3 stages
        if (stage > 1 && (stage - 1) % 3 == 0) {
            runChallengingStage();
        }

        bool stageClear = runStage();

        if (!stageClear) break;

        // Stage clear jingle
        gfx->setTextSize(2);
        gfx->setTextColor(COL_BULLET);
        gfx->setCursor(90, 110);
        gfx->print("STAGE CLEAR!");
        delay(1500);
        stage++;
    }

    if (score > highScore) {
        highScore = score;
        saveHS(highScore);
    }
    showGameOver();
}
