// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include <WiFi.h>
#include "keyboard.h"
#include "pm_input.h"
#include "touch.h"
#include "theme.h"
#include "apps.h"
#include "wifi_manager.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 135;
#else
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
#endif

char map_top_row_to_num(char c) {
    String topRow = "qwertyuio";
    int idx = topRow.indexOf(c);
    if (idx != -1) return (char)('1' + idx);
    return c;
}

void run_wifi_connect() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
#ifdef DEVICE_TLORAPAGER
    gfx->print("WIFI JOIN | USE 1-8 | Q EXIT");
#else
    gfx->print("WIFI JOIN | USE Q-I | TAP HEADER EXIT");
#endif

    gfx->setCursor(10, 35);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(C_CYAN);
        gfx->print("Active: " + WiFi.SSID());
    } else {
        gfx->setTextColor(C_GREY);
        gfx->print("Status: Disconnected");
    }

    gfx->setCursor(10, 55); gfx->setTextColor(C_WHITE);
    gfx->print("Scanning...");

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true, true);
        delay(100);
        WiFi.mode(WIFI_OFF);
        delay(100);
    }
    WiFi.mode(WIFI_STA);
    delay(100);

    int n = WiFi.scanNetworks(false, false);
    gfx->fillRect(0, 25, DISP_W, DISP_H - 25, C_BLACK);

    if (n <= 0) {
        gfx->setCursor(10, 50); gfx->setTextColor(C_RED);
        gfx->print("No networks found. Try again.");
        delay(2000); return;
    }

    int limit = (n > 8) ? 8 : n;
    String cachedSSIDs[8];

    gfx->setCursor(10, 30);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(C_CYAN); gfx->print("Active: " + WiFi.SSID());
    }

    // SSID column scales to available width after "[N] -RSS dBm "
    const int ssidMax = (DISP_W - 140) / 6;

    for (int i = 0; i < limit; i++) {
        cachedSSIDs[i] = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        uint16_t sigColor = (rssi > -70) ? C_GREEN : (rssi > -85 ? 0xFD20 : C_RED);

        gfx->setCursor(10, 50 + (i * 20));
        gfx->setTextColor(C_WHITE);
        gfx->printf("[%d] ", i + 1);

        gfx->setTextColor(sigColor);
        gfx->printf("%3d dBm ", rssi);

        gfx->setTextColor(C_WHITE);
        String ssid = cachedSSIDs[i];
        if ((int)ssid.length() > ssidMax) ssid = ssid.substring(0, ssidMax);
        gfx->print(ssid);
    }

    int selection = -1;
    while (true) {
        char c = get_keypress();
        if (pm_is_exit_key(c)) return;
        char mapped = map_top_row_to_num(c);
        if (mapped >= '1' && mapped < '1' + limit) {
            selection = mapped - '1';
            break;
        }
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                return;
            }
        }
        yield();
    }

    String targetSSID = cachedSSIDs[selection];

    if (WiFi.status() == WL_CONNECTED && targetSSID == WiFi.SSID()) {
        return;
    }

    gfx->fillRect(0, 24, DISP_W, DISP_H - 24, C_BLACK);
    gfx->setCursor(10, 50); gfx->setTextColor(C_WHITE); gfx->print("Joining: ");
    gfx->setTextColor(C_GREEN); gfx->print(targetSSID);

    String saved_pass = get_known_password(targetSSID);
    String pass = "";

    if (saved_pass != "") {
        gfx->setCursor(10, 80); gfx->setTextColor(C_GREEN);
        gfx->print("Saved Key Found! (Press ENTER to use)");
        gfx->setCursor(10, 100); gfx->setTextColor(C_GREY);
        gfx->print("Or type a new password:");

        String input = get_text_input(10, 120);
        if (input == "##EXIT##") return;
        if (input != "") pass = input;
        else pass = saved_pass;
    } else {
        gfx->setCursor(10, 80);
        gfx->setTextColor(C_WHITE);
        gfx->setTextSize(1);
        gfx->print("Password:");
        pass = get_text_input(10, 100);
        if (pass == "##EXIT##") return;
    }

    gfx->setCursor(10, 150); gfx->setTextColor(C_WHITE);
    gfx->print("Connecting...");

    WiFi.disconnect();
    delay(100);

    if (pass == "") WiFi.begin(targetSSID.c_str());
    else            WiFi.begin(targetSSID.c_str(), pass.c_str());

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        gfx->print(".");
        timeout++;
    }

    gfx->fillRect(0, 140, DISP_W, 40, C_BLACK);
    gfx->setCursor(10, 150);

    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(C_GREEN);
        gfx->print("Success! IP: ");
        gfx->print(WiFi.localIP());
        save_wifi_config(targetSSID.c_str(), pass.c_str());
    } else {
        gfx->setTextColor(C_RED);
        gfx->print("Failed. Check password.");
        WiFi.disconnect();
    }
    delay(3000);
}
