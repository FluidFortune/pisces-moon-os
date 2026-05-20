// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <Arduino.h>
#include <math.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "game_input.h"
#include "pole_position.h"
#include "theme.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

#ifdef DEVICE_CARDPUTER_ADV
static constexpr int VIEW_W = 240;
static constexpr int VIEW_H = 135;
static constexpr int HORIZON = 42;
static constexpr int CAR_Y = 104;
#elif defined(DEVICE_TLORAPAGER)
static constexpr int VIEW_W = 320;
static constexpr int VIEW_H = 222;
static constexpr int HORIZON = 76;
static constexpr int CAR_Y = 178;
#else
static constexpr int VIEW_W = 320;
static constexpr int VIEW_H = 240;
static constexpr int HORIZON = 82;
static constexpr int CAR_Y = 194;
#endif

static constexpr int ROAD_LEN = 5200;
static constexpr int NUM_RIVALS = 6;
static constexpr uint16_t COL_SKY = 0x5D9F;
static constexpr uint16_t COL_SKY_LOW = 0x867F;
static constexpr uint16_t COL_GRASS_A = 0x05A0;
static constexpr uint16_t COL_GRASS_B = 0x03A0;
static constexpr uint16_t COL_ROAD_A = 0x528A;
static constexpr uint16_t COL_ROAD_B = 0x4208;
static constexpr uint16_t COL_STRIPE = 0xFFFF;
static constexpr uint16_t COL_RUMBLE_R = 0xF800;
static constexpr uint16_t COL_RUMBLE_W = 0xFFFF;
static constexpr uint16_t COL_MOUNTAIN = 0x7BEF;
static constexpr uint16_t COL_MOUNTAIN_D = 0x4A49;

struct Rival {
    float z;
    float lane;
    float speed;
    uint16_t color;
};

static Rival rivals[NUM_RIVALS];
static float playerX;
static float speed;
static float distanceRun;
static float roadScroll;
static int timeLeft;
static int lap;
static int score;
static uint32_t lastSecond;
static uint32_t lastFrame;
static bool raceOver;

static int vx() {
    int w = gfx->width();
    return (w > VIEW_W) ? (w - VIEW_W) / 2 : 0;
}

static int vy() {
    int h = gfx->height();
    return (h > VIEW_H) ? (h - VIEW_H) / 2 : 0;
}

static float roadCurveAt(float z) {
    float p = distanceRun + z;
    return sinf(p * 0.0025f) * 0.78f + sinf(p * 0.0009f + 1.7f) * 0.52f;
}

static int projectY(float z) {
    float t = 1.0f - z / 900.0f;
    if (t < 0.0f) t = 0.0f;
    return vy() + HORIZON + (int)((VIEW_H - HORIZON) * t * t);
}

static int roadHalfWidth(int y) {
    int oy = vy();
    int span = max(1, VIEW_H - HORIZON);
    int dy = max(0, y - (oy + HORIZON));
    int w = 14 + (dy * dy * (VIEW_W / 2 - 18)) / (span * span);
    return min(VIEW_W / 2 - 4, max(12, w));
}

static int curveCenter(int y) {
    int ox = vx();
    int oy = vy();
    int span = max(1, VIEW_H - HORIZON);
    int dy = max(0, y - (oy + HORIZON));
    float depth = (float)dy / span;
    float curve = roadCurveAt((1.0f - depth) * 900.0f);
    return ox + VIEW_W / 2 + (int)(curve * depth * depth * VIEW_W * 0.30f);
}

static void resetRival(int i, float zBase) {
    rivals[i].z = zBase + i * 720.0f + random(0, 360);
    rivals[i].lane = (random(0, 3) - 1) * 0.48f + (random(-12, 13) / 100.0f);
    rivals[i].speed = 96.0f + random(0, 80);
    static const uint16_t colors[] = {0xF800, 0x07FF, 0xFFE0, 0xFD20, 0xF81F, 0x001F};
    rivals[i].color = colors[i % 6];
}

static void resetRace() {
    playerX = 0.0f;
    speed = 0.0f;
    distanceRun = 0.0f;
    roadScroll = 0.0f;
    timeLeft = 75;
    lap = 1;
    score = 0;
    raceOver = false;
    lastSecond = millis();
    lastFrame = millis();
    for (int i = 0; i < NUM_RIVALS; i++) resetRival(i, 900.0f);
}

static void drawBackground() {
    int ox = vx();
    int oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, HORIZON, COL_SKY);
    gfx->fillRect(ox, oy + HORIZON / 2, VIEW_W, HORIZON / 2, COL_SKY_LOW);

    int base = oy + HORIZON - 3;
    for (int x = -40; x < VIEW_W + 40; x += 54) {
        int peak = oy + HORIZON - 28 - ((x + (int)(distanceRun * 0.02f)) % 30);
        int sx = ox + x - ((int)(distanceRun * 0.03f) % 54);
        gfx->fillTriangle(sx, base, sx + 34, peak, sx + 82, base, COL_MOUNTAIN_D);
        gfx->fillTriangle(sx + 18, base, sx + 43, peak + 6, sx + 70, base, COL_MOUNTAIN);
    }

    gfx->fillRect(ox, oy + HORIZON, VIEW_W, VIEW_H - HORIZON, COL_GRASS_A);
}

static void drawRoad() {
    int ox = vx();
    int oy = vy();
    int slice = (VIEW_H < 150) ? 3 : 4;
    for (int y = oy + HORIZON; y < oy + VIEW_H; y += slice) {
        int center = curveCenter(y);
        int half = roadHalfWidth(y);
        int left = center - half;
        int right = center + half;
        int row = y - oy;
        bool alt = (((int)(row + roadScroll) / 13) & 1) != 0;
        uint16_t grass = alt ? COL_GRASS_A : COL_GRASS_B;
        uint16_t road = alt ? COL_ROAD_A : COL_ROAD_B;
        uint16_t rumble = alt ? COL_RUMBLE_R : COL_RUMBLE_W;

        gfx->fillRect(ox, y, max(0, left - ox), slice, grass);
        gfx->fillRect(left, y, right - left, slice, road);
        gfx->fillRect(right, y, max(0, ox + VIEW_W - right), slice, grass);
        gfx->fillRect(left, y, max(2, half / 18), slice, rumble);
        gfx->fillRect(right - max(2, half / 18), y, max(2, half / 18), slice, rumble);

        if (half > 34 && (((int)(row + roadScroll) / 28) & 1) == 0) {
            int laneW = max(1, half / 24);
            gfx->fillRect(center - laneW / 2, y, laneW, slice, COL_STRIPE);
        }
    }
}

static void drawRival(const Rival &r) {
    float z = r.z - distanceRun;
    while (z < 60.0f) z += ROAD_LEN;
    if (z > 900.0f) return;

    int y = projectY(z);
    int half = roadHalfWidth(y);
    int center = curveCenter(y);
    int carW = max(5, (int)((900.0f - z) * 0.035f));
    int carH = max(4, carW / 2);
    int x = center + (int)(r.lane * half) - carW / 2;
    int oy = vy();
    if (y < oy + HORIZON || y > oy + VIEW_H) return;

    gfx->fillRect(x, y - carH, carW, carH, r.color);
    gfx->fillRect(x + carW / 5, y - carH - max(2, carH / 3), carW * 3 / 5, max(2, carH / 3), 0x001F);
    gfx->drawFastHLine(x, y - 1, carW, C_BLACK);
}

static void drawPlayerCar() {
    int ox = vx();
    int oy = vy();
    int x = ox + VIEW_W / 2 + (int)(playerX * (VIEW_W * 0.36f));
    int w = (VIEW_W < 260) ? 30 : 42;
    int h = (VIEW_W < 260) ? 21 : 28;
    int y = oy + CAR_Y;
    gfx->fillTriangle(x, y - h, x - w / 2, y, x + w / 2, y, 0xF800);
    gfx->fillRect(x - w / 3, y - h + 7, w * 2 / 3, h - 6, 0xD800);
    gfx->fillRect(x - w / 5, y - h + 9, w * 2 / 5, 6, 0x07FF);
    gfx->fillRect(x - w / 2 - 2, y - 6, 7, 6, C_BLACK);
    gfx->fillRect(x + w / 2 - 5, y - 6, 7, 6, C_BLACK);
    gfx->fillTriangle(x, y - 3, x - 4, y + 8, x + 4, y + 8, 0xFD20);
}

static void drawHud() {
    int ox = vx();
    int oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, 12, C_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(ox + 3, oy + 3);
    gfx->printf("SPD %03d", (int)speed);
    gfx->setCursor(ox + VIEW_W / 2 - 24, oy + 3);
    gfx->printf("T %02d", timeLeft);
    gfx->setCursor(ox + VIEW_W - 70, oy + 3);
    gfx->printf("L%d %05d", lap, score);
}

static bool playerOffRoad() {
    return playerX < -1.05f || playerX > 1.05f;
}

static void updateRivals(float dt) {
    for (int i = 0; i < NUM_RIVALS; i++) {
        Rival &r = rivals[i];
        float rel = r.z - distanceRun;
        while (rel < -80.0f) {
            resetRival(i, distanceRun + 1200.0f + random(0, 1600));
            rel = r.z - distanceRun;
            score += 75;
        }
        r.z += r.speed * dt * 0.45f;
        if (r.z > distanceRun + ROAD_LEN) r.z -= ROAD_LEN;

        if (rel > 18.0f && rel < 90.0f && fabsf(playerX - r.lane) < 0.22f && speed > 70.0f) {
            speed *= 0.52f;
            score = max(0, score - 150);
            playerX += (playerX < r.lane) ? -0.18f : 0.18f;
        }
    }
}

static bool waitRaceOver() {
    int ox = vx();
    int oy = vy();
    int boxW = min(174, VIEW_W - 18);
    int boxX = ox + (VIEW_W - boxW) / 2;
    int boxY = oy + VIEW_H / 2 - 26;
    gfx->fillRect(boxX, boxY, boxW, 54, C_DARK);
    gfx->drawRect(boxX, boxY, boxW, 54, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(C_RED);
    gfx->setCursor(boxX + 18, boxY + 10);
    gfx->printf("TIME UP  SCORE %d", score);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(boxX + 16, boxY + 32);
    gfx->print("R/START RETRY  Q EXIT");
    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return false;
        if (input.start || input.key == 'r' || input.key == 'R') return true;
        delay(30);
        yield();
    }
}

static void drawTitle() {
    int ox = vx();
    int oy = vy();
    gfx->fillRect(ox, oy, VIEW_W, VIEW_H, C_BLACK);
    drawBackground();
    drawRoad();
    gfx->setTextSize((VIEW_W < 260) ? 1 : 2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(ox + VIEW_W / 2 - ((VIEW_W < 260) ? 42 : 84), oy + 22);
    gfx->print((VIEW_W < 260) ? "POLE RUN" : "POLE POSITION");
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(ox + VIEW_W / 2 - 61, oy + VIEW_H - 28);
    gfx->print("START/B TO RACE");
    gfx->setTextColor(C_GREY);
    gfx->setCursor(ox + VIEW_W / 2 - 76, oy + VIEW_H - 16);
    gfx->print("A/O accel  Z/down brake");
}

void run_pole_position() {
    resetRace();
    drawTitle();
    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return;
        if (input.start || input.a || input.b) break;
        delay(24);
        yield();
    }

    while (true) {
        resetRace();
        while (!raceOver) {
            uint32_t now = millis();
            float dt = (now - lastFrame) / 1000.0f;
            if (dt <= 0.0f || dt > 0.08f) dt = 0.016f;
            lastFrame = now;

            PMNesInput input = pm_read_nes_input(true);
            if (input.quit) return;
            if (input.start) {
                delay(160);
                continue;
            }

            if (input.up || input.a) speed += 118.0f * dt;
            else speed -= 22.0f * dt;
            if (input.down || input.b) speed -= 150.0f * dt;
            speed = min(246.0f, max(0.0f, speed));

            float steer = 0.0f;
            if (input.left) steer -= 1.0f;
            if (input.right) steer += 1.0f;
            playerX += steer * (0.95f + speed / 240.0f) * dt;
            playerX -= roadCurveAt(0.0f) * (speed / 240.0f) * 0.44f * dt;
            if (!input.left && !input.right) playerX *= 0.996f;
            playerX = max(-1.45f, min(1.45f, playerX));

            if (playerOffRoad()) speed -= 85.0f * dt;
            if (speed < 0.0f) speed = 0.0f;

            distanceRun += speed * dt * 1.85f;
            roadScroll += speed * dt * 0.55f;
            if (roadScroll > 500.0f) roadScroll -= 500.0f;
            if (distanceRun > lap * ROAD_LEN) {
                lap++;
                timeLeft += 35;
                score += 2000;
            }
            score += (int)(speed * dt);
            updateRivals(dt);

            if (now - lastSecond >= 1000) {
                lastSecond += 1000;
                if (--timeLeft <= 0) raceOver = true;
            }

            drawBackground();
            drawRoad();
            for (int i = 0; i < NUM_RIVALS; i++) drawRival(rivals[i]);
            drawPlayerCar();
            drawHud();
            delay(16);
            yield();
        }
        if (!waitRaceOver()) return;
    }
}
