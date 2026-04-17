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
#include "keyboard.h"
#include "touch.h" 
#include "theme.h"
#include "apps.h"
#include "wifi_manager.h" 

extern Arduino_GFX *gfx;

char map_top_row_to_num(char c) {
    String topRow = "qwertyuio";
    int idx = topRow.indexOf(c);
    if (idx != -1) return (char)('1' + idx);
    return c;
}

void run_wifi_connect() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
    gfx->print("WIFI JOIN | USE Q-I | TAP HEADER EXIT");
    
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
    
    // ☢️ THE RADIO NUKE (Only triggers if we aren't currently connected to the internet)
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true, true); // Erase the corrupted NVS state
        delay(100);
        WiFi.mode(WIFI_OFF);         // Physically power down the RF chip
        delay(100);
    }
    
    // Power the RF chip back up cleanly!
    WiFi.mode(WIFI_STA); 
    delay(100);
    
    int n = WiFi.scanNetworks(false, false); 
    
    gfx->fillRect(0, 25, 320, 215, C_BLACK); 
    
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
        gfx->print(cachedSSIDs[i].substring(0, 18));
    }

    int selection = -1;
    while(true) {
        char c = get_keypress();
        char mapped = map_top_row_to_num(c); 
        if (mapped >= '1' && mapped < '1' + limit) {
            selection = mapped - '1';
            break;
        }
        
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while(get_touch(&tx, &ty)) { delay(10); yield(); }
                return; 
            }
        }
        yield();
    }

    String targetSSID = cachedSSIDs[selection]; 
    
    if (WiFi.status() == WL_CONNECTED && targetSSID == WiFi.SSID()) {
        return;
    }
    
    gfx->fillRect(0, 24, 320, 216, C_BLACK);
    gfx->setCursor(10, 50); gfx->setTextColor(C_WHITE); gfx->print("Joining: ");
    gfx->setTextColor(C_GREEN); gfx->print(targetSSID);
    
    // --- NEW SMART PASSWORD SYSTEM ---
    String saved_pass = get_known_password(targetSSID);
    String pass = "";

    if (saved_pass != "") {
        gfx->setCursor(10, 80); gfx->setTextColor(C_GREEN); 
        gfx->print("Saved Key Found! (Press ENTER to use)");
        gfx->setCursor(10, 100); gfx->setTextColor(C_GREY);
        gfx->print("Or type a new password:");
        
        String input = get_text_input(10, 120); 
        if (input == "##EXIT##") return;
        
        // If the user just hits Enter, use the saved key. Otherwise, use what they typed.
        if (input != "") pass = input;     
        else pass = saved_pass;            
    } else {
        // Explicit cursor reset before printing — SD card reads above
        // (get_known_password) can leave GFX cursor in a dirty state,
        // causing the first character of "Password:" to be dropped.
        gfx->setCursor(10, 80);
        gfx->setTextColor(C_WHITE);
        gfx->setTextSize(1);
        gfx->print("Password:");
        pass = get_text_input(10, 100); 
        if (pass == "##EXIT##") return;
    }

    gfx->setCursor(10, 150); gfx->setTextColor(C_WHITE);
    gfx->print("Connecting...");

    // Clean disconnect before joining
    WiFi.disconnect();
    delay(100);

    if (pass == "") {
        WiFi.begin(targetSSID.c_str()); 
    } else {
        WiFi.begin(targetSSID.c_str(), pass.c_str());
    }

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        gfx->print(".");
        timeout++;
    }

    gfx->fillRect(0, 140, 320, 40, C_BLACK);
    gfx->setCursor(10, 150);
    
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(C_GREEN); 
        gfx->print("Success! IP: ");
        gfx->print(WiFi.localIP());
        
        // Save the new network to our Keyring!
        save_wifi_config(targetSSID.c_str(), pass.c_str());
        
    } else {
        gfx->setTextColor(C_RED); 
        gfx->print("Failed. Check password.");
        WiFi.disconnect(); 
    }
    delay(3000);
}