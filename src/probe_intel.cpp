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
 * PROBE INTEL — RF Device Intelligence
 *
 * User selects mode on launch:
 *
 *   SCAN MODE      — Active WiFi scan cycles. Standalone use.
 *                    Logs GPS-tagged CSV to SD. No host needed.
 *
 *   PROMISCUOUS    — True 802.11 monitor mode. Edge node use.
 *                    All management frames captured passively.
 *                    If Bridge is running → streams pkt JSON to host.
 *                    If Bridge is not running → shows live stats only.
 *
 * SPI Bus Treaty compliance:
 *   Scan mode SD writes: wrapped in spi_mutex (500ms timeout).
 *   Promiscuous mode: pm_promiscuous.h queue is ISR-safe; no SD writes.
 *   Neither mode holds the mutex during radio operations.
 */

#include "probe_intel.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SdFat.h>
#include <TinyGPSPlus.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "wardrive.h"       // wardrive_set_mode(), wardrive_active, wardrive_mode
#include "pm_promiscuous.h" // pm_stats_t, pm_promiscuous_get_stats()

extern Arduino_GFX*      gfx;
extern SdFat             sd;
extern TinyGPSPlus        gps;
extern SemaphoreHandle_t spi_mutex;

// ─────────────────────────────────────────────
//  Shared constants
// ─────────────────────────────────────────────
#define HEADER_H      24
#define FOOTER_H      25
#define BODY_TOP      (HEADER_H + 2)
#define BODY_BOT      (240 - FOOTER_H - 2)
#define ROW_H         16
#define MAX_ROWS      ((BODY_BOT - BODY_TOP) / ROW_H)

#define MAX_SCAN_NETS  48
#define LOG_DIR        "/"
#define LOG_PREFIX     "probe_"

// ─────────────────────────────────────────────
//  Scan mode state
// ─────────────────────────────────────────────
struct ScanNet {
    char     bssid[20];
    char     ssid[36];
    int      rssi;
    uint8_t  channel;
    bool     open;
    double   lat;
    double   lng;
    int      seen;
};

static ScanNet    scan_nets[MAX_SCAN_NETS];
static int        scan_net_count  = 0;
static int        scan_cursor     = 0;
static int        scan_scroll     = 0;
static uint32_t   scan_total      = 0;
static char       scan_log_path[32] = "";

// ─────────────────────────────────────────────
//  Helpers — shared across modes
// ─────────────────────────────────────────────
static uint16_t rssi_color(int rssi) {
    if (rssi > -55) return C_GREEN;
    if (rssi > -70) return 0xFFE0;
    if (rssi > -85) return 0xFD20;
    return C_RED;
}

static void draw_header(const char* title, uint16_t accent) {
    gfx->fillRect(0, 0, 320, HEADER_H, C_DARK);
    gfx->drawFastHLine(0, HEADER_H, 320, accent);
    gfx->setCursor(10, 7);
    gfx->setTextColor(accent);
    gfx->setTextSize(1);
    gfx->print(title);
}

static void draw_footer(const char* line, uint16_t col = C_GREEN) {
    gfx->fillRect(0, 240 - FOOTER_H, 320, FOOTER_H, C_DARK);
    gfx->setCursor(10, 240 - FOOTER_H + 8);
    gfx->setTextColor(col);
    gfx->print(line);
}

// ═════════════════════════════════════════════
//  MODE SELECT SCREEN
// ═════════════════════════════════════════════
typedef enum { MODE_SCAN = 0, MODE_PROMISC = 1 } pi_mode_t;

static pi_mode_t draw_mode_select(pi_mode_t initial) {
    int sel = (int)initial;

    auto redraw = [&]() {
        gfx->fillScreen(C_BLACK);

        // Title
        gfx->fillRect(0, 0, 320, HEADER_H, C_DARK);
        gfx->drawFastHLine(0, HEADER_H, 320, C_CYAN);
        gfx->setCursor(10, 7);
        gfx->setTextColor(C_CYAN);
        gfx->setTextSize(1);
        gfx->print("PROBE INTEL | SELECT MODE");

        // Option 0 — Scan Mode
        bool s0 = (sel == 0);
        gfx->fillRect(20, 50, 280, 60, s0 ? C_DARK : C_BLACK);
        gfx->drawRect(20, 50, 280, 60, s0 ? C_GREEN : C_GREY);
        gfx->setCursor(34, 60);
        gfx->setTextSize(1);
        gfx->setTextColor(s0 ? C_GREEN : C_WHITE);
        gfx->print(s0 ? "> SCAN MODE" : "  SCAN MODE");
        gfx->setCursor(34, 76);
        gfx->setTextColor(C_GREY);
        gfx->print("Active WiFi scans, GPS log to SD.");
        gfx->setCursor(34, 88);
        gfx->print("Best for: standalone field use.");

        // Option 1 — Promiscuous Mode
        bool s1 = (sel == 1);
        gfx->fillRect(20, 125, 280, 70, s1 ? C_DARK : C_BLACK);
        gfx->drawRect(20, 125, 280, 70, s1 ? C_RED : C_GREY);
        gfx->setCursor(34, 135);
        gfx->setTextSize(1);
        gfx->setTextColor(s1 ? C_RED : C_WHITE);
        gfx->print(s1 ? "> PROMISCUOUS MODE" : "  PROMISCUOUS MODE");
        gfx->setCursor(34, 151);
        gfx->setTextColor(C_GREY);
        gfx->print("Passive 802.11 monitor. Edge node.");
        gfx->setCursor(34, 163);
        gfx->print("Streams JSON via Bridge if connected.");
        gfx->setCursor(34, 175);
        gfx->print("Best for: host-driven sensor node.");

        // Footer
        gfx->fillRect(0, 240 - FOOTER_H, 320, FOOTER_H, C_DARK);
        gfx->setCursor(10, 240 - FOOTER_H + 8);
        gfx->setTextColor(C_GREEN);
        gfx->print("[TB] Select  [ENTER/CLK] Confirm  [Q] Back");
    };

    redraw();

    while (true) {
        // Touch — tap a box directly
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty >= 50 && ty <= 110) { sel = 0; redraw(); delay(120); return (pi_mode_t)sel; }
            if (ty >= 125 && ty <= 195){ sel = 1; redraw(); delay(120); return (pi_mode_t)sel; }
        }

        char k = get_keypress();
        if (k == 'q' || k == 'Q') return (pi_mode_t)-1;
        if (k == '\n' || k == '\r') return (pi_mode_t)sel;

        TrackballState tb = update_trackball();
        if ((tb.y == -1 || tb.y == 1) && sel != (sel + tb.y + 2) % 2) {
            sel = (sel + tb.y + 2) % 2;
            redraw();
        }
        if (tb.clicked) return (pi_mode_t)sel;

        delay(30);
        yield();
    }
}

// ═════════════════════════════════════════════
//  SCAN MODE
// ═════════════════════════════════════════════
static int scan_find_or_add(const char* bssid) {
    for (int i = 0; i < scan_net_count; i++) {
        if (strcmp(scan_nets[i].bssid, bssid) == 0) return i;
    }
    if (scan_net_count < MAX_SCAN_NETS) {
        strncpy(scan_nets[scan_net_count].bssid, bssid, 19);
        scan_nets[scan_net_count].seen = 0;
        return scan_net_count++;
    }
    return -1;
}

static void scan_open_log() {
    // Find next free session file
    for (int n = 1; n <= 9999; n++) {
        char path[32];
        snprintf(path, sizeof(path), "/probe_%04d.csv", n);
        bool exists = false;
        if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            exists = sd.exists(path);
            xSemaphoreGive(spi_mutex);
        }
        if (!exists) {
            strncpy(scan_log_path, path, 31);
            // Write CSV header
            if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                FsFile f = sd.open(scan_log_path, O_WRITE | O_CREAT);
                if (f) {
                    f.println("bssid,ssid,rssi,ch,enc,lat,lng");
                    f.close();
                }
                xSemaphoreGive(spi_mutex);
            }
            return;
        }
    }
    scan_log_path[0] = 0; // no log
}

static void scan_do_scan(bool& redraw_needed) {
    draw_header("SCAN MODE | SCANNING...", 0xFFE0);

    // Release WiFi for scan — do NOT hold spi_mutex during the scan itself
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) n = 0;

    double lat = gps.location.isValid() ? gps.location.lat() : 0.0;
    double lng = gps.location.isValid() ? gps.location.lng() : 0.0;

    // Now take mutex only for the SD write
    bool got_mutex = (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
    FsFile logfile;
    if (got_mutex && scan_log_path[0]) {
        logfile = sd.open(scan_log_path, O_WRITE | O_APPEND);
    }

    for (int i = 0; i < n; i++) {
        char bssid_buf[20];
        char ssid_buf[36];
        String mac  = WiFi.BSSIDstr(i);
        String ssid = WiFi.SSID(i);
        strncpy(bssid_buf, mac.c_str(),  19);
        strncpy(ssid_buf,  ssid.c_str(), 35);
        bssid_buf[19] = 0;
        ssid_buf[35]  = 0;
        // Sanitise SSID for CSV — replace commas with _
        for (char* p = ssid_buf; *p; p++) { if (*p == ',') *p = '_'; }

        int idx = scan_find_or_add(bssid_buf);
        if (idx >= 0) {
            strncpy(scan_nets[idx].ssid,    ssid_buf,  35);
            scan_nets[idx].rssi    = WiFi.RSSI(i);
            scan_nets[idx].channel = WiFi.channel(i);
            scan_nets[idx].open    = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
            scan_nets[idx].lat     = lat;
            scan_nets[idx].lng     = lng;
            scan_nets[idx].seen++;
            scan_total++;
        }

        if (logfile) {
            char line[128];
            snprintf(line, sizeof(line), "%s,%s,%d,%d,%s,%.6f,%.6f\n",
                bssid_buf, ssid_buf,
                WiFi.RSSI(i), WiFi.channel(i),
                WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "WPA",
                lat, lng);
            logfile.print(line);
        }
    }

    if (logfile) logfile.close();
    if (got_mutex) xSemaphoreGive(spi_mutex);

    WiFi.scanDelete();
    redraw_needed = true;
}

static void scan_draw_list() {
    gfx->fillRect(0, BODY_TOP, 320, BODY_BOT - BODY_TOP, C_BLACK);
    int shown = 0;
    for (int i = scan_scroll; i < scan_net_count && shown < MAX_ROWS; i++, shown++) {
        int y   = BODY_TOP + shown * ROW_H;
        bool sel = (i == scan_cursor);
        if (sel) gfx->fillRect(0, y, 320, ROW_H, C_DARK);

        // RSSI bar — 3px wide strip flush left
        int bar = max(0, min(30, (scan_nets[i].rssi + 100) * 30 / 60));
        gfx->fillRect(0, y + 4, bar, 7, rssi_color(scan_nets[i].rssi));

        gfx->setCursor(35, y + 3);
        gfx->setTextColor(sel ? C_CYAN : (scan_nets[i].open ? 0xFFE0 : C_WHITE));
        char ssid_short[20];
        strncpy(ssid_short, scan_nets[i].ssid[0] ? scan_nets[i].ssid : "(hidden)", 18);
        ssid_short[18] = 0;
        gfx->print(ssid_short);

        gfx->setCursor(245, y + 3);
        gfx->setTextColor(C_GREY);
        gfx->printf("ch%d", scan_nets[i].channel);

        gfx->setCursor(290, y + 3);
        gfx->setTextColor(rssi_color(scan_nets[i].rssi));
        gfx->printf("%d", scan_nets[i].rssi);
    }
    if (scan_net_count == 0) {
        gfx->setTextColor(C_GREY);
        gfx->setCursor(10, BODY_TOP + 20);
        gfx->print("No networks yet. Press [S] to scan.");
    }
}

static void run_scan_mode() {
    scan_net_count = 0;
    scan_cursor    = 0;
    scan_scroll    = 0;
    scan_total     = 0;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    scan_open_log();

    gfx->fillScreen(C_BLACK);
    draw_header("SCAN MODE | TAP HEADER TO EXIT", C_GREEN);
    char footer_buf[64];
    snprintf(footer_buf, sizeof(footer_buf), "[S] Scan  [M] Mode  [Q] Exit | Log: %s",
             scan_log_path[0] ? scan_log_path : "none");
    draw_footer(footer_buf);
    scan_draw_list();

    bool redraw_needed = false;
    uint32_t last_status_ms = 0;

    while (true) {
        // Header tap → exit
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < HEADER_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            return;
        }

        char k = get_keypress();
        if (k == 'q' || k == 'Q') return;
        if (k == 'm' || k == 'M') {
            // Switch mode — caller sees the return and re-enters mode select
            // We signal this by returning normally and letting run_probe_intel re-run mode select
            return;
        }
        if (k == 's' || k == 'S') {
            scan_do_scan(redraw_needed);
        }

        TrackballState tb = update_trackball();
        if (tb.y == -1 && scan_cursor > 0) {
            scan_cursor--;
            if (scan_cursor < scan_scroll) scan_scroll = scan_cursor;
            redraw_needed = true;
        }
        if (tb.y ==  1 && scan_cursor < scan_net_count - 1) {
            scan_cursor++;
            if (scan_cursor >= scan_scroll + MAX_ROWS) scan_scroll = scan_cursor - MAX_ROWS + 1;
            redraw_needed = true;
        }

        if (redraw_needed) {
            draw_header("SCAN MODE | TAP HEADER TO EXIT", C_GREEN);
            scan_draw_list();
            redraw_needed = false;
        }

        // Status bar refresh every 2s
        if (millis() - last_status_ms > 2000) {
            last_status_ms = millis();
            char status[80];
            snprintf(status, sizeof(status),
                "[S] Scan  [M] Mode | %d nets | %s | %s",
                scan_net_count,
                scan_log_path[0] ? scan_log_path : "no log",
                gps.location.isValid() ? "GPS OK" : "NO GPS");
            draw_footer(status);
        }

        delay(30);
        yield();
    }
}

// ═════════════════════════════════════════════
//  PROMISCUOUS MODE
// ═════════════════════════════════════════════

// Frame type display positions (y coords in body area)
#define PROW_BEACON   (BODY_TOP + 0  * 26)
#define PROW_PROBE    (BODY_TOP + 1  * 26)
#define PROW_DEAUTH   (BODY_TOP + 2  * 26)
#define PROW_AUTH     (BODY_TOP + 3  * 26)
#define PROW_OTHER    (BODY_TOP + 4  * 26)
#define PROW_DROPPED  (BODY_TOP + 5  * 26)

static void promisc_draw_static() {
    gfx->fillRect(0, BODY_TOP, 320, BODY_BOT - BODY_TOP, C_BLACK);

    const char* labels[] = { "Beacon:", "Probe-Req:", "Deauth:", "Auth:", "Other:", "Dropped:" };
    const uint16_t cols[] = { C_GREEN, C_CYAN, C_RED, 0xFFE0, C_GREY, C_GREY };
    const int ys[] = { PROW_BEACON, PROW_PROBE, PROW_DEAUTH, PROW_AUTH, PROW_OTHER, PROW_DROPPED };
    for (int i = 0; i < 6; i++) {
        gfx->setCursor(10, ys[i] + 4);
        gfx->setTextColor(cols[i]);
        gfx->setTextSize(1);
        gfx->print(labels[i]);
    }
}

static void promisc_draw_values(const pm_stats_t& s, uint8_t ch) {
    const int ys[] = { PROW_BEACON, PROW_PROBE, PROW_DEAUTH, PROW_AUTH, PROW_OTHER, PROW_DROPPED };
    const uint32_t vals[] = {
        s.beacon_count, s.probe_req_count,
        s.deauth_count, 0 /* auth not in pm_stats yet */,
        s.other_count,  s.dropped
    };
    const uint16_t cols[] = { C_GREEN, C_CYAN, C_RED, 0xFFE0, C_GREY, C_GREY };
    for (int i = 0; i < 6; i++) {
        gfx->fillRect(110, ys[i], 100, 20, C_BLACK);
        gfx->setCursor(110, ys[i] + 4);
        gfx->setTextColor(cols[i]);
        gfx->setTextSize(1);
        gfx->printf("%lu", vals[i]);
    }

    // Channel + fps
    gfx->fillRect(0, BODY_BOT - 18, 320, 18, C_BLACK);
    gfx->setCursor(10, BODY_BOT - 14);
    gfx->setTextColor(C_GREY);
    gfx->setTextSize(1);
    gfx->printf("Ch: %2d  |  FPS: %lu  |  Total: %lu",
                 ch, s.frames_per_sec, s.captured);

    // Deauth warning
    if (s.deauth_count > 0) {
        gfx->fillRect(230, PROW_DEAUTH, 85, 20, C_BLACK);
        gfx->setCursor(232, PROW_DEAUTH + 4);
        gfx->setTextColor(C_RED);
        gfx->print("<< DEAUTH!");
    }
}

static void run_promiscuous_mode() {
    // Transition wardrive to promiscuous mode
    // This tells the wardrive Ghost Engine to use pm_promiscuous rather than
    // WiFi.scanNetworks. If wardrive is active, it picks up the new mode on
    // its next cycle. We set wardrive_active here so the Ghost Engine runs.
    wardrive_set_mode(WARDRIVE_MODE_PROMISCUOUS);
    wardrive_active = true;
    // Note: we do NOT set wardrive_bridge_streaming here — that's the Bridge's
    // job. If Bridge is running, it already set this flag. If Bridge is not
    // running, pkt events won't be emitted over Serial — which is correct.

    gfx->fillScreen(C_BLACK);

    // Detect whether Bridge is also streaming
    bool bridge_live = wardrive_bridge_streaming;
    char title[48];
    snprintf(title, sizeof(title), "PROMISC%s | TAP TO EXIT",
             bridge_live ? " [EDGE]" : " [LOCAL]");
    draw_header(title, C_RED);
    promisc_draw_static();

    char footer_buf[80];
    if (bridge_live) {
        snprintf(footer_buf, sizeof(footer_buf),
                 "[M] Mode  [Q] Exit | Streaming to host");
    } else {
        snprintf(footer_buf, sizeof(footer_buf),
                 "[M] Mode  [Q] Exit | Local display only");
    }
    draw_footer(footer_buf, bridge_live ? C_RED : C_GREY);

    uint32_t last_redraw = 0;
    pm_stats_t stats;

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < HEADER_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            break;
        }

        char k = get_keypress();
        if (k == 'q' || k == 'Q') break;
        if (k == 'm' || k == 'M') break;  // back to mode select

        // Refresh stats every 500ms
        if (millis() - last_redraw >= 500) {
            pm_promiscuous_get_stats(&stats);
            uint8_t ch = pm_promiscuous_channel();
            promisc_draw_values(stats, ch);

            // Re-evaluate bridge state — user may have connected/disconnected
            bool now_live = wardrive_bridge_streaming;
            if (now_live != bridge_live) {
                bridge_live = now_live;
                snprintf(title, sizeof(title), "PROMISC%s | TAP TO EXIT",
                         bridge_live ? " [EDGE]" : " [LOCAL]");
                draw_header(title, C_RED);
                if (bridge_live) {
                    snprintf(footer_buf, sizeof(footer_buf),
                             "[M] Mode  [Q] Exit | Streaming to host");
                    draw_footer(footer_buf, C_RED);
                } else {
                    snprintf(footer_buf, sizeof(footer_buf),
                             "[M] Mode  [Q] Exit | Local display only");
                    draw_footer(footer_buf, C_GREY);
                }
            }

            last_redraw = millis();
        }

        delay(50);
        yield();
    }

    // Return wardrive to idle scan mode when leaving promiscuous view.
    // Do NOT call wardrive_active = false — the Ghost Engine may be running
    // independently. The mode switch is enough.
    wardrive_set_mode(WARDRIVE_MODE_SCAN);
}

// ═════════════════════════════════════════════
//  ENTRY POINT
// ═════════════════════════════════════════════
void run_probe_intel() {
    pi_mode_t mode = MODE_SCAN;

    while (true) {
        pi_mode_t selected = draw_mode_select(mode);

        // User pressed Q on mode select — exit the whole app
        if ((int)selected == -1) return;

        mode = selected;

        if (mode == MODE_SCAN) {
            run_scan_mode();
            // run_scan_mode returns when user presses Q, M, or taps header.
            // If M was pressed we loop back to mode select.
            // If Q was pressed we also exit — but since we can't distinguish,
            // we just loop to mode select (user presses Q again to truly exit).
            // A cleaner way: run_scan_mode returns an enum. See v1.2 todo.
        } else {
            run_promiscuous_mode();
        }
        // Loop — go back to mode select so user can switch modes mid-session
    }
}
