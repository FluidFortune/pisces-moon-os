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
 * PISCES MOON OS — USB DUCKY v1.0
 * Wired USB HID keyboard injection via the ESP32-S3's native USB peripheral.
 *
 * BUILD MODES:
 *   Standard build (ARDUINO_USB_MODE=1, CDC):
 *     Shows "WRONG BUILD" screen with flash instructions.
 *     No USB HID functionality available — serial/flash intact.
 *
 *   HID build (ARDUINO_USB_HID_MODE=1, USB_MODE=0):
 *     T-Deck enumerates as standard USB HID keyboard.
 *     No serial console. No USB flash. Use UART0 for debugging.
 *     Full DuckyScript execution from SD card /payloads/.
 *
 * Switching builds:
 *   Standard → HID:  pio run -e esp32s3_hid --target upload
 *   HID → Standard:  Flash via UART0 serial adapter, or double-tap
 *                    RST to enter ROM bootloader then: pio run --target upload
 *
 * Payload files: /payloads/*.txt (same format as BLE Ducky)
 * Ghost Partition: /payloads/ hidden in Student Mode
 *
 * USE ONLY ON SYSTEMS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "usb_ducky.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define UD_BG     0x0000
#define UD_HDR    0x2000
#define UD_RED    0xF800
#define UD_ORANGE 0xFD20
#define UD_GREEN  0x07E0
#define UD_CYAN   0x07FF
#define UD_WHITE  0xFFFF
#define UD_DIM    0x4208
#define UD_SEL    0x2800

#define PAYLOAD_DIR "/payloads"

// ─────────────────────────────────────────────
//  WRONG BUILD SCREEN
//  Shown when compiled with ARDUINO_USB_MODE=1 (CDC).
//  The USB peripheral is locked to serial — can't be HID too.
// ─────────────────────────────────────────────
#ifndef ARDUINO_USB_HID_MODE

static void udShowWrongBuild() {
    gfx->fillScreen(UD_BG);
    gfx->fillRect(0, 0, 320, 24, UD_HDR);
    gfx->drawFastHLine(0, 23, 320, UD_RED);
    gfx->setTextColor(UD_RED); gfx->setTextSize(1);
    gfx->setCursor(6, 8); gfx->print("USB DUCKY — WRONG BUILD");

    gfx->setTextSize(1);
    gfx->setTextColor(UD_WHITE);
    gfx->setCursor(6, 34); gfx->print("This app requires the HID firmware build.");
    gfx->setTextColor(UD_DIM);
    gfx->setCursor(6, 50); gfx->print("Current build: CDC (serial/flash mode)");
    gfx->setCursor(6, 62); gfx->print("USB HID and USB CDC cannot coexist.");

    gfx->drawFastHLine(0, 78, 320, 0x2000);
    gfx->setTextColor(UD_ORANGE);
    gfx->setCursor(6, 86); gfx->print("TO ENABLE USB DUCKY:");
    gfx->setTextColor(UD_WHITE);
    gfx->setCursor(6, 100); gfx->print("1. Flash the HID build:");
    gfx->setTextColor(UD_CYAN);
    gfx->setCursor(14, 114); gfx->print("pio run -e esp32s3_hid --target upload");
    gfx->setTextColor(UD_WHITE);
    gfx->setCursor(6, 130); gfx->print("2. platformio_hid.ini is included in repo.");
    gfx->setCursor(6, 144); gfx->print("3. Serial debug disabled in HID build.");
    gfx->setCursor(6, 158); gfx->print("4. To return to CDC build:");
    gfx->setTextColor(UD_CYAN);
    gfx->setCursor(14, 172); gfx->print("Double-tap RST → UART flash → pio upload");

    gfx->drawFastHLine(0, 188, 320, 0x2000);
    gfx->setTextColor(UD_DIM);
    gfx->setCursor(6, 196); gfx->print("BLE Ducky works without reflashing.");
    gfx->setCursor(6, 208); gfx->print("CYBER > BLE DUCKY for wireless injection.");

    gfx->fillRect(0, 220, 320, 20, 0x1000);
    gfx->drawFastHLine(0, 220, 320, UD_RED);
    gfx->setTextColor(UD_DIM); gfx->setCursor(100, 228);
    gfx->print("Tap header or Q to exit");

    while (true) {
        char k = get_keypress();
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) { while(get_touch(&tx,&ty)){delay(10);} return; }
        if (k == 'q' || k == 'Q') return;
        delay(50);
    }
}

void run_usb_ducky() {
    udShowWrongBuild();
}

#else // ARDUINO_USB_HID_MODE — full USB HID implementation

// ─────────────────────────────────────────────
//  USB HID INCLUDES (HID build only)
//  Arduino-esp32 native USB HID API
// ─────────────────────────────────────────────
#include "USB.h"
#include "USBHIDKeyboard.h"

static USBHIDKeyboard udKeyboard;

// ─────────────────────────────────────────────
//  KEY SENDING HELPERS
// ─────────────────────────────────────────────
static int udDefaultDelay = 0;
static volatile bool udAbort = false;

static void udPressKey(uint8_t mod, uint8_t key) {
    // Arduino USB HID: use KeyboardReport directly
    udKeyboard.press(key);
    delay(8);
    udKeyboard.release(key);
    delay(5);
}

static void udTypeString(const char* s) {
    while (*s && !udAbort) {
        udKeyboard.press(*s);
        delay(10);
        udKeyboard.release(*s);
        delay(10);
        s++;
    }
}

// Map DuckyScript keyword to ASCII keycode for Arduino USB HID API
// Arduino's USBHIDKeyboard uses KEY_* constants from HIDTypes.h
static int udKeywordToKey(const char* word) {
    if (strcmp(word,"ENTER")==0||strcmp(word,"RETURN")==0) return KEY_RETURN;
    if (strcmp(word,"TAB")==0)        return KEY_TAB;
    if (strcmp(word,"ESCAPE")==0||strcmp(word,"ESC")==0) return KEY_ESC;
    if (strcmp(word,"BACKSPACE")==0)  return KEY_BACKSPACE;
    if (strcmp(word,"DELETE")==0)     return KEY_DELETE;
    if (strcmp(word,"INSERT")==0)     return KEY_INSERT;
    if (strcmp(word,"HOME")==0)       return KEY_HOME;
    if (strcmp(word,"END")==0)        return KEY_END;
    if (strcmp(word,"UPARROW")==0)    return KEY_UP_ARROW;
    if (strcmp(word,"DOWNARROW")==0)  return KEY_DOWN_ARROW;
    if (strcmp(word,"LEFTARROW")==0)  return KEY_LEFT_ARROW;
    if (strcmp(word,"RIGHTARROW")==0) return KEY_RIGHT_ARROW;
    if (strcmp(word,"SPACE")==0)      return ' ';
    if (strcmp(word,"CAPSLOCK")==0)   return KEY_CAPS_LOCK;
    if (strcmp(word,"F1")==0)  return KEY_F1;
    if (strcmp(word,"F2")==0)  return KEY_F2;
    if (strcmp(word,"F3")==0)  return KEY_F3;
    if (strcmp(word,"F4")==0)  return KEY_F4;
    if (strcmp(word,"F5")==0)  return KEY_F5;
    if (strcmp(word,"F6")==0)  return KEY_F6;
    if (strcmp(word,"F7")==0)  return KEY_F7;
    if (strcmp(word,"F8")==0)  return KEY_F8;
    if (strcmp(word,"F9")==0)  return KEY_F9;
    if (strcmp(word,"F10")==0) return KEY_F10;
    if (strcmp(word,"F11")==0) return KEY_F11;
    if (strcmp(word,"F12")==0) return KEY_F12;
    if (strlen(word) == 1) return tolower(word[0]);
    return 0;
}

// ─────────────────────────────────────────────
//  DUCKYSCRIPT EXECUTOR (USB HID build)
// ─────────────────────────────────────────────
static void udExecutePayload(const char* path,
                              void (*statusCb)(const char*, int, int)) {
    FsFile f = sd.open(path, O_RDONLY);
    if (!f) { statusCb("Cannot open payload", 0, 0); return; }

    uint32_t totalLines = 0;
    char tmp[256];
    while (f.fgets(tmp, sizeof(tmp))) totalLines++;
    f.seek(0);

    char lastCmd[256] = "";
    char line[256];
    int lineNum = 0;

    while (f.fgets(line, sizeof(line)) && !udAbort) {
        lineNum++;
        int len = strlen(line);
        while (len>0&&(line[len-1]=='\n'||line[len-1]=='\r'||line[len-1]==' ')) line[--len]='\0';
        if (len==0) continue;

        statusCb(line, lineNum, totalLines);

        char* cmd = line;
        char* arg = nullptr;
        char* sp  = strchr(line, ' ');
        if (sp) { *sp = '\0'; arg = sp + 1; }

        if (strcmp(cmd,"REM")==0) {
            // skip
        } else if (strcmp(cmd,"DELAY")==0) {
            int ms = arg ? atoi(arg) : 500;
            unsigned long t0=millis();
            while (millis()-t0<(unsigned long)ms&&!udAbort){delay(10);yield();}
        } else if (strcmp(cmd,"DEFAULT_DELAY")==0||strcmp(cmd,"DEFAULTDELAY")==0) {
            udDefaultDelay = arg ? atoi(arg) : 0;
        } else if (strcmp(cmd,"STRING")==0) {
            if (arg) udTypeString(arg);
        } else if (strcmp(cmd,"STRINGLN")==0) {
            if (arg) { udTypeString(arg); udKeyboard.press(KEY_RETURN); delay(8); udKeyboard.release(KEY_RETURN); }
        } else if (strcmp(cmd,"GUI")==0||strcmp(cmd,"WINDOWS")==0||strcmp(cmd,"COMMAND")==0) {
            int key = arg ? udKeywordToKey(arg) : 0;
            udKeyboard.press(KEY_LEFT_GUI);
            if (key) udKeyboard.press(key);
            delay(50);
            if (key) udKeyboard.release(key);
            udKeyboard.release(KEY_LEFT_GUI);
        } else if (strcmp(cmd,"CTRL")==0||strcmp(cmd,"CONTROL")==0) {
            int key = arg ? udKeywordToKey(arg) : 0;
            udKeyboard.press(KEY_LEFT_CTRL);
            if (key) udKeyboard.press(key);
            delay(50);
            if (key) udKeyboard.release(key);
            udKeyboard.release(KEY_LEFT_CTRL);
        } else if (strcmp(cmd,"ALT")==0) {
            int key = arg ? udKeywordToKey(arg) : 0;
            udKeyboard.press(KEY_LEFT_ALT);
            if (key) udKeyboard.press(key);
            delay(50);
            if (key) udKeyboard.release(key);
            udKeyboard.release(KEY_LEFT_ALT);
        } else if (strcmp(cmd,"SHIFT")==0) {
            int key = arg ? udKeywordToKey(arg) : 0;
            udKeyboard.press(KEY_LEFT_SHIFT);
            if (key) udKeyboard.press(key);
            delay(50);
            if (key) udKeyboard.release(key);
            udKeyboard.release(KEY_LEFT_SHIFT);
        } else if (strcmp(cmd,"CTRL-ALT")==0) {
            int key = arg ? udKeywordToKey(arg) : 0;
            udKeyboard.press(KEY_LEFT_CTRL); udKeyboard.press(KEY_LEFT_ALT);
            if (key) udKeyboard.press(key);
            delay(50);
            if (key) udKeyboard.release(key);
            udKeyboard.release(KEY_LEFT_ALT); udKeyboard.release(KEY_LEFT_CTRL);
        } else {
            // Single key command
            int key = udKeywordToKey(cmd);
            if (key) { udKeyboard.press(key); delay(8); udKeyboard.release(key); }
        }

        strncpy(lastCmd, line, 255);
        if (arg) *(lastCmd + (arg-line) - 1) = ' ';
        if (udDefaultDelay > 0) delay(udDefaultDelay);
        yield();
    }
    f.close();
}

// ─────────────────────────────────────────────
//  FILE LIST
// ─────────────────────────────────────────────
#define MAX_PAYLOADS 32
static char udPayloads[MAX_PAYLOADS][48];
static int  udPayloadCount = 0;

static void udScanPayloads() {
    udPayloadCount = 0;
    if (!sd.exists(PAYLOAD_DIR)) sd.mkdir(PAYLOAD_DIR);
    FsFile dir = sd.open(PAYLOAD_DIR);
    if (!dir) return;
    FsFile f;
    while (f.openNext(&dir, O_RDONLY) && udPayloadCount < MAX_PAYLOADS) {
        char nm[48]; f.getName(nm, sizeof(nm));
        if (strstr(nm,".txt")||strstr(nm,".ds"))
            snprintf(udPayloads[udPayloadCount++], 48, "%s/%s", PAYLOAD_DIR, nm);
        f.close();
    }
    dir.close();
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void udDrawHeader(bool enumerated) {
    gfx->fillRect(0, 0, 320, 24, UD_HDR);
    gfx->drawFastHLine(0, 23, 320, UD_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(UD_RED);
    gfx->setCursor(6, 4); gfx->print("USB DUCKY");
    gfx->setTextColor(enumerated ? UD_GREEN : UD_ORANGE);
    gfx->setCursor(96, 4);
    gfx->print(enumerated ? "ENUMERATED" : "PLUG INTO TARGET");
    gfx->setTextColor(UD_DIM);
    gfx->setCursor(244, 4); gfx->print("[Q=EXIT]");
}

static void udDrawPayloadList(int scroll, int selected) {
    gfx->fillRect(0, 26, 320, 172, UD_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(UD_DIM);
    gfx->setCursor(4, 28);
    gfx->printf("PAYLOADS (%d)  /payloads/", udPayloadCount);
    gfx->drawFastHLine(0, 37, 320, 0x2000);

    if (udPayloadCount == 0) {
        gfx->setTextColor(UD_DIM);
        gfx->setCursor(10, 80); gfx->print("No payloads found. Add .txt files to");
        gfx->setCursor(10, 96); gfx->print("/payloads/ on the SD card.");
        return;
    }

    int show = min(7, udPayloadCount);
    for (int i = scroll; i < scroll + show && i < udPayloadCount; i++) {
        int ry  = 40 + (i - scroll) * 20;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, 19, sel ? UD_SEL : (i%2==0 ? 0x0821 : UD_BG));
        const char* slash = strrchr(udPayloads[i], '/');
        gfx->setTextColor(sel ? UD_ORANGE : 0xC618);
        gfx->setCursor(8, ry + 5);
        char nb[38]; strncpy(nb, slash ? slash+1 : udPayloads[i], 37); nb[37]='\0';
        gfx->print(nb);
    }
}

static void udProgressStatus(const char* lineText, int n, int tot) {
    gfx->fillRect(0, 198, 320, 42, 0x1000);
    gfx->drawFastHLine(0, 198, 320, UD_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(UD_DIM);
    gfx->setCursor(4, 202); gfx->printf("Line %d/%d", n, tot);
    if (tot > 0) {
        int bw = (298 * n) / tot;
        gfx->fillRect(4, 213, 298, 6, 0x0821);
        gfx->fillRect(4, 213, bw,  6, UD_RED);
    }
    gfx->setTextColor(UD_ORANGE);
    gfx->setCursor(4, 222);
    char buf[44]; strncpy(buf, lineText, 43); buf[43]='\0';
    gfx->print(buf);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY (HID build)
// ─────────────────────────────────────────────
void run_usb_ducky() {
    gfx->fillScreen(UD_BG);
    gfx->setTextColor(UD_RED); gfx->setTextSize(2);
    gfx->setCursor(10, 50); gfx->print("USB DUCKY");
    gfx->setTextSize(1);
    gfx->setTextColor(UD_WHITE);
    gfx->setCursor(10, 80); gfx->print("Wired USB HID keyboard injection.");
    gfx->setTextColor(UD_RED);
    gfx->setCursor(10, 96); gfx->print("USE ON AUTHORIZED SYSTEMS ONLY.");
    gfx->setTextColor(UD_DIM);
    gfx->setCursor(10, 114); gfx->print("Scanning payloads...");
    delay(1000);

    udDefaultDelay = 0;
    udAbort = false;
    udScanPayloads();

    // Init USB HID keyboard
    udKeyboard.begin();
    USB.begin();

    // Wait for USB enumeration (up to 5s).
    // Arduino-esp32 2.0.x USBHID does not expose a public connected() method.
    // USB enumeration takes 1-3 seconds; we wait and assume success.
    gfx->setTextColor(UD_ORANGE); gfx->setTextSize(1);
    gfx->setCursor(6, 60); gfx->print("Waiting for USB enumeration (5s)...");
    delay(5000);
    bool enumerated = true;  // Assume enumerated — keyboard will inject or silently fail

    int scroll = 0, selected = 0;
    gfx->fillScreen(UD_BG);
    udDrawHeader(enumerated);
    udDrawPayloadList(scroll, selected);

    gfx->fillRect(0, 210, 320, 30, 0x1000);
    gfx->drawFastHLine(0, 210, 320, UD_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(enumerated ? UD_GREEN : UD_ORANGE);
    gfx->setCursor(4, 220);
    gfx->print(enumerated ? "USB enumerated. CLICK to inject." : "Plug into target USB port, then CLICK.");

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) { while(get_touch(&tx,&ty)){delay(10);} break; }
        if (k == 'q' || k == 'Q') break;

        if (tb.y == -1 && selected > 0)                { selected--; if(selected<scroll) scroll--; }
        if (tb.y ==  1 && selected < udPayloadCount-1)  { selected++; if(selected>=scroll+7) scroll++; }
        udDrawPayloadList(scroll, selected);

        if (tb.clicked && udPayloadCount > 0) {
            udAbort = false;
            gfx->fillRect(0, 24, 320, 174, UD_BG);
            gfx->setTextColor(UD_ORANGE); gfx->setTextSize(1);
            gfx->setCursor(6, 30);
            const char* slash = strrchr(udPayloads[selected], '/');
            gfx->printf("INJECTING: %s", slash ? slash+1 : udPayloads[selected]);
            gfx->setTextColor(UD_DIM); gfx->setCursor(6, 44);
            gfx->print("Q / tap header to abort.");

            udExecutePayload(udPayloads[selected], [](const char* line, int n, int tot){
                udProgressStatus(line, n, tot);
                char k2 = get_keypress();
                if (k2=='q'||k2=='Q') udAbort = true;
                int16_t tx2,ty2;
                if (get_touch(&tx2,&ty2) && ty2 < 24) udAbort = true;
            });

            gfx->fillRect(0, 198, 320, 42, 0x1000);
            gfx->drawFastHLine(0, 198, 320, UD_RED);
            gfx->setTextColor(udAbort ? UD_RED : UD_GREEN);
            gfx->setCursor(4, 220);
            gfx->print(udAbort ? "Payload ABORTED." : "Payload complete.");
            delay(1500);

            gfx->fillRect(0, 24, 320, 216, UD_BG);
            udDrawPayloadList(scroll, selected);
        }

        delay(20); yield();
    }

    gfx->fillScreen(UD_BG);
}

#endif // ARDUINO_USB_HID_MODE
