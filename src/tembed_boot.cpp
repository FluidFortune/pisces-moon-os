// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  PISCES MOON OS — T-EMBED CC1101 BOOT + LAUNCHER
//
//  First-boot bring-up for the LilyGO T-Embed CC1101 Plus:
//  ESP32-S3 (8MB PSRAM, 16MB flash), 1.9" ST7789 320x170 landscape,
//  rotary encoder (no touch, no keyboard), CC1101 sub-GHz + nRF24 +
//  PN532 NFC + IR, all radios on one shared SPI bus.
//
//  Like the C5 / C28P / Maxine kiosks, this device owns its own
//  boot + launcher rather than the trackball/touch launcher used by
//  the T-Deck. loop() dispatches to tembed_launcher() (never returns).
//
//  Navigation is scroll-only: rotate the encoder to move the
//  highlight, push the encoder to open, side key to go back.
//
//  BRING-UP CHECKLIST (what this file proves at first boot):
//    1. Serial over USB-CDC
//    2. Board power rail enabled (GPIO15 HIGH)
//    3. Shared SPI bus up (SCK=11 MOSI=9 MISO=10)
//    4. ST7789 responds (CS=41 DC=16), backlight on (GPIO21)
//    5. Encoder turns move the highlight; push opens
//    6. CC1101 analyzer opens and reports RX
// ─────────────────────────────────────────────

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <Wire.h>
#include "soc/gpio_reg.h"
#include <Arduino_GFX_Library.h>
#include "hal_pins.h"
#include "tembed_input.h"
#include "tembed_ui.h"
#include "cc1101_analyzer.h"
#include "nrf24_scan.h"
#include "galaga.h"

extern Arduino_GFX* gfx;

// ─────────────────────────────────────────────
//  INPUT — rotary encoder + two buttons (tembed_input.h)
// ─────────────────────────────────────────────
namespace {

// Quadrature decode table indexed by (prevAB<<2 | AB).
const int8_t kQuadTable[16] = {
    0, -1,  1,  0,
    1,  0,  0, -1,
   -1,  0,  0,  1,
    0,  1, -1,  0
};
// Decoded in a pin-change interrupt so no step is lost however slowly an
// app polls (Galaga reads input once per frame; some screens block for a
// moment). The ISR reads both pins straight from GPIO_IN_REG and folds the
// transition into s_accum; tembed_encoder_delta() drains whole detents.
volatile uint8_t s_prevAB = 0;
volatile int     s_accum  = 0;
portMUX_TYPE     s_encMux = portMUX_INITIALIZER_UNLOCKED;

inline uint8_t readAB() {
    uint32_t in = REG_READ(GPIO_IN_REG);           // GPIO 0..31
    return (uint8_t)((((in >> ENCODER_A) & 1) << 1) | ((in >> ENCODER_B) & 1));
}

void IRAM_ATTR onEncoderEdge() {
    uint8_t ab = readAB();
    portENTER_CRITICAL_ISR(&s_encMux);
    s_accum += kQuadTable[(s_prevAB << 2) | ab];
    s_prevAB = ab;
    portEXIT_CRITICAL_ISR(&s_encMux);
}

struct Btn {
    uint8_t pin;
    bool    stable;        // true = released (pulled up)
    bool    lastRead;
    uint32_t tChange;
    bool    edge;          // latched press, cleared on read
};
Btn s_sel  = { ENCODER_BTN,    true, true, 0, false };
Btn s_back = { BOARD_USER_KEY, true, true, 0, false };

void pollBtn(Btn& b) {
    bool r = digitalRead(b.pin);     // active-low (pressed = LOW)
    if (r != b.lastRead) { b.lastRead = r; b.tChange = millis(); }
    if (millis() - b.tChange > 25 && r != b.stable) {
        b.stable = r;
        if (b.stable == false) b.edge = true;   // just pressed
    }
}
bool takeEdge(Btn& b) { bool e = b.edge; b.edge = false; return e; }

} // namespace

void tembed_input_init() {
    pinMode(ENCODER_A,   INPUT_PULLUP);
    pinMode(ENCODER_B,   INPUT_PULLUP);
    pinMode(ENCODER_BTN, INPUT_PULLUP);
    pinMode(BOARD_USER_KEY, INPUT_PULLUP);
    portENTER_CRITICAL(&s_encMux);
    s_prevAB = readAB();
    s_accum = 0;
    portEXIT_CRITICAL(&s_encMux);
    attachInterrupt(digitalPinToInterrupt(ENCODER_A), onEncoderEdge, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENCODER_B), onEncoderEdge, CHANGE);
    Serial.println("[TEMBED] Encoder (interrupt) + buttons ready");
}

int tembed_encoder_delta() {
    int detents = 0;
    portENTER_CRITICAL(&s_encMux);
    while (s_accum >= 4)  { detents++; s_accum -= 4; }
    while (s_accum <= -4) { detents--; s_accum += 4; }
    portEXIT_CRITICAL(&s_encMux);
    return detents;
}

bool tembed_btn_select() { pollBtn(s_sel);  return takeEdge(s_sel);  }
bool tembed_btn_back()   { pollBtn(s_back); return takeEdge(s_back); }

bool tembed_btn_select_down() { return digitalRead(ENCODER_BTN)    == LOW; }
bool tembed_btn_back_down()   { return digitalRead(BOARD_USER_KEY) == LOW; }

void tembed_input_flush() {
    // Bring the debouncers up to date with the pins as they are now, so a
    // press that is still held doesn't register as a new edge later.
    for (int i = 0; i < 3; i++) { pollBtn(s_sel); pollBtn(s_back); delay(10); }
    s_sel.stable  = s_sel.lastRead;
    s_back.stable = s_back.lastRead;
    s_sel.edge = s_back.edge = false;
    portENTER_CRITICAL(&s_encMux);
    s_prevAB = readAB();
    s_accum  = 0;
    portEXIT_CRITICAL(&s_encMux);
}

// ─────────────────────────────────────────────
//  SPLASH
// ─────────────────────────────────────────────
void tembed_splash() {
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0x07FF);
    gfx->setTextSize(3);
    gfx->setCursor(30, 40);
    gfx->print("PISCES MOON");
    gfx->setTextColor(0xFD20);
    gfx->setTextSize(2);
    gfx->setCursor(30, 80);
    gfx->print("T-Embed CC1101");
    gfx->setTextColor(0x4208);
    gfx->setTextSize(1);
    gfx->setCursor(30, 110);
    gfx->printf("PSRAM %u KB  free heap %u KB",
                (unsigned)(ESP.getPsramSize() / 1024),
                (unsigned)(ESP.getFreeHeap() / 1024));
    delay(1200);
}

// ─────────────────────────────────────────────
//  SETUP HOOK (called from main.cpp setup(), after display init)
// ─────────────────────────────────────────────
void tembed_setup() {
    tembed_input_init();
    // I2C: PN532 NFC (0x24), BQ27220 gauge (0x55), BQ25896 charger (0x6B).
    // 100 kHz with a short timeout, per LilyGO's PN532 example.
    Wire.begin(I2C_SDA, I2C_SCL, 100000);
    Wire.setTimeOut(20);
    tembed_time_apply_tz();      // saved timezone (Clock app), for when NTP sets the time
    Serial.printf("[TEMBED] setup: PSRAM %u bytes, free heap %u\n",
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());
    tembed_splash();
}

// ─────────────────────────────────────────────
//  LAUNCHER — scroll list, encoder driven
// ─────────────────────────────────────────────
namespace {

// A menu entry. run==nullptr marks a not-yet-implemented app: selecting
// it shows a "coming soon" card (with the note) that exits on the side key.
struct MenuItem { const char* name; const char* note; void (*run)(); };

void app_about() {
    tembed_input_flush();
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0x07FF); gfx->setTextSize(2);
    gfx->setCursor(6, 8);   gfx->print("T-Embed CC1101");
    gfx->setTextColor(0xFFFF); gfx->setTextSize(1);
    gfx->setCursor(6, 40);  gfx->print("Pisces Moon OS - special edition");
    gfx->setCursor(6, 56);  gfx->print("ESP32-S3  8MB PSRAM  16MB flash");
    gfx->setCursor(6, 72);  gfx->print("CC1101 sub-GHz + nRF24 + PN532 + IR");
    gfx->setCursor(6, 88);  gfx->print("320x170 ST7789, encoder only");
    gfx->setCursor(6, 104); gfx->printf("free heap %u KB  psram %u KB",
                (unsigned)(ESP.getFreeHeap()/1024),
                (unsigned)(ESP.getPsramSize()/1024));
    gfx->setTextColor(0x4208);
    gfx->setCursor(6, 150); gfx->print("SIDE = back");
    while (!tembed_btn_back()) { delay(10); }
}

// Galaga reads the encoder through game_input.cpp (raw button state for
// autofire / hold-to-quit), so settle the buttons before handing back.
void app_galaga() {
    tembed_input_flush();
    run_galaga();
    while (tembed_btn_back_down() || tembed_btn_select_down()) delay(5);
    tembed_input_flush();
}

const MenuItem kMenu[] = {
    { "CC1101 Analyzer",  "Sub-GHz RX: band scan, RSSI, OOK capture",    cc1101_analyzer_run },
    { "nRF24 Scan",       "2.4 GHz channel activity (nRF24)",            nrf24_scan_run },
    { "NFC Read",         "Read tag UID / NDEF via PN532",              tembed_nfc_run },
    { "WiFi Join",        "Connect to a WiFi network",                  tembed_wifi_run },
    { "BLE Radar",        "Nearby BLE devices and signal strength",     tembed_ble_radar_run },
    { "Stationary Survey","Log WiFi + BLE + sub-GHz at one spot",       tembed_survey_run },
    { "Galaga",           "Encoder-driven arcade",                      app_galaga },
    { "Clock",            "Time and battery",                           tembed_clock_run },
    { "Hardware Test",    "Check every radio, bus and sensor",          tembed_selftest_run },
    { "About",            "Device info",                               app_about },
};
const int kMenuCount = sizeof(kMenu) / sizeof(kMenu[0]);

// Placeholder screen for an app that isn't built yet. Exits on side key.
void app_coming_soon(const char* name, const char* note) {
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0x07FF); gfx->setTextSize(2);
    gfx->setCursor(6, 10); gfx->print(name);
    gfx->setTextColor(0xFD20); gfx->setTextSize(1);
    gfx->setCursor(6, 44); gfx->print("COMING SOON");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(6, 68); gfx->print(note);
    gfx->setTextColor(0x4208);
    gfx->setCursor(6, 96);  gfx->print("Compiled in, not yet wired to the");
    gfx->setCursor(6, 108); gfx->print("encoder. We'll make it real next.");
    gfx->setCursor(6, 150); gfx->print("SIDE = back");
    // Drain the select edge that launched this, then wait for back.
    tembed_btn_select();
    while (!tembed_btn_back()) { delay(10); }
}

// Scrollable list geometry (320x170): header, then a window of rows.
const int kRowH    = 22;
const int kTopY    = 34;
const int kVisible = 5;
int s_menuSel = 0;
int s_top = 0;

void drawMenu() {
    // Keep the selection inside the visible window.
    if (s_menuSel < s_top)              s_top = s_menuSel;
    if (s_menuSel >= s_top + kVisible)  s_top = s_menuSel - kVisible + 1;

    gfx->fillScreen(0x0000);
    gfx->setTextColor(0x7BEF); gfx->setTextSize(2);
    gfx->setCursor(6, 6); gfx->print("PISCES MOON  /  T-Embed");
    gfx->drawFastHLine(0, 28, 320, 0x4208);

    for (int r = 0; r < kVisible; r++) {
        int idx = s_top + r;
        if (idx >= kMenuCount) break;
        int y = kTopY + r * kRowH;
        bool sel = (idx == s_menuSel);
        if (sel) gfx->fillRect(4, y - 3, 312, kRowH, 0x001F);
        bool stub = (kMenu[idx].run == nullptr);
        uint16_t c = sel ? 0xFFFF : (stub ? 0x52AA : 0x7BEF);
        gfx->setTextColor(c); gfx->setTextSize(2);
        gfx->setCursor(14, y); gfx->print(kMenu[idx].name);
    }

    // Up/down scroll hints.
    gfx->setTextColor(0x4208); gfx->setTextSize(1);
    if (s_top > 0)                         { gfx->setCursor(300, kTopY);      gfx->print("^"); }
    if (s_top + kVisible < kMenuCount)     { gfx->setCursor(300, 150);        gfx->print("v"); }
    gfx->setCursor(6, 158); gfx->print("TURN=scroll   PUSH=open   SIDE=back");
}

} // namespace

void tembed_launcher() {
    drawMenu();
    for (;;) {
        int d = tembed_encoder_delta();
        if (d != 0) {
            s_menuSel += (d > 0 ? 1 : -1);
            if (s_menuSel < 0)            s_menuSel = kMenuCount - 1;
            if (s_menuSel >= kMenuCount)  s_menuSel = 0;
            drawMenu();
        }
        if (tembed_btn_select()) {
            if (kMenu[s_menuSel].run) kMenu[s_menuSel].run();
            else                  app_coming_soon(kMenu[s_menuSel].name, kMenu[s_menuSel].note);
            drawMenu();
        }
        tembed_btn_back();     // ignored at the home screen
        delay(5);
    }
}

#endif // DEVICE_TEMBED_CC1101
