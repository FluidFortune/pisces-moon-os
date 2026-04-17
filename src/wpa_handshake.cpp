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
 * PISCES MOON OS — WPA HANDSHAKE CAPTURE v1.0
 * Passive promiscuous mode EAPOL frame capture.
 * Detects WPA/WPA2 4-way handshakes and saves in .hccapx format.
 *
 * How it works:
 *   - Sets WiFi to promiscuous mode on the selected channel
 *   - Watches for EAPOL LLC frames (EtherType 0x888E) in 802.11 data frames
 *   - A complete handshake requires frames 1+2 or 2+3 (msg1-4 of 4-way)
 *   - Groups frames by BSSID+client MAC pair, saves when pair is complete
 *   - .hccapx format: standard input for Hashcat WPA cracking mode (-m 2500)
 *
 * PASSIVE ONLY — no transmission, no deauth injection.
 * USE ONLY ON NETWORKS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 *
 * Controls:
 *   Trackball L/R = change channel (1-13)
 *   Trackball U/D = scroll captured handshakes list
 *   Q / tap header = exit
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "wpa_handshake.h"

extern Arduino_GFX *gfx;
extern volatile bool wifi_in_use;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define WH_BG     0x0000
#define WH_HDR    0x1800
#define WH_RED    0xF800
#define WH_ORANGE 0xFD20
#define WH_GREEN  0x07E0
#define WH_CYAN   0x07FF
#define WH_WHITE  0xFFFF
#define WH_DIM    0x4208
#define WH_AMBER  0xFFE0

#define LOG_DIR   "/cyber_logs"

// ─────────────────────────────────────────────
//  802.11 STRUCTURES
// ─────────────────────────────────────────────
typedef struct {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6];   // DA (client)
    uint8_t  addr2[6];   // SA (AP or client)
    uint8_t  addr3[6];   // BSSID
    uint16_t seq_ctrl;
} __attribute__((packed)) wh_mac_hdr_t;

// LLC + SNAP header that precedes EAPOL
typedef struct {
    uint8_t  dsap;       // 0xAA
    uint8_t  ssap;       // 0xAA
    uint8_t  ctrl;       // 0x03
    uint8_t  oui[3];     // 0x00 0x00 0x00
    uint16_t type;       // 0x8E88 (big-endian 0x888E = EAPOL)
} __attribute__((packed)) wh_llc_t;

// EAPOL key frame (simplified — enough to extract MIC and nonces)
typedef struct {
    uint8_t  version;
    uint8_t  type;       // 3 = Key
    uint16_t length;
    uint8_t  descriptor; // 2 = RSN, 254 = WPA
    uint16_t key_info;   // Flags: bit 8 = key MIC, bit 9 = secure, etc.
    uint16_t key_len;
    uint64_t replay_ctr;
    uint8_t  key_nonce[32];
    uint8_t  key_iv[16];
    uint64_t key_rsc;
    uint64_t key_id;
    uint8_t  key_mic[16];
    uint16_t key_data_len;
    // key_data follows
} __attribute__((packed)) wh_eapol_key_t;

// ─────────────────────────────────────────────
//  HANDSHAKE STATE
// ─────────────────────────────────────────────
#define MAX_SESSIONS 8

struct WHSession {
    uint8_t  bssid[6];
    uint8_t  client[6];
    uint8_t  ssid[33];
    uint8_t  anonce[32];   // From AP message 1
    uint8_t  snonce[32];   // From client message 2
    uint8_t  mic[16];      // From message 2 or 4
    uint8_t  eapol_frame[256];
    uint16_t eapol_len;
    bool     have_anonce;
    bool     have_snonce;
    bool     have_mic;
    bool     saved;
    uint32_t lastSeen;
};

static WHSession whSessions[MAX_SESSIONS];
static int       whSessionCount  = 0;
static volatile int  whChannel   = 1;
static volatile uint32_t whTotal = 0;
static volatile uint32_t whSaved = 0;
static portMUX_TYPE whMux = portMUX_INITIALIZER_UNLOCKED;

// AP SSID table — populated from beacon frames on the same channel
struct WHAp { uint8_t bssid[6]; char ssid[33]; };
static WHAp  whApTable[32];
static int   whApCount = 0;

static void whGetSSID(const uint8_t* bssid, char* out, int outLen) {
    for (int i = 0; i < whApCount; i++) {
        if (memcmp(whApTable[i].bssid, bssid, 6) == 0) {
            strncpy(out, whApTable[i].ssid, outLen - 1);
            out[outLen-1] = '\0';
            return;
        }
    }
    snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0],bssid[1],bssid[2],bssid[3],bssid[4],bssid[5]);
}

static WHSession* whFindOrCreate(const uint8_t* bssid, const uint8_t* client) {
    for (int i = 0; i < whSessionCount; i++) {
        if (memcmp(whSessions[i].bssid,  bssid,  6) == 0 &&
            memcmp(whSessions[i].client, client, 6) == 0) {
            return &whSessions[i];
        }
    }
    if (whSessionCount >= MAX_SESSIONS) {
        // Evict oldest saved or oldest overall
        int victim = 0;
        for (int i = 1; i < MAX_SESSIONS; i++) {
            if (whSessions[i].saved && !whSessions[victim].saved) { victim = i; break; }
            if (whSessions[i].lastSeen < whSessions[victim].lastSeen) victim = i;
        }
        memset(&whSessions[victim], 0, sizeof(WHSession));
        whSessionCount = MAX_SESSIONS - 1;
    }
    WHSession& s = whSessions[whSessionCount++];
    memset(&s, 0, sizeof(WHSession));
    memcpy(s.bssid,  bssid,  6);
    memcpy(s.client, client, 6);
    char ssidbuf[33];
    whGetSSID(bssid, ssidbuf, 33);
    memcpy(s.ssid, ssidbuf, 33);
    s.lastSeen = millis();
    return &s;
}

// ─────────────────────────────────────────────
//  HCCAPX WRITER
//  hccapx v4: 393-byte struct per handshake
//  Standard Hashcat WPA input format (-m 2500)
// ─────────────────────────────────────────────
#pragma pack(push, 1)
struct hccapx_t {
    uint32_t signature;        // 0x58504348 "HCCPX"
    uint32_t version;          // 4
    uint8_t  message_pair;     // Which frames were captured (0=msg1+2)
    uint8_t  essid_len;
    uint8_t  essid[32];
    uint8_t  keyver;           // 1=WPA, 2=WPA2
    uint8_t  keymic[16];
    uint8_t  mac_ap[6];
    uint8_t  nonce_ap[32];
    uint8_t  mac_sta[6];
    uint8_t  nonce_sta[32];
    uint16_t eapol_len;
    uint8_t  eapol[256];
};
#pragma pack(pop)

static void whSaveHccapx(WHSession& s) {
    if (!sd.exists(LOG_DIR)) sd.mkdir(LOG_DIR);
    char fname[64];
    snprintf(fname, sizeof(fname), "%s/handshake_%010lu.hccapx", LOG_DIR, millis());

    FsFile f = sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) { Serial.printf("[WPA] Save failed: %s\n", fname); return; }

    hccapx_t hc;
    memset(&hc, 0, sizeof(hc));
    hc.signature = 0x58504348;
    hc.version   = 4;
    hc.message_pair = 0;   // msg1+msg2 captured
    hc.essid_len = min((int)strlen((char*)s.ssid), 32);
    memcpy(hc.essid,    s.ssid,    hc.essid_len);
    hc.keyver    = 2;      // WPA2 (most common)
    memcpy(hc.keymic,   s.mic,     16);
    memcpy(hc.mac_ap,   s.bssid,   6);
    memcpy(hc.nonce_ap, s.anonce,  32);
    memcpy(hc.mac_sta,  s.client,  6);
    memcpy(hc.nonce_sta,s.snonce,  32);
    hc.eapol_len = min((uint16_t)s.eapol_len, (uint16_t)256);
    memcpy(hc.eapol, s.eapol_frame, hc.eapol_len);

    f.write((uint8_t*)&hc, sizeof(hc));
    f.close();

    s.saved = true;
    whSaved++;
    Serial.printf("[WPA] Handshake saved: %s  SSID: %s\n", fname, s.ssid);
}

// ─────────────────────────────────────────────
//  PROMISCUOUS CALLBACK
// ─────────────────────────────────────────────
static void IRAM_ATTR whCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    // We need both MGMT (beacons for SSID) and DATA (EAPOL)
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    int pktLen = pkt->rx_ctrl.sig_len;
    if (pktLen < (int)sizeof(wh_mac_hdr_t)) return;

    wh_mac_hdr_t* hdr = (wh_mac_hdr_t*)pkt->payload;
    uint8_t ftype    = (hdr->frame_ctrl >> 2) & 0x03;
    uint8_t fsubtype = (hdr->frame_ctrl >> 4) & 0x0F;

    // Management: grab beacons for SSID table
    if (ftype == 0 && fsubtype == 8) {
        // Beacon body: 12 bytes fixed fields, then tagged params
        const uint8_t* body = pkt->payload + sizeof(wh_mac_hdr_t) + 12;
        int bodyLen = pktLen - sizeof(wh_mac_hdr_t) - 12;
        // Walk IE tags looking for SSID (tag 0)
        int off = 0;
        while (off + 2 <= bodyLen) {
            uint8_t tag = body[off];
            uint8_t len = body[off+1];
            if (off + 2 + len > bodyLen) break;
            if (tag == 0 && len > 0) {
                // Found SSID — update AP table
                portENTER_CRITICAL_ISR(&whMux);
                bool found = false;
                for (int i = 0; i < whApCount; i++) {
                    if (memcmp(whApTable[i].bssid, hdr->addr3, 6) == 0) { found = true; break; }
                }
                if (!found && whApCount < 32) {
                    memcpy(whApTable[whApCount].bssid, hdr->addr3, 6);
                    int copyLen = min((int)len, 32);
                    memcpy(whApTable[whApCount].ssid, body + off + 2, copyLen);
                    whApTable[whApCount].ssid[copyLen] = '\0';
                    whApCount++;
                }
                portEXIT_CRITICAL_ISR(&whMux);
                break;
            }
            off += 2 + len;
        }
        return;
    }

    // Data frames only from here
    if (ftype != 2) return;

    // Need at least MAC header + LLC (8 bytes) + EAPOL key header
    int offset = sizeof(wh_mac_hdr_t);
    // QoS data has 2 extra bytes
    if (fsubtype == 8 || fsubtype == 9 || fsubtype == 10 || fsubtype == 11) offset += 2;
    // 4-address frame has extra 6-byte addr4
    if ((hdr->frame_ctrl & 0x0300) == 0x0300) offset += 6;

    if (pktLen < offset + (int)sizeof(wh_llc_t) + (int)sizeof(wh_eapol_key_t)) return;

    wh_llc_t* llc = (wh_llc_t*)(pkt->payload + offset);
    // Check LLC/SNAP for EAPOL (0x888E, stored big-endian as 0x8E88)
    if (llc->dsap != 0xAA || llc->ssap != 0xAA) return;
    if (llc->type != 0x8E88) return;   // Not EAPOL

    whTotal++;

    wh_eapol_key_t* key = (wh_eapol_key_t*)(pkt->payload + offset + sizeof(wh_llc_t));
    if (key->type != 3) return;  // Not EAPOL-Key

    // Determine direction: to_ds=0, from_ds=1 → AP→STA (msg 1 or 3)
    //                      to_ds=1, from_ds=0 → STA→AP (msg 2 or 4)
    bool fromAP = ((hdr->frame_ctrl & 0x0100) == 0);   // from_ds bit (bit 8)
    const uint8_t* apMac     = fromAP ? hdr->addr2 : hdr->addr1;
    const uint8_t* clientMac = fromAP ? hdr->addr1 : hdr->addr2;

    // Key info flags
    uint16_t ki  = __builtin_bswap16(key->key_info);
    bool hasMIC  = (ki & 0x0100) != 0;
    bool isMsg1  = !hasMIC;   // Msg 1: no MIC, has ANonce
    bool isMsg2  = hasMIC && !((ki & 0x0200) != 0);  // Msg 2: MIC set, Secure not set
    bool isMsg3  = hasMIC && ((ki & 0x0200) != 0);   // Msg 3: MIC+Secure set

    portENTER_CRITICAL_ISR(&whMux);
    WHSession* s = whFindOrCreate(apMac, clientMac);
    s->lastSeen = millis();

    if (isMsg1) {
        memcpy(s->anonce, key->key_nonce, 32);
        s->have_anonce = true;
    } else if (isMsg2) {
        memcpy(s->snonce, key->key_nonce, 32);
        memcpy(s->mic,    key->key_mic,   16);
        s->have_snonce = true;
        s->have_mic    = true;
        // Copy EAPOL frame for hccapx
        int eapolStart = offset + sizeof(wh_llc_t);
        s->eapol_len = min(pktLen - eapolStart, 256);
        if (s->eapol_len > 0)
            memcpy(s->eapol_frame, pkt->payload + eapolStart, s->eapol_len);
    } else if (isMsg3) {
        memcpy(s->anonce, key->key_nonce, 32);
        s->have_anonce = true;
    }

    portEXIT_CRITICAL_ISR(&whMux);
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void whDrawHeader() {
    gfx->fillRect(0, 0, 320, 24, WH_HDR);
    gfx->drawFastHLine(0, 23, 320, WH_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(WH_RED);
    gfx->setCursor(6, 4);
    gfx->print("WPA HANDSHAKE CAPTURE");
    gfx->setTextColor(WH_DIM);
    gfx->setCursor(6, 14);
    gfx->printf("CH:%02d  EAPOL:%lu  SAVED:%lu", whChannel, whTotal, whSaved);
    gfx->setTextColor(WH_DIM);
    gfx->setCursor(264, 8);
    gfx->print("[Q=EXIT]");
}

static void whDrawSessionList(int scroll) {
    gfx->fillRect(0, 26, 320, 182, WH_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(WH_DIM);
    gfx->setCursor(4, 28);
    gfx->print("BSSID              STA MAC           STATUS");
    gfx->drawFastHLine(0, 37, 320, 0x2000);

    if (whSessionCount == 0) {
        gfx->setTextColor(WH_DIM);
        gfx->setCursor(10, 90); gfx->print("Waiting for EAPOL frames...");
        gfx->setCursor(10, 108); gfx->print("Clients must authenticate to see");
        gfx->setCursor(10, 122); gfx->print("handshakes. Channel hop if needed.");
        return;
    }

    int show = min(8, whSessionCount);
    for (int i = scroll; i < scroll + show && i < whSessionCount; i++) {
        WHSession& s = whSessions[i];
        int ry = 40 + (i - scroll) * 21;
        gfx->fillRect(0, ry, 320, 20, (i%2==0) ? 0x0821 : WH_BG);

        // AP MAC
        gfx->setTextColor(WH_CYAN);
        gfx->setCursor(4, ry + 3);
        gfx->printf("%02X:%02X:%02X:%02X:%02X:%02X",
            s.bssid[0],s.bssid[1],s.bssid[2],s.bssid[3],s.bssid[4],s.bssid[5]);
        // Client MAC
        gfx->setTextColor(WH_ORANGE);
        gfx->setCursor(4, ry + 12);
        gfx->printf("%02X:%02X:%02X:%02X:%02X:%02X",
            s.client[0],s.client[1],s.client[2],s.client[3],s.client[4],s.client[5]);
        // Status
        uint16_t statusCol;
        const char* statusStr;
        if (s.saved) {
            statusCol = WH_GREEN; statusStr = "SAVED";
        } else if (s.have_anonce && s.have_snonce && s.have_mic) {
            statusCol = WH_AMBER; statusStr = "COMPLETE";
        } else if (s.have_anonce || s.have_snonce) {
            statusCol = WH_ORANGE; statusStr = "PARTIAL";
        } else {
            statusCol = WH_DIM; statusStr = "EAPOL";
        }
        gfx->setTextColor(statusCol);
        gfx->setCursor(230, ry + 7);
        gfx->print(statusStr);

        // SSID
        gfx->setTextColor(WH_DIM);
        gfx->setCursor(282, ry + 3);
        char ssidShort[10]; strncpy(ssidShort, (char*)s.ssid, 8); ssidShort[8] = '\0';
        gfx->print(ssidShort);
    }
}

static void whDrawStatus() {
    gfx->fillRect(0, 210, 320, 30, 0x0800);
    gfx->drawFastHLine(0, 210, 320, WH_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(WH_DIM);
    gfx->setCursor(4, 220);
    gfx->print("</> CH  BALL:scroll  S:save pending  Q:exit");
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_wpa_handshake() {
    gfx->fillScreen(WH_BG);
    gfx->setTextColor(WH_ORANGE);
    gfx->setTextSize(2);
    gfx->setCursor(10, 50); gfx->print("WPA HANDSHAKE");
    gfx->setCursor(10, 72); gfx->print("CAPTURE");
    gfx->setTextSize(1);
    gfx->setTextColor(WH_WHITE);
    gfx->setCursor(10, 100); gfx->print("Passive EAPOL frame capture.");
    gfx->setCursor(10, 114); gfx->print("Saves .hccapx for Hashcat -m 2500.");
    gfx->setTextColor(WH_RED);
    gfx->setCursor(10, 132); gfx->print("USE ON AUTHORIZED NETWORKS ONLY.");
    gfx->setTextColor(WH_DIM);
    gfx->setCursor(10, 150); gfx->print("Starting on channel 1...");
    delay(2500);

    // Init
    whSessionCount = 0; whApCount = 0; whTotal = 0; whSaved = 0;
    whChannel = 1;
    memset(whSessions, 0, sizeof(whSessions));
    memset(whApTable,  0, sizeof(whApTable));

    wifi_in_use = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&whCallback);
    esp_wifi_set_channel(whChannel, WIFI_SECOND_CHAN_NONE);

    gfx->fillScreen(WH_BG);
    whDrawHeader();
    whDrawSessionList(0);
    whDrawStatus();

    int scroll = 0;
    unsigned long lastRedraw = millis();

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);} break;
        }
        if (k == 'q' || k == 'Q') break;

        // Channel change
        if (tb.x == -1 && whChannel > 1)  { whChannel--; esp_wifi_set_channel(whChannel, WIFI_SECOND_CHAN_NONE); }
        if (tb.x ==  1 && whChannel < 13) { whChannel++; esp_wifi_set_channel(whChannel, WIFI_SECOND_CHAN_NONE); }

        // Scroll sessions
        if (tb.y == -1 && scroll > 0) scroll--;
        if (tb.y ==  1 && scroll < whSessionCount - 1) scroll++;

        // Manual save of any complete-but-unsaved sessions
        if (k == 's' || k == 'S') {
            for (int i = 0; i < whSessionCount; i++) {
                WHSession& s = whSessions[i];
                if (!s.saved && s.have_anonce && s.have_snonce && s.have_mic) {
                    whSaveHccapx(s);
                }
            }
        }

        // Periodic redraw + auto-save complete sessions
        if (millis() - lastRedraw > 500) {
            // Auto-save newly complete sessions
            for (int i = 0; i < whSessionCount; i++) {
                WHSession& s = whSessions[i];
                if (!s.saved && s.have_anonce && s.have_snonce && s.have_mic) {
                    whSaveHccapx(s);
                }
            }
            whDrawHeader();
            whDrawSessionList(scroll);
            lastRedraw = millis();
        }

        delay(20); yield();
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    wifi_in_use = false;
    gfx->fillScreen(WH_BG);
}
