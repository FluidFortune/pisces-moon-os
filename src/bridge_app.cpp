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
 * Opens a JSON command/response protocol over USB Serial (921600 baud).
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
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include <esp_mac.h>
#include <TinyGPSPlus.h>
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "keyboard.h"
#include "pm_input.h"
#include "gemini_client.h"
#include "wardrive.h"
#include "pm_promiscuous.h"
#include "SdFat.h"
#include "time.h"
#include <TinyGPSPlus.h>

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif
extern SdFat           sd;
extern TinyGPSPlus     gps;
extern volatile bool   wifi_in_use;
extern volatile bool   sd_in_use;
extern SemaphoreHandle_t spi_mutex;

#define BRIDGE_BAUD   921600   // 921600 for promiscuous mode bandwidth headroom
                               // (115200 would saturate at ~58 frames/sec)
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
    resp["data"]["version"] = PISCES_OS_VERSION;
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
    resp["data"]["version"]      = PISCES_OS_VERSION;
    resp["data"]["ghost_engine"]  = wardrive_active ? "active" : "idle";
    resp["data"]["wardrive_mode"] = (wardrive_mode == WARDRIVE_MODE_PROMISCUOUS) ? "promiscuous" : "scan";
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
        xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {

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
        xSemaphoreGiveRecursive(spi_mutex);
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
        xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {

        FsFile file = sd.open(path, O_READ);
        if (!file || file.isDir()) {
            xSemaphoreGiveRecursive(spi_mutex);
            sendError("File not found or is directory");
            return;
        }

        uint32_t size = file.fileSize();
        if (size > 4096) {
            file.close();
            xSemaphoreGiveRecursive(spi_mutex);
            sendError("File too large for bridge (max 4KB)");
            return;
        }

        String content = "";
        content.reserve(size);
        while (file.available()) {
            content += (char)file.read();
        }
        file.close();
        xSemaphoreGiveRecursive(spi_mutex);

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
//  BRIDGE VISUALIZER STATE
//
//  The bridge screen is a real-time view of what the
//  device is broadcasting. Bars, spike traces, and a
//  rolling JSON line show activity at a glance.
//
//  Each radio subsystem is queried defensively — if a
//  module isn't present (e.g. no NFC reader attached),
//  the visualizer for that radio sits idle without
//  crashing the bridge.
// ─────────────────────────────────────────────
#define VIS_WIFI_MAX     20    // bars cap at 20 networks
#define VIS_BLE_MAX      20
#define VIS_LORA_HISTORY 80    // spike trace width
#define VIS_LORA_DECAY   8     // spike decay per redraw tick
#define VIS_JSON_FADE_MS 600   // ms over which TX flash fades to grey

// LoRa spike trace — circular buffer of signed deltas
// Positive = TX activity (spikes up), negative = RX (spikes down).
static int8_t  lora_trace[VIS_LORA_HISTORY] = {0};
static int     lora_trace_head = 0;
static uint32_t lora_tx_count = 0;
static uint32_t lora_rx_count = 0;

// NFC/RFID activity — pulse for ~400ms after last event
static uint32_t nfc_last_event_ms = 0;
static uint32_t rfid_last_event_ms = 0;

// JSON visualizer — last emitted line + timestamp
static char     vis_last_json[80] = "";
static uint32_t vis_last_json_ms = 0;
static bool     vis_last_was_tx  = true;  // true = outgoing, false = incoming

// Forward declaration — defined below with the oscilloscope primitives
static void lora_trace_pulse(bool outgoing);

// ─────────────────────────────────────────────
//  BRIDGE RECEPTION COUNTERS (v1.1 fix)
//
//  The visualizer bars reflect what the bridge is TRANSMITTING
//  to the host — i.e. what the host is sending us via streaming
//  events (wifi_seen, ble_seen) — NOT the T-Deck's own wardrive
//  scan counts. When the T-Deck is a passive bridge, networks_found
//  and bt_found are zero on-device even though the host is actively
//  wardriving and streaming results back.
//
//  These counters increment on each received event and decay slowly
//  so the bars feel alive during a streaming session.
// ─────────────────────────────────────────────
static int vis_rx_wifi_count = 0;   // networks seen via streaming events
static int vis_rx_ble_count  = 0;   // BLE devices seen via streaming events
static uint32_t vis_last_wifi_event_ms = 0;
static uint32_t vis_last_ble_event_ms  = 0;
static uint32_t vis_last_stream_ms     = 0;  // any JSON event — drives LoRa trace pulse

// Called from vis_record_tx() and the command handler when wifi_seen/ble_seen arrive
static void vis_on_wifi_seen() {
    vis_rx_wifi_count = min(vis_rx_wifi_count + 1, VIS_WIFI_MAX);
    vis_last_wifi_event_ms = millis();
    vis_last_stream_ms = millis();
    lora_trace_pulse(true);   // each wifi event pulses the trace upward
}

static void vis_on_ble_seen() {
    vis_rx_ble_count = min(vis_rx_ble_count + 1, VIS_BLE_MAX);
    vis_last_ble_event_ms = millis();
    vis_last_stream_ms = millis();
    lora_trace_pulse(false);  // BLE events pulse the trace downward
}

// Decay the reception counters every 10 seconds so the bars don't
// stay frozen at max after a wardrive session ends
static void vis_decay_rx_counters() {
    static uint32_t last_decay = 0;
    if (millis() - last_decay < 10000) return;
    last_decay = millis();
    if (vis_rx_wifi_count > 0) vis_rx_wifi_count--;
    if (vis_rx_ble_count  > 0) vis_rx_ble_count--;
}
static uint32_t bridge_tx_events = 0;
static uint32_t bridge_rx_cmds   = 0;
static char     last_cmd_label[16] = "--";

// Device ID — last 4 bytes of MAC, formatted A1B2C3D4
static char device_id[12] = "????????";

static void compute_device_id() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(device_id, sizeof(device_id), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
}

// Hook for the rest of bridge_app to record an event being sent.
// Call this just before Serial.println(jsonLine).
static void vis_record_tx(const char* jsonLine) {
    bridge_tx_events++;
    strncpy(vis_last_json, jsonLine, sizeof(vis_last_json) - 1);
    vis_last_json[sizeof(vis_last_json) - 1] = 0;
    vis_last_json_ms = millis();
    vis_last_was_tx  = true;
    vis_last_stream_ms = millis();
    // Pulse the LoRa trace upward on outgoing events
    // so the oscilloscope shows activity even without real LoRa radio TX
    lora_trace_pulse(true);
}

static void vis_record_rx(const char* cmdName) {
    bridge_rx_cmds++;
    strncpy(last_cmd_label, cmdName, sizeof(last_cmd_label) - 1);
    last_cmd_label[sizeof(last_cmd_label) - 1] = 0;
    vis_last_stream_ms = millis();
    // Pulse the LoRa trace downward on incoming commands
    lora_trace_pulse(false);
}

// Capability probes — defensive checks so bridge doesn't crash
// when an expected radio peripheral isn't actually attached.
static bool wifi_subsystem_ready() {
    // WiFi.h is always linked; consider it ready if wardrive globals exist
    return true;  // SDK always presents WiFi in some state
}

static bool ble_subsystem_ready() {
    // BLE is always available on ESP32-S3
    return true;
}

static bool lora_subsystem_ready() {
    // LoRa SX1262 is part of the T-Deck Plus hardware. We always
    // consider it ready; the trace will sit flat if no TX/RX happens.
    // If a future board variant ships without LoRa, this can probe
    // a global init flag here and return false.
    return true;
}

static bool gps_subsystem_ready() {
    // TinyGPSPlus exists; ready means we've ever parsed a sentence
    extern TinyGPSPlus gps;
    return gps.charsProcessed() > 10;
}

static bool nfc_subsystem_ready() {
    // T-Deck Plus has no built-in NFC. Always false unless a future
    // hardware revision exposes it. Visualizer will show "—".
    return false;
}

static bool rfid_subsystem_ready() {
    return false;
}

// ─────────────────────────────────────────────
//  BAR / SPIKE / PULSE PRIMITIVES
// ─────────────────────────────────────────────
static void draw_bar(int x, int y, int w, int h, int value, int max_val,
                     uint16_t fillColor) {
    // Clamp value 0..max_val
    if (value < 0) value = 0;
    if (value > max_val) value = max_val;
    int filled = (value * w) / max_val;

    gfx->drawRect(x, y, w, h, 0x4208);                  // dim border
    gfx->fillRect(x + 1, y + 1, w - 2, h - 2, C_BLACK); // wipe inside
    if (filled > 2) {
        gfx->fillRect(x + 1, y + 1, filled - 2, h - 2, fillColor);
    }
}

// Horizontal oscilloscope-style trace. Center line = idle.
// Up = TX, Down = RX. Trace is a circular buffer.
static void draw_lora_trace(int x, int y, int w, int h) {
    int cy = y + h / 2;
    gfx->fillRect(x, y, w, h, C_BLACK);
    gfx->drawFastHLine(x, cy, w, 0x4208);  // center idle line

    int samples = (w < VIS_LORA_HISTORY) ? w : VIS_LORA_HISTORY;
    for (int i = 0; i < samples; i++) {
        int idx = (lora_trace_head + VIS_LORA_HISTORY - samples + i) % VIS_LORA_HISTORY;
        int v = lora_trace[idx];
        if (v == 0) continue;
        int spike_h = (v * (h / 2 - 1)) / 64;  // scale -64..64 to -h/2..h/2
        if (spike_h > 0) {
            gfx->drawFastVLine(x + i, cy - spike_h, spike_h, C_GREEN);   // TX up
        } else if (spike_h < 0) {
            gfx->drawFastVLine(x + i, cy, -spike_h, C_CYAN);             // RX down
        }
    }
}

// Decay the LoRa trace one step — call each redraw tick
static void lora_trace_decay() {
    for (int i = 0; i < VIS_LORA_HISTORY; i++) {
        if (lora_trace[i] > 0) {
            int v = lora_trace[i] - VIS_LORA_DECAY;
            lora_trace[i] = (v < 0) ? 0 : v;
        } else if (lora_trace[i] < 0) {
            int v = lora_trace[i] + VIS_LORA_DECAY;
            lora_trace[i] = (v > 0) ? 0 : v;
        }
    }
}

// Push a new sample into the trace (called when LoRa TX/RX detected)
static void lora_trace_pulse(bool outgoing) {
    lora_trace[lora_trace_head] = outgoing ? 60 : -60;
    lora_trace_head = (lora_trace_head + 1) % VIS_LORA_HISTORY;
    if (outgoing) lora_tx_count++;
    else          lora_rx_count++;
}

// Pulse character: ◐ if recently active, · if idle, — if not present
static const char* pulse_glyph(bool present, uint32_t last_event_ms) {
    if (!present) return "--";
    if (millis() - last_event_ms < 400) return "*";  // active
    return ".";                                        // idle
}

// ─────────────────────────────────────────────
//  TOGGLE STATE
// ─────────────────────────────────────────────
static int  tog_cursor       = 0;
static bool bridge_ble_enabled = true;
static bool bridge_gps_enabled = true;

// Toggle layout — single horizontal row
#define TOG_Y       38
#define TOG_H       18
#define TOG_ROW_W   78    // each of 4 cells across 320px = 80px each

static const char* tog_short[] = { "WD", "BLE", "GPS", "PM" };

static bool tog_state(int i) {
    switch (i) {
        case 0: return wardrive_active;
        case 1: return bridge_ble_enabled;
        case 2: return bridge_gps_enabled;
        case 3: return (wardrive_mode == WARDRIVE_MODE_PROMISCUOUS);
        default: return false;
    }
}

static void tog_apply(int i) {
    switch (i) {
        case 0:
            wardrive_active = !wardrive_active;
            if (!wardrive_active) wardrive_bridge_streaming = false;
            break;
        case 1:
            bridge_ble_enabled = !bridge_ble_enabled;
            break;
        case 2:
            bridge_gps_enabled = !bridge_gps_enabled;
            break;
        case 3:
            if (wardrive_mode == WARDRIVE_MODE_PROMISCUOUS) {
                wardrive_set_mode(WARDRIVE_MODE_SCAN);
            } else {
                wardrive_set_mode(WARDRIVE_MODE_PROMISCUOUS);
                wardrive_active = true;
            }
            break;
    }
}

static void draw_toggle_row() {
    for (int i = 0; i < 4; i++) {
        int x   = i * 80;
        bool on = tog_state(i);
        bool sel = (i == tog_cursor);

        gfx->fillRect(x, TOG_Y, 79, TOG_H, C_DARK);
        if (sel) gfx->drawRect(x, TOG_Y, 79, TOG_H, C_CYAN);

        // Label
        gfx->setCursor(x + 4, TOG_Y + 5);
        gfx->setTextColor(sel ? C_CYAN : C_WHITE);
        gfx->setTextSize(1);
        gfx->print(tog_short[i]);

        // Badge
        uint16_t bc = on ? C_GREEN : 0x4208;
        gfx->fillRect(x + 30, TOG_Y + 3, 42, 12, on ? 0x0180 : 0x0841);
        gfx->drawRect(x + 30, TOG_Y + 3, 42, 12, bc);
        gfx->setCursor(x + 34, TOG_Y + 5);
        gfx->setTextColor(bc);
        gfx->print(on ? " ON " : " OFF");
    }
}

// ─────────────────────────────────────────────
//  FULL REDRAW
// ─────────────────────────────────────────────
static void drawBridgeShell(bool connected) {
    gfx->fillScreen(C_BLACK);

    // Header bar
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, 0x07FF);
    gfx->setCursor(10, 7);
    gfx->setTextColor(0x07FF);
    gfx->setTextSize(1);
    gfx->print("BRIDGE | " PM_EXIT_COPY);

    // Status strip
    gfx->fillRect(0, 25, 320, 12, C_DARK);

    // Toggle row
    draw_toggle_row();
    gfx->drawFastHLine(0, TOG_Y + TOG_H, 320, 0x4208);

    // Section labels
    int body_top = TOG_Y + TOG_H + 3;
    gfx->setTextColor(0x4208);
    gfx->setTextSize(1);
    gfx->setCursor(8,   body_top);      gfx->print("WiFi");
    gfx->setCursor(8,   body_top + 22); gfx->print("BLE");
    gfx->setCursor(8,   body_top + 44); gfx->print("GPS");
    gfx->setCursor(8,   body_top + 72); gfx->print("LoRa");

    // Dividers
    gfx->drawFastHLine(0, body_top + 67, 320, 0x4208);  // above LoRa
    gfx->drawFastHLine(0, 207,           320, 0x4208);  // above stream
}

// ─────────────────────────────────────────────
//  PARTIAL UPDATE — 10Hz
// ─────────────────────────────────────────────
static void drawBridgeUpdate(bool connected) {
    extern int      networks_found;
    extern volatile int bt_found;
    extern uint32_t last_scan_ms;

    // ── Status strip ──────────────────────────
    gfx->fillRect(0, 25, 320, 12, C_DARK);
    gfx->setTextSize(1);

    gfx->setTextColor(C_GREY);
    gfx->setCursor(8, 27);
    gfx->printf("ID:%s", device_id);

    gfx->setTextColor(connected ? C_GREEN : 0xFD20);
    gfx->setCursor(100, 27);
    gfx->print(connected ? "HOST:CONN" : "HOST:----");

    bool wd_strm = wardrive_bridge_streaming;
    bool blink   = ((millis() / 500) & 1);
    gfx->setTextColor(wd_strm ? (blink ? C_RED : 0xFD20) : 0x4208);
    gfx->setCursor(200, 27);
    gfx->print(wd_strm ? "STREAMING" : "LOCAL    ");

    // ── Toggle row ────────────────────────────
    draw_toggle_row();

    // ── Body layout ───────────────────────────
    int bx  = 44;    // bar start x (after label)
    int bw  = 252;   // bar width
    int bh  = 13;
    int bt  = TOG_Y + TOG_H + 4;  // body top

    // WiFi bar — current scan count only
    gfx->fillRect(bx, bt, bw + 24, bh, C_BLACK);
    int wn = wardrive_active ? min(networks_found, VIS_WIFI_MAX) : 0;
    draw_bar(bx, bt, bw, bh, wn, VIS_WIFI_MAX, C_GREEN);
    gfx->setCursor(bx + bw + 3, bt + 2);
    gfx->setTextColor(C_GREEN);
    gfx->printf("%2d", wn);

    // BLE bar — current window count only
    gfx->fillRect(bx, bt + 22, bw + 24, bh, C_BLACK);
    int bn = (wardrive_active && bridge_ble_enabled) ? min((int)bt_found, VIS_BLE_MAX) : 0;
    draw_bar(bx, bt + 22, bw, bh, bn, VIS_BLE_MAX, 0x07FF);
    gfx->setCursor(bx + bw + 3, bt + 24);
    gfx->setTextColor(0x07FF);
    gfx->printf("%2d", bn);

    // GPS line — full width, clean
    gfx->fillRect(bx, bt + 44, 276, 18, C_BLACK);
    if (!bridge_gps_enabled) {
        gfx->setTextColor(0x4208);
        gfx->setCursor(bx, bt + 47);
        gfx->print("disabled");
    } else if (gps_subsystem_ready() && gps.location.isValid()) {
        gfx->setTextColor(C_GREEN);
        gfx->setCursor(bx, bt + 47);
        gfx->printf("%.4f  %.4f  %.0fm  %dsat",
            gps.location.lat(), gps.location.lng(),
            gps.altitude.meters(), (int)gps.satellites.value());
    } else if (gps_subsystem_ready()) {
        gfx->setTextColor(0xFD20);
        gfx->setCursor(bx, bt + 47);
        gfx->print("searching...");
    } else {
        gfx->setTextColor(0x4208);
        gfx->setCursor(bx, bt + 47);
        gfx->print("not present");
    }

    // Promiscuous overlay — replaces GPS line when active
    if (wardrive_mode == WARDRIVE_MODE_PROMISCUOUS && wardrive_active) {
        pm_stats_t ps;
        pm_promiscuous_get_stats(&ps);
        uint8_t ch = pm_promiscuous_channel();
        gfx->fillRect(bx, bt + 44, 276, 18, C_BLACK);
        gfx->setTextColor(ps.deauth_count ? C_RED : C_GREEN);
        gfx->setCursor(bx, bt + 47);
        gfx->printf("BCN:%-4lu PRB:%-4lu DTH:%-3lu ch%-2d %lu/s",
            ps.beacon_count, ps.probe_req_count,
            ps.deauth_count, ch, ps.frames_per_sec);
    }

    // LoRa trace — full width oscilloscope
    gfx->fillRect(0, 170, 320, 36, C_BLACK);
    if (lora_subsystem_ready()) {
        lora_trace_decay();
        draw_lora_trace(44, 170, 276, 36);
        gfx->setTextColor(0x4208);
        gfx->setCursor(8, 182);
        gfx->print("TX^");
        gfx->setCursor(8, 192);
        gfx->print("RXv");
    } else {
        gfx->setTextColor(0x4208);
        gfx->setCursor(bx, 182);
        gfx->print("no radio");
    }

    // ── Stream line ───────────────────────────
    gfx->fillRect(0, 208, 320, 18, C_BLACK);
    if (vis_last_json[0]) {
        uint32_t age = millis() - vis_last_json_ms;
        uint16_t col = age < 200  ? (vis_last_was_tx ? C_GREEN : 0x07FF)
                     : age < 1200 ? (vis_last_was_tx ? 0x03E0  : 0x03FF)
                     : 0x4208;
        gfx->setTextColor(col);
        gfx->setCursor(4, 210);
        gfx->print(vis_last_was_tx ? "<" : ">");
        // Truncate to fit — 52 chars at size 1
        char buf[53];
        int  len = strlen(vis_last_json);
        if (len > 51) { strncpy(buf, vis_last_json, 48); strcpy(buf+48, "..."); }
        else           strncpy(buf, vis_last_json, 52);
        buf[52] = 0;
        gfx->setCursor(12, 210);
        gfx->print(buf);
    } else {
        gfx->setTextColor(0x4208);
        gfx->setCursor(4, 210);
        gfx->print("(idle)");
    }

    // ── Uptime ────────────────────────────────
    int footerY = max(0, gfx->height() - 14);
    gfx->fillRect(0, footerY, gfx->width(), 14, C_DARK);
    uint32_t up = millis() / 1000;
    gfx->setTextColor(0x4208);
    gfx->setCursor(4, footerY + 3);
    gfx->printf("Up:%02d:%02d:%02d  [W][B][G][P]  TB:sel  CLK:tog",
        (int)(up/3600), (int)((up/60)%60), (int)(up%60));
}

// ─────────────────────────────────────────────
//  MAIN BRIDGE LOOP
//  v1.1: Drives the visualizer at 10Hz between
//  serial reads. Records every TX event for the
//  rolling JSON line and pulses NFC/RFID/LoRa
//  trace based on command type.
// ─────────────────────────────────────────────
void run_bridge() {
    // Ensure Serial is at the right baud
    Serial.flush();
    Serial.begin(BRIDGE_BAUD);
    delay(100);

    // Compute device ID once at start
    compute_device_id();

    // Reset visualizer counters for a fresh session
    bridge_tx_events = 0;
    bridge_rx_cmds   = 0;
    vis_last_json[0] = 0;
    last_cmd_label[0] = '-';
    last_cmd_label[1] = '-';
    last_cmd_label[2] = 0;
    for (int i = 0; i < VIS_LORA_HISTORY; i++) lora_trace[i] = 0;

    // Static layout once, then partial updates only
    drawBridgeShell(false);
    drawBridgeUpdate(false);

    char cmdBuf[CMD_BUF_LEN];
    int  cmdLen     = 0;
    bool connected  = false;
    unsigned long lastActivity = millis();
    unsigned long lastVisUpdate = 0;

    // Announce readiness
    const char* readyMsg = "{\"event\":\"ready\",\"os\":\"Pisces Moon OS\",\"version\":" PISCES_OS_VERSION "}";
    vis_record_tx(readyMsg);
    Serial.println(readyMsg);

    while (true) {
        // ── Exit ──────────────────────────────
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 40) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            wardrive_bridge_streaming = false;  // v1.1 — stop emitting
            const char* msg = "{\"event\":\"disconnect\",\"reason\":\"user_exit\"}";
            vis_record_tx(msg);
            Serial.println(msg);
            break;
        }
        char k = get_keypress();
        if (pm_is_exit_key(k)) {
            wardrive_bridge_streaming = false;
            const char* msg = "{\"event\":\"disconnect\",\"reason\":\"user_exit\"}";
            vis_record_tx(msg);
            Serial.println(msg);
            break;
        }
        // Direct toggle hotkeys
        if (k == 'w' || k == 'W') { tog_cursor = 0; tog_apply(0); }
        if (k == 'b' || k == 'B') { tog_cursor = 1; tog_apply(1); }
        if (k == 'g' || k == 'G') { tog_cursor = 2; tog_apply(2); }
        if (k == 'p' || k == 'P') { tog_cursor = 3; tog_apply(3); }

        // Trackball navigation + click to toggle
        TrackballState tb = update_trackball();
        if (tb.y == -1 && tog_cursor > 0) tog_cursor--;
        if (tb.y ==  1 && tog_cursor < 3)  tog_cursor++;
        if (tb.clicked) tog_apply(tog_cursor);

        // Touch — tap a toggle cell directly (horizontal row)
        if (get_touch(&tx, &ty) && ty >= TOG_Y && ty < TOG_Y + TOG_H) {
            int col = tx / 80;
            if (col >= 0 && col < 4) {
                tog_cursor = col;
                tog_apply(col);
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
            }
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

                    // Record incoming command for the visualizer
                    vis_record_rx(cmd[0] ? cmd : "?");

                    if      (strcmp(cmd,"ping")==0)             cmdPing();
                    else if (strcmp(cmd,"status")==0)           cmdStatus();
                    else if (strcmp(cmd,"wardrive_status")==0)  cmdWardriveStatus();
                    else if (strcmp(cmd,"wardrive_start")==0) {
                        wardrive_set_mode(WARDRIVE_MODE_SCAN);
                        wardrive_active = true;
                        wardrive_bridge_streaming = true;
                        cmdWardriveStatus();
                    }
                    else if (strcmp(cmd,"wardrive_stop")==0) {
                        wardrive_active = false;
                        wardrive_bridge_streaming = false;
                        cmdWardriveStatus();
                    }
                    else if (strcmp(cmd,"promiscuous_start")==0) {
                        wardrive_set_mode(WARDRIVE_MODE_PROMISCUOUS);
                        wardrive_active = true;
                        wardrive_bridge_streaming = true;
                        Serial.println("{\"ok\":true,\"msg\":\"promiscuous mode starting\"}");
                    }
                    else if (strcmp(cmd,"promiscuous_stop")==0) {
                        wardrive_active = false;
                        wardrive_bridge_streaming = false;
                        wardrive_set_mode(WARDRIVE_MODE_SCAN);
                        Serial.println("{\"ok\":true,\"msg\":\"promiscuous mode stopped\"}");
                    }
                    else if (strcmp(cmd,"promiscuous_lock")==0) {
                        int ch = doc["channel"] | 1;
                        pm_promiscuous_lock_channel((uint8_t)ch);
                        Serial.printf("{\"ok\":true,\"msg\":\"locked ch %d\"}\n", ch);
                    }
                    else if (strcmp(cmd,"promiscuous_unlock")==0) {
                        pm_promiscuous_unlock_channel();
                        Serial.println("{\"ok\":true,\"msg\":\"channel hopping resumed\"}");
                    }
                    else if (strcmp(cmd,"promiscuous_filter")==0) {
                        uint32_t mask = doc["mask"] | 0xFFFF;
                        pm_promiscuous_set_subtype_mask(mask);
                        Serial.printf("{\"ok\":true,\"msg\":\"filter mask 0x%04X\"}\n", mask);
                    }
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

        // ── Visualizer update (10Hz) ──────────
        if (millis() - lastVisUpdate >= 100) {
            lastVisUpdate = millis();
            // Redraw the shell if mode changed (labels are different per mode)
            static wardrive_mode_t last_drawn_mode = WARDRIVE_MODE_SCAN;
            if (wardrive_mode != last_drawn_mode) {
                last_drawn_mode = wardrive_mode;
                drawBridgeShell(connected);  // repaint labels for new mode
            }
            drawBridgeUpdate(connected);
        }

        // ── Connection timeout ────────────────
        if (connected && millis() - lastActivity > 30000) {
            connected = false;
            const char* msg = "{\"event\":\"timeout\"}";
            vis_record_tx(msg);
            Serial.println(msg);
        }

        delay(10); yield();
    }
}
