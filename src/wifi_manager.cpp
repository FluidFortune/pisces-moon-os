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
#include "pm_storage.h"
#include "wifi_manager.h"

// v1.3.1: migrated from SdFat-direct to pm_storage HAL.
//
// Original implementation talked to the global `SdFat sd` object
// directly. That works on devices with SPI-attached SD cards
// (T-Deck Plus, T-LoRa Pager, Cardputer ADV) but on C28P (and
// Maxine) the SD card is on the SDIO 4-bit bus via SD_MMC — the
// SdFat global is never mounted to real storage there. Every
// save_wifi_config() call silently failed on those devices:
// sd.open() returned an invalid handle, the `if (outFile)` check
// dropped through to "ERROR: Failed to write", and WiFi credentials
// never persisted across reboots.
//
// pm_storage delegates to SdFat on the SPI-SD devices and to SD_MMC
// on the SDIO devices. The same code path now works on all five
// device targets. ArduinoJson's stream-mode deserialize/serialize
// aren't usable here because pm_storage::File doesn't inherit from
// Stream / Print — we marshal through a stack buffer / String
// instead. WiFi config files are well under 1 KB so this is cheap.

void save_wifi_config(const char* ssid, const char* password) {
    JsonDocument doc;

    if (pm_storage::exists("/wifi.cfg")) {
        pm_storage::File inFile = pm_storage::open("/wifi.cfg", pm_storage::Mode::Read);
        if (inFile) {
            char buf[1024];
            size_t got = inFile.read((uint8_t*)buf, sizeof(buf) - 1);
            buf[got] = 0;
            inFile.close();
            deserializeJson(doc, buf);
        }
    }

    doc["ssid"]  = ssid;
    doc["password"] = password;
    doc[ssid]    = password; // Keyring entry

    pm_storage::File outFile = pm_storage::open("/wifi.cfg", pm_storage::Mode::Write);
    if (outFile) {
        String body;
        serializeJson(doc, body);
        outFile.print(body.c_str());
        outFile.close();
        Serial.println("[WIFI] Credentials saved to /wifi.cfg");
    } else {
        Serial.println("[WIFI] ERROR: Failed to write /wifi.cfg");
    }
}

String get_known_password(String targetSSID) {
    if (!pm_storage::exists("/wifi.cfg")) return "";

    pm_storage::File file = pm_storage::open("/wifi.cfg", pm_storage::Mode::Read);
    if (!file) return "";

    char buf[1024];
    size_t got = file.read((uint8_t*)buf, sizeof(buf) - 1);
    buf[got] = 0;
    file.close();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);

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
    if (!pm_storage::exists("/wifi.cfg")) {
        Serial.println("[WIFI] No config found on SD — skipping auto-connect.");
        return;
    }

    pm_storage::File file = pm_storage::open("/wifi.cfg", pm_storage::Mode::Read);
    if (!file) {
        Serial.println("[WIFI] Could not open /wifi.cfg");
        return;
    }

    char buf[1024];
    size_t got = file.read((uint8_t*)buf, sizeof(buf) - 1);
    buf[got] = 0;
    file.close();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);

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