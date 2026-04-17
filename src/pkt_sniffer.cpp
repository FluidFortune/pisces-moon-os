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
 * PISCES MOON OS — PACKET SNIFFER v1.0
 * 802.11 promiscuous mode frame capture and display
 *
 * Educational tool for understanding WiFi frame structure.
 * USE ONLY ON NETWORKS YOU OWN OR HAVE EXPLICIT PERMISSION TO TEST.
 *
 * Captures and displays:
 *   Management frames: Beacons, Probe Req/Resp, Auth, Assoc, Deauth, Disassoc
 *   Control frames:    RTS, CTS, ACK
 *   Data frames:       Data, Null, QoS
 *
 * Display: Live scrolling frame log (last 8 frames), packet counters,
 *          channel selector, frame type distribution bar
 *
 * Controls:
 *   Trackball left/right = change channel (1-13)
 *   Trackball click      = pause/resume capture
 *   Q / header tap       = exit
 *
 * NOTE: Promiscuous mode on ESP32-S3 can only see 802.11 frames on the
 * currently tuned channel. Rotate channels to see all traffic.
 * The ESP32 will see frames even when not associated to a network.
 */

#include <Arduino.h>
#include <FS.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "pkt_sniffer.h"

extern Arduino_GFX *gfx;
extern volatile bool wifi_in_use;
extern SdFat sd;

// pkt_log_open/close/flush defined after FrameEntry, logMux etc.
// — see just before run_pkt_sniffer()

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG      0x0000
#define COL_HEADER  0x0008   // Very dark blue — hacker aesthetic
#define COL_MGMT    0x07E0   // Green — management frames
#define COL_CTRL    0xFFE0   // Yellow — control frames
#define COL_DATA    0x07FF   // Cyan — data frames
#define COL_DEAUTH  0xF800   // Red — deauth/disassoc (notable)
#define COL_DIM     0x4208
#define COL_TEXT    0xFFFF
#define COL_ACCENT  0xFD20

// ─────────────────────────────────────────────
//  FRAME LOG
// ─────────────────────────────────────────────
#define LOG_LINES 8
#define LOG_MAX   64

struct FrameEntry {
    char     type[12];    // Frame type string
    char     bssid[18];   // Source MAC
    int8_t   rssi;
    uint8_t  channel;
    uint16_t color;
};

static FrameEntry frameLog[LOG_MAX];
static volatile int logHead = 0;
static volatile int logCount = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

// Counters
static volatile uint32_t totalFrames = 0;
static volatile uint32_t mgmtCount = 0;
static volatile uint32_t ctrlCount = 0;
static volatile uint32_t dataCount = 0;
static volatile uint32_t deauthCount = 0;

static volatile bool capturing = true;
static volatile int  currentChannel = 1;

// ─────────────────────────────────────────────
//  802.11 FRAME HEADER STRUCTURES
// ─────────────────────────────────────────────
typedef struct {
    uint8_t  revision: 2;
    uint8_t  type:     2;   // 0=Mgmt, 1=Ctrl, 2=Data
    uint8_t  subtype:  4;
    uint8_t  to_ds:    1;
    uint8_t  from_ds:  1;
    uint8_t  more_frag:1;
    uint8_t  retry:    1;
    uint8_t  pwr_mgmt: 1;
    uint8_t  more_data:1;
    uint8_t  wep:      1;
    uint8_t  order:    1;
} __attribute__((packed)) frame_ctrl_t;

typedef struct {
    frame_ctrl_t  fc;
    uint16_t      duration;
    uint8_t       addr1[6]; // DA
    uint8_t       addr2[6]; // SA
    uint8_t       addr3[6]; // BSSID
    uint16_t      seq_ctrl;
} __attribute__((packed)) wifi_ieee80211_mac_hdr_t;

// Management frame subtypes
static const char* MGMT_SUBTYPES[] = {
    "ASSOC REQ", "ASSOC RSP", "REASSOC REQ", "REASSOC RSP",
    "PROBE REQ", "PROBE RSP", "TIMING ADV", "RESERVED",
    "BEACON",    "ATIM",      "DISASSOC",   "AUTH",
    "DEAUTH",    "ACTION",    "ACTION NACK","RESERVED"
};
// Control frame subtypes (starting at 7)
static const char* CTRL_SUBTYPES[] = {
    "?","?","?","?","?","?","?","WRAPPER",
    "BLOCK ACK", "BLOCK ACK", "PS-POLL","RTS","CTS","ACK","CF-END","CF+ACK"
};
// Data frame subtypes
static const char* DATA_SUBTYPES[] = {
    "DATA","DATA+CF","DATA+POLL","DATA+CF+POLL",
    "NULL","CF-ACK","CF-POLL","CF+ACK+POLL",
    "QOS DATA","QOS+CF","QOS+POLL","QOS+ALL",
    "QOS NULL","RESERVED","QOS CF-POLL","QOS CF+ACK"
};

// ─────────────────────────────────────────────
//  PROMISCUOUS CALLBACK
// ─────────────────────────────────────────────
static void IRAM_ATTR snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!capturing) return;

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    wifi_pkt_rx_ctrl_t* rx = &pkt->rx_ctrl;

    if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;

    wifi_ieee80211_mac_hdr_t* hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;

    uint8_t ftype   = hdr->fc.type;
    uint8_t fsubtype = hdr->fc.subtype;

    FrameEntry entry;
    entry.rssi    = rx->rssi;
    entry.channel = rx->channel;
    snprintf(entry.bssid, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             hdr->addr2[0], hdr->addr2[1], hdr->addr2[2],
             hdr->addr2[3], hdr->addr2[4], hdr->addr2[5]);

    totalFrames++;

    if (ftype == 0) { // Management
        mgmtCount++;
        strncpy(entry.type, MGMT_SUBTYPES[fsubtype & 0xF], 11);
        entry.color = (fsubtype == 12 || fsubtype == 10) ? COL_DEAUTH : COL_MGMT;
        if (fsubtype == 12) deauthCount++; // Deauth
        if (fsubtype == 10) deauthCount++; // Disassoc
    } else if (ftype == 1) { // Control
        ctrlCount++;
        strncpy(entry.type, CTRL_SUBTYPES[fsubtype & 0xF], 11);
        entry.color = COL_CTRL;
    } else if (ftype == 2) { // Data
        dataCount++;
        strncpy(entry.type, DATA_SUBTYPES[fsubtype & 0xF], 11);
        entry.color = COL_DATA;
    } else {
        strncpy(entry.type, "RESERVED", 11);
        entry.color = COL_DIM;
    }
    entry.type[11] = '\0';

    portENTER_CRITICAL_ISR(&logMux);
    frameLog[logHead] = entry;
    logHead = (logHead + 1) % LOG_MAX;
    if (logCount < LOG_MAX) logCount++;
    portEXIT_CRITICAL_ISR(&logMux);
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawHeader() {
    gfx->fillRect(0, 0, 320, 24, COL_HEADER);
    gfx->drawFastHLine(0, 23, 320, COL_MGMT);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_MGMT);
    gfx->setCursor(8, 4);
    gfx->print("PKT SNIFFER");
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, 13);
    gfx->printf("CH:%02d  %s", currentChannel, capturing ? "LIVE" : "PAUSED");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(240, 8);
    gfx->print("[TAP=EXIT]");
}

static void drawCounters() {
    gfx->fillRect(0, 26, 320, 16, 0x0821);
    gfx->setTextSize(1);
    gfx->setCursor(4, 30);
    gfx->setTextColor(COL_TEXT);
    gfx->printf("TOT:%5lu ", totalFrames);
    gfx->setTextColor(COL_MGMT);
    gfx->printf("M:%4lu ", mgmtCount);
    gfx->setTextColor(COL_CTRL);
    gfx->printf("C:%3lu ", ctrlCount);
    gfx->setTextColor(COL_DATA);
    gfx->printf("D:%4lu ", dataCount);
    gfx->setTextColor(COL_DEAUTH);
    gfx->printf("DA:%3lu", deauthCount);
}

static void drawTypeBar() {
    // Distribution bar at bottom
    int by = 224;
    gfx->fillRect(0, by, 320, 16, 0x0821);
    if (totalFrames == 0) return;
    int mw = (mgmtCount * 300) / totalFrames;
    int cw = (ctrlCount * 300) / totalFrames;
    int dw = (dataCount * 300) / totalFrames;
    int x = 10;
    gfx->fillRect(x, by+3, mw, 10, COL_MGMT); x += mw;
    gfx->fillRect(x, by+3, cw, 10, COL_CTRL); x += cw;
    gfx->fillRect(x, by+3, dw, 10, COL_DATA);
    gfx->setTextColor(COL_DIM); gfx->setTextSize(1);
    gfx->setCursor(4, by+4); gfx->print("M");
    gfx->setCursor(315, by+4); gfx->print("D");
}

static void drawFrameLog() {
    int startY = 44;
    int lineH  = 22;
    gfx->fillRect(0, startY, 320, LOG_LINES * lineH, COL_BG);

    portENTER_CRITICAL(&logMux);
    int count = min((int)logCount, (int)LOG_LINES);
    // Most recent first
    for (int i = 0; i < count; i++) {
        int idx = ((logHead - 1 - i) + LOG_MAX) % LOG_MAX;
        FrameEntry& e = frameLog[idx];
        int y = startY + i * lineH;

        // Row background — alternating
        gfx->fillRect(0, y, 320, lineH - 1, (i % 2 == 0) ? 0x0821 : COL_BG);

        // Frame type
        gfx->setTextColor(e.color);
        gfx->setTextSize(1);
        gfx->setCursor(4, y + 2);
        gfx->printf("%-11s", e.type);

        // RSSI bar
        int barLen = max(0, min(60, (e.rssi + 100) * 2)); // -100dBm=0px, -40dBm=120px → cap at 60
        uint16_t barCol = (e.rssi > -60) ? COL_MGMT : (e.rssi > -80) ? COL_ACCENT : COL_DEAUTH;
        gfx->fillRect(78, y + 4, barLen, 6, barCol);
        gfx->drawRect(78, y + 4, 60, 6, COL_DIM);

        // RSSI number
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(142, y + 2);
        gfx->printf("%4ddBm", e.rssi);

        // Channel
        gfx->setTextColor(COL_CTRL);
        gfx->setCursor(192, y + 2);
        gfx->printf("CH%02d", e.channel);

        // Source MAC (truncated)
        gfx->setTextColor(0x4A69);
        gfx->setCursor(228, y + 2);
        gfx->printf("%.8s", e.bssid); // First 8 chars = XX:XX:XX
    }
    portEXIT_CRITICAL(&logMux);
}

static void drawChannelHint() {
    gfx->fillRect(0, 215, 320, 8, COL_BG);
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(4, 216);
    gfx->print("< CH  CLK=PAUSE  CH >");
}

// ─────────────────────────────────────────────
//  SESSION LOG
//  Defined here — after FrameEntry, LOG_MAX,
//  logMux, logHead, logCount, frameLog are all
//  declared so the compiler can resolve them.
// ─────────────────────────────────────────────
#define PKT_LOG_DIR  "/cyber_logs"
static FsFile    _pktLogFile;
static bool      _pktLogOpen   = false;
static uint32_t  _sessionStart = 0;

static void pkt_log_open() {
    if (!sd.exists(PKT_LOG_DIR)) sd.mkdir(PKT_LOG_DIR);
    _sessionStart = millis();
    char fname[48];
    snprintf(fname, sizeof(fname), "%s/pkt_%010lu.csv", PKT_LOG_DIR, _sessionStart);
    _pktLogFile = sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (_pktLogFile) {
        _pktLogFile.println("uptime_ms,type,src_mac,rssi_dbm,channel");
        _pktLogOpen = true;
        Serial.printf("[PKT] Log opened: %s\n", fname);
    } else {
        Serial.println("[PKT] Log open FAILED");
        _pktLogOpen = false;
    }
}

static void pkt_log_close() {
    if (_pktLogOpen) {
        _pktLogFile.flush();
        _pktLogFile.close();
        _pktLogOpen = false;
        Serial.println("[PKT] Log closed.");
    }
}

static void pkt_log_flush() {
    if (!_pktLogOpen) return;
    portENTER_CRITICAL(&logMux);
    int count = logCount;
    int head  = logHead;
    FrameEntry snapshot[LOG_MAX];
    memcpy(snapshot, frameLog, sizeof(frameLog));
    logCount = 0;
    logHead  = 0;
    portEXIT_CRITICAL(&logMux);
    uint32_t now = millis();
    for (int i = 0; i < count; i++) {
        int idx = (head - count + i + LOG_MAX) % LOG_MAX;
        FrameEntry& e = snapshot[idx];
        _pktLogFile.printf("%lu,%s,%s,%d,%d\n",
            now, e.type, e.bssid, (int)e.rssi, (int)e.channel);
    }
    _pktLogFile.flush();
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_pkt_sniffer() {
    // Warn user
    gfx->fillScreen(COL_BG);
    gfx->setTextColor(COL_ACCENT);
    gfx->setTextSize(2);
    gfx->setCursor(20, 60);  gfx->print("PKT SNIFFER");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(10, 90);  gfx->print("Passive 802.11 frame capture.");
    gfx->setCursor(10, 104); gfx->print("Use on YOUR network only.");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(10, 120); gfx->print("Starting on channel 1...");
    delay(2000);

    // Initialize
    logHead = logCount = 0;
    totalFrames = mgmtCount = ctrlCount = dataCount = deauthCount = 0;
    capturing = true;
    currentChannel = 1;

    // Open session log on SD
    pkt_log_open();

    // Put WiFi into promiscuous mode
    wifi_in_use = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    gfx->fillScreen(COL_BG);
    drawHeader();
    drawCounters();
    drawFrameLog();
    drawChannelHint();
    drawTypeBar();

    unsigned long lastRedraw = millis();
    unsigned long lastCountRedraw = millis();

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        // Exit
        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        if (k == 'q' || k == 'Q') break;

        // Channel change
        bool chChanged = false;
        if (tb.x == -1 && currentChannel > 1)  { currentChannel--; chChanged = true; }
        if (tb.x ==  1 && currentChannel < 13)  { currentChannel++; chChanged = true; }
        if (chChanged) {
            esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
            drawHeader();
        }

        // Pause/resume
        if (tb.clicked || k == ' ') {
            capturing = !capturing;
            drawHeader();
        }

        // Redraw frame log every 250ms
        if (millis() - lastRedraw > 250) {
            drawFrameLog();
            pkt_log_flush();   // Write buffered frames to SD
            lastRedraw = millis();
        }
        // Redraw counters every 500ms
        if (millis() - lastCountRedraw > 500) {
            drawCounters();
            drawTypeBar();
            lastCountRedraw = millis();
        }

        delay(20);
        yield();
    }

    // Restore WiFi
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    wifi_in_use = false;
    pkt_log_flush();   // Final flush of any remaining frames
    pkt_log_close();   // Close and sync the log file
    gfx->fillScreen(COL_BG);
}