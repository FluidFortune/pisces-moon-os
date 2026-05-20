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
#include "touch.h"
#include "keyboard.h"
#include "pm_input.h"
#include "theme.h"
#include "apps.h"

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

String get_encryption_type(wifi_auth_mode_t type) {
    switch (type) {
        case WIFI_AUTH_OPEN:           return "OPEN";
        case WIFI_AUTH_WEP:            return "WEP ";
        case WIFI_AUTH_WPA_PSK:        return "WPA ";
        case WIFI_AUTH_WPA2_PSK:       return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:   return "WPA*";
        case WIFI_AUTH_WPA2_ENTERPRISE:return "ENT ";
        case WIFI_AUTH_WPA3_PSK:       return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:  return "WPA*";
        case WIFI_AUTH_WAPI_PSK:       return "WAPI";
        default:                       return "UNK ";
    }
}

void run_wifi_app() {
    bool running = true;

    while (running) {
        gfx->fillScreen(C_BLACK);
        gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
        gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
        gfx->print("WIFI SCANNER | R: RESCAN | " PM_EXIT_SHORT_COPY);

        gfx->setCursor(10, 50); gfx->setTextColor(C_WHITE);
        gfx->print("Initializing Radio & Scanning...");

        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        int n = WiFi.scanNetworks(false, false);

        gfx->fillRect(0, 25, DISP_W, DISP_H - 25, C_BLACK);

        if (n <= 0) {
            gfx->setCursor(10, 50); gfx->setTextColor(C_RED);
            gfx->print("No networks found.");
        } else {
            gfx->setCursor(10, 35);
            gfx->setTextColor(C_GREY);
            gfx->print("CH  SIG      SEC   SSID");

            // SSID column gets more room on T-LoRa Pager
            const int ssidMax = (DISP_W - 130) / 6;  // ~31 chars on T-Deck, ~58 on T-LoRa Pager

            // How many rows fit between y=55 and footer
            const int rowH      = 18;
            const int firstRowY = 55;
            const int maxRows   = (DISP_H - firstRowY - 10) / rowH;
            int limit = (n > maxRows) ? maxRows : n;

            for (int i = 0; i < limit; i++) {
                int y = firstRowY + (i * rowH);
                int rssi = WiFi.RSSI(i);
                uint16_t sigColor = (rssi > -70) ? C_GREEN : (rssi > -85 ? 0xFD20 : C_RED);

                gfx->setCursor(10, y);
                gfx->setTextColor(C_WHITE);
                gfx->printf("%2d ", WiFi.channel(i));

                gfx->setTextColor(sigColor);
                gfx->printf("%3d dBm ", rssi);

                gfx->setTextColor(0x07FF);
                gfx->print(get_encryption_type(WiFi.encryptionType(i)) + "  ");

                gfx->setTextColor(C_WHITE);
                String ssid = WiFi.SSID(i);
                if ((int)ssid.length() > ssidMax) ssid = ssid.substring(0, ssidMax);
                gfx->print(ssid);
            }
        }

        bool waiting = true;
        while (waiting) {
            char c = get_keypress();
            if (c == 'r' || c == 'R') {
                waiting = false;
            }
            if (pm_is_exit_key(c)) {
                running = false;
                waiting = false;
            }
            int16_t tx, ty;
            if (get_touch(&tx, &ty)) {
                if (ty < 30) {
                    while (get_touch(&tx, &ty)) { delay(10); yield(); }
                    running = false;
                    waiting = false;
                }
            }
            yield(); delay(20);
        }
    }
}
