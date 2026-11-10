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
 * PISCES MOON OS — WARDRIVE v2.14
 *
 * v2.14 — Cardputer ADV: BLE heap gate, LoRa status row
 *   NimBLE is brought up only when free heap covers its init cost PLUS
 *   a scan window's minimum (CP_BLE_SCAN_MIN_FREE_HEAP), with the init
 *   cost learned from each real init. The old 55 KB gate let init run
 *   with ~36 KB left, so every window was skipped and the 3rd skip
 *   deinit'd NimBLE: an init/deinit churn every few seconds with BLE
 *   stuck at 0. Below the gate a window costs two heap reads, BLE
 *   starts by itself once memory allows, and the screen shows "--"
 *   instead of a misleading 0. All devices: LoRa receiver state and
 *   nodes heard in the last 5 minutes on the live screen.
 *
 * v2.13 — Cardputer ADV: heavy-load BLE hardening
 *   Dense BLE environments can still stress NimBLE's advertise-report
 *   path even with healthy baseline heap. Cardputer now avoids extra
 *   std::string allocations in the advertisement callback, uses a
 *   larger duplicate cache, lowers passive-scan duty cycle slightly,
 *   reuses the WiFi AP result buffer for the task lifetime, and skips
 *   or deinitializes BLE windows if free heap / largest internal block
 *   cross safety floors.
 *
 * v2.12 — Cardputer ADV: Lazy BLE init at first scan window
 *   v2.11 successfully got NimBLE init past the watchdog hang via
 *   minimal-role config + coex-disable in platformio.ini. But eager
 *   BLE init at task startup left only ~3 KB free heap, and the
 *   first BLE advertisement to arrive crashed in operator new()
 *   inside NimBLEScan::handleGapEvent — no room to allocate the
 *   NimBLEAdvertisedDevice wrapper.
 *
 *   v2.12 defers NimBLE::init() from task startup to the first
 *   BLE scan window in the toggle loop. By that time WiFi's first
 *   scan has completed and freed its temporary allocations, leaving
 *   significantly more headroom for NimBLE's ~48 KB init.
 *
 *   Heap safety: BLE init is gated by a free-heap check (>50KB
 *   required). If insufficient, the cycle skips BLE and flips back
 *   to WiFi-only for that iteration; next BLE window retries. If
 *   BLE init succeeds but leaves <10KB headroom, BLE scanning is
 *   parked for the session (continues WiFi-only).
 *
 *   T-Deck Plus and T-LoRa Pager are unaffected — they still init
 *   BLE eagerly at task startup (have PSRAM headroom).
 *
 * v2.11 — Radio toggle 2s (was 4s) for better temporal resolution
 *   WiFi and BLE scans each take ~1-1.5s. At the previous 4-second
 *   toggle, each radio sat idle for ~2.5s every cycle — long enough
 *   to miss networks passed at walking or driving speed. 2-second
 *   toggle puts the radios in near-continuous use with only the
 *   mode-switch settle delay as overhead.
 *
 * v2.10 — Cardputer ADV: WiFi-BLE coex stability
 *   Previous v2.9 ran a "pre-BLE smoke scan" to verify WiFi worked
 *   before BLE attached. The scan itself succeeded (returned 4
 *   networks including 'Becker' at RSSI -45) but it allocated and
 *   freed buffers in a way that corrupted the WiFi driver's
 *   internal state. When NimBLE then tried to attach the
 *   WiFi/BLE coex layer, it called back into WiFi for fresh
 *   allocations, those failed with "Expected to init 4 rx buffer,
 *   actual is 2", and NimBLE::init() hung waiting for a coex
 *   handshake that never completed. Task watchdog killed it at 5s.
 *
 *   v2.10 removes the smoke scan entirely. WiFi.mode(STA) +
 *   WiFi.disconnect + WiFi.setSleep(true) is enough to claim
 *   descriptors. The actual scan happens inside the main loop
 *   AFTER NimBLE has attached cleanly. Also adds a 400ms settle
 *   delay between WiFi init and BLE init (was 200ms), and skips
 *   the per-iteration WiFi.mode(STA) reassertion on Cardputer
 *   (T-Deck and Pager still reassert, which they need for some
 *   power-save edge cases).
 *
 * v2.9 — Cardputer ADV: WiFi-before-BLE init, with diagnostics
 *   On Cardputer (ESP32-S3FN8, no PSRAM, 320 KB internal SRAM) the
 *   WiFi driver requires DMA-capable RX descriptors that MUST live
 *   in internal SRAM. NimBLE's ~30 KB init allocation fragments
 *   internal heap, and if it runs first WiFi can't allocate enough
 *   contiguous descriptor buffers — fails with "Expected to init N
 *   rx buffer, actual is M". Initializing WiFi first lets it claim
 *   its descriptors before NimBLE arrives. Combined with WiFi memory
 *   tuning flags in platformio.ini (CONFIG_ESP_WIFI_*_NUM=4/8/8),
 *   this lets WiFi come up on Cardputer.
 *
 *   T-Deck Plus and T-LoRa Pager retain the original BLE-first order
 *   (they have PSRAM headroom and init order doesn't matter).
 *
 *   Diagnostic prints under #ifdef DEVICE_CARDPUTER_ADV show free
 *   heap before/after each init step and per-scan results, so any
 *   remaining issue can be diagnosed from a single Serial log.
 *
 * v2.8 — Cardputer ADV layout
 *   Dedicated 240x135 layout branch for Cardputer ADV: 12px header,
 *   8px filename ribbon, two-column 3-row value grid (WIFI/SATS top,
 *   BLE/ALT middle, LAT/LNG bottom), and a 16px action bar at the
 *   bottom. All six fields visible without scroll. Header copy is
 *   "WARDRIVE | Q EXIT | M PAUSE" — touch handling is bypassed
 *   entirely on Cardputer.
 *
 * v2.7 — Lazy session-file creation
 *   The session CSV file is no longer created at task startup. On Pager,
 *   the SD card mount is deferred and may not complete for ~10 seconds
 *   after wardrive_task() begins; v2.6 would either create the file with
 *   a timestamp-based filename (no rotation, every boot overwrites the
 *   same low-numbered file) OR fall through to writes against a phantom
 *   filename that O_APPEND can't create. Either way, data loss.
 *
 *   v2.7 leaves _current_log_file empty until the scan loop observes
 *   g_sd_ready == true. The first SD-ready iteration claims the next
 *   /wardrive_NNNN.csv via _find_next_session_number() and writes the
 *   CSV header. All subsequent writes append to that file for the
 *   remainder of the session. T-Deck behavior is unchanged because
 *   g_sd_ready is asserted before wardrive_task() spawns.
 *
 * v2.6 — sd_in_use flag added — wardrive pauses SD writes while file
 *        manager active.
 */

#include "wardrive.h"
#include "wifi_mode.h"
#include <Arduino.h>
#include <FS.h>
#include <string>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include "SdFat.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "pm_input.h"
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
// LoRa integration — third radio observed by the wardrive task.
// Headers brought in only on devices that physically have an SX1262.
#include <SPI.h>
#include <RadioLib.h>
#include "pm_lora_pins.h"
#include "pm_lora_locator.h"
#include "pm_lora_lease.h"
#endif
#ifdef DEVICE_CARDPUTER_ADV
#include <esp_err.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <esp_system.h>   // esp_reset_reason() — diag breadcrumb
#endif

extern TinyGPSPlus gps;
extern SdFat sd;
extern volatile bool g_sd_ready;    // Set true once SD mount succeeds
#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif
extern volatile bool wifi_in_use;
extern volatile bool sd_in_use;     // Set by wifi_filemgr while serving
extern SemaphoreHandle_t spi_mutex;

int  networks_found  = 0;
volatile int  bt_found        = 0;
// Devices kept in the most recent COMPLETED BLE window. Written by the
// scan task only after a window finishes, so the UI never reads the
// mid-window reset of bt_found. A genuine 0 still shows as 0.
static volatile int bt_last_window = 0;
// True while BLE windows are actually running. False while BLE is held
// off for memory (Cardputer) or before the first window; the UI then
// shows "--" rather than a 0 that looks like "nothing nearby".
static volatile bool bt_ble_live = false;
int  esp_found       = 0;
int      networks_total = 0;
int      ble_total = 0;
bool wardrive_active = false;
wardrive_mode_t wardrive_mode = WARDRIVE_MODE_SCAN;
volatile bool wardrive_promiscuous_active = false;
volatile bool wardrive_bridge_streaming = false;
volatile uint32_t pm_frames_per_sec = 0;
volatile uint32_t pm_beacon_count = 0;
volatile uint32_t pm_probe_req_count = 0;
volatile uint32_t pm_deauth_count = 0;
volatile uint32_t pm_other_count = 0;
HardwareSerial SerialGPS(1);

// Who reads the GPS UART. On the Cardputer the P4 tether task
// (cardputer_i2c_module.cpp) reads the Cap GPS from boot, so the Map and
// Mesh Map apps have a fix without Wardrive running. The wardrive task
// must not open, read, re-baud or close that UART as well: two readers
// split the NMEA stream between them and fight over auto-baud, and the
// SerialGPS.end() at Wardrive teardown killed GPS for every app until
// reboot. Elsewhere the wardrive task is the only GPS reader.
#ifdef DEVICE_CARDPUTER_ADV
  #define WD_OWNS_GPS_UART 0
#else
  #define WD_OWNS_GPS_UART 1
#endif

static char _current_log_file[32] = "";

// ─────────────────────────────────────────────────────────────────────
//  Task lifecycle state — referenced from wardrive_task() (line ~465)
//  and from init_wardrive_core() / wardrive_teardown() (line ~730).
//  Declared here at file scope so the task loop can see them; defined
//  with their bodies in the lifecycle section below.
// ─────────────────────────────────────────────────────────────────────
static TaskHandle_t  s_wardrive_task   = NULL;
static volatile bool s_wardrive_teardown_requested = false;
static volatile bool s_wardrive_torn_down = false;

const char* wardrive_get_log_filename() {
    return _current_log_file;
}

void wardrive_set_mode(wardrive_mode_t mode) {
    if (wardrive_mode == mode) return;
    wardrive_mode = mode;
    wardrive_promiscuous_active = (mode == WARDRIVE_MODE_PROMISCUOUS);
    if (mode == WARDRIVE_MODE_SCAN) {
        wardrive_bridge_streaming = false;
        pm_frames_per_sec = 0;
        pm_beacon_count = 0;
        pm_probe_req_count = 0;
        pm_deauth_count = 0;
        pm_other_count = 0;
#ifndef DEVICE_CARDPUTER_ADV
        WiFi.mode(WIFI_STA);
#endif
    }
}

static int _find_next_session_number() {
    for (int n = 1; n <= 9999; n++) {
        char path[32];
        snprintf(path, sizeof(path), "/wardrive_%04d.csv", n);
        if (!sd.exists(path)) return n;
    }
    return 1;
}

static String gps_timestamp() {
    if (!gps.date.isValid() || !gps.time.isValid())
        return "1970-01-01 00:00:00";
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buf);
}

static bool isEspressifMAC(const char* mac) {
    static const char* espOUIs[] = {
        "24:0A:C4","24:6F:28","30:AE:A4","3C:71:BF","3C:61:05","48:3F:DA",
        "4C:11:AE","54:43:B2","58:BF:25","60:01:94","68:C6:3A","70:03:9F",
        "7C:9E:BD","84:0D:8E","84:CC:A8","8C:AA:B5","90:97:D5","94:B5:55",
        "98:CD:AC","A0:20:A6","A4:CF:12","A4:E5:7C","AC:0B:FB","B4:E6:2D",
        "BC:DD:C2","C4:4F:33","C8:2B:96","CC:50:E3","D8:BC:38","DC:4F:22",
        "E0:98:06","E8:68:E7","EC:94:CB","F0:08:D1","FC:F5:C4",nullptr
    };
    for (int i = 0; espOUIs[i]; i++)
        if (strncasecmp(mac, espOUIs[i], 8) == 0) return true;
    return false;
}

#ifdef DEVICE_CARDPUTER_ADV
// The ring buffer holds BLE_QUEUE_SIZE - 1 entries. 65 -> 64, matching
// CP_BLE_WINDOW_EVENT_BUDGET, so one full BLE window always fits and
// nothing is dropped for lack of queue space. (Was 32 -> 31 slots while
// windows delivered up to 64 events and the flush drained only 8 rows
// every 5 s, so the queue sat full and most BLE devices were discarded:
// field log showed windows alternating kept=8 / kept=0 drops=40.)
// Cost: ~3.6 KB internal RAM total, +1.8 KB vs before.
#define BLE_QUEUE_SIZE 65
// Rows written per SD open. The original TASK_WDT wedge came from one
// open/close per BLE hit; the WiFi path already writes up to 24 rows
// per open without trouble, so 32 per open keeps opens rare.
static constexpr int BLE_FLUSH_MAX_ROWS = 32;
// Max open/write/close batches per post-window drain (3 x 32 > 64).
static constexpr int BLE_DRAIN_MAX_BATCHES = 3;
#else
#define BLE_QUEUE_SIZE 32
static constexpr int BLE_FLUSH_MAX_ROWS = BLE_QUEUE_SIZE;
#endif
struct BLEResult { char mac[18]; char name[32]; int rssi; };
static BLEResult    bleQueue[BLE_QUEUE_SIZE];
static volatile int bleHead = 0;
static volatile int bleTail = 0;
static portMUX_TYPE bleMux  = portMUX_INITIALIZER_UNLOCKED;

static bool enqueueBLE(const char* mac, const char* name, int rssi) {
    bool queued = false;
    portENTER_CRITICAL(&bleMux);
    int next = (bleHead + 1) % BLE_QUEUE_SIZE;
    if (next != bleTail) {
        strncpy(bleQueue[bleHead].mac,  mac,  17); bleQueue[bleHead].mac[17]  = 0;
        strncpy(bleQueue[bleHead].name, name, 31); bleQueue[bleHead].name[31] = 0;
        bleQueue[bleHead].rssi = rssi;
        bleHead = next;
        queued = true;
    }
    portEXIT_CRITICAL(&bleMux);
    return queued;
}

#ifdef DEVICE_CARDPUTER_ADV
static volatile uint16_t s_ble_window_events = 0;
static volatile uint16_t s_ble_window_drops = 0;

static uint32_t cp_largest_internal_block() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void cp_format_ble_mac(const NimBLEAddress& addr, char out[18]) {
    const uint8_t* native = addr.getNative();
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             native[5], native[4], native[3], native[2], native[1], native[0]);
}

static void cp_extract_ble_name(NimBLEAdvertisedDevice* dev, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    strncpy(out, "Unknown", out_len - 1);
    out[out_len - 1] = '\0';
    if (!dev) return;

    const uint8_t* payload = dev->getPayload();
    const size_t payload_len = dev->getPayloadLength();
    size_t pos = 0;
    while (payload && pos + 1 < payload_len) {
        uint8_t field_len = payload[pos];
        if (field_len == 0) break;
        if (pos + field_len >= payload_len) break;

        uint8_t type = payload[pos + 1];
        if (type == 0x08 || type == 0x09) {  // shortened or complete local name
            size_t name_len = field_len - 1;
            if (name_len >= out_len) name_len = out_len - 1;
            memcpy(out, payload + pos + 2, name_len);
            out[name_len] = '\0';
            for (size_t i = 0; out[i]; ++i) {
                if (out[i] == ',' || out[i] == '\r' || out[i] == '\n') out[i] = ' ';
            }
            return;
        }
        pos += field_len + 1;
    }
}
#endif

class WardriveCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
#ifdef DEVICE_CARDPUTER_ADV
        constexpr uint16_t CP_BLE_WINDOW_EVENT_BUDGET = 64;
        if (s_ble_window_events >= CP_BLE_WINDOW_EVENT_BUDGET) {
            s_ble_window_drops++;
            return;
        }

        char mac[18];
        char name[32];
        cp_format_ble_mac(dev->getAddress(), mac);
        cp_extract_ble_name(dev, name, sizeof(name));
        s_ble_window_events++;
        if (enqueueBLE(mac, name, dev->getRSSI())) {
            bt_found++;
        } else {
            s_ble_window_drops++;
        }
#else
        std::string macStr = dev->getAddress().toString();
        std::string nameStr = dev->haveName() ? dev->getName() : std::string("Unknown");
        if (enqueueBLE(macStr.c_str(), nameStr.c_str(), dev->getRSSI())) {
            bt_found++;
        }
#endif
    }
};

static NimBLEScan*        wdScan      = nullptr;
static WardriveCallbacks* wdCallbacks = nullptr;
static bool               nimbleInit  = false;

#ifdef DEVICE_CARDPUTER_ADV
static esp_netif_t* cpWifiNetif = nullptr;
static bool         cpWifiInit  = false;
static bool         cpWifiUp    = false;
static constexpr uint16_t CP_SCAN_MAX_APS = 24;
static constexpr uint32_t CP_BLE_SCAN_MIN_FREE_HEAP = 70000;
static constexpr uint32_t CP_BLE_SCAN_MIN_LARGEST_BLOCK = 20000;
static constexpr uint32_t CP_BLE_RECOVER_FREE_HEAP = 52000;
static constexpr uint32_t CP_BLE_RECOVER_LARGEST_BLOCK = 14000;
static constexpr uint32_t CP_BLE_SCAN_SECONDS = 1;
// NimBLE init cost estimate (host task, controller, pools). Replaced by
// the measured cost after each real init, never below this floor.
static constexpr uint32_t CP_BLE_INIT_COST_FLOOR = 48000;
static constexpr uint32_t CP_BLE_INIT_COST_CEIL  = 90000;
// Init needs a contiguous block for the host task stack + pools.
static constexpr uint32_t CP_BLE_INIT_MIN_LARGEST_BLOCK = 24000;
static uint32_t s_ble_init_cost = CP_BLE_INIT_COST_FLOOR;

// ─────────────────────────────────────────────────────────────────
//  DIAGNOSTIC LOG (Cardputer ADV) — untethered RF/heap telemetry
//
//  Mirrors the [WARDRIVE/CP] serial diagnostics to /diag_NNNN.txt on
//  SD so a full Uber shift can be captured with no laptop tethered.
//  Read the card at the desk afterward.
//
//  Crash-survival design: each line is flushed and the file CLOSED
//  immediately, so a hard crash loses at most the in-flight line. The
//  first line of every session is the reset reason (esp_reset_reason)
//  decoded — so after a crash-and-reboot the NEXT session's header
//  tells you what class of failure happened (PANIC / TASK_WDT /
//  BROWNOUT / etc.), correlated with the last heap line before the gap.
//
//  Uses the shared spi_mutex so it never collides with the wardrive
//  CSV writer or the file manager on the SPI bus.
// ─────────────────────────────────────────────────────────────────
static char _diag_log_file[32] = "";

static const char* cp_reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_RESET";
        case ESP_RST_SW:        return "SW_RESET";
        case ESP_RST_PANIC:     return "PANIC (crash/exception)";
        case ESP_RST_INT_WDT:   return "INT_WDT (interrupt watchdog)";
        case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog)";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT (power sag)";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Create /diag_NNNN.txt (matching the wardrive CSV session number when
// possible) and write the reset-reason header. Best-effort; safe to call
// repeatedly (no-op once created).
static void cp_diag_open() {
    if (_diag_log_file[0] != '\0') return;
    if (!g_sd_ready || sd_in_use) return;
    if (!spi_mutex) return;
    if (xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    int n = 1;
    for (; n <= 9999; n++) {
        char path[32];
        snprintf(path, sizeof(path), "/diag_%04d.txt", n);
        if (!sd.exists(path)) break;
    }
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "/diag_%04d.txt", n);

    FsFile f = sd.open(tmp, O_WRITE | O_CREAT | O_TRUNC);
    if (f) {
        f.printf("=== Pisces Moon diag session %04d ===\n", n);
        f.printf("RESET REASON: %s\n", cp_reset_reason_str());
        f.printf("boot free heap: %u  largest block: %u\n",
                 (unsigned)ESP.getFreeHeap(),
                 (unsigned)cp_largest_internal_block());
        f.println("fields: uptime_ms,event,wifi_count,free_heap,largest_block,lat,lng");
        f.flush();
        f.close();
        strncpy(_diag_log_file, tmp, sizeof(_diag_log_file) - 1);
        _diag_log_file[sizeof(_diag_log_file) - 1] = '\0';
        Serial.printf("[WARDRIVE/CP] Diag log: %s (reset: %s)\n",
                      _diag_log_file, cp_reset_reason_str());
    }
    xSemaphoreGiveRecursive(spi_mutex);
}

// Append one GPS-stamped diagnostic line. event is a short tag like
// "scan", "ble_skip", "ble_deinit". Flushes + closes immediately so a
// crash cannot lose more than this one line.
static void cp_diag_log(const char* event, int wifi_count,
                        uint32_t free_heap, uint32_t largest_block) {
    if (_diag_log_file[0] == '\0') return;
    if (sd_in_use) return;
    if (!spi_mutex) return;
    if (xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    FsFile f = sd.open(_diag_log_file, O_WRITE | O_APPEND);
    if (f) {
        double lat = gps.location.isValid() ? gps.location.lat() : 0.0;
        double lng = gps.location.isValid() ? gps.location.lng() : 0.0;
        f.printf("%lu,%s,%d,%u,%u,%.6f,%.6f\n",
                 (unsigned long)millis(), event, wifi_count,
                 (unsigned)free_heap, (unsigned)largest_block, lat, lng);
        f.flush();
        f.close();
    }
    xSemaphoreGiveRecursive(spi_mutex);
}

static void cp_sd_diag_throttled(const char* event) {
    static uint32_t last_log_ms = 0;
    uint32_t now = millis();
    if (now - last_log_ms < 3000) return;
    last_log_ms = now;
    Serial.printf("[WARDRIVE/CP] SD %s ready=%d busy=%d file=%s heap=%u largest=%u\n",
                  event,
                  g_sd_ready ? 1 : 0,
                  sd_in_use ? 1 : 0,
                  _current_log_file[0] ? _current_log_file : "(none)",
                  (unsigned)ESP.getFreeHeap(),
                  (unsigned)cp_largest_internal_block());
}

static void cp_format_mac(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char* cp_auth_name(wifi_auth_mode_t auth) {
    return (auth == WIFI_AUTH_OPEN) ? "OPEN" : "WPA";
}

static void cp_sanitize_ssid(const uint8_t *raw, char out[33]) {
    memcpy(out, raw, 32);
    out[32] = '\0';
    for (int i = 0; out[i]; i++) {
        if (out[i] == ',' || out[i] == '\r' || out[i] == '\n') out[i] = ' ';
    }
}

static bool cp_init_scan_wifi() {
    if (cpWifiInit && cpWifiUp) return true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[WARDRIVE/CP] esp_netif_init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[WARDRIVE/CP] esp_event_loop_create_default failed: %s\n", esp_err_to_name(err));
        return false;
    }

    if (!cpWifiNetif) {
        cpWifiNetif = esp_netif_create_default_wifi_sta();
        if (!cpWifiNetif) {
            Serial.println("[WARDRIVE/CP] esp_netif_create_default_wifi_sta failed");
            return false;
        }
    }

    if (!cpWifiInit) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.static_rx_buf_num  = 4;
        cfg.dynamic_rx_buf_num = 8;
        cfg.static_tx_buf_num  = 0;
        cfg.dynamic_tx_buf_num = 8;
        cfg.tx_buf_type        = 1;  // dynamic TX buffers
        cfg.cache_tx_buf_num   = 4;  // Arduino core notes this must stay non-zero
        cfg.ampdu_rx_enable    = 0;
        cfg.ampdu_tx_enable    = 0;
        cfg.amsdu_tx_enable    = 0;
        cfg.nvs_enable         = 0;
        cfg.rx_ba_win          = 0;
        cfg.mgmt_sbuf_num      = 6;
        cfg.feature_caps      &= ~(CONFIG_FEATURE_WPA3_SAE_BIT |
                                   CONFIG_FEATURE_FTM_INITIATOR_BIT |
                                   CONFIG_FEATURE_FTM_RESPONDER_BIT);
        cfg.espnow_max_encrypt_num = 0;

        err = esp_wifi_init(&cfg);
        if (err == ESP_OK || err == ESP_ERR_WIFI_INIT_STATE) {
            cpWifiInit = true;
        } else {
            Serial.printf("[WARDRIVE/CP] esp_wifi_init scan cfg failed: %s\n", esp_err_to_name(err));
            return false;
        }
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        Serial.printf("[WARDRIVE/CP] esp_wifi_set_mode STA failed: %s\n", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_start();
    if (err == ESP_OK || err == ESP_ERR_WIFI_INIT_STATE) {
        cpWifiUp = true;
    } else {
        Serial.printf("[WARDRIVE/CP] esp_wifi_start failed: %s\n", esp_err_to_name(err));
        return false;
    }

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return true;
}

static void cp_deinit_scan_wifi() {
    if (cpWifiUp) {
        esp_wifi_scan_stop();
        esp_wifi_stop();
        cpWifiUp = false;
    }
    if (cpWifiInit) {
        esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            Serial.printf("[WARDRIVE/CP] esp_wifi_deinit warning: %s\n", esp_err_to_name(err));
        }
        cpWifiInit = false;
    }
    if (cpWifiNetif) {
        esp_wifi_clear_default_wifi_driver_and_handlers(cpWifiNetif);
        esp_netif_destroy(cpWifiNetif);
        cpWifiNetif = nullptr;
    }
}

static int cp_scan_wifi_records(wifi_ap_record_t *records, uint16_t max_records,
                                uint16_t *total_seen) {
    if (total_seen) *total_seen = 0;
    if (!records || max_records == 0) return -1;
    if (!cp_init_scan_wifi()) return -1;

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = true;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        Serial.printf("[WARDRIVE/CP] esp_wifi_scan_start failed: %s\n", esp_err_to_name(err));
        return -1;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        Serial.printf("[WARDRIVE/CP] esp_wifi_scan_get_ap_num failed: %s\n", esp_err_to_name(err));
        return -1;
    }

    uint16_t n = min(ap_count, max_records);
    err = esp_wifi_scan_get_ap_records(&n, records);
    if (err != ESP_OK) {
        Serial.printf("[WARDRIVE/CP] esp_wifi_scan_get_ap_records failed: %s\n", esp_err_to_name(err));
        return -1;
    }
    if (total_seen) *total_seen = ap_count;
    return (int)n;
}
#endif

static void initWardriveBLE() {
    if (nimbleInit) return;
#ifdef DEVICE_CARDPUTER_ADV
    // Keep enough duplicate-cache slots for dense environments. A tiny
    // cache re-reports the same advertisers and hammers the host task
    // with allocate/callback/free churn.
    NimBLEDevice::setScanDuplicateCacheSize(64);
#endif
    NimBLEDevice::init("");
    wdScan      = NimBLEDevice::getScan();
    if (!wdCallbacks) wdCallbacks = new WardriveCallbacks();
    wdScan->setAdvertisedDeviceCallbacks(wdCallbacks, false);
#ifdef DEVICE_CARDPUTER_ADV
    wdScan->setActiveScan(false);
    wdScan->setMaxResults(0);
    wdScan->setInterval(240);
    wdScan->setWindow(60);
#else
    wdScan->setActiveScan(true);
    wdScan->setInterval(160);
    wdScan->setWindow(80);
#endif
    nimbleInit = true;
}

// ─────────────────────────────────────────────────────────────
// Lazy session-file creation.
//
// Called from inside the scan loop on every iteration. Returns true if
// _current_log_file is valid (file exists or was just created). Returns
// false if SD isn't ready yet — caller must skip writes.
//
// Performs at most ONE successful creation per task lifetime: once
// _current_log_file is non-empty, this is a fast-path no-op.
// ─────────────────────────────────────────────────────────────
static bool ensure_session_file() {
    if (_current_log_file[0] != '\0') return true;     // already created
    if (!g_sd_ready) {
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("session_wait_not_ready");
#endif
        return false;                                  // SD not ready yet
    }
    if (sd_in_use) {
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("session_wait_busy");
#endif
        return false;                                  // file mgr has the card
    }

    if (!spi_mutex) {
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("session_no_mutex");
#endif
        return false;
    }
    if (xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("session_mutex_timeout");
#endif
        return false;
    }

    int session_num = _find_next_session_number();
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "/wardrive_%04d.csv", session_num);

    FsFile file = sd.open(tmp, O_WRITE | O_CREAT | O_TRUNC);
    bool ok = false;
    if (file) {
        file.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,"
                     "CurrentLatitude,CurrentLongitude,"
                     "AltitudeMeters,AccuracyMeters,Type");
        file.close();
        // Only adopt the filename after we successfully wrote the header.
        strncpy(_current_log_file, tmp, sizeof(_current_log_file) - 1);
        _current_log_file[sizeof(_current_log_file) - 1] = '\0';
        ok = true;
    }
    xSemaphoreGiveRecursive(spi_mutex);

    if (ok) {
        Serial.printf("[WARDRIVE] Session file created: %s\n", _current_log_file);
    } else {
        Serial.printf("[WARDRIVE] Failed to create session file %s — will retry\n", tmp);
    }
    return ok;
}

// Returns the number of rows written (0 if SD unavailable, no GPS,
// mutex timeout, or queue empty).
static int flushBLEQueue(const char* log_file) {
    // Skip SD writes while file manager has the card
    if (sd_in_use) return 0;
    // Skip if no session file yet
    if (!log_file || log_file[0] == '\0') return 0;

    if (!gps.location.isValid()) {
        portENTER_CRITICAL(&bleMux);
        bleTail = bleHead;
        portEXIT_CRITICAL(&bleMux);
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("ble_flush_no_gps");
#endif
        return 0;
    }

    // Nothing queued: skip the mutex + SD open entirely.
    bool empty;
    portENTER_CRITICAL(&bleMux);
    empty = (bleTail == bleHead);
    portEXIT_CRITICAL(&bleMux);
    if (empty) return 0;

    if (!spi_mutex) {
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("ble_flush_no_mutex");
#endif
        return 0;
    }

    if (xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("ble_flush_mutex_timeout");
#endif
        return 0;
    }

    FsFile file = sd.open(log_file, O_WRITE | O_APPEND);
    if (!file) {
        xSemaphoreGiveRecursive(spi_mutex);
#ifdef DEVICE_CARDPUTER_ADV
        cp_sd_diag_throttled("ble_flush_open_fail");
#endif
        return 0;
    }

    int flushed = 0;
    while (flushed < BLE_FLUSH_MAX_ROWS) {
        portENTER_CRITICAL(&bleMux);
        if (bleTail == bleHead) {
            portEXIT_CRITICAL(&bleMux);
            break;
        }
        BLEResult r = bleQueue[bleTail];
        bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
        portEXIT_CRITICAL(&bleMux);

        file.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,%s\n",
            r.mac, r.name, "BT-LE", gps_timestamp().c_str(),
            0, r.rssi,
            gps.location.lat(), gps.location.lng(),
            gps.altitude.feet(), 10, "BT-LE");
        flushed++;
    }

    file.close();
    xSemaphoreGiveRecursive(spi_mutex);

#ifdef DEVICE_CARDPUTER_ADV
    if (flushed > 0) {
        bool more_queued = false;
        portENTER_CRITICAL(&bleMux);
        more_queued = (bleTail != bleHead);
        portEXIT_CRITICAL(&bleMux);
        Serial.printf("[WARDRIVE/CP] BLE flush: wrote=%d more=%d heap=%u largest=%u\n",
                      flushed, more_queued ? 1 : 0,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)cp_largest_internal_block());
    }
#endif

    if (flushed > 0) vTaskDelay(pdMS_TO_TICKS(1));
    return flushed;
}

#ifdef DEVICE_CARDPUTER_ADV
// Drain the BLE queue right after a BLE window completes. Each batch
// is its own mutex-take / open / write / close, and we yield between
// batches so the Bus Treaty lets other SPI users (diag log, LoRa, file
// manager) interleave. Bounded to BLE_DRAIN_MAX_BATCHES so a stuck
// card can't hold the task here.
static void cp_drain_ble_queue(const char* log_file) {
    for (int b = 0; b < BLE_DRAIN_MAX_BATCHES; b++) {
        int n = flushBLEQueue(log_file);
        if (n < BLE_FLUSH_MAX_ROWS) break;   // empty, or SD unavailable
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#endif

// ─────────────────────────────────────────────────────────────
//  wardrive_log_observation() — public single-row append for
//  non-WiFi/BLE sources (the RF Sniffer's LoRa/Meshtastic rows).
//
//  Writes into the SAME /wardrive_NNNN.csv session file as the WiFi
//  and BLE writers, in the identical WiGLE-style row format, under the
//  same SPI Bus Treaty mutex. Timestamp, lat/lon and altitude come from
//  the live GPS so the row is consistent with the rest of the session
//  and only lands when a fix is present. Creates/rotates the session
//  file lazily via ensure_session_file() if a session isn't open yet
//  (e.g. the RF Sniffer is the first thing the user opened on Cardputer,
//  where the Ghost Engine task isn't spawned at boot).
// ─────────────────────────────────────────────────────────────
bool wardrive_log_observation(const char* mac, const char* ssid,
                              const char* authMode, int channel,
                              int rssi, const char* type) {
    if (sd_in_use) return false;
    if (!gps.location.isValid()) return false;     // wardrive rows are GPS-tagged
    if (!ensure_session_file()) return false;       // create/rotate if needed
    if (!spi_mutex) return false;
    if (xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    bool ok = false;
    FsFile file = sd.open(_current_log_file, O_WRITE | O_APPEND);
    if (file) {
        file.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,%s\n",
            mac, ssid, authMode, gps_timestamp().c_str(),
            channel, rssi,
            gps.location.lat(), gps.location.lng(),
            gps.altitude.meters(), 10, type);
        file.close();
        ok = true;
    }
    xSemaphoreGiveRecursive(spi_mutex);
    return ok;
}

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
// ─────────────────────────────────────────────────────────────
//  LoRa observation — third radio in the wardrive task
//
//  Passive Meshtastic LongFast receiver. The SX1262 is initialized once
//  when the task is first armed (lazy, so a wardrive-deferred Cardputer
//  boot doesn't pay for it until the user enters WarDrive), runs in
//  promiscuous receive (no dest filter, no dedup), and emits one CSV
//  row per heard packet via wardrive_log_observation() as Type=LORA.
//
//  This RX path is INDEPENDENT of the WiFi/BLE toggle. The SX1262 sits
//  on its own SPI bus (or shares with SD via the Treaty mutex on
//  Cardputer); it never competes with the WiFi scanner or NimBLE for
//  airtime. The task drains the DIO1-flagged interrupt each loop pass
//  and continues whatever WiFi/BLE work it was doing.
//
//  Meshtastic header (16 bytes little-endian) is the same struct used
//  by mesh_messenger.cpp — we only read the cleartext header (origin
//  NodeID, hop_limit/hop_start, channel hash). No payload decryption.
//
//  Per-node throttle: in a dense mesh a single node can emit several
//  packets per second. The CSV writer is throttled per-node by
//  WARDRIVE_LORA_PER_NODE_MS to keep the session log readable.
// ─────────────────────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
extern SPIClass cardputerSdSPI;   // HSPI bus shared with SD on Cardputer
#endif

// Region LongFast default frequencies. US default; expand via build
// flag later if needed. Same modem params as mesh_messenger.cpp.
#define WARDRIVE_LORA_FREQ_MHZ   906.875f
#define WARDRIVE_LORA_BW_KHZ     250.0f
#define WARDRIVE_LORA_SF         11
#define WARDRIVE_LORA_CR         5         // 4/5
#define WARDRIVE_LORA_SYNC       0x2B      // Meshtastic
#define WARDRIVE_LORA_PREAMBLE   16
#define WARDRIVE_LORA_TX_UNUSED  2         // never keyed; begin() needs a value

// Per-node throttle and table size. Sized down on no-PSRAM Cardputer.
#ifdef DEVICE_NO_PSRAM
  #define WARDRIVE_LORA_MAX_NODES 24
#else
  #define WARDRIVE_LORA_MAX_NODES 96
#endif
#define WARDRIVE_LORA_PER_NODE_MS 5000UL   // re-log a given node every 5s

// Meshtastic packet header (cleartext, 16 bytes little-endian).
struct __attribute__((packed)) WdMeshHeader {
    uint32_t dest;
    uint32_t from;
    uint32_t id;
    uint8_t  flags;       // [2:0]=hop_limit, [3]=want_ack, [4]=via_mqtt, [7:5]=hop_start
    uint8_t  channel;
    uint8_t  nextHop;
    uint8_t  relayNode;
};

static SPISettings   lora_spi_settings(2000000, MSBFIRST, SPI_MODE0);
static Module*       lora_module       = nullptr;
static SX1262*       lora_radio        = nullptr;
static volatile bool lora_rx_flag      = false;
static volatile bool lora_ready        = false;
static bool          lora_init_tried   = false;
static uint32_t      lora_my_node_id   = 0;
static volatile uint32_t lora_pkt_total = 0;

struct WdLoraThrottle {
    uint32_t nodeId;
    uint32_t lastLogMs;
};
static WdLoraThrottle lora_throttle[WARDRIVE_LORA_MAX_NODES];
static int            lora_throttle_count = 0;

static void IRAM_ATTR wdLoraRxISR() { lora_rx_flag = true; }

static uint32_t wdLoraSelfNodeId() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];
}

// Returns true if this node should be logged now (and updates its
// throttle slot). False if it was logged too recently or the table is
// full. The table is small enough that a linear scan is cheap.
static bool wdLoraShouldLog(uint32_t nodeId) {
    uint32_t now = millis();
    for (int i = 0; i < lora_throttle_count; i++) {
        if (lora_throttle[i].nodeId == nodeId) {
            if (now - lora_throttle[i].lastLogMs < WARDRIVE_LORA_PER_NODE_MS) {
                return false;
            }
            lora_throttle[i].lastLogMs = now;
            return true;
        }
    }
    if (lora_throttle_count < WARDRIVE_LORA_MAX_NODES) {
        lora_throttle[lora_throttle_count].nodeId = nodeId;
        lora_throttle[lora_throttle_count].lastLogMs = now;
        lora_throttle_count++;
        return true;
    }
    // Table full — reuse the oldest slot.
    int oldest = 0;
    for (int i = 1; i < lora_throttle_count; i++) {
        if ((int32_t)(lora_throttle[i].lastLogMs - lora_throttle[oldest].lastLogMs) < 0)
            oldest = i;
    }
    lora_throttle[oldest].nodeId = nodeId;
    lora_throttle[oldest].lastLogMs = now;
    return true;
}

// Lazy radio bring-up. Returns true if the SX1262 is alive and armed
// for receive. Idempotent: subsequent calls after a successful init
// are no-ops. A single failed attempt is remembered so we don't pound
// the bus retrying every loop pass; if init fails (most common cause:
// no LoRa module is wired on this particular T-Deck) the wardrive task
// continues without LoRa and the CSV simply contains no LORA rows.
static bool wdLoraInit() {
    if (lora_ready) return true;
    if (lora_init_tried) return false;
    // Radio lease: if Mesh Messenger (or, on Cardputer, the P4 bridge)
    // has the chip, don't touch it. Not counted as a try, so the loop
    // asks again and takes the radio once it is free.
    if (!pm_lora_lease_request(PM_LORA_WARDRIVE)) return false;
    lora_init_tried = true;

#ifdef DEVICE_CARDPUTER_ADV
    lora_module = new Module(PM_LORA_CS, PM_LORA_IRQ, PM_LORA_RST, PM_LORA_BUSY,
                             cardputerSdSPI, lora_spi_settings);
#else
    lora_module = new Module(PM_LORA_CS, PM_LORA_IRQ, PM_LORA_RST, PM_LORA_BUSY,
                             SPI, lora_spi_settings);
#endif
    lora_radio = new SX1262(lora_module);

    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("[WARDRIVE/LORA] Radio init: SPI mutex timeout");
        delete lora_radio;  lora_radio  = nullptr;
        delete lora_module; lora_module = nullptr;
        pm_lora_lease_release(PM_LORA_WARDRIVE);
        return false;
    }
    int st = lora_radio->begin(WARDRIVE_LORA_FREQ_MHZ, WARDRIVE_LORA_BW_KHZ,
                               WARDRIVE_LORA_SF, WARDRIVE_LORA_CR,
                               WARDRIVE_LORA_SYNC, WARDRIVE_LORA_TX_UNUSED,
                               WARDRIVE_LORA_PREAMBLE,
                               PM_LORA_TCXO_V, PM_LORA_USE_LDO);
    if (st != RADIOLIB_ERR_NONE) {
        xSemaphoreGiveRecursive(spi_mutex);
        Serial.printf("[WARDRIVE/LORA] Radio init failed: %d (no LoRa hardware?)\n", st);
        delete lora_radio;  lora_radio  = nullptr;
        delete lora_module; lora_module = nullptr;
        pm_lora_lease_release(PM_LORA_WARDRIVE);
        return false;
    }
    lora_radio->setDio2AsRfSwitch(true);
    lora_radio->setCurrentLimit(140.0f);
    lora_radio->setRxBoostedGainMode(true);
    lora_radio->setPacketReceivedAction(wdLoraRxISR);
    lora_rx_flag = false;
    int rxs = lora_radio->startReceive();
    if (rxs != RADIOLIB_ERR_NONE) {
        lora_radio->clearPacketReceivedAction();
        lora_radio->standby();
        xSemaphoreGiveRecursive(spi_mutex);
        Serial.printf("[WARDRIVE/LORA] startReceive failed: %d\n", rxs);
        delete lora_radio;  lora_radio  = nullptr;
        delete lora_module; lora_module = nullptr;
        pm_lora_lease_release(PM_LORA_WARDRIVE);
        return false;
    }
    xSemaphoreGiveRecursive(spi_mutex);
    lora_my_node_id = wdLoraSelfNodeId();
    pm_lora_locator_reset();
    lora_ready = true;
    Serial.printf("[WARDRIVE/LORA] Radio ready. %.3f MHz SF%d BW%.0fk — self !%08x\n",
                  WARDRIVE_LORA_FREQ_MHZ, WARDRIVE_LORA_SF,
                  WARDRIVE_LORA_BW_KHZ, (unsigned)lora_my_node_id);
    return true;
}

static void wdLoraDeinit() {
    if (lora_radio) {
        lora_radio->clearPacketReceivedAction();
        lora_rx_flag = false;
        if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            lora_radio->standby();
            xSemaphoreGiveRecursive(spi_mutex);
        }
        delete lora_radio; lora_radio = nullptr;
    }
    if (lora_module) { delete lora_module; lora_module = nullptr; }
    lora_ready = false;
    lora_init_tried = false;
    lora_throttle_count = 0;
    lora_pkt_total = 0;
}

// Drain whatever the SX1262 has buffered. Bounded by RFS_DRAIN_PER_FRAME
// to keep one busy mesh from monopolizing the loop. Each accepted packet
// becomes a wardrive_log_observation() call (which itself is gated on
// GPS fix, SD readiness, and the per-node throttle).
static void wdLoraDrain() {
    if (!lora_ready || !lora_radio) return;
#ifdef DEVICE_NO_PSRAM
    const int max_per_call = 4;
#else
    const int max_per_call = 10;
#endif
    for (int pass = 0; pass < max_per_call; pass++) {
        if (!lora_rx_flag) break;
        lora_rx_flag = false;

        if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
            lora_rx_flag = true;     // bus busy — try again next loop
            break;
        }
        uint8_t buf[256];
        size_t rxLen = lora_radio->getPacketLength();
        size_t readLen = (rxLen > 0) ? min(rxLen, sizeof(buf)) : sizeof(buf);
        int st = lora_radio->readData(buf, readLen);
        int   rssi = 0;
        float snr  = 0.0f;
        if (st == RADIOLIB_ERR_NONE || st == RADIOLIB_ERR_CRC_MISMATCH) {
            rssi = (int)lora_radio->getRSSI();
            snr  = lora_radio->getSNR();
        }
        lora_radio->startReceive();   // re-arm immediately
        xSemaphoreGiveRecursive(spi_mutex);

        if (st != RADIOLIB_ERR_NONE || rxLen <= sizeof(WdMeshHeader) || rxLen > sizeof(buf))
            continue;

        const WdMeshHeader* hdr = (const WdMeshHeader*)buf;
        uint32_t from = hdr->from;
        if (from == 0 || from == lora_my_node_id) continue;   // skip self / null

        lora_pkt_total++;

        int hopLimit = hdr->flags & 0x07;
        int hopStart = (hdr->flags >> 5) & 0x07;
        int hops = hopStart - hopLimit; if (hops < 0) hops = 0;
        bool direct = (hops == 0);

        // Feed every spatially useful packet to the streaming estimator
        // before the CSV throttle. Relay packets update presence metadata
        // but are excluded from the fit inside pm_lora_locator_observe().
        if (gps.location.isValid() && gps.location.age() < 10000UL) {
            pm_lora_locator_observe(from,
                                    gps.location.lat(), gps.location.lng(),
                                    rssi, snr, (uint8_t)hops, millis());
        }

        if (!wdLoraShouldLog(from)) continue;

        // Format for wardrive CSV:
        //   MAC   = 00:00:aa:bb:cc:dd  (origin NodeID padded to 6 octets)
        //   SSID  = !aabbccdd          (or "[RELAY] !aabbccdd" when hops>0)
        //   Auth  = LORA
        //   Chan  = Meshtastic channel hash
        //   Type  = LORA
        char mac[18];
        snprintf(mac, sizeof(mac), "00:00:%02x:%02x:%02x:%02x",
                 (unsigned)((from >> 24) & 0xFF), (unsigned)((from >> 16) & 0xFF),
                 (unsigned)((from >>  8) & 0xFF), (unsigned)( from        & 0xFF));
        char ssid[24];
        if (direct) snprintf(ssid, sizeof(ssid), "!%08x",          (unsigned)from);
        else        snprintf(ssid, sizeof(ssid), "[RELAY] !%08x",  (unsigned)from);

        wardrive_log_observation(mac, ssid, "LORA", (int)hdr->channel, rssi, "LORA");
        (void)snr;     // SNR is not part of the WiGLE schema; reserved for future
    }
}
#endif  // SX1262 LoRa devices only

void wardrive_task(void *pvParameters) {
#ifdef DEVICE_TDECK_PLUS
    // T-Deck: GPIO 10 = BOARD_POWERON, drives the screen and PMU rail.
    // Keep it driven high while wardrive task is alive.
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
#endif
    // T-LoraPager has no equivalent — all peripheral power is on the
    // XL9555 expander and handled by main.cpp's xl9555_boot_sequence().

    // Boot settling — radios + UI. Wait until 15.5 s after BOOT, not
    // 15.5 s after spawn: the Cardputer spawns this task only when the
    // user opens Wardrive (often minutes after boot), and a fixed 15 s
    // delay there just meant 15 s of "waiting..." on every open.
    {
        const uint32_t settle_until_ms = 15500;
        uint32_t now = millis();
        vTaskDelay(pdMS_TO_TICKS(now < settle_until_ms ? settle_until_ms - now : 50));
    }

    // ─────────────────────────────────────────────────────────
    //  GPS UART per device — pins AND baud differ by hardware
    //
    //  T-Deck Plus / T-LoraPager carry ATGM336H configured for
    //  38400 baud (Quectel/AT6668 default for those modules).
    //  Cardputer ADV uses M5Stack's Cap LoRa-1262/868 expansion,
    //  whose ATGM336H/AT6668 ships from M5Stack at 115200 baud
    //  (M5Stack reference code in docs.m5stack.com confirms).
    //
    //  Wrong baud = NMEA characters arrive as gibberish, fail
    //  TinyGPSPlus checksum, look "silent." Starting at the
    //  right value avoids dead time at boot.
    // ─────────────────────────────────────────────────────────
#if WD_OWNS_GPS_UART
    uint32_t current_baud = 38400;
    SerialGPS.setRxBufferSize(512);

#ifdef DEVICE_TLORAPAGER
    // T-LoraPager UART pins per schematic:
    //   GPS_TX  -> ESP32 RX  = GPIO 4
    //   GPS_RX  <- ESP32 TX  = GPIO 12
    // SerialGPS.begin(baud, config, RX, TX)
    SerialGPS.begin(current_baud, SERIAL_8N1, 4, 12);
#else
    // T-Deck Plus UART pins (RX=44, TX=43).
    SerialGPS.begin(current_baud, SERIAL_8N1, 44, 43);
#endif
    unsigned long last_baud_switch = millis();
    uint32_t      last_chars_delta = 0;
#else
    // Cardputer ADV (Cap LoRa-1262/868, GPS RX=15 TX=13 @ 115200): the
    // tether task already owns this UART. See WD_OWNS_GPS_UART.
#endif

    unsigned long last_switch_time = 0;
    unsigned long last_ble_flush   = 0;
    bool          scanningWiFi     = false;  // first timed toggle enters WiFi
#ifdef DEVICE_CARDPUTER_ADV
    wifi_ap_record_t* cpRecords =
        (wifi_ap_record_t*)heap_caps_calloc(CP_SCAN_MAX_APS, sizeof(wifi_ap_record_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!cpRecords) {
        Serial.println("[WARDRIVE/CP] WiFi scan record buffer allocation failed");
    }
#endif

    // Session file is created lazily — see ensure_session_file().
    // _current_log_file remains "" until SD is ready and the first
    // creation succeeds. On T-Deck this happens on the very first
    // scan-loop iteration; on Pager it may take several seconds.
    _current_log_file[0] = '\0';
    Serial.println("[WARDRIVE] Task started — session file will be created when SD is ready");

#ifdef DEVICE_CARDPUTER_ADV
    // ─────────────────────────────────────────────────────
    //  Cardputer ADV (no PSRAM): WiFi-first init.
    //
    //  WiFi RX descriptors are DMA buffers — they MUST live in
    //  internal SRAM (PSRAM can't host DMA on ESP32-S3, and even
    //  if it could, this device has none). NimBLE allocates ~30 KB
    //  of internal SRAM at init; if it runs first, WiFi's later
    //  allocation can fail with "Expected to init N rx buffer,
    //  actual is M" because of heap fragmentation.
    //
    //  Initializing WiFi first lets it claim contiguous descriptor
    //  memory from a less-fragmented heap. NimBLE's smaller, more
    //  flexible allocations adapt to what's left.
    //
    //  v2.12: BLE init is DEFERRED. WiFi-first eager init at task
    //  start leaves only ~3KB free heap after BLE attaches — too
    //  little for NimBLE to alloc an advertisement-report object,
    //  causing std::terminate() the moment the first BLE adv
    //  arrives. We init WiFi here, then defer NimBLE::init() until
    //  the first BLE scan window (lazy, inside the toggle loop).
    //  By that time WiFi's first scan has completed and freed its
    //  temporary allocations, giving NimBLE the headroom it needs.
    //
    //  IMPORTANT: We do NOT run a pre-BLE smoke scan here. Previous
    //  iterations did, and it allocated/freed buffers in a way that
    //  corrupted the WiFi driver state when NimBLE then tried to
    //  attach the coex layer — the "Expected to init 4 rx buffer,
    //  actual is 2" error would re-appear during NimBLE init and
    //  the task would hang waiting for a coex handshake that never
    //  completed. WiFi mode + disconnect is enough to claim the
    //  descriptors; let the actual scan happen inside the main loop
    //  AFTER BLE has attached cleanly.
    // ─────────────────────────────────────────────────────
    Serial.printf("[WARDRIVE/CP] Free heap before WiFi init: %u\n",
                  (unsigned)ESP.getFreeHeap());
    Serial.println("[WARDRIVE/CP] esp_wifi_init(scan-only cfg)...");
    bool wifi_ok = cp_init_scan_wifi();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_mode_t raw_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&raw_mode);
    Serial.printf("[WARDRIVE/CP] WiFi scan cfg %s. mode=%d\n",
                  wifi_ok ? "active" : "FAILED", (int)raw_mode);

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // modem-sleep on; avoids crash when BT activates
    vTaskDelay(pdMS_TO_TICKS(400));   // let WiFi driver fully settle before BLE attaches

    Serial.printf("[WARDRIVE/CP] Free heap after WiFi init: %u\n",
                  (unsigned)ESP.getFreeHeap());
    Serial.println("[WARDRIVE/CP] BLE init DEFERRED — will lazy-init at first BLE window");

    vTaskDelay(pdMS_TO_TICKS(200));
#else
    // T-Deck Plus and T-LoRa Pager retain the original BLE-first order
    // (they have PSRAM headroom and the init order doesn't matter).
    initWardriveBLE();
    WiFi.mode(WIFI_STA);
    if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

    // LoRa receiver — lazy init alongside the WiFi+BLE radios. On a
    // device with no SX1262 wired (some T-Deck Plus variants) this
    // call returns false and the wardrive task continues WiFi+BLE
    // only. Done OUTSIDE the WiFi/BLE coex path because LoRa is its
    // own radio and doesn't share a bus contention story with them.
#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
    wdLoraInit();
#endif

    for (;;) {
        // ── Teardown check ───────────────────────────────────────
        // wardrive_teardown() (called from wifi_mode.cpp when switching
        // from SCANNER to CLIENT mode) sets this flag. We clean up all
        // resources and vTaskDelete ourselves. After this point the
        // FreeRTOS task is gone; init_wardrive_core() must be called
        // again to respawn.
        if (s_wardrive_teardown_requested) {
            Serial.println("[WARDRIVE] Task tearing down...");
            wardrive_active = false;

            // Stop any in-flight BLE scan first
            if (wdScan) {
                wdScan->stop();
                wdScan->setAdvertisedDeviceCallbacks(nullptr, false);
            }
#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
            // Take LoRa down before WiFi/BLE teardown so its SPI ISR
            // cannot fire during the rest of the shutdown.
            wdLoraDeinit();
            pm_lora_lease_release(PM_LORA_WARDRIVE);
#endif
            // Full NimBLE shutdown — frees host task + ~48KB
            bt_ble_live = false;
            bt_last_window = 0;
            if (nimbleInit) {
                NimBLEDevice::deinit(true);
                nimbleInit = false;
                wdScan = nullptr;
                Serial.printf("[WARDRIVE] NimBLE deinit complete, heap: %u\n",
                              (unsigned)ESP.getFreeHeap());
            }
#if WD_OWNS_GPS_UART
            // Stop GPS UART (frees rx buffer)
            SerialGPS.end();
#endif
            // Disconnect WiFi cleanly (releases scanner-mode driver state)
#ifdef DEVICE_CARDPUTER_ADV
            cp_deinit_scan_wifi();
            free(cpRecords);
            cpRecords = nullptr;
#else
            WiFi.scanDelete();
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
#endif
            Serial.printf("[WARDRIVE] WiFi off, heap: %u\n",
                          (unsigned)ESP.getFreeHeap());

            // Signal completion BEFORE deleting ourselves (caller is
            // waiting on this flag)
            s_wardrive_torn_down = true;
            s_wardrive_task = NULL;
            vTaskDelete(NULL);
            // Unreachable
            return;
        }

#if WD_OWNS_GPS_UART
        while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());

        // GPS auto-baud
        if (millis() - last_baud_switch > 5000) {
            uint32_t current_chars = gps.charsProcessed();
            uint32_t delta = current_chars - last_chars_delta;
            last_chars_delta = current_chars;
            if (delta < 10 || gps.passedChecksum() == 0) {
                current_baud = (current_baud == 38400) ? 9600 : 38400;
                SerialGPS.updateBaudRate(current_baud);
                Serial.printf("[GPS] Silent (%lu chars/5s). Trying %lu baud.\n",
                              (unsigned long)delta, (unsigned long)current_baud);
            } else {
                Serial.printf("[GPS] Locked at %lu baud (%lu chars/5s, valid_fix=%s)\n",
                              (unsigned long)current_baud, (unsigned long)delta,
                              gps.location.isValid() ? "YES" : "NO");
            }
            last_baud_switch = millis();
        }
#endif

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
        // Radio lease (pm_lora_lease.h). Checked even while paused so
        // Mesh Messenger never waits on a paused Wardrive. Yield from
        // this task, so our SX1262 object is never deleted under us;
        // take the radio back once it is free again.
        if (pm_lora_lease_should_yield(PM_LORA_WARDRIVE)) {
            wdLoraDeinit();
            pm_lora_lease_release(PM_LORA_WARDRIVE);
            Serial.println("[WARDRIVE/LORA] Radio released to a higher-priority app; "
                           "will take it back when free");
        } else if (!lora_ready && !lora_init_tried) {
            wdLoraInit();
        }
#endif

        if (!wardrive_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
        // Drain LoRa only while the Ghost Engine is active. This makes the
        // Wardrive pause control apply to WiFi, BLE, and LoRa consistently.
        // The drain remains independent of the WiFi/BLE time slice.
        wdLoraDrain();
#endif

        if (wardrive_mode == WARDRIVE_MODE_PROMISCUOUS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Lazy session-file creation — first iteration where SD is ready
        // (or the next iteration after sd_in_use clears) we create the
        // /wardrive_NNNN.csv file with proper rotation. Until then writes
        // are skipped silently.
        bool have_session = ensure_session_file();
#ifdef DEVICE_CARDPUTER_ADV
        cp_diag_open();   // open /diag_NNNN.txt once SD is ready (writes reset reason header)
#endif

        // TIME-SLICED RADIO — skip SD writes if file manager is active
        // ─────────────────────────────────────────────────────
        //  Radio time-slicing — 2 second toggle (was 4s in v2.9).
        //
        //  Each radio's scan window is ~1-1.5s (WiFi.scanNetworks
        //  takes ~1.5s, BLE scan is a configured 1500ms window).
        //  At 4s toggling, each radio sat idle for 2.5s every
        //  cycle — long enough to miss networks/devices passed at
        //  walking or driving speed. At 2s toggling, each radio
        //  is effectively always working, with only the mode-switch
        //  settle time as overhead. Temporal resolution doubles.
        // ─────────────────────────────────────────────────────
        if (millis() - last_switch_time > 2000 && !wifi_in_use) {
            last_switch_time = millis();
            scanningWiFi = !scanningWiFi;

            if (scanningWiFi) {
                if (wdScan) wdScan->clearResults();
#ifndef DEVICE_CARDPUTER_ADV
                // T-Deck and Pager re-assert STA mode each scan. On Cardputer
                // this re-triggers the WiFi/BLE coex init and can fail buffer
                // allocation — driver mode persists from init, no need to
                // reassert. Original line preserved for the other targets.
                WiFi.mode(WIFI_STA);
#endif
                vTaskDelay(pdMS_TO_TICKS(50));
#if WD_OWNS_GPS_UART
                while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());
#endif

#ifdef DEVICE_CARDPUTER_ADV
                uint16_t cpTotalSeen = 0;
                int n = -1;
                if (cpRecords) {
                    n = cp_scan_wifi_records(cpRecords, CP_SCAN_MAX_APS, &cpTotalSeen);
                } else {
                    Serial.println("[WARDRIVE/CP] WiFi scan record allocation failed");
                }
#else
                int n = WiFi.scanNetworks(false, true);
#endif
#if WD_OWNS_GPS_UART
                while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());
#endif

#ifdef DEVICE_CARDPUTER_ADV
                wifi_mode_t raw_mode = WIFI_MODE_NULL;
                esp_wifi_get_mode(&raw_mode);
                Serial.printf("[WARDRIVE/CP] scan: n=%d total=%u mode=%d heap=%u largest=%u session=%d sd_ready=%d sd_busy=%d\n",
                              n, (unsigned)cpTotalSeen, (int)raw_mode,
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)cp_largest_internal_block(),
                              have_session ? 1 : 0,
                              g_sd_ready ? 1 : 0,
                              sd_in_use ? 1 : 0);
                cp_diag_log("scan",
                            cpTotalSeen > 0 ? (int)cpTotalSeen : n,
                            ESP.getFreeHeap(), cp_largest_internal_block());
#endif

                // Only write to SD if we have a session file AND the file
                // manager isn't using the card.
                if (n > 0 && have_session && !sd_in_use) {
                    int reported = n;
#ifdef DEVICE_CARDPUTER_ADV
                    reported = cpTotalSeen > 0 ? (int)cpTotalSeen : n;
#endif
                    networks_found = reported;
                    networks_total += reported;
                    if (gps.location.isValid()) {
                        if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            bool wrote_wifi_rows = false;
                            FsFile file = sd.open(_current_log_file, O_WRITE | O_APPEND);
                            if (file) {
                                for (int i = 0; i < n; ++i) {
#ifdef DEVICE_CARDPUTER_ADV
                                    char wMac[18];
                                    char rawSsid[33];
                                    char ssid[48];
                                    cp_format_mac(cpRecords[i].bssid, wMac);
                                    cp_sanitize_ssid(cpRecords[i].ssid, rawSsid);
                                    bool wEsp = isEspressifMAC(wMac);
                                    if (wEsp) {
                                        esp_found++;
                                        snprintf(ssid, sizeof(ssid), "[ESP32] %s", rawSsid);
                                    } else {
                                        snprintf(ssid, sizeof(ssid), "%s", rawSsid);
                                    }
                                    file.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,%s\n",
                                        wMac, ssid, cp_auth_name(cpRecords[i].authmode),
                                        gps_timestamp().c_str(),
                                        cpRecords[i].primary, cpRecords[i].rssi,
                                        gps.location.lat(), gps.location.lng(),
                                        gps.altitude.meters(), 5, "WIFI");
#else
                                    String wMac = WiFi.BSSIDstr(i);
                                    bool   wEsp = isEspressifMAC(wMac.c_str());
                                    if (wEsp) esp_found++;
                                    String ssid = wEsp ? "[ESP32] " + WiFi.SSID(i)
                                                       : WiFi.SSID(i);
                                    file.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,%s\n",
                                        wMac.c_str(), ssid.c_str(),
                                        WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "WPA",
                                        gps_timestamp().c_str(),
                                        WiFi.channel(i), WiFi.RSSI(i),
                                        gps.location.lat(), gps.location.lng(),
                                        gps.altitude.meters(), 5, "WIFI");
#endif
                                }
                                file.close();
                                wrote_wifi_rows = true;
#ifdef DEVICE_CARDPUTER_ADV
                            } else {
                                cp_sd_diag_throttled("wifi_append_open_fail");
#endif
                            }
                            xSemaphoreGiveRecursive(spi_mutex);
                            if (wrote_wifi_rows) vTaskDelay(pdMS_TO_TICKS(1));
#ifdef DEVICE_CARDPUTER_ADV
                        } else {
                            cp_sd_diag_throttled("wifi_append_mutex_timeout");
#endif
                        }
#ifdef DEVICE_CARDPUTER_ADV
                    } else {
                        cp_sd_diag_throttled("wifi_append_no_gps");
#endif
                    }
                } else if (n > 0) {
                    int reported = n;
#ifdef DEVICE_CARDPUTER_ADV
                    reported = cpTotalSeen > 0 ? (int)cpTotalSeen : n;
#endif
                    networks_found = reported; // Update count even if we skip the write
                    networks_total += reported;
                }
#ifndef DEVICE_CARDPUTER_ADV
                WiFi.scanDelete();
#endif

            } else {
                // BLE window
#ifdef DEVICE_CARDPUTER_ADV
                // Lazy BLE init: defer until first BLE window. This lets
                // WiFi's first scan complete and free temporary allocations
                // before NimBLE claims its ~48KB. Without this, eager BLE
                // init leaves only ~3KB free and the next BLE advertisement
                // arrival triggers std::terminate() in operator new.
                //
                // Threshold logic (post-v1.2 sdkconfig-flags revert):
                //   Untuned NimBLE consumes ~48KB at init. We attempted to
                //   shrink it via -DCONFIG_BT_NIMBLE_* build flags but the
                //   Arduino-ESP32 framework links against pre-compiled
                //   libbt.a built with defaults, so the flags caused a
                //   library/controller mismatch that hung init. Flags
                //   removed; NimBLE runs at its full default footprint.
                //
                //   The pressure is now relieved by other architectural
                //   work in v1.2:
                //     - WiFi mode-locking ensures wardrive owns the WiFi
                //       radio exclusively (Gemini / Bridge / file-mgr
                //       cannot run during a wardrive session)
                //     - Lazy boot init (no autoconnect, no Gemini at
                //       startup) gives ~30-50KB more headroom at wardrive
                //       task start vs prior builds
                //
                //   Expected heap budget on Cardputer ADV after these:
                //     pre-WiFi:    ~150-180KB free
                //     post-WiFi:   ~95-130KB free (52KB cost)
                //     post-NimBLE: ~50-80KB free (48KB cost)
                //     runtime headroom: 50KB+ for ad allocations
                //
                //   Gate (v2.14): NimBLE comes up only when the heap can
                //   hold its init cost AND a scan window's minimum
                //   (CP_BLE_SCAN_MIN_FREE_HEAP) afterwards. Below the gate
                //   the window is skipped for two heap reads: no init, no
                //   deinit, no churn. BLE starts by itself as soon as
                //   memory allows. The init cost is measured on every real
                //   init, so the gate follows this build's NimBLE footprint.
                //   If memory runs short after init, NimBLEDevice::deinit
                //   (true) stops the host task entirely so no GAP events
                //   can arrive and crash on an exhausted heap.
                static uint32_t bleGateSkips = 0;
                if (!nimbleInit) {
                    uint32_t heap_before    = ESP.getFreeHeap();
                    uint32_t largest_before = cp_largest_internal_block();
                    uint32_t gate = s_ble_init_cost + CP_BLE_SCAN_MIN_FREE_HEAP;
                    if (heap_before < gate ||
                        largest_before < CP_BLE_INIT_MIN_LARGEST_BLOCK) {
                        bt_ble_live = false;
                        bt_last_window = 0;
                        // First skip, then about once a minute (a window
                        // comes every ~2 s): enough to tune the numbers
                        // from serial or the diag log without flooding.
                        if (bleGateSkips++ % 30 == 0) {
                            Serial.printf("[WARDRIVE/CP] BLE held off: heap=%u need=%u "
                                          "(init~%u + scan %u) largest=%u need=%u\n",
                                          (unsigned)heap_before, (unsigned)gate,
                                          (unsigned)s_ble_init_cost,
                                          (unsigned)CP_BLE_SCAN_MIN_FREE_HEAP,
                                          (unsigned)largest_before,
                                          (unsigned)CP_BLE_INIT_MIN_LARGEST_BLOCK);
                            cp_diag_log("ble_gate", networks_found,
                                        heap_before, largest_before);
                        }
                        scanningWiFi = true;
                        continue;
                    }
                    bleGateSkips = 0;
                    Serial.printf("[WARDRIVE/CP] Lazy BLE init, free heap: %u largest: %u\n",
                                  (unsigned)heap_before, (unsigned)largest_before);
                    initWardriveBLE();
                    uint32_t heap_after = ESP.getFreeHeap();
                    int32_t  init_cost  = (int32_t)(heap_before - heap_after);
                    if (init_cost > 0) {
                        uint32_t c = (uint32_t)init_cost;
                        if (c < CP_BLE_INIT_COST_FLOOR) c = CP_BLE_INIT_COST_FLOOR;
                        if (c > CP_BLE_INIT_COST_CEIL)  c = CP_BLE_INIT_COST_CEIL;
                        s_ble_init_cost = c;
                    }
                    Serial.printf("[WARDRIVE/CP] Lazy BLE init done, free heap: %u (init cost %d)\n",
                                  (unsigned)heap_after, (int)init_cost);
                    cp_diag_log("ble_init", networks_found,
                                heap_after, cp_largest_internal_block());
                    // Emergency floor: each NimBLEAdvertisedDevice is
                    // ~150-300 bytes. With less than 10 KB left, tear
                    // NimBLE down before any GAP event can arrive. The
                    // learned init cost raises the gate, so it will not
                    // come straight back up into the same hole.
                    if (heap_after < 10000) {
                        Serial.printf("[WARDRIVE/CP] WARNING: Insufficient heap "
                                      "after BLE init (%u<10000). Tearing down "
                                      "NimBLE before any GAP event arrives.\n",
                                      (unsigned)heap_after);
                        if (wdScan) wdScan->stop();
                        NimBLEDevice::deinit(true);
                        nimbleInit = false;
                        wdScan = nullptr;
                        bt_ble_live = false;
                        bt_last_window = 0;
                        scanningWiFi = true;
                        continue;
                    }
                }
#endif
                // BLE window — reset per-scan counter before starting
                bt_found = 0;
                uint32_t ble_scan_seconds = 2;
#ifdef DEVICE_CARDPUTER_ADV
                static uint8_t pressureSkips = 0;
                uint32_t pre_ble_heap = ESP.getFreeHeap();
                uint32_t pre_ble_largest = cp_largest_internal_block();
                if (pre_ble_heap < CP_BLE_SCAN_MIN_FREE_HEAP ||
                    pre_ble_largest < CP_BLE_SCAN_MIN_LARGEST_BLOCK) {
                    pressureSkips++;
                    bt_ble_live = false;
                    bt_last_window = 0;
                    Serial.printf("[WARDRIVE/CP] BLE window skipped under memory pressure "
                                  "(heap=%u largest=%u skip=%u)\n",
                                  (unsigned)pre_ble_heap,
                                  (unsigned)pre_ble_largest,
                                  (unsigned)pressureSkips);
                    cp_diag_log("ble_skip", networks_found,
                                pre_ble_heap, pre_ble_largest);
                    if (pressureSkips >= 3 && nimbleInit) {
                        Serial.println("[WARDRIVE/CP] BLE pressure persists; deinit NimBLE for recovery");
                        cp_diag_log("ble_deinit_pressure", networks_found,
                                    pre_ble_heap, pre_ble_largest);
                        if (wdScan) wdScan->stop();
                        NimBLEDevice::deinit(true);
                        nimbleInit = false;
                        wdScan = nullptr;
                    }
                    scanningWiFi = true;
                    continue;
                }
                pressureSkips = 0;
                s_ble_window_events = 0;
                s_ble_window_drops = 0;
                ble_scan_seconds = CP_BLE_SCAN_SECONDS;
#endif
                wdScan->start(ble_scan_seconds, false);
#ifdef DEVICE_CARDPUTER_ADV
                uint32_t post_ble_heap = ESP.getFreeHeap();
                uint32_t post_ble_largest = cp_largest_internal_block();
                // Lifetime low-water mark of internal heap since boot.
                // The per-window numbers above are snapshots; this one
                // shows how close anything (a GAP burst, a WiFi scan)
                // ever came to the edge, which is what the thresholds
                // above should be tuned against.
                uint32_t heap_low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                static uint32_t s_logged_heap_low = UINT32_MAX;
                Serial.printf("[WARDRIVE/CP] BLE window: seen=%u kept=%d drops=%u "
                              "heap=%u largest=%u low=%u\n",
                              (unsigned)s_ble_window_events, bt_found,
                              (unsigned)s_ble_window_drops,
                              (unsigned)post_ble_heap,
                              (unsigned)post_ble_largest,
                              (unsigned)heap_low);
                cp_diag_log("ble_window", bt_found, post_ble_heap, post_ble_largest);
                // New low by at least 1 KB: record it in the diag log so
                // an untethered drive shows when and where it happened.
                if (heap_low + 1024 < s_logged_heap_low) {
                    s_logged_heap_low = heap_low;
                    cp_diag_log("heap_low", bt_found, heap_low, post_ble_largest);
                }
                if (post_ble_heap < CP_BLE_RECOVER_FREE_HEAP ||
                    post_ble_largest < CP_BLE_RECOVER_LARGEST_BLOCK) {
                    Serial.println("[WARDRIVE/CP] BLE heap floor breached; deinit NimBLE before next window");
                    bt_ble_live = false;
                    bt_last_window = 0;
                    cp_diag_log("ble_deinit_floor", networks_found,
                                post_ble_heap, post_ble_largest);
                    if (wdScan) wdScan->stop();
                    NimBLEDevice::deinit(true);
                    nimbleInit = false;
                    wdScan = nullptr;
                    scanningWiFi = true;
                    continue;
                }
#endif
                ble_total += bt_found;
                bt_last_window = bt_found;
                bt_ble_live = true;
#ifdef DEVICE_CARDPUTER_ADV
                // Drain now instead of waiting for the 5 s periodic
                // flush, so the queue is empty before the next window.
                if (have_session) {
                    cp_drain_ble_queue(_current_log_file);
                    last_ble_flush = millis();
                }
#endif
            }
        }

        if (have_session && millis() - last_ble_flush > 5000) {
            flushBLEQueue(_current_log_file); // sd_in_use checked inside
            last_ble_flush = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─────────────────────────────────────────────────────────────────────
//  wardrive task lifecycle
//
//  Lifecycle for WiFi mode-locking:
//  - init_wardrive_core() spawns the FreeRTOS task on Core 0.
//  - The task runs forever, alternating WiFi/BLE scans, until either:
//      (a) wardrive_teardown() is called — task exits cleanly, frees
//          NimBLE, closes session file, allows g_wifi_mode switch
//      (b) The device reboots.
//  - On Cardputer, this is the path used to free memory for WiFi-
//    client apps (Gemini, Bridge, file manager). T-Deck Plus and
//    Pager have PSRAM headroom so they never call teardown — the
//    task lives for the whole boot session.
//
//  State variables s_wardrive_task / s_wardrive_teardown_requested /
//  s_wardrive_torn_down are declared at file scope near the top of
//  this file (line ~160) so the task loop can reference them — they
//  are not redeclared here.
// ─────────────────────────────────────────────────────────────────────

void init_wardrive_core() {
    // Idempotent — multiple calls are safe. On Cardputer this is called
    // from run_wardrive() the first time the user enters the app, and may
    // be called again if the user exits and re-enters or after a mode-
    // lock teardown. The task is only spawned when not already running.
    if (s_wardrive_task != NULL) return;
    s_wardrive_teardown_requested = false;
    s_wardrive_torn_down = false;
    xTaskCreatePinnedToCore(wardrive_task, "WarDriveCore", 8192, NULL, 1,
                            &s_wardrive_task, 0);
}

// Request a clean teardown of the wardrive task and all its resources.
// Called from wifi_mode.cpp when switching from SCANNER to CLIENT mode.
// Blocks until the task confirms it has exited. Returns false if
// teardown fails to complete within timeout_ms.
//
// After successful teardown:
//   - wardrive_task is no longer running
//   - NimBLE is fully deinitialized (host task gone)
//   - WiFi scanner state is torn down
//   - Session CSV file is closed
//   - All wardrive heap is freed (~13KB internal + 48KB NimBLE = ~61KB)
//   - init_wardrive_core() can be called again to respawn
bool wardrive_teardown(uint32_t timeout_ms) {
    if (s_wardrive_task == NULL) {
        // Not running — nothing to tear down.
        return true;
    }
    Serial.println("[WARDRIVE] Teardown requested");
    wardrive_active = false;
    s_wardrive_teardown_requested = true;

    uint32_t start = millis();
    while (!s_wardrive_torn_down && (millis() - start) < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_wardrive_torn_down) {
        Serial.println("[WARDRIVE] Teardown TIMEOUT — task did not exit");
        return false;
    }
    Serial.printf("[WARDRIVE] Teardown complete, free heap: %u\n",
                  (unsigned)ESP.getFreeHeap());
    return true;
}

void wardrive_ble_stop() {
    wardrive_active = false;
    if (wdScan) {
        wdScan->stop();
        wdScan->setAdvertisedDeviceCallbacks(nullptr, false);
    }
}

void wardrive_ble_resume() {
    if (wdScan && wdCallbacks)
        wdScan->setAdvertisedDeviceCallbacks(wdCallbacks, false);
    wardrive_active = true;
}

bool wardrive_lora_is_ready() {
#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
    return lora_ready;
#else
    return false;
#endif
}

uint32_t wardrive_lora_packet_count() {
#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
    return lora_pkt_total;
#else
    return 0;
#endif
}

int wardrive_lora_nodes_recent(uint32_t window_ms) {
#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
    // Read-only walk of the per-node throttle table (written by the scan
    // task). lastLogMs trails the true last-heard time by at most
    // WARDRIVE_LORA_PER_NODE_MS, which is noise against a minutes-long
    // window. A torn read can miscount by one for one refresh; no lock
    // is taken so the UI never waits on the radio task.
    int n = lora_throttle_count;
    if (n > WARDRIVE_LORA_MAX_NODES) n = WARDRIVE_LORA_MAX_NODES;
    uint32_t now = millis();
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (now - lora_throttle[i].lastLogMs < window_ms) count++;
    }
    return count;
#else
    (void)window_ms;
    return 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────
//  run_wardrive() — Live wardrive UI
//
//  Three device layouts (resolved at compile time):
//
//    T-Deck Plus   320 x 240   6 rows at textSize 2, single column,
//                              touch header / touch button to control.
//    T-LoRa Pager  480 x 222   6 rows at textSize 2, single column,
//                              Q to exit, M to toggle pause.
//    Cardputer ADV 240 x 135   6 fields in a compact 2-column grid at
//                              textSize 1, all visible without scroll,
//                              Q to exit, M to toggle pause.
// ─────────────────────────────────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV
static void run_wardrive_cardputer() {
    // Mode-lock check: on Cardputer, wardrive requires WIFI_MODE_PM_SCANNER.
    // If WiFi is currently in CLIENT mode (e.g. Gemini was just used), the
    // user will be prompted to confirm the switch — accepting tears down
    // the WiFi client connection to free ~52KB for scanner mode + NimBLE.
    if (!request_wifi_mode(WIFI_MODE_PM_SCANNER, "Wardrive")) {
        // User declined or teardown failed — return to launcher
        return;
    }

    // First-entry task spawn: on Cardputer the wardrive task is NOT
    // started at boot (see main.cpp's PROCESS SPAWN block). After
    // request_wifi_mode succeeds, init_wardrive_core() was called
    // inside init_wifi_scanner(). On re-entry after a prior teardown,
    // init_wardrive_core() spawns a fresh task.
    init_wardrive_core();

    const int W = 240;
    const int H = 135;

    gfx->fillScreen(0x0000);

    // Header — 12px tall, leaves 123px of working area
    gfx->fillRect(0, 0, W, 12, 0x18C3);
    gfx->setCursor(4, 3);
    gfx->setTextColor(0x07E0); gfx->setTextSize(1);
    gfx->print("WARDRIVE | Q EXIT | M PAUSE");

    // Log filename ribbon below header
    gfx->setTextSize(1); gfx->setTextColor(0x4208);
    gfx->setCursor(4, 15);
    gfx->print(_current_log_file[0] ? _current_log_file : "waiting...");

    // Two-column grid:
    //   Col A (x=4, value x=34)    Col B (x=124, value x=154)
    //     WIFI  ###                  SATS  ##
    //     BLE   ###                  ALT   #ft
    //     LAT   ##.####              LNG   ##.####
    //     LORA  RX                   NODE  # (5m)
    const int rowY[4] = { 28, 44, 60, 76 };

    // Static labels (drawn once)
    gfx->setTextColor(0xFFFF); gfx->setTextSize(1);
    gfx->setCursor(  4, rowY[0]); gfx->print("WIFI");
    gfx->setCursor(  4, rowY[1]); gfx->print("BLE ");
    gfx->setCursor(  4, rowY[2]); gfx->print("LAT ");
    gfx->setCursor(124, rowY[0]); gfx->print("SATS");
    gfx->setCursor(124, rowY[1]); gfx->print("ALT ");
    gfx->setCursor(124, rowY[2]); gfx->print("LNG ");
    gfx->setCursor(  4, rowY[3]); gfx->print("LORA");
    gfx->setCursor(124, rowY[3]); gfx->print("NODE");

    // Bottom action bar
    const int btnH = 16;
    const int btnY = H - btnH - 2;

    // Value cell rects for flicker-free refresh
    const int colAValueX = 34;
    const int colAValueW = 86;
    const int colBValueX = 154;
    const int colBValueW = 82;

    char ribbon_cached[32] = "";

    while (true) {
        // Refresh filename ribbon if it changed
        if (strncmp(ribbon_cached, _current_log_file, sizeof(ribbon_cached)) != 0) {
            strncpy(ribbon_cached, _current_log_file, sizeof(ribbon_cached) - 1);
            ribbon_cached[sizeof(ribbon_cached) - 1] = '\0';
            gfx->fillRect(4, 15, W - 8, 8, 0x0000);
            gfx->setTextSize(1); gfx->setTextColor(0x4208);
            gfx->setCursor(4, 15);
            gfx->print(_current_log_file[0] ? _current_log_file : "waiting...");
        }

        // Clear the eight value cells
        for (int i = 0; i < 4; i++) {
            gfx->fillRect(colAValueX, rowY[i], colAValueW, 9, 0x0000);
            gfx->fillRect(colBValueX, rowY[i], colBValueW, 9, 0x0000);
        }

        gfx->setTextSize(1);

        // Column A values
        gfx->setTextColor(networks_found > 0 ? 0x07E0 : 0xFD20);
        gfx->setCursor(colAValueX, rowY[0]); gfx->print(networks_found);
        // Current BLE count: devices kept in the last completed window.
        // "--" while BLE is held off for memory or not started yet.
        gfx->setCursor(colAValueX, rowY[1]);
        if (bt_ble_live) {
            int bt_shown = bt_last_window;
            gfx->setTextColor(bt_shown > 0 ? 0x07FF : 0xFD20);
            gfx->print(bt_shown);
        } else {
            gfx->setTextColor(0x8410);
            gfx->print("--");
        }
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(colAValueX, rowY[2]);
        gfx->printf("%.4f", gps.location.isValid() ? gps.location.lat() : 0.0);
        // LoRa receiver state and distinct nodes heard in the last 5 min.
        {
            bool lora_on = wardrive_lora_is_ready();
            gfx->setCursor(colAValueX, rowY[3]);
            gfx->setTextColor(lora_on ? 0x07E0 : 0x8410);
            gfx->print(lora_on ? "RX" : "--");
            gfx->setCursor(colBValueX, rowY[3]);
            if (lora_on) {
                int nodes = wardrive_lora_nodes_recent(300000UL);
                gfx->setTextColor(nodes > 0 ? 0x07FF : 0xFD20);
                gfx->printf("%d (5m)", nodes);
            } else {
                gfx->setTextColor(0x8410);
                gfx->print("-");
            }
        }

        // Column B values
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(colBValueX, rowY[0]); gfx->print(gps.satellites.value());
        gfx->setTextColor(0xC618);
        gfx->setCursor(colBValueX, rowY[1]);
        gfx->printf("%.0fft", gps.location.isValid() ? gps.altitude.feet() : 0.0);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(colBValueX, rowY[2]);
        gfx->printf("%.4f", gps.location.isValid() ? gps.location.lng() : 0.0);

        // ESP32 counter just above the action bar
        if (esp_found > 0) {
            gfx->fillRect(4, btnY - 10, W - 8, 8, 0x0000);
            gfx->setTextSize(1); gfx->setTextColor(0xF800);
            gfx->setCursor(4, btnY - 10);
            gfx->printf("ESP32s: %d", esp_found);
        }

        // Action bar
        uint16_t btnColor = wardrive_active ? 0x07E0 : 0xF800;
        uint16_t txtColor = wardrive_active ? 0x0000 : 0xFFFF;
        gfx->fillRect(2, btnY, W - 4, btnH, btnColor);
        gfx->setTextColor(txtColor); gfx->setTextSize(1);
        gfx->setCursor(8, btnY + 4);
        gfx->print(wardrive_active ? "LOGGING WIFI+BLE (M=PAUSE)"
                                   : "PAUSED (M=START)");

        char k = get_keypress();
        if (k == 'q' || k == 'Q' || k == 27) return;
        if (k == 'm' || k == 'M') {
            wardrive_active = !wardrive_active;
        }

        delay(500); yield();
    }
}
#endif // DEVICE_CARDPUTER_ADV

void run_wardrive() {
#ifdef DEVICE_CARDPUTER_ADV
    run_wardrive_cardputer();
    return;
#endif

#ifdef DEVICE_MAXINE
    // Maxine uses its own wardrive UI in maxine_apps.cpp
    // (maxine_run_wardrive). The shared scan task and anomaly engine
    // are reused; only the render layer is per-device. This stub
    // satisfies the linker on Maxine, which never calls run_wardrive().
    return;
#endif

    // ── Display dimensions ────────────────────────────────
    // T-Deck Plus:   320 x 240
    // T-LoRa Pager:  480 x 222 (full width)
#ifdef DEVICE_TLORAPAGER
    const int W = 480;
    const int H = 222;
#else
    const int W = 320;
    const int H = 240;
#endif

    gfx->fillScreen(0x0000);

    // Header — full screen width
    gfx->fillRect(0, 0, W, 24, 0x18C3);
    gfx->setCursor(10, 7);
    gfx->setTextColor(0x07E0); gfx->setTextSize(1);
#ifdef DEVICE_TLORAPAGER
    gfx->print("WARDRIVE LIVE | Q EXIT | M PAUSE");
#else
    gfx->print("WARDRIVE LIVE | TAP HEADER TO EXIT");
#endif

    // Log filename ribbon just below header
    gfx->setTextSize(1); gfx->setTextColor(0x4208);
    gfx->setCursor(10, 27);
    gfx->print(_current_log_file[0] ? _current_log_file : "waiting...");

    // Field labels (size 2, ~16px tall, 25px row spacing)
    gfx->setTextColor(0xFFFF); gfx->setTextSize(2);
    gfx->setCursor(10, 38);  gfx->print("WIFI:");
    gfx->setCursor(10, 63);  gfx->print("BLE: ");
    gfx->setCursor(10, 88);  gfx->print("SATS:");
    gfx->setCursor(10, 113); gfx->print("ALT: ");
    gfx->setCursor(10, 138); gfx->print("LAT: ");
    gfx->setCursor(10, 163); gfx->print("LNG: ");

    // Bottom button geometry (used both inside and outside the loop)
    const int btnH      = 30;
    const int btnY      = H - btnH - 6;        // 6px margin from bottom
    const int btnMargin = 10;
    const int btnW      = W - (btnMargin * 2);

    // Data values column starts at x=90, fills rightward to W-10
    const int dataX  = 90;
    const int dataW  = W - dataX - 10;
    const int dataY  = 38;
    const int dataH  = btnY - dataY - 8;        // stop above the button

    // Cache the displayed filename so we can refresh the ribbon when the
    // session file is created mid-app-view (Pager: SD comes up late).
    char ribbon_cached[32] = "";

    while (true) {
        // Refresh filename ribbon if it changed since last redraw
        if (strncmp(ribbon_cached, _current_log_file, sizeof(ribbon_cached)) != 0) {
            strncpy(ribbon_cached, _current_log_file, sizeof(ribbon_cached) - 1);
            ribbon_cached[sizeof(ribbon_cached) - 1] = '\0';
            gfx->fillRect(10, 27, W - 20, 10, 0x0000);
            gfx->setTextSize(1); gfx->setTextColor(0x4208);
            gfx->setCursor(10, 27);
            gfx->print(_current_log_file[0] ? _current_log_file : "waiting...");
        }

        // Clear and redraw the values column
        gfx->fillRect(dataX, dataY, dataW, dataH, 0x0000);
        gfx->setTextSize(2);

        gfx->setTextColor(networks_found > 0 ? 0x07E0 : 0xFD20);
        gfx->setCursor(dataX, 38);  gfx->println(networks_found);
        // Current BLE count: devices kept in the last completed window.
        // "--" until the first window completes.
        gfx->setCursor(dataX, 63);
        if (bt_ble_live) {
            int bt_shown = bt_last_window;
            gfx->setTextColor(bt_shown > 0 ? 0x07FF : 0xFD20);
            gfx->println(bt_shown);
        } else {
            gfx->setTextColor(0x8410);
            gfx->println("--");
        }
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(dataX, 88);  gfx->println(gps.satellites.value());
        gfx->setTextColor(0xC618);
        gfx->setCursor(dataX, 113);
        gfx->printf("%.0f FT", gps.location.isValid() ? gps.altitude.feet() : 0.0);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(dataX, 138);
        gfx->printf("%.5f", gps.location.isValid() ? gps.location.lat() : 0.0);
        gfx->setCursor(dataX, 163);
        gfx->printf("%.5f", gps.location.isValid() ? gps.location.lng() : 0.0);

        // LoRa block, right side of the WIFI/BLE/SATS rows (short values
        // there, so it never collides): receiver state, then distinct
        // nodes heard in the last 5 min. Inside the cleared value rect,
        // so the label is redrawn with the values.
        {
            const int loraX = W - 120;
            bool lora_on = wardrive_lora_is_ready();
            gfx->setTextColor(0xFFFF);
            gfx->setCursor(loraX, 38);  gfx->print("LORA");
            gfx->setTextColor(lora_on ? 0x07E0 : 0x8410);
            gfx->setCursor(loraX, 63);  gfx->print(lora_on ? "RX" : "--");
            if (lora_on) {
                int nodes = wardrive_lora_nodes_recent(300000UL);
                gfx->setTextColor(nodes > 0 ? 0x07FF : 0xFD20);
                gfx->setCursor(loraX, 88);
                gfx->printf("%d (5m)", nodes);
            }
        }

        // ESP32 counter — right-aligned just below data column
        if (esp_found > 0) {
            gfx->setTextSize(1); gfx->setTextColor(0xF800);
            // ~11 chars ("ESP32s: NNN") * 6 px = 66, right-aligned to W-10
            gfx->setCursor(W - 76, btnY - 14);
            gfx->printf("ESP32s: %d", esp_found);
        }

        // Bottom action button — anchored to actual bottom
        uint16_t btnColor = wardrive_active ? 0x07E0 : 0xF800;
        uint16_t txtColor = wardrive_active ? 0x0000 : 0xFFFF;
        gfx->fillRect(btnMargin, btnY, btnW, btnH, btnColor);
        gfx->setTextColor(txtColor); gfx->setTextSize(1);
        gfx->setCursor(btnMargin + 10, btnY + 12);
#ifdef DEVICE_TLORAPAGER
        gfx->print(wardrive_active ? "LOGGING WIFI+BLE (M TO PAUSE)"
                                   : "PAUSED (M TO START)");
#else
        gfx->print(wardrive_active ? "LOGGING WIFI+BLE (TAP TO PAUSE)"
                                   : "PAUSED (TAP TO START)");
#endif

        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                break;
            } else if (ty >= btnY) {
                wardrive_active = !wardrive_active;
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
            }
        }
        char k = get_keypress();
#ifdef DEVICE_TLORAPAGER
        if (k == 'q' || k == 'Q' || k == 27) break;
        if (k == 'm' || k == 'M') {
            wardrive_active = !wardrive_active;
        }
#else
        if (pm_is_exit_key(k)) break;
#endif
        delay(500); yield();
    }
}
