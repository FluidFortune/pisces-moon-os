// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c5_wardrive_engine.cpp — Dual-band scan engine for C5
//
//  WHAT'S NEW vs. c28p_wardrive_engine.cpp
//
//  The C5 is the only Pisces Moon device with dual-band WiFi 6
//  (2.4 GHz + 5 GHz). This engine is the C28P stationary engine
//  with two structural changes to exploit that hardware:
//
//  1. ASYNC WIFI SCAN
//     The C5 is single-core RISC-V. WiFi.scanNetworks(blocking)
//     freezes the UI for ~1.5 s every scan cycle. Here we use
//     the async variant: WiFi.scanNetworks(true, true) returns
//     immediately; the task polls WiFi.scanComplete() with short
//     vTaskDelay(20) yields between polls so the Arduino loop
//     (display / touch / launcher) stays responsive.
//
//  2. 5 GHz BAND ALTERNATION
//     Odd scan cycles set WIFI_BAND_MODE_5G_ONLY before scanning;
//     even cycles use WIFI_BAND_MODE_2G_ONLY. This doubles the
//     scan period per band but gives full spectral coverage —
//     5 GHz APs appear in results for the first time in the fleet.
//     The band is recorded in every log entry so post-processing
//     can separate the two populations.
//
//  SINGLE-CORE PRIORITY MODEL
//     The Arduino loop task runs at FreeRTOS priority 1.
//     This task is spawned at priority 1 — same tier — so the
//     scheduler time-slices between them. The vTaskDelay(20) in
//     the scan-wait loop and the idle window at the bottom of the
//     main loop each guarantee CPU handoff to the UI.
//     Do NOT raise this task above priority 1; on a single core
//     that would starve the display and the touch handler.
//
//  PMU1 UART FORWARDING
//     When a P4 is connected over the QWIIC-to-Grove cable, every
//     scan result is fan-fanned out as a PMU1 line (in addition to
//     the local SD / NoSQL write). The P4 receives WF and BLE
//     events exactly as it does from the Cardputer ADV peer. The
//     PING watchdog keeps the link alive; on timeout the C5 resets
//     its UART and re-sends HELLO.
//
//  SHARED SYMBOLS
//     Same set as c28p_wardrive_engine.cpp — this file provides the
//     wardrive.h externs so wardrive.cpp is not compiled for C5.
// ─────────────────────────────────────────────

#ifdef DEVICE_C5

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>          // esp_wifi_set_band_mode(), esp_wifi_scan_start()
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "wardrive.h"
#include "nosql_store.h"
#include "pm_storage.h"
#include "hal_c5.h"

// Anomaly detection (c28p_anomaly.cpp — shared between C28P, Maxine, C5)
extern void c28p_anomaly_init();
extern bool c28p_anomaly_observe(const uint8_t bssid[6], int rssi, const char* ssid);
extern void c28p_anomaly_scan_complete(const uint8_t seen_bssids[][6], int seen_count);

// ─────────────────────────────────────────────
//  Shared symbol definitions (wardrive.h externs)
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
extern volatile bool wifi_in_use;
extern volatile bool sd_in_use;
volatile bool wardrive_bridge_streaming = false;
volatile bool wardrive_raw_log = false;

static TaskHandle_t  s_wd_task     = nullptr;
static volatile bool s_stop_requested = false;
static SemaphoreHandle_t s_stop_done  = nullptr;

// ─────────────────────────────────────────────
//  PMU1 UART — peer link to P4
//
//  Formats and emits PMU1-protocol lines over UART1 at 921600 baud.
//  The P4-side parser in pm_c5_uart/ consumes these.
//
//  Lifecycle:
//    pmu1_init()        — begin UART, send HELLO
//    pmu1_emit_wifi()   — WF line per network observed
//    pmu1_emit_ble()    — BLE line per device observed
//    pmu1_poll()        — call periodically to read P4 commands
//    pmu1_watchdog()    — call every second; resets if PING absent
//
//  If PIN_PMU1_TX or PIN_PMU1_RX is -1 (stubs not filled in),
//  PMU1 is silently skipped — standalone C5 mode.
// ─────────────────────────────────────────────

// PMU1 capabilities bitmask for this device:
//   bit 3 = WIFI, bit 4 = BLE, bit 5 = WIFI_PROMISC (planned),
//   bit 6 = WIFI_SCAN, bit 7 = WIFI_5GHZ (C5 only)
#define C5_PMU1_CAPS  0xD8   // bits 3,4,6,7 = 0b11011000

static HardwareSerial s_pmu1(PMU1_UART_NUM);
static bool           s_pmu1_active = false;
static uint32_t       s_pmu1_last_ping_ms = 0;
static bool           s_pmu1_ping_watchdog = false;

static void pmu1_init() {
    if (PIN_PMU1_TX < 0 || PIN_PMU1_RX < 0) {
        Serial.println("[C5-WD] PMU1: TX/RX pins unset — standalone mode");
        s_pmu1_active = false;
        return;
    }
    s_pmu1.begin(PMU1_BAUD, SERIAL_8N1, PIN_PMU1_RX, PIN_PMU1_TX);
    delay(50);
    // HELLO with C5 capability flags
    s_pmu1.printf("PMU1 HELLO caps=0x%02X\r\n", (unsigned)C5_PMU1_CAPS);
    s_pmu1_active = true;
    s_pmu1_last_ping_ms = millis();
    s_pmu1_ping_watchdog = false;
    Serial.printf("[C5-WD] PMU1: HELLO sent on UART%d @ %d baud "
                  "(TX=%d RX=%d)\n",
                  PMU1_UART_NUM, PMU1_BAUD, PIN_PMU1_TX, PIN_PMU1_RX);
}

// Emit one WiFi observation as a PMU1 WF line.
// band_ghz: 2 or 5, recorded so the P4 can filter by band.
static void pmu1_emit_wifi(const uint8_t bssid[6], const char* ssid,
                            int rssi, int ch, int enc, int band_ghz) {
    if (!s_pmu1_active) return;
    // Build a compact hex payload: "ssid_ascii" truncated at 32 chars.
    // Embedded spaces/commas replaced with '_' for safe tokenising.
    char safe_ssid[36];
    int j = 0;
    for (int i = 0; ssid[i] && i < 32; i++) {
        char c = ssid[i];
        safe_ssid[j++] = (c == ' ' || c == ',') ? '_' : c;
    }
    safe_ssid[j] = 0;
    s_pmu1.printf("PMU1 WF type=%d ch=%d rssi=%d "
                  "mac=%02X%02X%02X%02X%02X%02X "
                  "enc=%d band=%dg name=%s\r\n",
                  0,  // type 0 = AP from active scan
                  ch, rssi,
                  bssid[0], bssid[1], bssid[2],
                  bssid[3], bssid[4], bssid[5],
                  enc, band_ghz, safe_ssid);
}

static void pmu1_emit_ble(const char* addr, int rssi, const char* name) {
    if (!s_pmu1_active) return;
    s_pmu1.printf("PMU1 BLE mac=%s rssi=%d type=public name=%s\r\n",
                  addr, rssi, name[0] ? name : "?");
}

// Read and handle P4 → C5 commands. Call in the main scan loop.
// Handles: PING, CMD ble_scan_start, CMD ble_scan_stop,
//          CMD wifi_promisc_start[_5ghz], CMD wifi_set_channel.
// Unknown lines are echoed to serial for debugging.
static void pmu1_poll() {
    if (!s_pmu1_active) return;
    while (s_pmu1.available()) {
        String line = s_pmu1.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        if (line.startsWith("PMU1 PING")) {
            s_pmu1_last_ping_ms = millis();
            s_pmu1_ping_watchdog = true;
        } else if (line.startsWith("PMU1 CMD ble_scan_start")) {
            // BLE scan is always running in this engine — no-op
        } else if (line.startsWith("PMU1 CMD ble_scan_stop")) {
            // Would pause BLE window; stub for now
        } else if (line.startsWith("PMU1 CMD wifi_promisc_start")) {
            // Promiscuous mode planned for v1.3
            Serial.println("[C5-WD] PMU1: wifi_promisc_start requested (v1.3)");
        } else if (line.startsWith("PMU1 CMD wifi_promisc_stop")) {
            Serial.println("[C5-WD] PMU1: wifi_promisc_stop (no-op)");
        } else if (line.startsWith("PMU1 CMD wifi_set_channel")) {
            Serial.printf("[C5-WD] PMU1: channel set requested: %s\n",
                          line.c_str());
        } else {
            Serial.printf("[C5-WD] PMU1 unknown: %s\n", line.c_str());
        }
    }
}

// PING watchdog: if the C5 was receiving PINGs and they stop for
// >5 seconds, reset the UART and re-send HELLO.
static void pmu1_watchdog() {
    if (!s_pmu1_active) return;
    if (!s_pmu1_ping_watchdog) return;   // never received a PING yet
    if (millis() - s_pmu1_last_ping_ms > 5000) {
        Serial.println("[C5-WD] PMU1: PING timeout — resetting UART + HELLO");
        s_pmu1.end();
        delay(50);
        pmu1_init();
    }
}

// ─────────────────────────────────────────────
//  CSV LOG (identical structure to c28p_wardrive_engine.cpp)
// ─────────────────────────────────────────────
static pm_storage::File s_csv_file;
static bool             s_csv_open = false;
static char             s_csv_filename[32] = {0};

static void csv_quote_ssid(const char* in, char* out, size_t outsz) {
    size_t w = 0;
    if (outsz < 4) { if (outsz) out[0] = 0; return; }
    out[w++] = '"';
    for (size_t i = 0; in[i] && i < 32 && w < outsz - 3; i++) {
        char c = in[i];
        if (c == '"' && w < outsz - 4) { out[w++] = '"'; out[w++] = '"'; }
        else if (c < 0x20 || c == ',') out[w++] = ' ';
        else out[w++] = c;
    }
    out[w++] = '"';
    out[w] = 0;
}

static void wd_csv_open() {
    if (s_csv_open) return;
    int slot = 1;
    for (; slot <= 999; slot++) {
        snprintf(s_csv_filename, sizeof(s_csv_filename),
                 "/wardrive_c5_%03d.csv", slot);
        if (!pm_storage::exists(s_csv_filename)) break;
    }
    if (slot > 999) snprintf(s_csv_filename, sizeof(s_csv_filename),
                             "/wardrive_c5_999.csv");
    s_csv_file = pm_storage::open(s_csv_filename, pm_storage::Mode::Write);
    if (!s_csv_file) {
        Serial.printf("[C5-WD] CSV open failed: %s\n", s_csv_filename);
        return;
    }
    // Extra 'band' column vs C28P CSV — records 2 or 5 per observation
    s_csv_file.print("bssid,ssid,rssi,channel,encryption,band_ghz,"
                     "t_ms,observed_uptime_s\n");
    s_csv_file.flush();
    s_csv_open = true;
    Serial.printf("[C5-WD] CSV log: %s\n", s_csv_filename);
}

static void wd_csv_close() {
    if (!s_csv_open) return;
    s_csv_file.close();
    s_csv_open = false;
}

static void wd_csv_write_row(const uint8_t bssid[6], const char* ssid,
                              int rssi, int channel, int enc, int band_ghz) {
    if (!s_csv_open) return;
    char ssid_q[72];
    csv_quote_ssid(ssid ? ssid : "", ssid_q, sizeof(ssid_q));
    s_csv_file.printf("%02X:%02X:%02X:%02X:%02X:%02X,%s,%d,%d,%d,%d,%lu,%lu\n",
                      bssid[0], bssid[1], bssid[2],
                      bssid[3], bssid[4], bssid[5],
                      ssid_q, rssi, channel, enc, band_ghz,
                      (unsigned long)millis(),
                      (unsigned long)(millis() / 1000));
    static int rows_since_flush = 0;
    if (++rows_since_flush >= 32) { s_csv_file.flush(); rows_since_flush = 0; }
}

// ─────────────────────────────────────────────
//  BLE scan helpers
// ─────────────────────────────────────────────
static NimBLEScan* s_ble_scanner    = nullptr;
static bool        s_ble_initialized = false;

static void ensure_ble_init() {
    if (s_ble_initialized) return;
    NimBLEDevice::init("");
    s_ble_scanner = NimBLEDevice::getScan();
    s_ble_scanner->setActiveScan(false);
    s_ble_scanner->setInterval(100);
    s_ble_scanner->setWindow(80);
    s_ble_initialized = true;
}

// ─────────────────────────────────────────────
//  5 GHz band alternation
//
//  The defining capability of the C5 port. On even scan cycles we
//  scan 2.4 GHz only; on odd cycles 5 GHz only. Results are tagged
//  with the band so clients (NoSQL, CSV, PMU1 WF lines) can filter.
//
//  WIFI_BAND_MODE_* is an ESP-IDF 5.5 API.
//  Header: esp_wifi.h
//  Values: WIFI_BAND_MODE_2G_ONLY, WIFI_BAND_MODE_5G_ONLY,
//          WIFI_BAND_MODE_AUTO
//  Verify exact symbol names against ESP-IDF 5.5 release notes —
//  they may be WIFI_BAND_MODE_LR (2.4) and WIFI_BAND_MODE_HT (5)
//  depending on the exact IDF patch level.
// ─────────────────────────────────────────────
static uint32_t s_scan_cycle = 0;

static int active_band_ghz() {
    return (s_scan_cycle % 2 == 0) ? 2 : 5;
}

static void set_scan_band() {
    int ghz = active_band_ghz();
    // esp_wifi_set_band_mode is defined in esp_wifi.h (IDF 5.5+).
    // If build fails here, check the IDF version and symbol name.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    wifi_band_mode_t mode = (ghz == 5) ? WIFI_BAND_MODE_5G_ONLY
                                       : WIFI_BAND_MODE_2G_ONLY;
    esp_err_t err = esp_wifi_set_band_mode(mode);
    if (err != ESP_OK) {
        Serial.printf("[C5-WD] set_band_mode(%dG) err 0x%x\n", ghz, err);
    }
#else
    // Pre-5.5 IDF: 5 GHz not available, stay on 2.4 GHz silently.
    (void)ghz;
    Serial.println("[C5-WD] WARN: IDF < 5.5 — 5 GHz band mode unavailable");
#endif
    Serial.printf("[C5-WD] Scan cycle %lu → %d GHz\n",
                  (unsigned long)s_scan_cycle, ghz);
}

// ─────────────────────────────────────────────
//  WiFi scan iteration (ASYNC — single-core safe)
//
//  KEY DIFFERENCE from c28p_wardrive_engine.cpp:
//    C28P: WiFi.scanNetworks(false, true)  — BLOCKING ~1.5 s
//    C5:   WiFi.scanNetworks(true,  true)  — async, returns instantly
//
//  We then poll WiFi.scanComplete() with 20 ms yields. The UI task
//  gets CPU during every vTaskDelay so the launcher/touch stay live.
//  The maximum wait is 8 seconds (400 polls × 20 ms); a timeout is
//  treated as zero results so the loop continues gracefully.
// ─────────────────────────────────────────────
static int do_wifi_scan() {
    set_scan_band();
    int band_ghz = active_band_ghz();

    wifi_in_use = true;

    // Start async scan — returns immediately
    int result = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
    if (result == WIFI_SCAN_FAILED) {
        wifi_in_use = false;
        Serial.println("[C5-WD] scan start failed");
        return 0;
    }

    // Poll until complete, yielding to the UI between polls.
    // WIFI_SCAN_RUNNING = -1; completion returns count >= 0.
    const int MAX_POLLS = 400;   // 400 × 20 ms = 8 s max wait
    for (int i = 0; i < MAX_POLLS && !s_stop_requested; i++) {
        result = WiFi.scanComplete();
        if (result >= 0) break;
        vTaskDelay(pdMS_TO_TICKS(20));   // yield — single core critical
    }

    int n = (result >= 0) ? result : 0;
    if (n == 0) {
        wifi_in_use = false;
        WiFi.scanDelete();
        return 0;
    }

    static uint8_t scan_bssids[64][6];
    int captured = 0;

    for (int i = 0; i < n; i++) {
        uint8_t* bssid_ptr = WiFi.BSSID(i);
        if (!bssid_ptr) continue;

        int rssi = WiFi.RSSI(i);
        String ssid = WiFi.SSID(i);
        int ch  = WiFi.channel(i);
        int enc = (int)WiFi.encryptionType(i);

        c28p_anomaly_observe(bssid_ptr, rssi, ssid.c_str());

        if (captured < 64) {
            memcpy(scan_bssids[captured], bssid_ptr, 6);
            captured++;
        }

        if (!sd_in_use) {
            // NoSQL entry
            char title[40];
            snprintf(title, sizeof(title), "%lu_%02X%02X%02X%02X%02X%02X",
                     (unsigned long)(millis() / 1000),
                     bssid_ptr[0], bssid_ptr[1], bssid_ptr[2],
                     bssid_ptr[3], bssid_ptr[4], bssid_ptr[5]);

            char content[200];
            snprintf(content, sizeof(content),
                     "{\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                     "\"ssid\":\"%.32s\",\"rssi\":%d,\"ch\":%d,"
                     "\"enc\":%d,\"band\":%d,\"t_ms\":%lu}",
                     bssid_ptr[0], bssid_ptr[1], bssid_ptr[2],
                     bssid_ptr[3], bssid_ptr[4], bssid_ptr[5],
                     ssid.c_str(), rssi, ch, enc, band_ghz,
                     (unsigned long)millis());

            nosql_save_entry("wardrive", title, content);
            wd_csv_write_row(bssid_ptr, ssid.c_str(), rssi, ch, enc, band_ghz);
        }

        // Fan out to P4 over PMU1 UART
        pmu1_emit_wifi(bssid_ptr, ssid.c_str(), rssi, ch, enc, band_ghz);
    }

    networks_found = n;
    networks_total += n;
    last_scan_ms = millis();

    WiFi.scanDelete();
    wifi_in_use = false;

    c28p_anomaly_scan_complete(scan_bssids, captured);
    return n;
}

// ─────────────────────────────────────────────
//  BLE scan window (~1 s passive, same as C28P)
// ─────────────────────────────────────────────
static int do_ble_scan() {
    ensure_ble_init();
    if (!s_ble_scanner) return 0;

    // NimBLE 2.x: the blocking start(seconds, is_continue) form was
    // replaced by getResults(milliseconds, is_continue). Note the unit
    // change — 1000ms here, not 1 second. BLE Classic is absent on C5;
    // only BLE 5.0 LE is supported.
    NimBLEScanResults results = s_ble_scanner->getResults(1000, false);
    int count = results.getCount();
    bt_found  = count;
    ble_total += count;

    for (int i = 0; i < count; i++) {
        // NimBLE 2.x: getDevice returns const NimBLEAdvertisedDevice*
        // instead of a value. Access fields with -> not . .
        const NimBLEAdvertisedDevice* dev = results.getDevice(i);
        if (!dev) continue;
        std::string addr_str = dev->getAddress().toString();
        std::string name     = dev->getName();
        int rssi             = dev->getRSSI();

        char title[40];
        snprintf(title, sizeof(title), "%lu_%s",
                 (unsigned long)(millis() / 1000), addr_str.c_str());

        char content[180];
        snprintf(content, sizeof(content),
                 "{\"addr\":\"%s\",\"name\":\"%.32s\",\"rssi\":%d,\"t_ms\":%lu}",
                 addr_str.c_str(), name.c_str(), rssi,
                 (unsigned long)millis());

        if (!sd_in_use) nosql_save_entry("ble_log", title, content);

        pmu1_emit_ble(addr_str.c_str(), rssi, name.c_str());
    }

    s_ble_scanner->clearResults();
    return count;
}

// ─────────────────────────────────────────────
//  Main scan task
//
//  Pinned to Core 0 (the only core on C5). Priority 1 matches the
//  Arduino loop task so the scheduler time-slices between them.
//  The vTaskDelay calls throughout — scan-wait polling, idle window
//  at the bottom — are the yield points that keep the UI alive.
//
//  Loop:
//    set band → async scan → poll → process results
//    → BLE scan window
//    → PMU1 poll (incoming P4 commands)
//    → PMU1 watchdog
//    → 2 s idle (100 × 20 ms, each yielding)
//    → next cycle (band alternates)
// ─────────────────────────────────────────────
static void c5_wardrive_task(void* /*pv*/) {
    Serial.println("[C5-WD] Scan task starting — single core, async scan");

    nosql_init("wardrive");
    nosql_init("ble_log");
    c28p_anomaly_init();
    wd_csv_open();
    pmu1_init();

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, false);
    vTaskDelay(pdMS_TO_TICKS(100));

    wardrive_active    = true;
    s_stop_requested   = false;

    while (!s_stop_requested) {
        do_wifi_scan();
        s_scan_cycle++;

        if (s_stop_requested) break;

        do_ble_scan();

        pmu1_poll();
        pmu1_watchdog();

        // Idle window — 2 seconds of 20 ms yields.
        // UI gets CPU during every iteration; this is the main
        // "background" time the launcher / touch handler runs in.
        for (int i = 0; i < 100 && !s_stop_requested; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
            // Service PMU1 commands during idle too
            if (i % 10 == 0) { pmu1_poll(); pmu1_watchdog(); }
        }
    }

    wardrive_active = false;
    Serial.println("[C5-WD] Scan task exiting");
    wd_csv_close();

    if (s_stop_done) xSemaphoreGive(s_stop_done);
    s_wd_task = nullptr;
    vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────
//  Public API — satisfies wardrive.h declarations
// ─────────────────────────────────────────────
void init_wardrive_core() {
    if (wardrive_active || s_wd_task) {
        Serial.println("[C5-WD] init_wardrive_core: already running");
        return;
    }
    if (!s_stop_done) s_stop_done = xSemaphoreCreateBinary();

    // Priority 1 = same as Arduino loop — time-slices with UI on single core.
    // Do NOT use priority 2+ here; that would starve the display handler.
    xTaskCreatePinnedToCore(c5_wardrive_task, "C5_WD",
                            6144, nullptr, 1, &s_wd_task, 0);
    Serial.println("[C5-WD] Scan task spawned");
}

bool wardrive_teardown(uint32_t timeout_ms) {
    if (!wardrive_active && !s_wd_task) return true;
    s_stop_requested = true;
    if (s_stop_done &&
        xSemaphoreTake(s_stop_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
        return true;
    Serial.printf("[C5-WD] teardown timeout after %lums\n",
                  (unsigned long)timeout_ms);
    return false;
}

void wardrive_set_mode(wardrive_mode_t mode) { wardrive_mode = mode; }
void wardrive_ble_stop()   { /* stub — no independent BLE pause on C5 */ }
void wardrive_ble_resume() { /* stub */ }

const char* wardrive_get_log_filename() {
    return s_csv_open ? s_csv_filename : "";
}

void wardrive_task(void* /*pv*/) {
    Serial.println("[C5-WD] wardrive_task stub — use init_wardrive_core()");
    vTaskDelete(nullptr);
}

void run_wardrive() {
    // Dispatched by c5_boot.cpp's launcher → c5_run_wardrive()
    extern void c5_run_wardrive();
    c5_run_wardrive();
}

#endif // DEVICE_C5
