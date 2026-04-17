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

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "touch.h"
#include "keyboard.h"
#include "theme.h"
#include "apps.h"

extern Arduino_GFX *gfx;

// Translates the raw ESP32 security enum into readable text
String get_encryption_type(wifi_auth_mode_t type) {
    switch (type) {
        case WIFI_AUTH_OPEN: return "OPEN";
        case WIFI_AUTH_WEP: return "WEP ";
        case WIFI_AUTH_WPA_PSK: return "WPA ";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA*";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT ";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA*";
        case WIFI_AUTH_WAPI_PSK: return "WAPI";
        default: return "UNK ";
    }
}

void run_wifi_app() {
    bool running = true;
    
    while(running) {
        gfx->fillScreen(C_BLACK);
        gfx->fillRect(0, 0, 320, 24, C_DARK);
        gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
        gfx->print("WIFI SCANNER | R: RESCAN | TAP HEADER EXIT");
        
        gfx->setCursor(10, 50); gfx->setTextColor(C_WHITE);
        gfx->print("Initializing Radio & Scanning...");
        
        // OVERRIDE: Clear the Wardrive background scan
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);
        
        int n = WiFi.scanNetworks(false, false); 
        
        gfx->fillRect(0, 25, 320, 215, C_BLACK); 
        
        if (n <= 0) {
            gfx->setCursor(10, 50); gfx->setTextColor(C_RED);
            gfx->print("No networks found.");
        } else {
            // Column Headers
            gfx->setCursor(10, 35);
            gfx->setTextColor(C_GREY);
            gfx->print("CH  SIG      SEC   SSID");
            
            // Display up to 10 networks so they fit on the screen
            int limit = (n > 10) ? 10 : n; 
            for (int i = 0; i < limit; i++) {
                int y = 55 + (i * 18);
                int rssi = WiFi.RSSI(i);
                
                // Signal Color Logic
                uint16_t sigColor = (rssi > -70) ? C_GREEN : (rssi > -85 ? 0xFD20 : C_RED);
                
                // 1. Channel
                gfx->setCursor(10, y);
                gfx->setTextColor(C_WHITE);
                gfx->printf("%2d ", WiFi.channel(i));
                
                // 2. RSSI (Color Coded)
                gfx->setTextColor(sigColor);
                gfx->printf("%3d dBm ", rssi);
                
                // 3. Security Type (Cyan)
                gfx->setTextColor(0x07FF); 
                gfx->print(get_encryption_type(WiFi.encryptionType(i)) + "  ");
                
                // 4. SSID
                gfx->setTextColor(C_WHITE);
                gfx->print(WiFi.SSID(i).substring(0, 16));
            }
        }

        // Wait for exit or manual rescan
        bool waiting = true;
        while(waiting) {
            char c = get_keypress();
            if (c == 'r' || c == 'R') {
                waiting = false; // Break the inner loop to trigger a fresh scan
            }
            
            int16_t tx, ty;
            if (get_touch(&tx, &ty)) {
                if (ty < 30) {
                    while(get_touch(&tx, &ty)) { delay(10); yield(); }
                    running = false;  // Kill the main loop
                    waiting = false;  // Kill the input loop
                }
            }
            yield(); delay(20);
        }
    }
}