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
 * PISCES MOON OS — PROBE REQUEST INTELLIGENCE v1.0
 * Passively captures 802.11 probe requests and builds a device fingerprint
 * map: which devices are probing for which SSIDs.
 *
 * What probe requests reveal:
 *   - SSIDs a device has previously connected to (stored in its PNL)
 *   - Device identity via MAC OUI vendor lookup
 *   - Device mobility (RSSI changes over time suggest movement)
 *   - Network history of corporate/home networks being sought
 *
 * Display views:
 *   DEVICES — unique devices, sorted by probe count, with OUI vendor
 *   SSIDS   — unique SSIDs being probed, sorted by frequency
 *   MAP     — per-device SSID list: tap device to see all its probed SSIDs
 *
 * All data saved to /cyber_logs/probe_NNNNNNNNNN.json on exit.
 *
 * PASSIVE ONLY — no transmission.
 * USE ONLY IN ENVIRONMENTS WHERE YOU HAVE AUTHORIZATION.
 *
 * Controls:
 *   Trackball L/R  = change channel
 *   Trackball U/D  = scroll list
 *   Trackball CLICK = toggle view / expand device
 *   Q / tap header  = exit
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
#include "probe_intel.h"

extern Arduino_GFX *gfx;
extern volatile bool wifi_in_use;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define PI_BG     0x0000
#define PI_HDR    0x000C   // Deep blue
#define PI_BLUE   0x001F
#define PI_CYAN   0x07FF
#define PI_GREEN  0x07E0
#define PI_YELLOW 0xFFE0
#define PI_RED    0xF800
#define PI_DIM    0x4208
#define PI_WHITE  0xFFFF
#define PI_SEL    0x0018

#define LOG_DIR   "/cyber_logs"

// ─────────────────────────────────────────────
//  DATA STRUCTURES
// ─────────────────────────────────────────────
#define MAX_DEVICES   48
#define MAX_SSIDS     64
#define MAX_SSIDS_PER_DEV 16
#define MAX_CHANNEL   13

// OUI table — first 3 bytes → vendor short name
// Top 30 most common for probe request context
struct OUIEntry { uint8_t oui[3]; const char* vendor; };
static const OUIEntry OUI_TABLE[] = {
    {{0xAC,0x37,0x43}, "Apple"    }, {{0xF4,0x5C,0x89}, "Apple"  },
    {{0x18,0x65,0x90}, "Apple"    }, {{0x3C,0x22,0xFB}, "Apple"  },
    {{0x28,0x39,0x5E}, "Apple"    }, {{0x60,0xF8,0x1D}, "Apple"  },
    {{0xA4,0xC3,0xF0}, "Samsung"  }, {{0x8C,0x77,0x12}, "Samsung"},
    {{0x50,0x01,0xBB}, "Samsung"  }, {{0xF4,0x60,0xE2}, "LG"     },
    {{0x00,0x17,0xF2}, "Apple"    }, {{0x78,0x4F,0x43}, "Apple"  },
    {{0x38,0xCA,0xDA}, "Apple"    }, {{0xB0,0x34,0x95}, "Apple"  },
    {{0xCC,0x08,0xFB}, "Apple"    }, {{0x68,0xFB,0x7E}, "Apple"  },
    {{0x20,0xA9,0x9B}, "Apple"    }, {{0x04,0x4B,0xED}, "Qualcomm"},
    {{0xE8,0x9F,0x80}, "Intel"    }, {{0x94,0xB8,0x6D}, "Intel"  },
    {{0x00,0x50,0xF2}, "Microsoft"}, {{0x28,0xD2,0x44}, "Microsoft"},
    {{0x98,0xDE,0xD0}, "Huawei"   }, {{0xBC,0x9F,0xEF}, "Xiaomi" },
    {{0x00,0x0C,0xE7}, "Wi-Fi"    }, {{0x00,0x26,0xB9}, "Nintendo"},
};
#define OUI_COUNT (sizeof(OUI_TABLE)/sizeof(OUI_TABLE[0]))

static const char* piOUILookup(const uint8_t* mac) {
    for (int i = 0; i < (int)OUI_COUNT; i++) {
        if (OUI_TABLE[i].oui[0] == mac[0] &&
            OUI_TABLE[i].oui[1] == mac[1] &&
            OUI_TABLE[i].oui[2] == mac[2]) return OUI_TABLE[i].vendor;
    }
    return "Unknown";
}

struct PIDevice {
    uint8_t  mac[6];
    char     vendor[12];
    int32_t  rssi;           // Last seen RSSI
    uint32_t probeCount;     // Total probe frames from this device
    uint32_t lastSeen;
    char     ssids[MAX_SSIDS_PER_DEV][33];
    int      ssidCount;
};

struct PISSIDEntry {
    char    ssid[33];
    uint32_t count;   // Times we've seen this SSID probed
};

// Device and SSID tables — allocated in PSRAM on launch, freed on exit.
// PIDevice[48] is ~27KB, too large for static BSS.
static PIDevice*    piDevices = nullptr;
static int          piDevCount  = 0;
static PISSIDEntry* piSSIDs   = nullptr;
static int          piSSIDCount = 0;

static volatile int piChannel    = 1;
static volatile uint32_t piTotal = 0;
static portMUX_TYPE piMux = portMUX_INITIALIZER_UNLOCKED;

// ─────────────────────────────────────────────
//  SSID REGISTRY
// ─────────────────────────────────────────────
static void piRegisterSSID(const char* ssid) {
    if (!ssid || ssid[0] == '\0') return;  // Wildcard probe
    for (int i = 0; i < piSSIDCount; i++) {
        if (strcmp(piSSIDs[i].ssid, ssid) == 0) { piSSIDs[i].count++; return; }
    }
    if (piSSIDCount < MAX_SSIDS) {
        strncpy(piSSIDs[piSSIDCount].ssid, ssid, 32);
        piSSIDs[piSSIDCount].ssid[32] = '\0';
        piSSIDs[piSSIDCount].count = 1;
        piSSIDCount++;
    }
}

// ─────────────────────────────────────────────
//  802.11 HEADER + SSID PARSING
// ─────────────────────────────────────────────
typedef struct {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6];
    uint8_t  addr2[6];  // Source MAC
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} __attribute__((packed)) pi_mac_hdr_t;

// ─────────────────────────────────────────────
//  PROMISCUOUS CALLBACK
// ─────────────────────────────────────────────
static void IRAM_ATTR piCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    int pktLen = pkt->rx_ctrl.sig_len;
    if (pktLen < (int)sizeof(pi_mac_hdr_t)) return;

    pi_mac_hdr_t* hdr = (pi_mac_hdr_t*)pkt->payload;
    uint8_t ftype    = (hdr->frame_ctrl >> 2) & 0x03;
    uint8_t fsubtype = (hdr->frame_ctrl >> 4) & 0x0F;

    // Probe Request only (type=0, subtype=4)
    if (ftype != 0 || fsubtype != 4) return;

    piTotal++;

    const uint8_t* body    = pkt->payload + sizeof(pi_mac_hdr_t);
    int            bodyLen = pktLen - sizeof(pi_mac_hdr_t);
    int8_t         rssi    = pkt->rx_ctrl.rssi;

    // Parse SSID from IE tag 0
    char ssid[33] = "";
    int off = 0;
    while (off + 2 <= bodyLen) {
        uint8_t tag = body[off];
        uint8_t len = body[off+1];
        if (off + 2 + len > bodyLen) break;
        if (tag == 0) {
            if (len > 0) {
                int copyLen = min((int)len, 32);
                memcpy(ssid, body + off + 2, copyLen);
                ssid[copyLen] = '\0';
            }
            break;
        }
        off += 2 + len;
    }

    portENTER_CRITICAL_ISR(&piMux);

    // Find or create device entry
    PIDevice* dev = nullptr;
    for (int i = 0; i < piDevCount; i++) {
        if (memcmp(piDevices[i].mac, hdr->addr2, 6) == 0) { dev = &piDevices[i]; break; }
    }
    if (!dev && piDevCount < MAX_DEVICES) {
        dev = &piDevices[piDevCount++];
        memset(dev, 0, sizeof(PIDevice));
        memcpy(dev->mac, hdr->addr2, 6);
        const char* v = piOUILookup(hdr->addr2);
        strncpy(dev->vendor, v, 11); dev->vendor[11] = '\0';
    }

    if (dev) {
        dev->rssi = rssi;
        dev->probeCount++;
        dev->lastSeen = (uint32_t)(millis() / 1000);

        // Add SSID to device's list if not already present
        if (ssid[0] != '\0') {
            bool found = false;
            for (int j = 0; j < dev->ssidCount; j++) {
                if (strcmp(dev->ssids[j], ssid) == 0) { found = true; break; }
            }
            if (!found && dev->ssidCount < MAX_SSIDS_PER_DEV) {
                strncpy(dev->ssids[dev->ssidCount++], ssid, 32);
                dev->ssids[dev->ssidCount-1][32] = '\0';
            }
        }

        piRegisterSSID(ssid);
    }

    portEXIT_CRITICAL_ISR(&piMux);
}

// ─────────────────────────────────────────────
//  VIEWS
// ─────────────────────────────────────────────
#define VIEW_DEVICES 0
#define VIEW_SSIDS   1
#define VIEW_DETAIL  2

static void piDrawHeader(int view) {
    gfx->fillRect(0, 0, 320, 24, PI_HDR);
    gfx->drawFastHLine(0, 23, 320, PI_CYAN);
    gfx->setTextSize(1);
    gfx->setTextColor(PI_CYAN);
    gfx->setCursor(6, 4);
    gfx->print("PROBE INTEL");
    gfx->setTextColor(PI_DIM);
    gfx->setCursor(6, 14);
    gfx->printf("CH:%02d  TOTAL:%lu  DEV:%d  SSID:%d",
        piChannel, piTotal, piDevCount, piSSIDCount);
    const char* tabs[] = {"[DEV]","[NET]"};
    gfx->setCursor(234, 4);
    for (int i = 0; i < 2; i++) {
        gfx->setTextColor(i == view ? PI_CYAN : PI_DIM);
        gfx->print(tabs[i]); gfx->print(" ");
    }
    gfx->setTextColor(PI_DIM);
    gfx->setCursor(286, 14);
    gfx->print("Q=OUT");
}

static void piDrawDeviceView(int scroll, int selected) {
    gfx->fillRect(0, 26, 320, 182, PI_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(PI_DIM);
    gfx->setCursor(4, 28);
    gfx->print("MAC               VENDOR      CNT  NETS RSSI");
    gfx->drawFastHLine(0, 37, 320, 0x0018);

    if (piDevCount == 0) {
        gfx->setCursor(10, 90); gfx->setTextColor(PI_DIM);
        gfx->print("Waiting for probe requests...");
        gfx->setCursor(10, 106); gfx->print("Move to area with active devices.");
        return;
    }

    int show = min(8, piDevCount);
    for (int i = scroll; i < scroll + show && i < piDevCount; i++) {
        PIDevice& d = piDevices[i];
        int ry  = 40 + (i - scroll) * 21;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, 20, sel ? PI_SEL : (i%2==0 ? 0x0821 : PI_BG));
        // MAC
        gfx->setTextColor(sel ? PI_CYAN : 0x8C71);
        gfx->setCursor(4, ry + 2);
        gfx->printf("%02X:%02X:%02X:%02X:%02X:%02X",
            d.mac[0],d.mac[1],d.mac[2],d.mac[3],d.mac[4],d.mac[5]);
        // Vendor
        gfx->setTextColor(sel ? PI_WHITE : PI_DIM);
        gfx->setCursor(120, ry + 2);
        gfx->printf("%-10s", d.vendor);
        // Count
        gfx->setTextColor(sel ? PI_YELLOW : PI_DIM);
        gfx->setCursor(186, ry + 2);
        gfx->printf("%4lu", d.probeCount);
        // SSID count
        gfx->setTextColor(sel ? PI_GREEN : 0x2945);
        gfx->setCursor(220, ry + 2);
        gfx->printf("%3d", d.ssidCount);
        // RSSI
        uint16_t rc = (d.rssi > -60) ? PI_GREEN : (d.rssi > -80) ? PI_YELLOW : PI_RED;
        gfx->setTextColor(rc);
        gfx->setCursor(256, ry + 2);
        gfx->printf("%4d", (int)d.rssi);
        // First SSID as preview
        if (d.ssidCount > 0) {
            gfx->setTextColor(0x2945);
            gfx->setCursor(4, ry + 12);
            char preview[28];
            snprintf(preview, sizeof(preview), "Seeks: %s%s",
                d.ssids[0],
                d.ssidCount > 1 ? " ..." : "");
            gfx->print(preview);
        }
    }

    gfx->setTextColor(PI_DIM);
    gfx->setCursor(4, 226);
    gfx->print("CLICK=details  </> CH  BALL=scroll  V=switch view");
}

static void piDrawSSIDView(int scroll) {
    gfx->fillRect(0, 26, 320, 182, PI_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(PI_DIM);
    gfx->setCursor(4, 28);
    gfx->print("SSID BEING PROBED                   COUNT");
    gfx->drawFastHLine(0, 37, 320, 0x0018);

    if (piSSIDCount == 0) {
        gfx->setCursor(10, 90); gfx->setTextColor(PI_DIM);
        gfx->print("No specific SSIDs probed yet.");
        gfx->setCursor(10, 106); gfx->print("(Wildcard probes don't reveal SSIDs)");
        return;
    }

    // Sort by count (bubble sort — small list)
    PISSIDEntry sorted[MAX_SSIDS];
    memcpy(sorted, piSSIDs, piSSIDCount * sizeof(PISSIDEntry));
    for (int i = 0; i < piSSIDCount - 1; i++)
        for (int j = i + 1; j < piSSIDCount; j++)
            if (sorted[j].count > sorted[i].count) {
                PISSIDEntry tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp;
            }

    int show = min(9, piSSIDCount);
    for (int i = scroll; i < scroll + show && i < piSSIDCount; i++) {
        int ry = 40 + (i - scroll) * 21;
        gfx->fillRect(0, ry, 320, 20, i%2==0 ? 0x0821 : PI_BG);
        gfx->setTextColor(PI_GREEN);
        gfx->setCursor(4, ry + 5);
        char ssidBuf[35]; strncpy(ssidBuf, sorted[i].ssid, 33); ssidBuf[33] = '\0';
        gfx->print(ssidBuf);
        gfx->setTextColor(PI_YELLOW);
        gfx->setCursor(268, ry + 5);
        gfx->printf("%6lu", sorted[i].count);
    }
}

static void piDrawDetailView(int deviceIdx) {
    if (deviceIdx >= piDevCount) return;
    PIDevice& d = piDevices[deviceIdx];
    gfx->fillRect(0, 26, 320, 182, PI_BG);
    gfx->setTextSize(1);

    gfx->setTextColor(PI_CYAN);
    gfx->setCursor(4, 30);
    gfx->printf("%02X:%02X:%02X:%02X:%02X:%02X",
        d.mac[0],d.mac[1],d.mac[2],d.mac[3],d.mac[4],d.mac[5]);
    gfx->setTextColor(PI_DIM);
    gfx->setCursor(130, 30);
    gfx->print(d.vendor);
    gfx->setTextColor(PI_DIM);
    gfx->setCursor(4, 42);
    gfx->printf("Probes: %lu  Nets known: %d  RSSI: %d",
        d.probeCount, d.ssidCount, (int)d.rssi);
    gfx->drawFastHLine(0, 52, 320, 0x0018);

    gfx->setTextColor(PI_DIM);
    gfx->setCursor(4, 56);
    gfx->print("Previously connected to:");

    for (int i = 0; i < d.ssidCount && i < 8; i++) {
        int ry = 68 + i * 16;
        uint16_t col = (i % 2 == 0) ? PI_GREEN : PI_CYAN;
        gfx->setTextColor(col);
        gfx->setCursor(12, ry);
        gfx->printf("%d. %s", i + 1, d.ssids[i]);
    }
    if (d.ssidCount == 0) {
        gfx->setTextColor(PI_DIM);
        gfx->setCursor(12, 70);
        gfx->print("(Wildcard probes only — no SSID revealed)");
    }
    if (d.ssidCount > 8) {
        gfx->setTextColor(PI_DIM);
        gfx->setCursor(4, 198);
        gfx->printf("... and %d more", d.ssidCount - 8);
    }

    gfx->setTextColor(PI_DIM);
    gfx->setCursor(4, 212);
    gfx->print("[Q/tap header = back to device list]");
}

// ─────────────────────────────────────────────
//  SESSION LOG
// ─────────────────────────────────────────────
static void piWriteLog() {
    if (!sd.exists(LOG_DIR)) sd.mkdir(LOG_DIR);
    char fname[64];
    snprintf(fname, sizeof(fname), "%s/probe_%010lu.json", LOG_DIR, millis());
    FsFile f = sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return;

    f.print("{\"devices\":[\n");
    for (int i = 0; i < piDevCount; i++) {
        PIDevice& d = piDevices[i];
        if (i > 0) f.print(",\n");
        f.printf("  {\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
                 "\"vendor\":\"%s\",\"probes\":%lu,\"rssi\":%d,\"ssids\":[",
            d.mac[0],d.mac[1],d.mac[2],d.mac[3],d.mac[4],d.mac[5],
            d.vendor, d.probeCount, (int)d.rssi);
        for (int j = 0; j < d.ssidCount; j++) {
            if (j > 0) f.print(",");
            String s = String(d.ssids[j]);
            s.replace("\"", "'");
            f.printf("\"%s\"", s.c_str());
        }
        f.print("]}");
    }
    f.print("\n]}\n");
    f.close();
    Serial.printf("[PROBE] Log: %s\n", fname);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_probe_intel() {
    gfx->fillScreen(PI_BG);
    gfx->setTextColor(PI_CYAN);
    gfx->setTextSize(2);
    gfx->setCursor(10, 50); gfx->print("PROBE INTEL");
    gfx->setTextSize(1);
    gfx->setTextColor(PI_WHITE);
    gfx->setCursor(10, 80); gfx->print("Passive probe request analysis.");
    gfx->setTextColor(PI_DIM);
    gfx->setCursor(10, 96); gfx->print("Reveals network history of nearby devices.");
    gfx->setCursor(10, 112); gfx->print("Use only in authorized environments.");
    delay(2000);

    // Allocate tables in PSRAM — too large for static BSS
    piDevices = (PIDevice*)ps_malloc(MAX_DEVICES * sizeof(PIDevice));
    piSSIDs   = (PISSIDEntry*)ps_malloc(MAX_SSIDS * sizeof(PISSIDEntry));
    if (!piDevices || !piSSIDs) {
        gfx->setTextColor(PI_RED); gfx->setCursor(10, 140);
        gfx->print("Out of PSRAM. Tap to exit.");
        while (true) {
            if (get_keypress()) break;
            int16_t tx, ty; if (get_touch(&tx, &ty)) break;
            delay(50);
        }
        free(piDevices); free(piSSIDs);
        piDevices = nullptr; piSSIDs = nullptr;
        return;
    }

    piDevCount = 0; piSSIDCount = 0; piTotal = 0; piChannel = 1;
    memset(piDevices, 0, MAX_DEVICES * sizeof(PIDevice));
    memset(piSSIDs,   0, MAX_SSIDS   * sizeof(PISSIDEntry));

    wifi_in_use = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&piCallback);
    esp_wifi_set_channel(piChannel, WIFI_SECOND_CHAN_NONE);

    int view = VIEW_DEVICES;
    int scroll = 0, selected = 0;
    unsigned long lastRedraw = millis();
    bool inDetail = false;

    gfx->fillScreen(PI_BG);
    piDrawHeader(view);
    piDrawDeviceView(scroll, selected);

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (inDetail) {
                inDetail = false;
                view = VIEW_DEVICES;
                gfx->fillScreen(PI_BG);
                piDrawHeader(view);
                piDrawDeviceView(scroll, selected);
            } else {
                break;
            }
            continue;
        }
        if (k == 'q' || k == 'Q') {
            if (inDetail) {
                inDetail = false; view = VIEW_DEVICES;
                gfx->fillScreen(PI_BG); piDrawHeader(view);
                piDrawDeviceView(scroll, selected);
            } else break;
            continue;
        }

        // View switch
        if ((k == 'v' || k == 'V') && !inDetail) {
            view = (view == VIEW_DEVICES) ? VIEW_SSIDS : VIEW_DEVICES;
            scroll = 0; selected = 0;
            gfx->fillScreen(PI_BG); piDrawHeader(view);
            if (view == VIEW_DEVICES) piDrawDeviceView(scroll, selected);
            else piDrawSSIDView(scroll);
            continue;
        }

        // Channel change
        bool chChanged = false;
        if (tb.x == -1 && piChannel > 1)  { piChannel--; chChanged = true; }
        if (tb.x ==  1 && piChannel < MAX_CHANNEL) { piChannel++; chChanged = true; }
        if (chChanged) esp_wifi_set_channel(piChannel, WIFI_SECOND_CHAN_NONE);

        // Scroll
        int maxScroll = 0;
        if (view == VIEW_DEVICES) maxScroll = max(0, piDevCount - 8);
        else maxScroll = max(0, piSSIDCount - 9);

        if (tb.y == -1 && scroll > 0)        { scroll--; if (selected > 0 && view==VIEW_DEVICES) selected--; }
        if (tb.y ==  1 && scroll < maxScroll) { scroll++; if (view==VIEW_DEVICES && selected < piDevCount-1) selected++; }

        // Device detail
        if (tb.clicked && view == VIEW_DEVICES && !inDetail && piDevCount > 0) {
            inDetail = true;
            view = VIEW_DETAIL;
            gfx->fillScreen(PI_BG);
            piDrawHeader(VIEW_DEVICES);
            piDrawDetailView(selected);
            continue;
        }

        if (millis() - lastRedraw > 750) {
            piDrawHeader(view);
            if (inDetail)              piDrawDetailView(selected);
            else if (view==VIEW_DEVICES) piDrawDeviceView(scroll, selected);
            else                         piDrawSSIDView(scroll);
            lastRedraw = millis();
        }

        delay(20); yield();
    }

    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_OFF);
    wifi_in_use = false;
    piWriteLog();
    free(piDevices); piDevices = nullptr;
    free(piSSIDs);   piSSIDs   = nullptr;
    gfx->fillScreen(PI_BG);
}
