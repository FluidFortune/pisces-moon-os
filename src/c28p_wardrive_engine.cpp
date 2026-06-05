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

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "wardrive.h"
#include "nosql_store.h"

// ─────────────────────────────────────────────
//  CSV LOG BACKEND (v1.3 — via pm_storage HAL)
//
//  In addition to NoSQL JSON entries, we maintain a parallel CSV log
//  file at the root of the SD card so external tools (Wigle uploaders,
//  awk scripts, spreadsheets) can ingest sessions directly. The CSV
//  writer used to repeat nosql_store.cpp's dual-backend dance with
//  WD_FS / WD_FILE / WD_OPEN_WRITE macros. v1.3 collapses both into
//  pm_storage::File operations — SD_MMC on C28P, SdFat on Maxine, no
//  visible branching at the call sites.
// ─────────────────────────────────────────────
#include "pm_storage.h"

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
//  CSV LOG STATE
//
//  One CSV file opened per scan-task lifetime. Filename rotates
//  on each launch (wardrive_001.csv, wardrive_002.csv, ...) so a
//  user pulling the card to a desktop sees discrete sessions
//  rather than one ever-growing file with mixed timestamps.
//
//  Filename rotation: scan upward from 1 to 999 looking for the
//  first slot that doesn't yet exist on disk. 999 is the cap;
//  beyond that we reuse 999 and append (no truncation) so logging
//  never silently stops. In practice nobody hits 999 sessions on
//  a single SD card before reformatting.
// ─────────────────────────────────────────────
static pm_storage::File s_csv_file;
static bool             s_csv_open = false;
static char             s_csv_filename[32] = {0};

// Quote an SSID for CSV. Wraps in double quotes and escapes any
// embedded double-quote by doubling it (RFC 4180). Truncates at
// 32 chars to bound the line length. Output buffer must hold at
// least 68 bytes (2 + 32 + 32 + 2 + 1).
static void csv_quote_ssid(const char* in, char* out, size_t outsz) {
    size_t w = 0;
    if (outsz < 4) { if (outsz) out[0] = 0; return; }
    out[w++] = '"';
    size_t i = 0;
    while (in[i] && i < 32 && w < outsz - 3) {
        char c = in[i++];
        if (c == '"' && w < outsz - 4) { out[w++] = '"'; out[w++] = '"'; }
        else if (c < 0x20 || c == ',') out[w++] = ' ';   // sanitize control / delimiter
        else out[w++] = c;
    }
    out[w++] = '"';
    out[w] = 0;
}

static void wd_csv_open() {
    if (s_csv_open) return;

    // Find next available filename
    int slot = 1;
    for (; slot <= 999; slot++) {
        snprintf(s_csv_filename, sizeof(s_csv_filename),
                 "/wardrive_%03d.csv", slot);
        if (!pm_storage::exists(s_csv_filename)) break;
    }
    if (slot > 999) {
        snprintf(s_csv_filename, sizeof(s_csv_filename),
                 "/wardrive_999.csv");
    }

    s_csv_file = pm_storage::open(s_csv_filename, pm_storage::Mode::Write);
    if (!s_csv_file) {
        Serial.printf("[C28P-WD-Engine] CSV open failed: %s\n",
                      s_csv_filename);
        s_csv_open = false;
        return;
    }
    // Header row. Columns mirror what mobile wardrive.cpp writes
    // minus the GPS fields (kiosk has no GPS). Wigle uploaders that
    // expect lat/lon will see empty columns; that's the convention
    // for stationary observations across the v1.2.x family.
    s_csv_file.print("bssid,ssid,rssi,channel,encryption,t_ms,"
                     "observed_uptime_s\n");
    s_csv_file.flush();
    s_csv_open = true;
    Serial.printf("[C28P-WD-Engine] CSV log opened: %s\n", s_csv_filename);
}

static void wd_csv_close() {
    if (!s_csv_open) return;
    s_csv_file.close();
    s_csv_open = false;
    Serial.printf("[C28P-WD-Engine] CSV log closed: %s\n", s_csv_filename);
}

// Append one observation row. Called from do_wifi_scan() per
// network seen, alongside the NoSQL JSON write.
static void wd_csv_write_row(const uint8_t bssid[6], const char* ssid,
                              int rssi, int channel, int encryption) {
    if (!s_csv_open) return;
    char ssid_q[72];
    csv_quote_ssid(ssid ? ssid : "", ssid_q, sizeof(ssid_q));
    s_csv_file.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,%d,%d,%d,%lu,%lu\n",
                      bssid[0], bssid[1], bssid[2],
                      bssid[3], bssid[4], bssid[5],
                      ssid_q, rssi, channel, encryption,
                      (unsigned long)millis(),
                      (unsigned long)(millis() / 1000));
    // Periodic flush so a power-cut doesn't lose the trailing rows.
    // Every observation flush would slow the scan loop, so we batch
    // — a 32-row interval keeps worst-case loss tiny on a typical
    // 5-30 networks/scan setup.
    static int rows_since_flush = 0;
    if (++rows_since_flush >= 32) {
        s_csv_file.flush();
        rows_since_flush = 0;
    }
}

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
            // CSV mirror for desktop / Wigle tooling parity. Independent
            // of the NoSQL write — same data, different format.
            wd_csv_write_row(bssid_ptr, ssid.c_str(), rssi,
                             WiFi.channel(i),
                             (int)WiFi.encryptionType(i));
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

    // Open the CSV log file. One file per task lifetime; rotates on
    // each launch so external tooling sees clean session boundaries.
    wd_csv_open();

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

    // Close the CSV log file so the trailing rows are flushed and
    // the file handle isn't leaked across teardown / restart cycles.
    wd_csv_close();

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
    // After v1.2.2, the C28P/Maxine engine does write a CSV — return
    // the current session's filename so callers (Bridge app, file
    // manager) can display or copy it. Empty string means "no log
    // currently open" (scan task not running, or open failed).
    return s_csv_open ? s_csv_filename : "";
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
// On C28P the UI is c28p_run_wardrive() (touch-driven). On Maxine the
// UI is maxine_run_wardrive() (also touch-driven). Both kiosks dispatch
// to their own touch UI; the keyboard-driven mobile UI isn't relevant.
// We provide a device-aware stub so wardrive.h's declaration is
// satisfied without pulling in c28p_wardrive.cpp on Maxine.
void run_wardrive() {
#ifdef DEVICE_C28P
    extern void c28p_run_wardrive();
    c28p_run_wardrive();
#else
    // Maxine: the launcher dispatches maxine_run_wardrive() directly.
    // run_wardrive() should never be called here — stub to satisfy
    // wardrive.h's declaration.
#endif
}

#endif // DEVICE_C28P || DEVICE_MAXINE