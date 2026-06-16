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
 * MESH MAP — live Meshtastic/LoRa nodes on the moving map
 *
 * The spatial, in-field counterpart to WarDrive's LoRa logging. Listens
 * passively on Meshtastic LongFast (same SX1262 RX path as
 * mesh_messenger.cpp / the wardrive LoRa integration), and for every
 * packet heard, places its ORIGIN node at our current GPS fix — i.e.
 * "I heard !abc12345 here, at -74 dBm" — as a colored dot with a range
 * ring, on the shared map engine. Color tracks RSSI; ring radius is a
 * crude path-loss range estimate (direct packets only).
 *
 * This is detection + where-heard, not a transmitter fix. A node we
 * hear is plotted at OUR position when we heard it strongest, exactly
 * as the wardrive CSV records it. The map just makes it spatial live,
 * which the CSV→Clinician pipeline only does after the fact.
 *
 * SX1262 LoRa devices only (T-Deck Plus, T-LoRa Pager, Cardputer ADV).
 * The body is guarded; on non-LoRa targets this is an empty TU.
 */

#include "pm_map_apps.h"

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)

#include "pm_map_engine.h"
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "pm_lora_pins.h"

#if defined(DEVICE_TLORAPAGER)
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif

#ifdef DEVICE_CARDPUTER_ADV
extern SPIClass cardputerSdSPI;
#endif

extern Arduino_GFX        *gfx;
extern TinyGPSPlus         gps;
extern SemaphoreHandle_t   spi_mutex;

// LongFast modem params — identical to mesh_messenger.cpp.
#define MM_FREQ     906.875f
#define MM_BW       250.0f
#define MM_SF       11
#define MM_CR       5
#define MM_SYNC     0x2B
#define MM_PREAMBLE 16
#define MM_TX_UNUSED 2

#define MM_HDR_H 16

#ifdef DEVICE_NO_PSRAM
  #define MM_MAX_NODES 24
#else
  #define MM_MAX_NODES 96
#endif

struct __attribute__((packed)) MMHeader {
    uint32_t dest, from, id;
    uint8_t  flags, channel, nextHop, relayNode;
};

struct MeshNode {
    uint32_t nodeId;
    double   lat, lon;      // where we heard it strongest
    int16_t  bestRssi;
    uint8_t  minHops;
    uint32_t pkts;
    uint32_t lastMs;
};
static MeshNode s_nodes[MM_MAX_NODES];
static int      s_nodeCount = 0;

static SPISettings   s_set(2000000, MSBFIRST, SPI_MODE0);
static Module*       s_mod   = nullptr;
static SX1262*       s_radio = nullptr;
static volatile bool s_rxFlag = false;
static bool          s_ready = false;
static uint32_t      s_self = 0;

static void IRAM_ATTR mmISR() { s_rxFlag = true; }

static uint32_t selfId() {
    uint8_t m[6]; esp_read_mac(m, ESP_MAC_BT);
    return ((uint32_t)m[2]<<24)|((uint32_t)m[3]<<16)|((uint32_t)m[4]<<8)|m[5];
}

static bool radioInit() {
#ifdef DEVICE_CARDPUTER_ADV
    s_mod = new Module(PM_LORA_CS, PM_LORA_IRQ, PM_LORA_RST, PM_LORA_BUSY, cardputerSdSPI, s_set);
#else
    s_mod = new Module(PM_LORA_CS, PM_LORA_IRQ, PM_LORA_RST, PM_LORA_BUSY, SPI, s_set);
#endif
    s_radio = new SX1262(s_mod);
    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        delete s_radio; s_radio = nullptr; delete s_mod; s_mod = nullptr; return false;
    }
    int st = s_radio->begin(MM_FREQ, MM_BW, MM_SF, MM_CR, MM_SYNC, MM_TX_UNUSED, MM_PREAMBLE);
    if (st != RADIOLIB_ERR_NONE) {
        xSemaphoreGiveRecursive(spi_mutex);
        delete s_radio; s_radio = nullptr; delete s_mod; s_mod = nullptr; return false;
    }
    s_radio->setDio2AsRfSwitch(true);
    s_radio->setCurrentLimit(140.0f);
    s_radio->setRxBoostedGainMode(true);
    s_radio->setPacketReceivedAction(mmISR);
    s_rxFlag = false;
    int rx = s_radio->startReceive();
    xSemaphoreGiveRecursive(spi_mutex);
    if (rx != RADIOLIB_ERR_NONE) return false;
    s_self = selfId();
    s_ready = true;
    return true;
}

static void radioDeinit() {
    if (s_radio) {
        s_radio->clearPacketReceivedAction();
        if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_radio->standby(); xSemaphoreGiveRecursive(spi_mutex);
        }
        delete s_radio; s_radio = nullptr;
    }
    if (s_mod) { delete s_mod; s_mod = nullptr; }
    s_ready = false;
}

static int findNode(uint32_t id) {
    for (int i = 0; i < s_nodeCount; i++) if (s_nodes[i].nodeId == id) return i;
    return -1;
}

static void ingest(const uint8_t* buf, int len, int rssi) {
    if (len <= (int)sizeof(MMHeader)) return;
    const MMHeader* h = (const MMHeader*)buf;
    uint32_t from = h->from;
    if (from == 0 || from == s_self) return;
    if (!gps.location.isValid()) return;          // need a fix to place it

    int hopLimit = h->flags & 0x07;
    int hopStart = (h->flags >> 5) & 0x07;
    int hops = hopStart - hopLimit; if (hops < 0) hops = 0;

    int idx = findNode(from);
    if (idx < 0) {
        if (s_nodeCount >= MM_MAX_NODES) return;
        idx = s_nodeCount++;
        s_nodes[idx].nodeId = from;
        s_nodes[idx].bestRssi = -200;
        s_nodes[idx].minHops = 255;
        s_nodes[idx].pkts = 0;
    }
    MeshNode& n = s_nodes[idx];
    n.pkts++;
    n.lastMs = millis();
    if ((uint8_t)hops < n.minHops) n.minHops = (uint8_t)hops;
    if (rssi > n.bestRssi) {
        n.bestRssi = rssi;
        n.lat = gps.location.lat();
        n.lon = gps.location.lng();
    }
}

static void drain() {
    for (int p = 0; p < 8; p++) {
        if (!s_radio || !s_rxFlag) break;
        s_rxFlag = false;
        if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(20)) != pdTRUE) { s_rxFlag = true; break; }
        uint8_t buf[256];
        size_t rxLen = s_radio->getPacketLength();
        size_t rd = (rxLen > 0) ? min(rxLen, sizeof(buf)) : sizeof(buf);
        int st = s_radio->readData(buf, rd);
        int rssi = 0;
        if (st == RADIOLIB_ERR_NONE || st == RADIOLIB_ERR_CRC_MISMATCH) rssi = (int)s_radio->getRSSI();
        s_radio->startReceive();
        xSemaphoreGiveRecursive(spi_mutex);
        if (st == RADIOLIB_ERR_NONE && rxLen > sizeof(MMHeader) && rxLen <= sizeof(buf))
            ingest(buf, (int)rxLen, rssi);
    }
}

// RSSI -> color ramp (strong green .. mid amber .. weak red)
static uint16_t rssiColor(int rssi) {
    if (rssi >= -70) return 0x07E0;
    if (rssi >= -95) return 0xFD20;
    return 0xF800;
}
// crude path-loss range (meters) for direct packets
static double rangeM(int rssi) {
    double d = pow(10.0, (22.0 - 32.0 - (double)rssi) / (10.0 * 2.7));
    if (d < 1) d = 1; if (d > 50000) d = 50000;
    return d;
}

static void mmHeader() {
    int w = gfx->width();
    gfx->fillRect(0, 0, w, MM_HDR_H, 0x0000);
    gfx->drawFastHLine(0, MM_HDR_H - 1, w, 0x07E0);
    gfx->setTextSize(1);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(4, 4);
    gfx->print("MESH MAP");
    char r[28];
    snprintf(r, sizeof(r), "%d nodes %s", s_nodeCount, gps.location.isValid() ? "FIX" : "no-fix");
    int rw = (int)strlen(r) * 6;
    gfx->setTextColor(gps.location.isValid() ? 0x07E0 : 0xFD20);
    gfx->setCursor(w - rw - 4, 4);
    gfx->print(r);
}

static void drawNodes() {
    for (int i = 0; i < s_nodeCount; i++) {
        MeshNode& n = s_nodes[i];
        uint16_t c = rssiColor(n.bestRssi);
        // range ring only for direct (0-hop) packets where it's meaningful
        if (n.minHops == 0) pm_map_ring(n.lat, n.lon, rangeM(n.bestRssi), (c & 0x39E7));
        pm_map_marker(n.lat, n.lon, 4, c, 0xFFFF);
        char lbl[12];
        snprintf(lbl, sizeof(lbl), "%08x", (unsigned)n.nodeId);
        pm_map_label(n.lat, n.lon, 6, -3, lbl + 4, c);   // last 4 hex of id
    }
}

void run_mesh_map() {
    pm_map_begin("osm");
    int w = gfx->width(), h = gfx->height();
    pm_map_set_content_rect(0, MM_HDR_H, w, h - MM_HDR_H);
    if (gps.location.isValid())
        pm_map_set_view(gps.location.lat(), gps.location.lng(), 14);
    pm_map_recenter_gps();

    s_nodeCount = 0;

    gfx->fillScreen(0x0000);
    mmHeader();
    gfx->setTextColor(0xFFFF); gfx->setCursor(8, h/2 - 4);
    gfx->print("Starting SX1262...");

    if (!radioInit()) {
        gfx->fillScreen(0x0000); mmHeader();
        gfx->setTextColor(0xF800); gfx->setCursor(8, h/2 - 4);
        gfx->print("Radio init FAILED");
        gfx->setTextColor(0x4208); gfx->setCursor(8, h/2 + 10);
        gfx->print("Q to exit");
        while (true) {
            char k = get_keypress(); int16_t tx, ty;
            if (k=='q'||k=='Q'||k==27) break;
            if (get_touch(&tx,&ty)) { while (get_touch(&tx,&ty)) delay(8); break; }
            delay(40);
        }
        radioDeinit(); pm_map_end(); gfx->fillScreen(0x0000); return;
    }

    bool running = true, needRedraw = true;
    uint32_t lastUi = 0;
    int16_t dragLastX = -1, dragLastY = -1; bool dragging = false;

    while (running) {
        drain();

        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty; bool touched = get_touch(&tx, &ty);

        if (touched && ty < MM_HDR_H) { while (get_touch(&tx,&ty)) delay(8); running = false; continue; }
        if (touched) {
            if (!dragging) { dragging = true; dragLastX = tx; dragLastY = ty; }
            else { int dx = tx-dragLastX, dy = ty-dragLastY;
                   if (dx||dy){ pm_map_handle_pan(dx,dy); dragLastX=tx; dragLastY=ty; needRedraw=true; } }
        } else dragging = false;

        if (tb.x || tb.y) { pm_map_handle_pan(-tb.x*24, -tb.y*24); needRedraw = true; }
        if (tb.clicked)   { pm_map_recenter_gps(); needRedraw = true; }

        if (k) switch (k) {
            case 'q': case 'Q': case 27: running = false; break;
            case '+': case '=': case '.': pm_map_zoom_in();  needRedraw = true; break;
            case '-': case '_': case ',': pm_map_zoom_out(); needRedraw = true; break;
            case 'w': case 'W': pm_map_handle_pan(0,  40); needRedraw = true; break;
            case 's': case 'S': pm_map_handle_pan(0, -40); needRedraw = true; break;
            case 'a': case 'A': pm_map_handle_pan( 40, 0); needRedraw = true; break;
            case 'd': case 'D': pm_map_handle_pan(-40, 0); needRedraw = true; break;
            case 'g': case 'G': pm_map_recenter_gps(); needRedraw = true; break;
            case 'c': case 'C': s_nodeCount = 0; needRedraw = true; break;
            default: break;
        }

        if (pm_map_follow() && gps.location.isValid()) needRedraw = true;
        if (millis() - lastUi > 1000) needRedraw = true;   // refresh node ages/fix

        if (needRedraw) {
            pm_map_draw_basemap();
            drawNodes();
            pm_map_draw_hud("MESH MAP");
            mmHeader();
            needRedraw = false;
            lastUi = millis();
        }

        delay(15);
        yield();
    }

    radioDeinit();
    pm_map_end();
    gfx->fillScreen(0x0000);
}

#endif // SX1262 LoRa devices only
