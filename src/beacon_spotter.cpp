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
 * PISCES MOON OS — BEACON SPOTTER v1.0
 * Passive 802.11 management frame analyzer
 *
 * Specifically monitors and logs:
 *   BEACON frames     — AP advertisements (SSID, channel, rates, security)
 *   PROBE REQ frames  — Devices searching for known networks (device fingerprinting)
 *   PROBE RSP frames  — APs responding to probes
 *   DEAUTH frames     — Disassociation attacks / normal disconnect
 *   DISASSOC frames   — Similar to deauth
 *
 * Educational value:
 *   - See which devices are probing for which SSIDs (reveals past network history)
 *   - Detect deauth floods (possible attack or interference)
 *   - Identify hidden SSIDs when they respond to probes
 *   - Understand beacon interval and AP behavior
 *
 * USE ONLY FOR PASSIVE OBSERVATION ON NETWORKS/ENVIRONMENTS YOU OWN.
 * This tool does NOT transmit any frames.
 *
 * Controls:
 *   Trackball up/down    = scroll AP list
 *   Trackball left/right = change channel
 *   Trackball click      = toggle view (AP list / Probe log / Deauth alert)
 *   Q / header tap       = exit
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <FS.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "beacon_spotter.h"

extern Arduino_GFX *gfx;
extern volatile bool wifi_in_use;
extern SdFat sd;

// beacon_log_write() is defined after the data structures it references
// — see below, just before run_beacon_spotter()

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG      0x0000
#define COL_HDR     0x0010   // Deep blue
#define COL_BEACON  0x07E0   // Green
#define COL_PROBE   0xFFE0   // Yellow
#define COL_DEAUTH  0xF800   // Red
#define COL_OPEN    0x07FF   // Cyan — open network
#define COL_ENC     0xFD20   // Orange — encrypted
#define COL_DIM     0x4208
#define COL_TEXT    0xFFFF

// ─────────────────────────────────────────────
//  AP TABLE
// ─────────────────────────────────────────────
#define MAX_APS  32
#define MAX_PROBES 32

struct APEntry {
    char    ssid[33];
    uint8_t bssid[6];
    int8_t  rssi;
    uint8_t channel;
    bool    encrypted;
    bool    hidden;
    uint32_t beaconCount;
    uint32_t lastSeen;
};

struct ProbeEntry {
    uint8_t  srcMac[6];
    char     ssid[33];      // empty = wildcard probe
    uint32_t lastSeen;
};

struct DeauthEntry {
    uint8_t  srcMac[6];
    uint8_t  dstMac[6];
    uint32_t count;
    uint32_t lastSeen;
};

static APEntry    apTable[MAX_APS];
static int        apCount = 0;
static ProbeEntry probeLog[MAX_PROBES];
static int        probeCount = 0;
static DeauthEntry deauthLog[16];
static int        deauthCount = 0;

static volatile int  currentChannel = 1;
static portMUX_TYPE  bsMux = portMUX_INITIALIZER_UNLOCKED;

// ─────────────────────────────────────────────
//  FRAME STRUCTURES
// ─────────────────────────────────────────────
typedef struct {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} __attribute__((packed)) mac_hdr_t;

typedef struct {
    uint64_t timestamp;
    uint16_t beacon_interval;
    uint16_t capabilities;
} __attribute__((packed)) beacon_fixed_t;

// Parse SSID from beacon/probe body
static void parseSSID(const uint8_t* body, int bodyLen, char* out, int outLen) {
    out[0] = '\0';
    if (bodyLen < 2) return;
    // Walk tagged parameters
    int offset = 0;
    while (offset + 2 <= bodyLen) {
        uint8_t tag    = body[offset];
        uint8_t tagLen = body[offset + 1];
        if (offset + 2 + tagLen > bodyLen) break;
        if (tag == 0) { // SSID element
            int copyLen = min((int)tagLen, outLen - 1);
            memcpy(out, body + offset + 2, copyLen);
            out[copyLen] = '\0';
            return;
        }
        offset += 2 + tagLen;
    }
}

// Check if capabilities indicate encryption
static bool parseSecurity(const uint8_t* body, int bodyLen) {
    if (bodyLen < sizeof(beacon_fixed_t)) return false;
    beacon_fixed_t* fixed = (beacon_fixed_t*)body;
    return (fixed->capabilities & 0x0010) != 0; // Privacy bit
}

// ─────────────────────────────────────────────
//  PROMISCUOUS CALLBACK
// ─────────────────────────────────────────────
static void IRAM_ATTR beaconCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    int pktLen = pkt->rx_ctrl.sig_len;
    if (pktLen < (int)sizeof(mac_hdr_t)) return;

    mac_hdr_t* hdr = (mac_hdr_t*)pkt->payload;
    uint8_t subtype = (hdr->frame_ctrl >> 4) & 0x0F;
    uint8_t ftype   = (hdr->frame_ctrl >> 2) & 0x03;
    if (ftype != 0) return; // Management only

    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t ch  = pkt->rx_ctrl.channel;
    uint32_t now = (uint32_t)(millis() / 1000);

    const uint8_t* body = pkt->payload + sizeof(mac_hdr_t);
    int bodyLen = pktLen - sizeof(mac_hdr_t);

    portENTER_CRITICAL_ISR(&bsMux);

    if (subtype == 8) { // Beacon
        char ssid[33]; ssid[0] = '\0';
        parseSSID(body, bodyLen, ssid, 33);
        bool enc = parseSecurity(body, bodyLen);
        bool hidden = (ssid[0] == '\0');

        // Update or add AP
        bool found = false;
        for (int i = 0; i < apCount; i++) {
            if (memcmp(apTable[i].bssid, hdr->addr2, 6) == 0) {
                apTable[i].rssi = rssi;
                apTable[i].lastSeen = now;
                apTable[i].beaconCount++;
                if (!hidden && apTable[i].hidden) {
                    // SSID revealed by beacon
                    strncpy(apTable[i].ssid, ssid, 32);
                    apTable[i].hidden = false;
                }
                found = true; break;
            }
        }
        if (!found && apCount < MAX_APS) {
            APEntry& ap = apTable[apCount++];
            strncpy(ap.ssid, ssid, 32); ap.ssid[32] = '\0';
            memcpy(ap.bssid, hdr->addr2, 6);
            ap.rssi = rssi; ap.channel = ch;
            ap.encrypted = enc; ap.hidden = hidden;
            ap.beaconCount = 1; ap.lastSeen = now;
        }
    }
    else if (subtype == 4) { // Probe Request
        char ssid[33]; ssid[0] = '\0';
        parseSSID(body, bodyLen, ssid, 33);
        // Log unique probes
        bool found = false;
        for (int i = 0; i < probeCount; i++) {
            if (memcmp(probeLog[i].srcMac, hdr->addr2, 6) == 0 &&
                strcmp(probeLog[i].ssid, ssid) == 0) {
                probeLog[i].lastSeen = now;
                found = true; break;
            }
        }
        if (!found && probeCount < MAX_PROBES) {
            ProbeEntry& p = probeLog[probeCount++];
            memcpy(p.srcMac, hdr->addr2, 6);
            strncpy(p.ssid, ssid, 32); p.ssid[32] = '\0';
            p.lastSeen = now;
        }
    }
    else if (subtype == 12 || subtype == 10) { // Deauth or Disassoc
        bool found = false;
        for (int i = 0; i < deauthCount; i++) {
            if (memcmp(deauthLog[i].srcMac, hdr->addr2, 6) == 0) {
                deauthLog[i].count++;
                deauthLog[i].lastSeen = now;
                found = true; break;
            }
        }
        if (!found && deauthCount < 16) {
            DeauthEntry& d = deauthLog[deauthCount++];
            memcpy(d.srcMac, hdr->addr2, 6);
            memcpy(d.dstMac, hdr->addr1, 6);
            d.count = 1; d.lastSeen = now;
        }
    }

    portEXIT_CRITICAL_ISR(&bsMux);
}

// ─────────────────────────────────────────────
//  VIEWS
// ─────────────────────────────────────────────
#define VIEW_APS     0
#define VIEW_PROBES  1
#define VIEW_DEAUTH  2

static void drawMainHeader(int view, int channel) {
    gfx->fillRect(0, 0, 320, 24, COL_HDR);
    gfx->drawFastHLine(0, 23, 320, COL_BEACON);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_BEACON);
    gfx->setCursor(8, 4);
    gfx->print("BEACON SPOTTER");
    gfx->setTextColor(COL_PROBE);
    gfx->setCursor(8, 13);
    gfx->printf("CH:%02d  CLK=VIEW  </> CH", channel);
    // View tab indicator
    const char* tabs[] = {"[APS]","[PROBE]","[DEAUTH]"};
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(200, 4);
    for (int i = 0; i < 3; i++) {
        if (i == view) gfx->setTextColor(COL_PROBE);
        else           gfx->setTextColor(COL_DIM);
        gfx->print(tabs[i]); gfx->print(" ");
    }
}

static void drawAPView(int scroll) {
    gfx->fillRect(0, 26, 320, 198, COL_BG);
    // Column headers
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(4, 28);
    gfx->print("SSID              CH  RSI  ENC BCN");
    gfx->drawFastHLine(0, 37, 320, 0x2104);

    portENTER_CRITICAL(&bsMux);
    int displayCount = min(apCount, 8);
    for (int i = 0; i < displayCount; i++) {
        int idx = (scroll + i) % max(1, apCount);
        if (idx >= apCount) break;
        APEntry& ap = apTable[idx];
        int y = 40 + i * 22;

        gfx->fillRect(0, y, 320, 21, (i%2==0)?0x0821:COL_BG);

        // SSID or <hidden>
        gfx->setTextColor(ap.hidden ? COL_DIM : COL_TEXT);
        gfx->setCursor(4, y + 2);
        char ssidBuf[19]; strncpy(ssidBuf, ap.hidden ? "<hidden>" : ap.ssid, 18); ssidBuf[18] = '\0';
        gfx->print(ssidBuf);

        // Channel
        gfx->setTextColor(COL_PROBE);
        gfx->setCursor(186, y + 2);
        gfx->printf("%02d", ap.channel);

        // RSSI bar
        int barW = max(0, min(30, (ap.rssi + 100)));
        uint16_t barC = ap.rssi > -60 ? COL_BEACON : ap.rssi > -80 ? COL_PROBE : COL_DEAUTH;
        gfx->fillRect(202, y+5, barW, 6, barC);
        gfx->drawRect(202, y+5, 30, 6, COL_DIM);

        // Encryption
        gfx->setTextColor(ap.encrypted ? COL_ENC : COL_OPEN);
        gfx->setCursor(238, y + 2);
        gfx->print(ap.encrypted ? "WPA" : "OPN");

        // Beacon count
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(272, y + 2);
        gfx->printf("%5lu", ap.beaconCount);
    }
    portEXIT_CRITICAL(&bsMux);

    gfx->setTextColor(COL_DIM);
    gfx->setCursor(4, 228);
    gfx->printf("APs seen: %d", apCount);
}

static void drawProbeView(int scroll) {
    gfx->fillRect(0, 26, 320, 198, COL_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(4, 28);
    gfx->print("SOURCE MAC        PROBING FOR SSID");
    gfx->drawFastHLine(0, 37, 320, 0x2104);

    portENTER_CRITICAL(&bsMux);
    int displayCount = min(probeCount, 9);
    for (int i = 0; i < displayCount; i++) {
        int idx = (scroll + i) % max(1, probeCount);
        if (idx >= probeCount) break;
        ProbeEntry& p = probeLog[idx];
        int y = 40 + i * 21;
        gfx->fillRect(0, y, 320, 20, (i%2==0)?0x0821:COL_BG);

        // MAC
        gfx->setTextColor(COL_PROBE);
        gfx->setCursor(4, y+3);
        gfx->printf("%02X:%02X:%02X:%02X:%02X:%02X",
            p.srcMac[0],p.srcMac[1],p.srcMac[2],
            p.srcMac[3],p.srcMac[4],p.srcMac[5]);

        // SSID
        gfx->setTextColor(p.ssid[0] ? COL_TEXT : COL_DIM);
        gfx->setCursor(120, y+3);
        gfx->print(p.ssid[0] ? p.ssid : "<wildcard>");
    }
    portEXIT_CRITICAL(&bsMux);

    gfx->setTextColor(COL_DIM);
    gfx->setCursor(4, 228);
    gfx->printf("Unique probes: %d", probeCount);
}

static void drawDeauthView() {
    gfx->fillRect(0, 26, 320, 198, COL_BG);
    gfx->setTextSize(1);

    if (deauthCount == 0) {
        gfx->setTextColor(COL_BEACON);
        gfx->setCursor(40, 120);
        gfx->print("No DEAUTH/DISASSOC frames seen.");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(40, 136);
        gfx->print("(Good sign — no attack detected)");
        return;
    }

    gfx->setTextColor(COL_DEAUTH);
    gfx->setCursor(4, 28);
    gfx->print("! DEAUTH/DISASSOC SOURCES DETECTED !");
    gfx->drawFastHLine(0, 37, 320, COL_DEAUTH);

    portENTER_CRITICAL(&bsMux);
    for (int i = 0; i < min(deauthCount, 8); i++) {
        DeauthEntry& d = deauthLog[i];
        int y = 42 + i * 22;
        gfx->fillRect(0, y, 320, 21, (i%2==0)?0x1800:COL_BG);
        gfx->setTextColor(COL_DEAUTH);
        gfx->setCursor(4, y+3);
        gfx->printf("SRC: %02X:%02X:%02X:%02X:%02X:%02X",
            d.srcMac[0],d.srcMac[1],d.srcMac[2],
            d.srcMac[3],d.srcMac[4],d.srcMac[5]);
        gfx->setTextColor(COL_PROBE);
        gfx->setCursor(4, y+12);
        gfx->printf("  -> %02X:%02X:%02X:%02X:%02X:%02X  x%lu",
            d.dstMac[0],d.dstMac[1],d.dstMac[2],
            d.dstMac[3],d.dstMac[4],d.dstMac[5],
            d.count);
    }
    portEXIT_CRITICAL(&bsMux);
}

// ─────────────────────────────────────────────
//  SESSION LOG
//  Defined here — after all struct definitions
//  (APEntry, ProbeEntry, DeauthEntry, bsMux etc.)
//  so the compiler can resolve all references.
// ─────────────────────────────────────────────
#define BEACON_LOG_DIR "/cyber_logs"

static void beacon_log_write() {
    if (!sd.exists(BEACON_LOG_DIR)) sd.mkdir(BEACON_LOG_DIR);

    char fname[52];
    snprintf(fname, sizeof(fname), "%s/beacon_%010lu.json", BEACON_LOG_DIR, millis());

    FsFile f = sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) {
        Serial.println("[BEACON] Log write FAILED");
        return;
    }

    // ── AP Table ──
    f.print("{\"aps\":[");
    portENTER_CRITICAL(&bsMux);
    for (int i = 0; i < apCount; i++) {
        APEntry& ap = apTable[i];
        if (i > 0) f.print(",");
        f.printf("{\"ssid\":\"%s\",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"channel\":%d,\"rssi\":%d,\"enc\":%s,\"hidden\":%s,\"beacons\":%lu}",
            ap.ssid,
            ap.bssid[0],ap.bssid[1],ap.bssid[2],
            ap.bssid[3],ap.bssid[4],ap.bssid[5],
            ap.channel, (int)ap.rssi,
            ap.encrypted ? "true" : "false",
            ap.hidden    ? "true" : "false",
            ap.beaconCount);
    }
    f.print("],");

    // ── Probe Log ──
    f.print("\"probes\":[");
    for (int i = 0; i < probeCount; i++) {
        ProbeEntry& p = probeLog[i];
        if (i > 0) f.print(",");
        f.printf("{\"src\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ssid\":\"%s\"}",
            p.srcMac[0],p.srcMac[1],p.srcMac[2],
            p.srcMac[3],p.srcMac[4],p.srcMac[5],
            p.ssid);
    }
    f.print("],");

    // ── Deauth Events ──
    f.print("\"deauths\":[");
    for (int i = 0; i < deauthCount; i++) {
        DeauthEntry& d = deauthLog[i];
        if (i > 0) f.print(",");
        f.printf("{\"src\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                  "\"dst\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"count\":%lu}",
            d.srcMac[0],d.srcMac[1],d.srcMac[2],
            d.srcMac[3],d.srcMac[4],d.srcMac[5],
            d.dstMac[0],d.dstMac[1],d.dstMac[2],
            d.dstMac[3],d.dstMac[4],d.dstMac[5],
            d.count);
    }
    portEXIT_CRITICAL(&bsMux);
    f.print("]}");

    f.flush();
    f.close();
    Serial.printf("[BEACON] Log saved: %s  APs:%d Probes:%d Deauths:%d\n",
        fname, apCount, probeCount, deauthCount);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_beacon_spotter() {
    gfx->fillScreen(COL_BG);
    gfx->setTextColor(COL_PROBE);
    gfx->setTextSize(2);
    gfx->setCursor(20, 60); gfx->print("BEACON SPOTTER");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(10, 90); gfx->print("Passive 802.11 management");
    gfx->setCursor(10, 104); gfx->print("frame analysis. Read-only.");
    delay(1500);

    // Init
    apCount = probeCount = deauthCount = 0;
    currentChannel = 6; // Channel 6 — most common default

    wifi_in_use = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&beaconCallback);
    // Filter to management frames only
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    int view = VIEW_APS;
    int scroll = 0;

    gfx->fillScreen(COL_BG);
    drawMainHeader(view, currentChannel);
    drawAPView(scroll);

    unsigned long lastRedraw = millis();

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        if (k == 'q' || k == 'Q') break;

        bool changed = false;

        // Channel change
        if (tb.x == -1 && currentChannel > 1)  { currentChannel--; esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE); changed = true; }
        if (tb.x ==  1 && currentChannel < 13)  { currentChannel++; esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE); changed = true; }

        // Scroll
        if (tb.y == -1 && scroll > 0) { scroll--; changed = true; }
        if (tb.y ==  1) { scroll++; changed = true; }

        // Cycle view
        if (tb.clicked) { view = (view + 1) % 3; scroll = 0; changed = true; }

        // Periodic redraw
        if (changed || millis() - lastRedraw > 1000) {
            drawMainHeader(view, currentChannel);
            if      (view == VIEW_APS)    drawAPView(scroll);
            else if (view == VIEW_PROBES) drawProbeView(scroll);
            else                          drawDeauthView();
            lastRedraw = millis();
        }

        delay(50);
        yield();
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    wifi_in_use = false;
    beacon_log_write();   // Save full session to SD on exit
    gfx->fillScreen(COL_BG);
}