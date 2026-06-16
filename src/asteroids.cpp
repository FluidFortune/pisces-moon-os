// Pisces Moon OS — Asteroids
// Copyright (C) 2026 Eric Becker / Fluid Fortune  AGPL-3.0-or-later
//
// Classic vector-style asteroids. Triangular ship rotates and
// thrusts in space with momentum. Shoot rocks; each hit splits
// a large rock into two medium, medium into two small, small
// destroyed. Clear all rocks to advance to the next wave.
//
// Input: LEFT/RIGHT rotates ship, UP thrusts, A fires, B
// hyperspace (random teleport — emergency only, occasional self-
// destruct).

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_audio.h"
#include "game_input.h"
#include "theme.h"
#include "asteroids.h"
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

#ifdef DEVICE_CARDPUTER_ADV
static constexpr int VIEW_W = 240, VIEW_H = 135;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 480, VIEW_H = 222;
#elif defined(DEVICE_C28P) || defined(DEVICE_C5)
static constexpr int VIEW_W = 240, VIEW_H = 200;
#elif defined(DEVICE_MAXINE)
static constexpr int VIEW_W = 480, VIEW_H = 520;
#else
static constexpr int VIEW_W = 320, VIEW_H = 240;
#endif

static int vx() { int w = gfx->width(); return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0; }
static int vy() {
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    return 0;
#else
    int h = gfx->height(); return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
#endif
}
static constexpr int HUD_H = 12;

// Ship state (fixed-point: 16 bits integer, 16 bits fraction)
static int32_t sx, sy;       // position in 1/256 pixels
static int32_t svx, svy;     // velocity per frame in 1/256 px
static int32_t s_ang;        // 0..255 angle (256 = full circle)
static int     lives, score, wave;
static int     respawn_timer;

// Bullets
struct Bullet { int32_t x, y, vx, vy; int life; };
static Bullet bullets[6];

// Asteroids
struct Rock { int32_t x, y, vx, vy; uint8_t size; uint8_t shape; bool alive; };
static Rock rocks[20];

static int32_t cos8(int a) {
    // a is 0..255 = 0..2pi
    static const int8_t TBL[64] = {
        127,126,125,122,118,113,106,99,90,80,70,60,49,37,25,12,
        0,-12,-25,-37,-49,-60,-70,-80,-90,-99,-106,-113,-118,-122,-125,-126,
        -127,-126,-125,-122,-118,-113,-106,-99,-90,-80,-70,-60,-49,-37,-25,-12,
        0,12,25,37,49,60,70,80,90,99,106,113,118,122,125,126,
    };
    return TBL[a & 63] * ((a < 64 || (a >= 128 && a < 192)) ? 1 : 1);
}

// Quick sin/cos in q8: angle a in 0..255 (256 = 2pi).
static int32_t cos_q8(uint8_t a) {
    // approximate via 8 octants
    static const int16_t TBL[256] = {
        256,255,255,255,254,253,251,250,247,245,242,239,236,233,229,225,
        221,216,212,207,202,197,191,185,179,173,167,160,154,147,140,133,
        126,118,110,103,95,87,79,70,62,54,45,36,28,19,10,2,
        -7,-16,-25,-34,-42,-51,-60,-68,-77,-85,-93,-101,-109,-117,-124,-132,
        -139,-146,-153,-159,-166,-172,-178,-184,-190,-195,-200,-205,-210,-215,-219,-223,
        -227,-230,-234,-237,-240,-242,-245,-247,-249,-250,-252,-253,-254,-254,-255,-255,
        -256,-255,-255,-254,-254,-253,-252,-250,-249,-247,-245,-242,-240,-237,-234,-230,
        -227,-223,-219,-215,-210,-205,-200,-195,-190,-184,-178,-172,-166,-159,-153,-146,
        -139,-132,-124,-117,-109,-101,-93,-85,-77,-68,-60,-51,-42,-34,-25,-16,
        -7,2,10,19,28,36,45,54,62,70,79,87,95,103,110,118,
        126,133,140,147,154,160,167,173,179,185,191,197,202,207,212,216,
        221,225,229,233,236,239,242,245,247,250,251,253,254,255,255,255,
        256,255,255,255,254,253,251,250,247,245,242,239,236,233,229,225,
        221,216,212,207,202,197,191,185,179,173,167,160,154,147,140,133,
        126,118,110,103,95,87,79,70,62,54,45,36,28,19,10,2,
        -7,-16,-25,-34,-42,-51,-60,-68,-77,-85,-93,-101,-109,-117,-124,-132,
    };
    return TBL[a];
}
static int32_t sin_q8(uint8_t a) { return cos_q8((uint8_t)(a - 64)); }

static int rock_radius(uint8_t size) { return size == 0 ? 6 : size == 1 ? 12 : 22; }

static void spawn_wave(int n) {
    for (int i = 0; i < (int)(sizeof(rocks) / sizeof(rocks[0])); i++) rocks[i].alive = false;
    for (int i = 0; i < n; i++) {
        Rock& r = rocks[i];
        r.alive = true;
        r.size = 2;
        r.shape = random(256);
        // Spawn at edge
        if (random(2)) {
            r.x = (int32_t)(random(2) ? 0 : VIEW_W) * 256;
            r.y = (int32_t)random(HUD_H, VIEW_H) * 256;
        } else {
            r.x = (int32_t)random(VIEW_W) * 256;
            r.y = (int32_t)(random(2) ? HUD_H : VIEW_H) * 256;
        }
        int a = random(256);
        int sp = 60 + random(80);
        r.vx = (cos_q8(a) * sp) / 256;
        r.vy = (sin_q8(a) * sp) / 256;
    }
}

static void respawn_ship() {
    sx = (int32_t)(VIEW_W / 2) * 256;
    sy = (int32_t)((VIEW_H + HUD_H) / 2) * 256;
    svx = svy = 0;
    s_ang = 192;   // pointing up
    respawn_timer = 60;
}

static void fire_bullet() {
    for (int i = 0; i < (int)(sizeof(bullets) / sizeof(bullets[0])); i++) {
        if (bullets[i].life <= 0) {
            bullets[i].x = sx;
            bullets[i].y = sy;
            int sp = 600;
            bullets[i].vx = (cos_q8(s_ang) * sp) / 256;
            bullets[i].vy = (sin_q8(s_ang) * sp) / 256;
            bullets[i].life = 45;
            pm_game_audio_fx_drop();
            return;
        }
    }
}

static void wrap(int32_t& x, int32_t lim) {
    if (x < 0) x += lim * 256;
    if (x >= lim * 256) x -= lim * 256;
}

static void draw_hud() {
    int ox = vx(), oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, HUD_H, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(ox + 4, oy + 3);
    gfx->printf("WAVE %d  SC %05d  L%d", wave + 1, score, lives);
}

static void draw_ship() {
    if (respawn_timer > 0 && (respawn_timer & 4)) return;
    int ox = vx(), oy = vy();
    int cx = ox + (int)(sx >> 8);
    int cy = oy + (int)(sy >> 8);
    int32_t nose_x = (cos_q8(s_ang) * 8) / 256;
    int32_t nose_y = (sin_q8(s_ang) * 8) / 256;
    int32_t left_x = (cos_q8(s_ang + 96) * 6) / 256;
    int32_t left_y = (sin_q8(s_ang + 96) * 6) / 256;
    int32_t right_x = (cos_q8(s_ang - 96) * 6) / 256;
    int32_t right_y = (sin_q8(s_ang - 96) * 6) / 256;
    gfx->drawTriangle(cx + nose_x, cy + nose_y,
                      cx + left_x, cy + left_y,
                      cx + right_x, cy + right_y, 0xFFFF);
}

static void draw_rock(const Rock& r) {
    int ox = vx(), oy = vy();
    int cx = ox + (int)(r.x >> 8);
    int cy = oy + (int)(r.y >> 8);
    int rad = rock_radius(r.size);
    // Approximate with a circle for speed (the original used vector polygons)
    gfx->drawCircle(cx, cy, rad, 0xC618);
    // Surface speckle
    for (int i = 0; i < 3; i++) {
        int ang = (r.shape + i * 80) & 0xFF;
        int px = cx + (cos_q8(ang) * (rad / 2)) / 256;
        int py = cy + (sin_q8(ang) * (rad / 2)) / 256;
        gfx->drawPixel(px, py, 0xFFFF);
    }
}

static void draw_bullet(const Bullet& b) {
    if (b.life <= 0) return;
    int ox = vx(), oy = vy();
    gfx->fillRect(ox + (int)(b.x >> 8) - 1, oy + (int)(b.y >> 8) - 1, 2, 2, 0xFFE0);
}

static void clear_play() {
    int ox = vx(), oy = vy();
#if defined(DEVICE_C28P) || defined(DEVICE_C5) || defined(DEVICE_MAXINE)
    gfx->fillRect(ox, oy + HUD_H, VIEW_W, VIEW_H - HUD_H, 0x0000);
#else
    gfx->fillRect(ox, oy, VIEW_W, VIEW_H, 0x0000);
#endif
}

static int rocks_alive() {
    int n = 0;
    for (int i = 0; i < (int)(sizeof(rocks) / sizeof(rocks[0])); i++) if (rocks[i].alive) n++;
    return n;
}

static void split_rock(Rock& r) {
    int score_add[3] = { 100, 50, 20 };
    score += score_add[r.size];
    if (r.size == 0) { r.alive = false; return; }
    // Spawn two children
    uint8_t new_size = r.size - 1;
    int spawned = 0;
    for (int i = 0; i < (int)(sizeof(rocks) / sizeof(rocks[0])) && spawned < 2; i++) {
        if (rocks[i].alive) continue;
        Rock& c = rocks[i];
        c.alive = true;
        c.size = new_size;
        c.shape = random(256);
        c.x = r.x; c.y = r.y;
        int a = random(256);
        int sp = 80 + random(80);
        c.vx = (cos_q8(a) * sp) / 256;
        c.vy = (sin_q8(a) * sp) / 256;
        spawned++;
    }
    r.alive = false;
}

void run_asteroids() {
    pm_game_audio_begin();
#ifdef DEVICE_C28P
    c28p_dpad_render();
#endif
#ifdef DEVICE_C5
    c5_dpad_render();
#endif
#ifdef DEVICE_MAXINE
    maxine_dpad_render();
#endif
    score = 0;
    lives = 3;
    wave = 0;
    respawn_ship();
    spawn_wave(3 + wave);
    for (int i = 0; i < (int)(sizeof(bullets) / sizeof(bullets[0])); i++) bullets[i].life = 0;

    clear_play();
    draw_hud();

    bool was_a = false, was_b = false;
    uint32_t last_frame = millis();
    while (true) {
        PMNesInput in = pm_read_nes_input(true);
        if (in.quit) return;

        if (in.left)  s_ang = (s_ang - 4) & 0xFF;
        if (in.right) s_ang = (s_ang + 4) & 0xFF;
        if (in.up) {
            svx += (cos_q8(s_ang) * 6) / 256;
            svy += (sin_q8(s_ang) * 6) / 256;
            if (svx >  400) svx =  400;
            if (svx < -400) svx = -400;
            if (svy >  400) svy =  400;
            if (svy < -400) svy = -400;
        } else {
            // Slight drag
            svx = svx * 240 / 256;
            svy = svy * 240 / 256;
        }
        if (in.a && !was_a && respawn_timer == 0) fire_bullet();
        if (in.b && !was_b && respawn_timer == 0) {
            // Hyperspace
            sx = (int32_t)random(VIEW_W) * 256;
            sy = (int32_t)random(HUD_H, VIEW_H) * 256;
            svx = svy = 0;
            if (random(20) == 0) {
                // Self-destruct risk
                lives--;
                respawn_timer = 60;
            }
        }
        was_a = in.a; was_b = in.b;

        sx += svx;
        sy += svy;
        wrap(sx, VIEW_W);
        // Wrap Y between HUD_H and VIEW_H
        if (sy < HUD_H * 256)   sy += (VIEW_H - HUD_H) * 256;
        if (sy >= VIEW_H * 256) sy -= (VIEW_H - HUD_H) * 256;

        for (int i = 0; i < (int)(sizeof(bullets) / sizeof(bullets[0])); i++) {
            if (bullets[i].life <= 0) continue;
            bullets[i].x += bullets[i].vx;
            bullets[i].y += bullets[i].vy;
            wrap(bullets[i].x, VIEW_W);
            if (bullets[i].y < HUD_H * 256)   bullets[i].y += (VIEW_H - HUD_H) * 256;
            if (bullets[i].y >= VIEW_H * 256) bullets[i].y -= (VIEW_H - HUD_H) * 256;
            bullets[i].life--;
        }
        for (int i = 0; i < (int)(sizeof(rocks) / sizeof(rocks[0])); i++) {
            if (!rocks[i].alive) continue;
            rocks[i].x += rocks[i].vx;
            rocks[i].y += rocks[i].vy;
            wrap(rocks[i].x, VIEW_W);
            if (rocks[i].y < HUD_H * 256)   rocks[i].y += (VIEW_H - HUD_H) * 256;
            if (rocks[i].y >= VIEW_H * 256) rocks[i].y -= (VIEW_H - HUD_H) * 256;
        }

        // Bullet-rock collisions
        for (int i = 0; i < (int)(sizeof(bullets) / sizeof(bullets[0])); i++) {
            if (bullets[i].life <= 0) continue;
            for (int j = 0; j < (int)(sizeof(rocks) / sizeof(rocks[0])); j++) {
                if (!rocks[j].alive) continue;
                int dx = (int)((bullets[i].x - rocks[j].x) >> 8);
                int dy = (int)((bullets[i].y - rocks[j].y) >> 8);
                int rr = rock_radius(rocks[j].size);
                if (dx*dx + dy*dy < rr*rr) {
                    bullets[i].life = 0;
                    Rock saved = rocks[j];
                    split_rock(rocks[j]);
                    (void)saved;
                    pm_game_audio_fx_line();
                    break;
                }
            }
        }

        // Ship-rock collisions
        if (respawn_timer == 0) {
            for (int j = 0; j < (int)(sizeof(rocks) / sizeof(rocks[0])); j++) {
                if (!rocks[j].alive) continue;
                int dx = (int)((sx - rocks[j].x) >> 8);
                int dy = (int)((sy - rocks[j].y) >> 8);
                int rr = rock_radius(rocks[j].size) + 4;
                if (dx*dx + dy*dy < rr*rr) {
                    lives--;
                    if (lives <= 0) {
                        clear_play();
                        gfx->setTextSize(2);
                        gfx->setTextColor(0xF800);
                        gfx->setCursor(vx() + VIEW_W/2 - 54, vy() + VIEW_H/2 - 8);
                        gfx->print("GAME OVER");
                        delay(2400);
                        return;
                    }
                    respawn_ship();
                    break;
                }
            }
        }
        if (respawn_timer > 0) respawn_timer--;

        // Next wave
        if (rocks_alive() == 0) {
            wave++;
            spawn_wave(3 + wave);
        }

        // Render
        clear_play();
        draw_hud();
        for (int i = 0; i < (int)(sizeof(rocks) / sizeof(rocks[0])); i++)
            if (rocks[i].alive) draw_rock(rocks[i]);
        for (int i = 0; i < (int)(sizeof(bullets) / sizeof(bullets[0])); i++)
            draw_bullet(bullets[i]);
        draw_ship();

        uint32_t now = millis();
        if (now - last_frame < 33) delay(33 - (now - last_frame));
        last_frame = millis();
        yield();
    }
}
