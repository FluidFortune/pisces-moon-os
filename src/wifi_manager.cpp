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

#include <WiFi.h>
#include <ArduinoJson.h>
#include "SdFat.h"
#include "wifi_manager.h"

extern SdFat sd;

void save_wifi_config(const char* ssid, const char* password) {
    JsonDocument doc;

    if (sd.exists("/wifi.cfg")) {
        FsFile inFile = sd.open("/wifi.cfg", O_READ);
        if (inFile) {
            deserializeJson(doc, inFile);
            inFile.close();
        }
    }

    doc["ssid"]  = ssid;
    doc["password"] = password;
    doc[ssid]    = password; // Keyring entry

    FsFile outFile = sd.open("/wifi.cfg", O_WRITE | O_CREAT | O_TRUNC);
    if (outFile) {
        serializeJson(doc, outFile);
        outFile.close();
        Serial.println("[WIFI] Credentials saved to /wifi.cfg");
    } else {
        Serial.println("[WIFI] ERROR: Failed to write /wifi.cfg");
    }
}

String get_known_password(String targetSSID) {
    if (!sd.exists("/wifi.cfg")) return "";

    FsFile file = sd.open("/wifi.cfg", O_READ);
    if (!file) return "";

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (!err && doc.containsKey(targetSSID)) {
        return doc[targetSSID].as<String>();
    }
    return "";
}

bool connect_to_wifi(const char* ssid, const char* password) {
    WiFi.disconnect(true, true);
    delay(100);
    WiFi.mode(WIFI_STA);
    delay(100);

    WiFi.begin(ssid, password);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        save_wifi_config(ssid, password);
        return true;
    }
    WiFi.disconnect();
    return false;
}

void auto_connect_wifi() {
    if (!sd.exists("/wifi.cfg")) {
        Serial.println("[WIFI] No config found on SD — skipping auto-connect.");
        return;
    }

    FsFile file = sd.open("/wifi.cfg", O_READ);
    if (!file) {
        Serial.println("[WIFI] Could not open /wifi.cfg");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.println("[WIFI] Config parse error — skipping.");
        return;
    }

    String savedSSID = doc["ssid"].as<String>();
    String savedPass = doc["password"].as<String>();

    if (savedSSID.length() == 0) {
        Serial.println("[WIFI] No SSID in config — skipping.");
        return;
    }

    Serial.println("[WIFI] Auto-connecting to: " + savedSSID);

    // Clean radio state before connecting.
    // disconnect(true, true) erases NVS WiFi credentials so we don't
    // fight with the ESP32 IDF trying to reconnect to a stale entry.
    WiFi.disconnect(true, true);
    delay(200);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(200);

    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    // Block and wait up to 10 seconds for the connection.
    // This runs during setup() before the launcher starts, so blocking
    // here is safe and prevents the wardrive task from racing us.
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
        Serial.printf("[WIFI] Connecting... attempt %d/20\n", attempts);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WIFI] Connected! IP: " + WiFi.localIP().toString());
    } else {
        Serial.println("[WIFI] Auto-connect failed — will retry manually.");
        // Don't disconnect — leave the radio in STA mode so a manual
        // connect attempt from the app doesn't have to cold-start the radio.
    }
}