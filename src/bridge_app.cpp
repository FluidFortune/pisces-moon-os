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
 * PISCES MOON OS — BRIDGE APP
 * Web Serial bridge for the Pisces Moon Web Emulator.
 *
 * Opens a JSON command/response protocol over USB Serial (115200 baud).
 * Works on any ESP32 — not just the T-Deck Plus.
 * The web emulator at piscesdemo.fluidfortune.com connects via
 * the Web Serial API (Chrome/Edge) and drives the device remotely.
 *
 * PROTOCOL:
 *   Browser sends: {"cmd":"<command>","args":{...}}\n
 *   Device replies: {"ok":true,"data":{...}}\n  or
 *                   {"ok":false,"error":"<message>"}\n
 *
 * COMMANDS:
 *   ping            — heartbeat, returns device info
 *   status          — full OS status snapshot
 *   wardrive_status — Ghost Engine counters + GPS
 *   wardrive_start  — set wardrive_active = true
 *   wardrive_stop   — set wardrive_active = false
 *   wifi_scan       — trigger a WiFi scan, return results
 *   ble_scan        — return last BLE scan results
 *   gps             — current GPS fix data
 *   gemini          — send prompt to ask_gemini(), return response
 *   sysinfo         — heap, PSRAM, uptime, chip info
 *   ls              — list SD card directory
 *   cat             — read small file from SD card
 *
 * SECURITY:
 *   Bridge mode requires explicit launch from the SYSTEM menu.
 *   It exits on any keypress or header tap.
 *   Serial data is never forwarded to any network — purely local USB.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "theme.h"
#include "keyboard.h"
#include "gemini_client.h"
#include "wardrive.h"
#include "SdFat.h"
#include "time.h"
#include <TinyGPSPlus.h>

extern Arduino_GFX    *gfx;
extern SdFat           sd;
extern TinyGPSPlus     gps;
extern volatile bool   wifi_in_use;
extern volatile bool   sd_in_use;
extern SemaphoreHandle_t spi_mutex;

#define BRIDGE_BAUD   115200
#define CMD_BUF_LEN   2048

// ─────────────────────────────────────────────
//  RESPONSE HELPERS
// ─────────────────────────────────────────────
static void sendOK(JsonObject& data) {
    JsonDocument resp;
    resp["ok"] = true;
    resp["data"] = data;
    serializeJson(resp, Serial);
    Serial.println();
}

static void sendOKVal(const char* key, JsonVariant val) {
    JsonDocument resp;
    resp["ok"] = true;
    resp["data"][key] = val;
    serializeJson(resp, Serial);
    Serial.println();
}

static void sendError(const char* msg) {
    JsonDocument resp;
    resp["ok"]    = false;
    resp["error"] = msg;
    serializeJson(resp, Serial);
    Serial.println();
}

static void sendEvent(const char* type, JsonObject& data) {
    JsonDocument evt;
    evt["event"] = type;
    evt["data"]  = data;
    serializeJson(evt, Serial);
    Serial.println();
}

// ─────────────────────────────────────────────
//  COMMAND HANDLERS
// ─────────────────────────────────────────────
static void cmdPing() {
    JsonDocument resp;
    resp["ok"] = true;
    resp["data"]["pong"]    = true;
    resp["data"]["os"]      = "Pisces Moon OS";
    resp["data"]["version"] = "1.0.0";
    resp["data"]["codename"]= "The Arsenal";
    resp["data"]["chip"]    = ESP.getChipModel();
    resp["data"]["cores"]   = ESP.getChipCores();
    resp["data"]["freq_mhz"]= ESP.getCpuFreqMHz();
    resp["data"]["sdk"]     = ESP.getSdkVersion();
    resp["data"]["uptime_s"]= millis() / 1000;
    serializeJson(resp, Serial);
    Serial.println();
}

static void cmdStatus() {
    JsonDocument resp;
    resp["ok"] = true;

    // System
    resp["data"]["os"]        = "Pisces Moon OS";
    resp["data"]["version"]   = "1.0.0";
    resp["data"]["uptime_s"]  = millis() / 1000;
    resp["data"]["free_heap"] = ESP.getFreeHeap();
    resp["data"]["free_psram"]= ESP.getFreePsram();

    // WiFi
    resp["data"]["wifi"]["connected"] = (WiFi.status() == WL_CONNECTED);
    resp["data"]["wifi"]["ssid"]      = WiFi.SSID();
    resp["data"]["wifi"]["ip"]        = WiFi.localIP().toString();
    resp["data"]["wifi"]["rssi"]      = WiFi.RSSI();

    // Wardrive
    resp["data"]["wardrive"]["active"]   = wardrive_active;
    resp["data"]["wardrive"]["networks"] = networks_found;
    resp["data"]["wardrive"]["ble"]      = bt_found;
    resp["data"]["wardrive"]["logfile"]  = wardrive_get_log_filename();

    // GPS
    resp["data"]["gps"]["valid"] = gps.location.isValid();
    if (gps.location.isValid()) {
        resp["data"]["gps"]["lat"]  = gps.location.lat();
        resp["data"]["gps"]["lng"]  = gps.location.lng();
        resp["data"]["gps"]["alt_ft"] = gps.altitude.feet();
        resp["data"]["gps"]["sats"] = gps.satellites.value();
    }

    // Time
    if (time(nullptr) > 100000UL) {
        struct tm t; getLocalTime(&t);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &t);
        resp["data"]["time"] = buf;
    } else {
        resp["data"]["time"] = "not synced";
    }

    serializeJson(resp, Serial);
    Serial.println();
}

static void cmdWardriveStatus() {
    JsonDocument resp;
    resp["ok"] = true;
    resp["data"]["active"]   = wardrive_active;
    resp["data"]["networks"] = networks_found;
    resp["data"]["ble"]      = bt_found;
    resp["data"]["logfile"]  = wardrive_get_log_filename();
    resp["data"]["gps_valid"]= gps.location.isValid();
    if (gps.location.isValid()) {
        resp["data"]["lat"]    = gps.location.lat();
        resp["data"]["lng"]    = gps.location.lng();
        resp["data"]["alt_ft"] = gps.altitude.feet();
        resp["data"]["sats"]   = gps.satellites.value();
    }
    serializeJson(resp, Serial);
    Serial.println();
}

static void cmdWifiScan() {
    if (wifi_in_use) { sendError("WiFi in use by another app"); return; }
    wifi_in_use = true;
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true);
    wifi_in_use = false;

    JsonDocument resp;
    resp["ok"] = true;
    resp["data"]["count"] = n;
    JsonArray nets = resp["data"]["networks"].to<JsonArray>();
    for (int i = 0; i < min(n, 20); i++) {
        JsonObject net = nets.add<JsonObject>();
        net["ssid"]    = WiFi.SSID(i);
        net["bssid"]   = WiFi.BSSIDstr(i);
        net["rssi"]    = WiFi.RSSI(i);
        net["channel"] = WiFi.channel(i);
        net["enc"]     = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "OPEN" : "WPA";
    }
    WiFi.scanDelete();
    serializeJson(resp, Serial);
    Serial.println();
}

static void cmdSysInfo() {
    JsonDocument resp;
    resp["ok"] = true;
    resp["data"]["chip"]        = ESP.getChipModel();
    resp["data"]["cores"]       = ESP.getChipCores();
    resp["data"]["freq_mhz"]    = ESP.getCpuFreqMHz();
    resp["data"]["flash_mb"]    = ESP.getFlashChipSize() / (1024*1024);
    resp["data"]["free_heap"]   = ESP.getFreeHeap();
    resp["data"]["total_heap"]  = ESP.getHeapSize();
    resp["data"]["free_psram"]  = ESP.getFreePsram();
    resp["data"]["total_psram"] = ESP.getPsramSize();
    resp["data"]["uptime_s"]    = millis() / 1000;
    resp["data"]["sdk"]         = ESP.getSdkVersion();
    resp["data"]["idf"]         = IDF_VER;
    serializeJson(resp, Serial);
    Serial.println();
}

static void cmdGPS() {
    JsonDocument resp;
    resp["ok"] = true;
    resp["data"]["valid"]     = gps.location.isValid();
    resp["data"]["sats"]      = gps.satellites.value();
    resp["data"]["chars_proc"]= gps.charsProcessed();
    resp["data"]["checksum_ok"]= gps.passedChecksum();
    if (gps.location.isValid()) {
        resp["data"]["lat"]    = gps.location.lat();
        resp["data"]["lng"]    = gps.location.lng();
        resp["data"]["alt_ft"] = gps.altitude.feet();
        resp["data"]["alt_m"]  = gps.altitude.meters();
        resp["data"]["speed_mph"]= gps.speed.mph();
        resp["data"]["course"] = gps.course.deg();
        if (gps.date.isValid()) {
            char buf[12];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                gps.date.year(), gps.date.month(), gps.date.day());
            resp["data"]["date"] = buf;
        }
        if (gps.time.isValid()) {
            char buf[10];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                gps.time.hour(), gps.time.minute(), gps.time.second());
            resp["data"]["time"] = buf;
        }
    }
    serializeJson(resp, Serial);
    Serial.println();
}

static void cmdGemini(JsonObject& args) {
    const char* prompt = args["prompt"] | "";
    if (strlen(prompt) == 0) { sendError("prompt required"); return; }
    if (!gemini_has_key()) { sendError("No GEMINI_API_KEY configured"); return; }
    if (WiFi.status() != WL_CONNECTED) { sendError("No WiFi connection"); return; }

    // Send thinking event
    Serial.println("{\"event\":\"thinking\"}");

    String response = ask_gemini(String(prompt));

    JsonDocument resp;
    resp["ok"]             = true;
    resp["data"]["prompt"] = prompt;
    resp["data"]["response"]= response;
    serializeJson(resp, Serial);
    Serial.println();
}

static void cmdLS(JsonObject& args) {
    const char* path = args["path"] | "/";

    if (!sd_in_use && spi_mutex &&
        xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {

        JsonDocument resp;
        resp["ok"] = true;
        resp["data"]["path"] = path;
        JsonArray entries = resp["data"]["entries"].to<JsonArray>();

        FsFile dir = sd.open(path);
        if (dir && dir.isDir()) {
            FsFile entry;
            int count = 0;
            while (entry.openNext(&dir, O_RDONLY) && count < 50) {
                char name[64];
                entry.getName(name, sizeof(name));
                JsonObject e = entries.add<JsonObject>();
                e["name"]  = name;
                e["dir"]   = entry.isDir();
                e["size"]  = entry.fileSize();
                entry.close();
                count++;
            }
            dir.close();
        } else {
            resp["ok"]    = false;
            resp["error"] = "Cannot open directory";
        }
        xSemaphoreGive(spi_mutex);
        serializeJson(resp, Serial);
        Serial.println();
    } else {
        sendError("SD card busy");
    }
}

static void cmdCat(JsonObject& args) {
    const char* path = args["path"] | "";
    if (strlen(path) == 0) { sendError("path required"); return; }

    if (!sd_in_use && spi_mutex &&
        xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {

        FsFile file = sd.open(path, O_READ);
        if (!file || file.isDir()) {
            xSemaphoreGive(spi_mutex);
            sendError("File not found or is directory");
            return;
        }

        uint32_t size = file.fileSize();
        if (size > 4096) {
            file.close();
            xSemaphoreGive(spi_mutex);
            sendError("File too large for bridge (max 4KB)");
            return;
        }

        String content = "";
        content.reserve(size);
        while (file.available()) {
            content += (char)file.read();
        }
        file.close();
        xSemaphoreGive(spi_mutex);

        JsonDocument resp;
        resp["ok"]            = true;
        resp["data"]["path"]  = path;
        resp["data"]["size"]  = size;
        resp["data"]["content"] = content;
        serializeJson(resp, Serial);
        Serial.println();
    } else {
        sendError("SD card busy");
    }
}

// ─────────────────────────────────────────────
//  DRAW BRIDGE SCREEN
// ─────────────────────────────────────────────
static void drawBridgeScreen(bool connected) {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, 0x07FF);
    gfx->setCursor(10, 7); gfx->setTextColor(0x07FF); gfx->setTextSize(1);
    gfx->print("BRIDGE MODE | TAP HEADER TO EXIT");

    gfx->setTextSize(2); gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 35); gfx->print("WEB BRIDGE");

    gfx->setTextSize(1);
    gfx->setTextColor(connected ? C_GREEN : 0xFD20);
    gfx->setCursor(10, 62);
    gfx->print(connected ? "CONNECTED" : "WAITING FOR CONNECTION...");

    gfx->setTextColor(C_GREY); gfx->setCursor(10, 80);
    gfx->print("Open piscesdemo.fluidfortune.com");
    gfx->setCursor(10, 94); gfx->print("in Chrome or Edge, then click");
    gfx->setCursor(10, 108); gfx->print("CONNECT DEVICE.");

    gfx->setTextColor(0x4208); gfx->setCursor(10, 128);
    gfx->print("115200 baud | USB Serial | JSON");

    gfx->fillRect(0, 160, 320, 60, 0x000A);
    gfx->drawFastHLine(0, 160, 320, 0x001F);
    gfx->setTextColor(0x001F); gfx->setCursor(10, 168);
    gfx->print("Works on any ESP32 device.");
    gfx->setCursor(10, 180); gfx->print("Commands: ping status wardrive");
    gfx->setCursor(10, 192); gfx->print("wifi_scan gps gemini ls cat");
}

// ─────────────────────────────────────────────
//  MAIN BRIDGE LOOP
// ─────────────────────────────────────────────
void run_bridge() {
    // Ensure Serial is at the right baud
    Serial.flush();
    Serial.begin(BRIDGE_BAUD);
    delay(100);

    drawBridgeScreen(false);

    char cmdBuf[CMD_BUF_LEN];
    int  cmdLen     = 0;
    bool connected  = false;
    unsigned long lastActivity = millis();

    // Announce readiness
    Serial.println("{\"event\":\"ready\",\"os\":\"Pisces Moon OS\",\"version\":\"1.0.0\"}");

    while (true) {
        // ── Exit ──────────────────────────────
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 40) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            Serial.println("{\"event\":\"disconnect\",\"reason\":\"user_exit\"}");
            break;
        }
        char k = get_keypress();
        if (k == 'q' || k == 'Q') {
            Serial.println("{\"event\":\"disconnect\",\"reason\":\"user_exit\"}");
            break;
        }

        // ── Read Serial ───────────────────────
        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (cmdLen > 0) {
                    cmdBuf[cmdLen] = '\0';
                    lastActivity = millis();

                    if (!connected) {
                        connected = true;
                        drawBridgeScreen(true);
                    }

                    // Parse and dispatch
                    JsonDocument doc;
                    DeserializationError err = deserializeJson(doc, cmdBuf, cmdLen);
                    cmdLen = 0;

                    if (err) {
                        sendError("JSON parse error");
                        continue;
                    }

                    const char* cmd = doc["cmd"] | "";
                    JsonObject  args = doc["args"].isNull()
                                       ? doc.as<JsonObject>()
                                       : doc["args"].as<JsonObject>();

                    if      (strcmp(cmd,"ping")==0)             cmdPing();
                    else if (strcmp(cmd,"status")==0)           cmdStatus();
                    else if (strcmp(cmd,"wardrive_status")==0)  cmdWardriveStatus();
                    else if (strcmp(cmd,"wardrive_start")==0) { wardrive_active=true;  cmdWardriveStatus(); }
                    else if (strcmp(cmd,"wardrive_stop")==0)  { wardrive_active=false; cmdWardriveStatus(); }
                    else if (strcmp(cmd,"wifi_scan")==0)        cmdWifiScan();
                    else if (strcmp(cmd,"gps")==0)              cmdGPS();
                    else if (strcmp(cmd,"sysinfo")==0)          cmdSysInfo();
                    else if (strcmp(cmd,"gemini")==0)           cmdGemini(args);
                    else if (strcmp(cmd,"ls")==0)               cmdLS(args);
                    else if (strcmp(cmd,"cat")==0)              cmdCat(args);
                    else sendError("Unknown command");

                    yield();
                }
            } else {
                if (cmdLen < CMD_BUF_LEN - 1) {
                    cmdBuf[cmdLen++] = c;
                }
            }
        }

        // ── Connection timeout ────────────────
        if (connected && millis() - lastActivity > 30000) {
            connected = false;
            drawBridgeScreen(false);
            Serial.println("{\"event\":\"timeout\"}");
        }

        delay(10); yield();
    }
}
