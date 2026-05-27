// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_wardrive_engine.cpp — Stationary scan engine for C28P
//
//  PURPOSE:
//
//  A C28P-specific replacement for wardrive.cpp's scan task.
//  The existing wardrive.cpp is designed for mobile wardriving:
//  GPS timestamp interleaving, location columns in the CSV,
//  channel hopping correlated with motion. The C28P has none
//  of that context, and forcing the mobile engine into a
//  stationary use case carries complexity it doesn't need.
//
//  This file implements ONLY what the C28P needs:
//    - WiFi scan loop pinned to Core 0 (Ghost Engine principle:
//      once started, never stops)
//    - BLE scan window interleaved with WiFi
//    - NoSQL writes of observations to "wardrive" category
//    - Anomaly detection hooks (c28p_anomaly_observe per result,
//      c28p_anomaly_scan_complete at end of each scan)
//    - Stats counters that the UI reads (networks_total, ble_total)
//
//  SHARED SYMBOLS WITH wardrive.cpp:
//
//  Because the C28P doesn't compile wardrive.cpp into its build
//  (see platformio.ini src_filter), we provide our own definitions
//  of the externs that wardrive.h declares:
//    networks_total, ble_total, esp_found, wardrive_active,
//    last_scan_ms, wardrive_mode, wifi_in_use, sd_in_use,
//    wardrive_promiscuous_active, wardrive_bridge_streaming,
//    wardrive_raw_log, networks_found, bt_found
//
//  Also: init_wardrive_core(), wardrive_teardown(),
//        wardrive_set_mode(), wardrive_ble_stop(),
//        wardrive_ble_resume(), wardrive_task()
//
//  This means the C28P link satisfies wardrive.h's declarations
//  without pulling in the full mobile engine.
//
//  TASK ARCHITECTURE:
//
//    Spawned: xTaskCreatePinnedToCore on Core 0
//    Stack:   6 KB (smaller than mobile engine — no CSV columns)
//    Priority: 1
//    Loop:    every ~3s, WiFi scan, then BLE 1s window, then sleep
//
//  The task NEVER exits voluntarily. wardrive_teardown() is the
//  only way to stop it, used by the UI's PAUSE button.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "wardrive.h"
#include "nosql_store.h"

// Anomaly detection hooks (c28p_anomaly.cpp)
extern void c28p_anomaly_init();
extern bool c28p_anomaly_observe(const uint8_t bssid[6], int rssi, const char* ssid);
extern void c28p_anomaly_scan_complete(const uint8_t seen_bssids[][6], int seen_count);

// ─────────────────────────────────────────────
//  Shared symbol definitions
//
//  These are declared extern in wardrive.h. Mobile devices get
//  them from wardrive.cpp; the C28P gets them from here.
// ─────────────────────────────────────────────
int  networks_found = 0;
volatile int  bt_found = 0;
int  esp_found = 0;
bool wardrive_active = false;
wardrive_mode_t wardrive_mode = WARDRIVE_MODE_SCAN;
volatile bool wardrive_promiscuous_active = false;
int  networks_total = 0;
int  ble_total = 0;
uint32_t last_scan_ms = 0;
// wifi_in_use and sd_in_use are owned by gemini_client.cpp.
// We use them as externs here, not redefine them.
extern volatile bool wifi_in_use;
extern volatile bool sd_in_use;
volatile bool wardrive_bridge_streaming = false;
volatile bool wardrive_raw_log = false;

static TaskHandle_t s_wd_task = nullptr;
static volatile bool s_stop_requested = false;
static SemaphoreHandle_t s_stop_done = nullptr;

// ─────────────────────────────────────────────
//  BLE scan helpers
// ─────────────────────────────────────────────
static NimBLEScan* s_ble_scanner = nullptr;
static bool s_ble_initialized = false;

static void ensure_ble_init() {
    if (s_ble_initialized) return;
    NimBLEDevice::init("");
    s_ble_scanner = NimBLEDevice::getScan();
    s_ble_scanner->setActiveScan(false);   // passive — quieter, less power
    s_ble_scanner->setInterval(100);
    s_ble_scanner->setWindow(80);
    s_ble_initialized = true;
}

// ─────────────────────────────────────────────
//  WiFi scan iteration
//
//  Performs one scan, walks the results, writes to NoSQL and
//  feeds the anomaly detector. Returns the number of networks
//  observed.
// ─────────────────────────────────────────────
static int do_wifi_scan() {
    wifi_in_use = true;

    int n = WiFi.scanNetworks(false, true);   // passive scan, show hidden
    if (n < 0) {
        wifi_in_use = false;
        return 0;
    }

    // Buffer for anomaly_scan_complete — captures BSSIDs seen this scan
    // Cap at 64 entries to bound the stack footprint; in dense RF a
    // C28P sees ~10-30 networks typically. Anything over 64 just gets
    // observed individually but not included in the "vanished" delta.
    static uint8_t scan_bssids[64][6];
    int captured = 0;

    for (int i = 0; i < n; i++) {
        uint8_t* bssid_ptr = WiFi.BSSID(i);
        if (!bssid_ptr) continue;

        int rssi = WiFi.RSSI(i);
        String ssid = WiFi.SSID(i);

        // Feed the anomaly detector
        bool is_new = c28p_anomaly_observe(bssid_ptr, rssi, ssid.c_str());
        (void)is_new;

        // Capture for the post-scan vanished-set computation
        if (captured < 64) {
            memcpy(scan_bssids[captured], bssid_ptr, 6);
            captured++;
        }

        // Write observation to NoSQL "wardrive" category. The C28P
        // has no GPS so we record lat/lon as 0. The Wigle correlation
        // (planned v1.3) can match BSSID against other devices'
        // wardrive entries to recover location.
        char title[40];
        snprintf(title, sizeof(title), "%lu_%02X%02X%02X%02X%02X%02X",
                 (unsigned long)(millis() / 1000),
                 bssid_ptr[0], bssid_ptr[1], bssid_ptr[2],
                 bssid_ptr[3], bssid_ptr[4], bssid_ptr[5]);

        char content[180];
        snprintf(content, sizeof(content),
                 "{\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"ssid\":\"%.32s\",\"rssi\":%d,\"ch\":%d,"
                 "\"enc\":%d,\"t_ms\":%lu}",
                 bssid_ptr[0], bssid_ptr[1], bssid_ptr[2],
                 bssid_ptr[3], bssid_ptr[4], bssid_ptr[5],
                 ssid.c_str(), rssi, WiFi.channel(i),
                 (int)WiFi.encryptionType(i),
                 (unsigned long)millis());

        // Best-effort write — if SD is busy, skip this observation
        // rather than block the scan loop.
        if (!sd_in_use) {
            nosql_save_entry("wardrive", title, content);
        }
    }

    networks_found = n;
    networks_total += n;
    last_scan_ms = millis();

    WiFi.scanDelete();
    wifi_in_use = false;

    // Finalize anomaly bookkeeping for this scan window
    c28p_anomaly_scan_complete(scan_bssids, captured);

    return n;
}

// ─────────────────────────────────────────────
//  BLE scan window
//
//  Active for ~1s, then results are walked and counted.
//  BLE results don't go into the "wardrive" category — they
//  get a separate "ble_log" category to avoid mixing the two
//  data streams.
// ─────────────────────────────────────────────
static int do_ble_scan() {
    ensure_ble_init();
    if (!s_ble_scanner) return 0;

    NimBLEScanResults results = s_ble_scanner->start(1, false);
    int count = results.getCount();
    bt_found = count;
    ble_total += count;

    for (int i = 0; i < count; i++) {
        // NimBLE 1.4.1: getDevice(i) returns NimBLEAdvertisedDevice by value.
        // The returned object's accessors are non-const, so we can't hold
        // a const reference — store by value and call methods directly.
        NimBLEAdvertisedDevice dev = results.getDevice(i);

        std::string addr_str = dev.getAddress().toString();
        std::string name = dev.getName();
        int rssi = dev.getRSSI();

        char title[40];
        snprintf(title, sizeof(title), "%lu_%s",
                 (unsigned long)(millis() / 1000), addr_str.c_str());

        char content[180];
        snprintf(content, sizeof(content),
                 "{\"addr\":\"%s\",\"name\":\"%.32s\",\"rssi\":%d,\"t_ms\":%lu}",
                 addr_str.c_str(), name.c_str(), rssi,
                 (unsigned long)millis());

        if (!sd_in_use) {
            nosql_save_entry("ble_log", title, content);
        }
    }

    s_ble_scanner->clearResults();
    return count;
}

// ─────────────────────────────────────────────
//  Scan task
//
//  Pinned to Core 0. Loops forever, scanning WiFi then BLE,
//  with a short idle period between cycles. Exits only when
//  s_stop_requested is set (used by wardrive_teardown()).
// ─────────────────────────────────────────────
static void c28p_wardrive_task(void* /*pv*/) {
    Serial.println("[C28P-WD-Engine] Scan task starting on Core 0");

    // Init NoSQL categories used by this engine
    nosql_init("wardrive");
    nosql_init("ble_log");
    c28p_anomaly_init();

    // Ensure WiFi is in station mode (no AP). Doesn't actually
    // connect — just enables the scanner.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, false);   // disconnect but keep credentials
    delay(100);

    wardrive_active = true;
    s_stop_requested = false;

    while (!s_stop_requested) {
        // C28P always runs WiFi + BLE in v1.2.1 — no per-radio mode toggle.
        // PROMISCUOUS mode (raw 802.11 monitor) is a v1.3 feature; for now
        // the C28P sticks to active scan + BLE passive.
        do_wifi_scan();
        if (s_stop_requested) break;
        do_ble_scan();
        // Idle window between scan cycles. The C28P is stationary —
        // there's no rush. 2s between cycles keeps CPU usage low and
        // gives the BLE radio time to settle.
        for (int i = 0; i < 100 && !s_stop_requested; i++) {
            delay(20);
        }
    }

    wardrive_active = false;
    Serial.println("[C28P-WD-Engine] Scan task exiting");

    if (s_stop_done) xSemaphoreGive(s_stop_done);
    s_wd_task = nullptr;
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────
//  Public API — matches wardrive.h declarations
// ─────────────────────────────────────────────
void init_wardrive_core() {
    if (wardrive_active || s_wd_task) {
        Serial.println("[C28P-WD-Engine] init_wardrive_core: already running");
        return;
    }

    if (!s_stop_done) {
        s_stop_done = xSemaphoreCreateBinary();
    }

    xTaskCreatePinnedToCore(c28p_wardrive_task, "C28P_WD",
                            6144, nullptr, 1, &s_wd_task, 0);
    Serial.println("[C28P-WD-Engine] Scan task spawned");
}

bool wardrive_teardown(uint32_t timeout_ms) {
    if (!wardrive_active && !s_wd_task) return true;

    s_stop_requested = true;
    if (s_stop_done) {
        if (xSemaphoreTake(s_stop_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            return true;
        }
    }
    // Last-resort: scan task may be stuck inside scanNetworks().
    // Don't forcibly kill — that would leak the WiFi driver. Just
    // report failure and let the next scan cycle finish.
    Serial.printf("[C28P-WD-Engine] teardown timeout after %lums\n",
                  (unsigned long)timeout_ms);
    return false;
}

void wardrive_set_mode(wardrive_mode_t mode) {
    wardrive_mode = mode;
}

void wardrive_ble_stop() {
    // v1.2.1: C28P doesn't yet support pausing the BLE half independently.
    // The gamepad-BLE handoff use case isn't relevant on C28P (no gamepad).
    // Stub for ABI compatibility.
}

void wardrive_ble_resume() {
    // Counterpart to wardrive_ble_stop — also a stub on C28P.
}

const char* wardrive_get_log_filename() {
    // C28P uses NoSQL for wardrive observations, not a single CSV file.
    // Return empty string so any caller treating this as a path knows
    // there's no file-based log.
    return "";
}

// wardrive_task signature is referenced by wardrive.h for the
// mobile engine. C28P's task has a different signature internally;
// we provide a stub so the symbol resolves if anyone takes its
// address (no-op on C28P — use init_wardrive_core() instead).
void wardrive_task(void* /*pv*/) {
    // Should never be called on C28P. Spin briefly then exit.
    Serial.println("[C28P-WD-Engine] wardrive_task stub called — ignored");
    vTaskDelete(nullptr);
}

// run_wardrive() is the keyboard-driven UI from the mobile engine.
// On C28P the UI is c28p_run_wardrive() (touch-driven). Provide a
// stub so wardrive.h's declaration is satisfied; the real C28P UI
// is in c28p_wardrive.cpp.
void run_wardrive() {
    extern void c28p_run_wardrive();
    c28p_run_wardrive();
}

#endif // DEVICE_C28P