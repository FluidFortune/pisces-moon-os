// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_tracker_scan.cpp — AirTag / Tile / Samsung SmartTag detector
//
//  TOUCH KIOSK ONLY (C28P + Maxine). Standalone BLE filter pass
//  layered on top of NimBLE. Surfaces tracker-class devices with
//  proximity + persistence data so the user can answer the
//  question "am I being followed?"
//
//  DETECTION:
//
//  BLE advertisements include a "Manufacturer Specific Data"
//  field, where the first two bytes (little-endian) are the
//  company ID assigned by the Bluetooth SIG. We filter on:
//
//    APPLE FIND MY  (Company 0x004C)
//      Apple uses 0x004C for many purposes — AirDrop, iBeacons,
//      AirPods proximity, etc. The AirTag / FindMy accessory
//      advertisement is identified by the type byte at offset 2:
//        0x12  Find My / offline finding  (length 0x19)
//        0x07  Nearby info (iPhone, AirPods active) — NOT a tracker
//        0x10  AirDrop
//        0x02  iBeacon
//      We only flag 0x12.
//
//    TILE  (Company 0x00A8)
//      All Tile trackers, no sub-type filter needed.
//
//    SAMSUNG SMARTTAG  (Company 0x0075)
//      Same — all Samsung BLE manufacturer data on 0x0075 is
//      treated as a candidate SmartTag.
//
//  SCAN ARCHITECTURE — POLLING, NOT CALLBACKS:
//
//  NimBLE-Arduino 1.4.1 (pinned in platformio.ini) does not
//  expose the NimBLEScanCallbacks class — that's a 2.x API. We
//  use the same blocking polling pattern the wardrive engine uses:
//  scanner->start(1, false) returns a NimBLEScanResults after a
//  1-second window. We walk it in the UI loop, accumulate trackers
//  across scan windows, redraw, repeat. The user taps STOP to end.
//
//  Devices are tracked by BLE address. AirTags rotate their
//  resolvable random address every ~15 minutes for privacy, so
//  what looks like "one AirTag" over an hour may appear as 4
//  separate entries — that's a property of the protocol, not
//  our detector. For a single scan session of 30-60 seconds the
//  address is stable enough that one tracker = one row.
//
//  RSSI BANDS (rough free-space estimates at 2.4 GHz with tracker
//  TX power around -7 to 0 dBm):
//      >= -50   "VERY CLOSE" (< 1m)
//      >= -65   "NEAR"       (1-3m)
//      >= -80   "MEDIUM"     (3-10m)
//      <  -80   "FAR"        (>10m)
//  Material attenuation makes these wildly inaccurate in absolute
//  terms but the *trend* (getting stronger over time as you move?)
//  is the actionable signal for a stalking scenario.
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include "pm_tracker_scan.h"

extern Arduino_GFX *gfx;

#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _ts_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
static const int TS_W            = 240;
static const int TS_H            = 320;
static const int TS_EXIT_H       = 14;
static const int TS_STATUS_H     = 38;
static const int TS_LIST_Y       = 56;
static const int TS_LIST_H       = 220;
static const int TS_ROW_H        = 36;
static const int TS_BTN_Y        = 282;
static const int TS_BTN_H        = 36;
static const int TS_TEXT_NORMAL  = 1;
static const int TS_TEXT_BIG     = 2;
static const int TS_CHAR_W       = 6;
static const int TS_CHAR_H       = 8;
static const int TS_MAX_VISIBLE  = 6;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _ts_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int TS_W            = 480;
static const int TS_H            = 800;
static const int TS_EXIT_H       = 36;
static const int TS_STATUS_H     = 80;
static const int TS_LIST_Y       = 124;
static const int TS_LIST_H       = 560;
static const int TS_ROW_H        = 80;
static const int TS_BTN_Y        = 696;
static const int TS_BTN_H        = 88;
static const int TS_TEXT_NORMAL  = 2;
static const int TS_TEXT_BIG     = 4;
static const int TS_CHAR_W       = 6;
static const int TS_CHAR_H       = 8;
static const int TS_MAX_VISIBLE  = 7;
#endif

// ─── Theme ───
static const uint16_t COL_BG       = 0x0000;
static const uint16_t COL_HEADER   = 0x0841;
static const uint16_t COL_ACCENT   = 0x07FF;
static const uint16_t COL_GREEN    = 0x07E0;
static const uint16_t COL_AMBER    = 0xFD20;
static const uint16_t COL_RED      = 0xF800;
static const uint16_t COL_WHITE    = 0xFFFF;
static const uint16_t COL_DIM      = 0x4208;
static const uint16_t COL_TILE     = 0x18C3;

// ─── Tracker types ───
enum TrackerType {
    TT_APPLE_FINDMY = 0,
    TT_TILE         = 1,
    TT_SAMSUNG      = 2,
};
static const char* TT_LABEL[3] = { "APPLE FINDMY", "TILE", "SAMSUNG" };
static const uint16_t TT_COLOR[3] = { 0xF81F, 0x07E0, 0x07FF };  // magenta, green, cyan

// ─── Tracker entry ───
struct TrackerEntry {
    uint8_t  addr[6];
    int      rssi_last;
    int      rssi_peak;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    TrackerType type;
};
static constexpr int MAX_TRACKERS = 16;
static TrackerEntry g_trackers[MAX_TRACKERS];
static int g_tracker_count = 0;

// ─── Scan state ───
static NimBLEScan* g_scanner = nullptr;
static bool        g_ble_inited = false;
static bool        g_scan_running = false;
static uint32_t    g_scan_start_ms = 0;
static int         g_scroll = 0;

// ─── Tracker bookkeeping ───
static int find_or_add(const uint8_t addr[6], TrackerType type) {
    for (int i = 0; i < g_tracker_count; i++) {
        if (memcmp(g_trackers[i].addr, addr, 6) == 0) return i;
    }
    if (g_tracker_count >= MAX_TRACKERS) return -1;
    int idx = g_tracker_count++;
    memcpy(g_trackers[idx].addr, addr, 6);
    g_trackers[idx].rssi_last = -127;
    g_trackers[idx].rssi_peak = -127;
    g_trackers[idx].first_seen_ms = millis();
    g_trackers[idx].last_seen_ms  = millis();
    g_trackers[idx].type = type;
    return idx;
}

// ─── Apple FindMy / Tile / Samsung filter ───
static bool classify(const uint8_t* mfg, size_t mfg_len, TrackerType* out_type) {
    if (mfg_len < 2) return false;
    uint16_t company = (uint16_t)mfg[0] | ((uint16_t)mfg[1] << 8);

    if (company == 0x004C) {
        // Apple — only flag offline-finding (AirTag-style) frames.
        // Type byte at offset 2; length byte at offset 3.
        if (mfg_len < 4) return false;
        uint8_t type_byte = mfg[2];
        uint8_t len_byte  = mfg[3];
        if (type_byte == 0x12 && len_byte == 0x19) {
            *out_type = TT_APPLE_FINDMY;
            return true;
        }
        return false;
    }
    if (company == 0x00A8) {
        *out_type = TT_TILE;
        return true;
    }
    if (company == 0x0075) {
        *out_type = TT_SAMSUNG;
        return true;
    }
    return false;
}

// ─── BLE init (lazy, shared with wardrive engine) ───
static void ensure_ble() {
    if (g_ble_inited) return;
    // NimBLEDevice::init is idempotent in 1.4.1 — if the wardrive
    // engine already called it, this is a no-op.
    NimBLEDevice::init("");
    g_scanner = NimBLEDevice::getScan();
    g_scanner->setActiveScan(false);   // passive
    g_scanner->setInterval(100);
    g_scanner->setWindow(80);
    g_ble_inited = true;
}

// ─── Per-scan-window: do one 1-second scan, walk results ───
static void scan_window_and_collect() {
    ensure_ble();
    if (!g_scanner) return;

    NimBLEScanResults results = g_scanner->start(1, false);
    int count = results.getCount();
    for (int i = 0; i < count; i++) {
        NimBLEAdvertisedDevice dev = results.getDevice(i);
        if (!dev.haveManufacturerData()) continue;

        std::string mfg = dev.getManufacturerData();
        TrackerType type;
        if (!classify((const uint8_t*)mfg.data(), mfg.length(), &type)) continue;

        // Get the 6-byte address. NimBLEAddress stores bytes in
        // little-endian on this version; reverse to MSB-first for
        // human-readable display.
        NimBLEAddress nbaddr = dev.getAddress();
        const uint8_t* raw = nbaddr.getNative();
        uint8_t addr_be[6];
        for (int j = 0; j < 6; j++) addr_be[j] = raw[5 - j];

        int idx = find_or_add(addr_be, type);
        if (idx < 0) continue;
        int rssi = dev.getRSSI();
        g_trackers[idx].rssi_last = rssi;
        if (rssi > g_trackers[idx].rssi_peak) g_trackers[idx].rssi_peak = rssi;
        g_trackers[idx].last_seen_ms = millis();
    }
    g_scanner->clearResults();
}

static void clear_results() {
    g_tracker_count = 0;
    g_scroll = 0;
}

// ─── Drawing ───
static const char* rssi_band(int rssi) {
    if (rssi >= -50) return "VERY CLOSE";
    if (rssi >= -65) return "NEAR";
    if (rssi >= -80) return "MEDIUM";
    return "FAR";
}
static uint16_t rssi_color(int rssi) {
    if (rssi >= -65) return COL_RED;
    if (rssi >= -80) return COL_AMBER;
    return COL_DIM;
}

static void draw_exit_bar() {
    gfx->fillRect(0, 0, TS_W, TS_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, (TS_EXIT_H - 8) / 2);
    gfx->print("TRACKER SCAN");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(TS_W - 50, (TS_EXIT_H - 8) / 2);
    gfx->print("< EXIT");
}

static void draw_status() {
    gfx->fillRect(0, TS_EXIT_H + 2, TS_W, TS_STATUS_H, COL_BG);

    char line[64];
    if (g_scan_running) {
        uint32_t elapsed = (millis() - g_scan_start_ms) / 1000;
        snprintf(line, sizeof(line), "SCANNING  %lus  %d found",
                 (unsigned long)elapsed, g_tracker_count);
        gfx->setTextColor(COL_GREEN);
    } else {
        if (g_tracker_count == 0) {
            snprintf(line, sizeof(line), "Idle - tap SCAN to begin");
            gfx->setTextColor(COL_DIM);
        } else {
            snprintf(line, sizeof(line), "STOPPED  %d trackers seen",
                     g_tracker_count);
            gfx->setTextColor(COL_AMBER);
        }
    }
    gfx->setTextSize(TS_TEXT_NORMAL);
    gfx->setCursor(8, TS_EXIT_H + 8);
    gfx->print(line);

    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(8, TS_EXIT_H + 8 + TS_CHAR_H * TS_TEXT_NORMAL + 4);
    gfx->print("Apple FindMy + Tile + Samsung");
}

static void draw_row(int slot, const TrackerEntry& e) {
    int y = TS_LIST_Y + slot * TS_ROW_H;
    gfx->fillRect(0, y, TS_W, TS_ROW_H - 2, 0x0820);
    gfx->drawFastHLine(0, y + TS_ROW_H - 2, TS_W, COL_DIM);

    gfx->setTextSize(TS_TEXT_NORMAL);
    gfx->setTextColor(TT_COLOR[e.type]);
    gfx->setCursor(6, y + 4);
    gfx->print(TT_LABEL[e.type]);

    char addr_buf[24];
    snprintf(addr_buf, sizeof(addr_buf),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             e.addr[0], e.addr[1], e.addr[2],
             e.addr[3], e.addr[4], e.addr[5]);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, y + 4 + TS_CHAR_H * TS_TEXT_NORMAL + 2);
    gfx->print(addr_buf);

    gfx->setTextSize(TS_TEXT_NORMAL);
    gfx->setTextColor(rssi_color(e.rssi_last));
    const char* band = rssi_band(e.rssi_last);
    int bw = (int)strlen(band) * TS_CHAR_W * TS_TEXT_NORMAL;
    gfx->setCursor(TS_W - bw - 6, y + 4);
    gfx->print(band);

    char meta[24];
    uint32_t seen_for = (e.last_seen_ms - e.first_seen_ms) / 1000;
    snprintf(meta, sizeof(meta), "%d dBm  %lus", e.rssi_last,
             (unsigned long)seen_for);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_WHITE);
    int mw = (int)strlen(meta) * 6;
    gfx->setCursor(TS_W - mw - 6, y + 4 + TS_CHAR_H * TS_TEXT_NORMAL + 2);
    gfx->print(meta);
}

static void draw_list() {
    gfx->fillRect(0, TS_LIST_Y, TS_W, TS_LIST_H, COL_BG);

    if (g_tracker_count == 0) {
        gfx->setTextSize(TS_TEXT_NORMAL);
        gfx->setTextColor(COL_DIM);
        const char* msg = g_scan_running ? "Listening..."
                                          : "No trackers detected";
        int tw = (int)strlen(msg) * TS_CHAR_W * TS_TEXT_NORMAL;
        gfx->setCursor((TS_W - tw) / 2, TS_LIST_Y + 40);
        gfx->print(msg);
        return;
    }

    // Sort by RSSI (strongest first) — closest threats at the top.
    int order[MAX_TRACKERS];
    for (int i = 0; i < g_tracker_count; i++) order[i] = i;
    for (int i = 1; i < g_tracker_count; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 &&
               g_trackers[order[j]].rssi_last < g_trackers[key].rssi_last) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    int max_scroll = g_tracker_count - TS_MAX_VISIBLE;
    if (max_scroll < 0) max_scroll = 0;
    if (g_scroll > max_scroll) g_scroll = max_scroll;
    if (g_scroll < 0) g_scroll = 0;

    int slots = TS_MAX_VISIBLE;
    if (slots > g_tracker_count) slots = g_tracker_count;
    for (int s = 0; s < slots; s++) {
        int idx = order[g_scroll + s];
        draw_row(s, g_trackers[idx]);
    }
}

static void draw_buttons() {
    gfx->fillRect(0, TS_BTN_Y, TS_W, TS_BTN_H, COL_BG);
    const char* lbl0 = g_scan_running ? "STOP" : "SCAN";
    uint16_t col0 = g_scan_running ? COL_RED : COL_GREEN;
    const char* lbl1 = "CLEAR";
    uint16_t col1 = COL_AMBER;

    int bw = (TS_W - 24) / 2;
    int xs[2] = { 8, 8 + bw + 8 };
    const char* lbls[2] = { lbl0, lbl1 };
    uint16_t cols[2] = { col0, col1 };

    for (int i = 0; i < 2; i++) {
        gfx->fillRect(xs[i], TS_BTN_Y, bw, TS_BTN_H, COL_TILE);
        gfx->drawRect(xs[i], TS_BTN_Y, bw, TS_BTN_H, cols[i]);
        gfx->setTextSize(TS_TEXT_BIG);
        gfx->setTextColor(cols[i]);
        int tw = (int)strlen(lbls[i]) * TS_CHAR_W * TS_TEXT_BIG;
        gfx->setCursor(xs[i] + (bw - tw) / 2,
                       TS_BTN_Y + (TS_BTN_H - TS_CHAR_H * TS_TEXT_BIG) / 2);
        gfx->print(lbls[i]);
    }
}

static void full_redraw() {
    gfx->fillScreen(COL_BG);
    draw_exit_bar();
    draw_status();
    draw_list();
    draw_buttons();
}

// ─── Entry point ───
void pm_run_tracker_scan() {
    Serial.println("[TRACKER] entry");
    clear_results();
    g_scan_running = false;
    full_redraw();

    bool was_touched = false;
    int  press_zone = -1;

    while (true) {
        // If scanning, do a 1s BLE scan window. This blocks the UI
        // thread for ~1 second per cycle. Touch responsiveness is
        // therefore ~1 Hz during scan — acceptable for this tool;
        // the user mostly just watches the list populate.
        if (g_scan_running) {
            scan_window_and_collect();
            draw_status();
            draw_list();
        }

        // Touch poll
        int16_t tx, ty;
        bool touched = _ts_touch(&tx, &ty);

        if (touched && !was_touched) {
            press_zone = -1;
            if (ty < TS_EXIT_H) {
                press_zone = -2;
            } else if (ty >= TS_BTN_Y && ty < TS_BTN_Y + TS_BTN_H) {
                int bw = (TS_W - 24) / 2;
                if (tx < 8 + bw) press_zone = 0;
                else if (tx >= 8 + bw + 8) press_zone = 1;
            } else if (ty >= TS_LIST_Y && ty < TS_LIST_Y + TS_LIST_H
                       && g_tracker_count > TS_MAX_VISIBLE) {
                int rel = ty - TS_LIST_Y;
                if (rel < TS_LIST_H / 3) press_zone = 2;
                else if (rel > 2 * TS_LIST_H / 3) press_zone = 3;
            }
        } else if (!touched && was_touched) {
            int z = press_zone;
            press_zone = -1;
            if (z == -2) {
                if (g_scan_running && g_scanner) g_scanner->stop();
                g_scan_running = false;
                gfx->fillScreen(COL_BG);
                return;
            }
            if (z == 0) {
                if (g_scan_running) {
                    if (g_scanner) g_scanner->stop();
                    g_scan_running = false;
                } else {
                    g_scan_start_ms = millis();
                    g_scan_running = true;
                }
                draw_status();
                draw_buttons();
            } else if (z == 1) {
                if (g_scan_running && g_scanner) g_scanner->stop();
                g_scan_running = false;
                clear_results();
                full_redraw();
            } else if (z == 2) {
                if (g_scroll > 0) { g_scroll--; draw_list(); }
            } else if (z == 3) {
                int max_s = g_tracker_count - TS_MAX_VISIBLE;
                if (g_scroll < max_s) { g_scroll++; draw_list(); }
            }
        }
        was_touched = touched;

        // Only sleep when idle — during scan, the 1-second blocking
        // scan_window_and_collect() provides the pacing.
        if (!g_scan_running) {
            delay(30);
            yield();
        }
    }
}

#endif // DEVICE_C28P || DEVICE_MAXINE
