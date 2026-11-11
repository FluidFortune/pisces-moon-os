// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// tembed_ble_radar.cpp — nearby BLE devices, strongest first (T-Embed)
//
// Passive-ish radar: a continuous NimBLE scan feeds a small device table;
// the list is sorted by current RSSI and refreshes twice a second. Known
// tracker beacons are tagged (Apple Find My, Samsung SmartTag, Tile), and
// common vendors are named from the advertised company ID.
//
// PUSH on a device opens TRACK view: a big smoothed RSSI readout with a
// closer/farther trend, for hot/cold locating one device. Receive only —
// no connections, no pairing.
//
// The NimBLE scan callback runs on the BLE host task, so the table is
// guarded by a spinlock and the callback copies what it needs first.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "tembed_ui.h"
#include "tembed_input.h"

using namespace tui;

namespace {

struct Dev {
    bool     used;
    uint8_t  mac[6];
    uint8_t  addrType;
    char     name[21];
    int8_t   rssi;          // latest
    int8_t   rssiMax;
    float    rssiAvg;       // exponential moving average
    uint16_t mfg;           // company ID, 0xFFFF = none
    uint8_t  tag;           // Tag enum
    uint32_t firstSeen, lastSeen, advCount;
};

enum Tag : uint8_t { TAG_NONE = 0, TAG_FINDMY, TAG_SMARTTAG, TAG_TILE, TAG_AIRPODS, TAG_APPLE_NEARBY };

constexpr int  kMaxDev   = 96;
constexpr uint32_t kStaleMs = 30000;
Dev          g_dev[kMaxDev];
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_advTotal = 0;

const char* vendorName(uint16_t id) {
    switch (id) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x0075: return "Samsung";
        case 0x00E0: return "Google";
        case 0x0087: return "Garmin";
        case 0x0157: return "Huami";
        case 0x038F: return "Xiaomi";
        case 0x0059: return "Nordic";
        case 0x02E5: return "Espressif";
        case 0x0171: return "Amazon";
        case 0x000F: return "Broadcom";
        case 0x0499: return "Ruuvi";
        case 0x0822: return "Govee";
        case 0x0131: return "Cypress";
        case 0x01DA: return "Logitech";
        case 0x00D2: return "Bose";
        case 0x012D: return "Sony";
        case 0x0310: return "SGL";
        default:     return nullptr;
    }
}

const char* tagName(uint8_t t) {
    switch (t) {
        case TAG_FINDMY:       return "FindMy";
        case TAG_SMARTTAG:     return "SmartTag";
        case TAG_TILE:         return "Tile";
        case TAG_AIRPODS:      return "AirPods";
        case TAG_APPLE_NEARBY: return "iPhone/Mac";
        default:               return nullptr;
    }
}

class RadarCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* d) override {
        // Gather everything outside the lock (std::string allocates).
        uint8_t mac[6];
        memcpy(mac, d->getAddress().getNative(), 6);
        uint8_t atype = d->getAddressType();
        int rssi = d->getRSSI();
        char name[21] = {0};
        if (d->haveName()) { std::string n = d->getName(); strncpy(name, n.c_str(), 20); }
        uint16_t mfg = 0xFFFF;
        uint8_t tag = TAG_NONE;
        if (d->haveManufacturerData()) {
            std::string m = d->getManufacturerData();
            if (m.size() >= 2) mfg = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
            if (mfg == 0x004C && m.size() >= 3) {
                uint8_t type = (uint8_t)m[2];
                if (type == 0x12)      tag = TAG_FINDMY;      // offline finding
                else if (type == 0x07) tag = TAG_AIRPODS;     // proximity pairing
                else if (type == 0x10) tag = TAG_APPLE_NEARBY;
            }
        }
        if (tag == TAG_NONE && d->haveServiceUUID()) {
            if (d->isAdvertisingService(NimBLEUUID((uint16_t)0xFD5A))) tag = TAG_SMARTTAG;
            else if (d->isAdvertisingService(NimBLEUUID((uint16_t)0xFEED)) ||
                     d->isAdvertisingService(NimBLEUUID((uint16_t)0xFEEC))) tag = TAG_TILE;
        }
        uint32_t now = millis();

        portENTER_CRITICAL(&g_mux);
        g_advTotal++;
        int slot = -1, freeSlot = -1, oldest = 0;
        for (int i = 0; i < kMaxDev; i++) {
            if (!g_dev[i].used) { if (freeSlot < 0) freeSlot = i; continue; }
            if (memcmp(g_dev[i].mac, mac, 6) == 0) { slot = i; break; }
            if (g_dev[i].lastSeen < g_dev[oldest].lastSeen || !g_dev[oldest].used) oldest = i;
        }
        if (slot < 0) {
            slot = freeSlot >= 0 ? freeSlot : oldest;
            Dev& n = g_dev[slot];
            memset(&n, 0, sizeof(n));
            n.used = true;
            memcpy(n.mac, mac, 6);
            n.firstSeen = now;
            n.rssiMax = -127;
            n.rssiAvg = rssi;
            n.mfg = 0xFFFF;
        }
        Dev& e = g_dev[slot];
        e.addrType = atype;
        e.rssi = (int8_t)rssi;
        if (rssi > e.rssiMax) e.rssiMax = (int8_t)rssi;
        e.rssiAvg = e.rssiAvg * 0.7f + rssi * 0.3f;
        if (name[0]) memcpy(e.name, name, sizeof(e.name));
        if (mfg != 0xFFFF) e.mfg = mfg;
        if (tag != TAG_NONE) e.tag = tag;
        e.lastSeen = now;
        e.advCount++;
        portEXIT_CRITICAL(&g_mux);
    }
};

RadarCB g_cb;

// Sorted snapshot for drawing (copied out of the live table).
Dev g_view[kMaxDev];
int g_viewCount = 0;

void snapshot() {
    uint32_t now = millis();
    portENTER_CRITICAL(&g_mux);
    g_viewCount = 0;
    for (int i = 0; i < kMaxDev; i++) {
        if (!g_dev[i].used) continue;
        if (now - g_dev[i].lastSeen > kStaleMs) { g_dev[i].used = false; continue; }
        g_view[g_viewCount++] = g_dev[i];
    }
    portEXIT_CRITICAL(&g_mux);
    for (int a = 1; a < g_viewCount; a++)
        for (int b = a; b > 0 && g_view[b].rssiAvg > g_view[b - 1].rssiAvg; b--) {
            Dev t = g_view[b]; g_view[b] = g_view[b - 1]; g_view[b - 1] = t;
        }
}

void macStr(const uint8_t* m, char* out) {
    // NimBLE stores the address little-endian; print it the usual way.
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[5], m[4], m[3], m[2], m[1], m[0]);
}

// ── List view ────────────────────────────────────────────────────
constexpr int kRowY = 32, kRowH = 24, kVisible = 5;
int     g_sel = 0, g_top = 0;
uint8_t g_selMac[6];
bool    g_haveSel = false;

void drawList() {
    // Keep the highlight on the same device across re-sorts.
    if (g_haveSel) {
        for (int i = 0; i < g_viewCount; i++)
            if (memcmp(g_view[i].mac, g_selMac, 6) == 0) { g_sel = i; break; }
    }
    if (g_sel >= g_viewCount) g_sel = g_viewCount ? g_viewCount - 1 : 0;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + kVisible) g_top = g_sel - kVisible + 1;
    if (g_viewCount) { memcpy(g_selMac, g_view[g_sel].mac, 6); g_haveSel = true; }

    char tag[24];
    snprintf(tag, sizeof(tag), "%d dev", g_viewCount);
    headerTag(tag, HI);

    uint32_t now = millis();
    clearBand(kRowY - 2, kVisible * kRowH + 2);
    if (g_viewCount == 0) {
        gfx->setTextSize(2); gfx->setTextColor(DIM);
        gfx->setCursor(6, 70); gfx->print("Listening...");
        return;
    }
    for (int r = 0; r < kVisible; r++) {
        int idx = g_top + r;
        if (idx >= g_viewCount) break;
        const Dev& d = g_view[idx];
        int y = kRowY + r * kRowH;
        bool sel = idx == g_sel;
        bool stale = now - d.lastSeen > 8000;
        if (sel) gfx->fillRect(2, y - 1, 316, kRowH - 2, SEL);

        char label[22];
        if (d.name[0]) { strncpy(label, d.name, 14); label[14] = 0; }
        else { char m[18]; macStr(d.mac, m); strncpy(label, m + 6, 14); label[14] = 0; }  // last 4 octets
        gfx->setTextSize(2);
        gfx->setTextColor(sel ? TXT : (stale ? DIM : (d.name[0] ? TXT : HDR)));
        gfx->setCursor(8, y + 3);
        gfx->print(label);

        const char* t = tagName(d.tag);
        const char* v = vendorName(d.mfg);
        gfx->setTextSize(1);
        gfx->setCursor(184, y + 1);
        if (t) { gfx->setTextColor(sel ? TXT : MAG); gfx->print(t); }
        else if (v) { gfx->setTextColor(sel ? TXT : SUBT); gfx->print(v); }
        gfx->setCursor(184, y + 11);
        gfx->setTextColor(sel ? TXT : DIM);
        gfx->print(d.addrType == BLE_ADDR_RANDOM ? "rand" : "pub");

        gfx->setTextSize(2);
        gfx->setTextColor(sel ? TXT : (d.rssi > -60 ? GOOD : d.rssi > -80 ? WARN : BAD));
        gfx->setCursor(240, y + 3);
        gfx->printf("%4d", (int)d.rssiAvg);
        rssiBars(300, y + 4, (int)d.rssiAvg);
    }
}

// ── Track view ───────────────────────────────────────────────────
bool findDev(const uint8_t* mac, Dev& out) {
    bool found = false;
    portENTER_CRITICAL(&g_mux);
    for (int i = 0; i < kMaxDev; i++)
        if (g_dev[i].used && memcmp(g_dev[i].mac, mac, 6) == 0) { out = g_dev[i]; found = true; break; }
    portEXIT_CRITICAL(&g_mux);
    return found;
}

void runTrack(const uint8_t* mac) {
    header("BLE TRACK");
    footer("Walk around: bigger number = closer.  PUSH/SIDE=back");
    tembed_input_flush();

    // Static identity line, drawn once.
    Dev d;
    char m[18]; macStr(mac, m);
    gfx->setTextSize(1); gfx->setTextColor(SUBT);
    gfx->setCursor(6, 30); gfx->print(m);
    if (findDev(mac, d)) {
        if (d.name[0]) { gfx->setTextColor(TXT); gfx->setCursor(120, 30); gfx->print(d.name); }
        const char* t = tagName(d.tag); const char* v = vendorName(d.mfg);
        gfx->setTextColor(t ? MAG : SUBT); gfx->setCursor(250, 30);
        gfx->print(t ? t : (v ? v : ""));
    }
    gfx->setTextSize(2); gfx->setTextColor(HDR);
    gfx->setCursor(160, 66); gfx->print("dBm");

    float hist[8]; int hn = 0;     // ~1 s of 120 ms samples
    float baseline = 0; bool haveBase = false;
    int trend = 0, lastTrendShown = 99;
    uint32_t lastDraw = 0, lastTrend = millis();
    bool wasLost = false;

    for (;;) {
        if (tembed_btn_back() || tembed_btn_select()) break;
        uint32_t now = millis();
        if (now - lastDraw < 120) { delay(10); continue; }
        lastDraw = now;

        if (!findDev(mac, d)) {
            if (!wasLost) {
                clearBand(44, 110);
                gfx->setTextSize(2); gfx->setTextColor(BAD);
                gfx->setCursor(6, 70); gfx->print("Lost (not heard 30 s)");
                wasLost = true;
            }
            continue;
        }
        if (wasLost) {                 // came back: restore the layout
            clearBand(44, 110);
            gfx->setTextSize(2); gfx->setTextColor(HDR);
            gfx->setCursor(160, 66); gfx->print("dBm");
            wasLost = false; lastTrendShown = 99;
        }

        hist[hn % 8] = d.rssiAvg; hn++;
        int nh = hn < 8 ? hn : 8; float sum = 0;
        for (int i = 0; i < nh; i++) sum += hist[i];
        float avg = sum / nh;
        uint32_t age = now - d.lastSeen;
        uint16_t col = age > 3000 ? DIM : (avg > -60 ? GOOD : avg > -80 ? WARN : BAD);

        gfx->setTextSize(6);
        gfx->setTextColor(col, BG);
        gfx->setCursor(10, 46);
        gfx->printf("%4d", (int)avg);

        if (!haveBase) { baseline = avg; haveBase = true; lastTrend = now; }
        if (now - lastTrend > 1500) {
            float delta = avg - baseline;
            trend = delta > 3 ? 1 : (delta < -3 ? -1 : 0);
            baseline = avg; lastTrend = now;
        }
        if (trend != lastTrendShown) {
            lastTrendShown = trend;
            gfx->setTextSize(2);
            gfx->setCursor(220, 52);
            if (trend > 0)      { gfx->setTextColor(GOOD, BG);  gfx->print("CLOSER "); }
            else if (trend < 0) { gfx->setTextColor(BAD, BG); gfx->print("FARTHER"); }
            else                { gfx->setTextColor(DIM, BG); gfx->print("steady "); }
        }

        meter(6, 104, 308, 16, avg, -100, -30, col);
        gfx->setTextSize(1); gfx->setTextColor(DIM, BG);
        gfx->setCursor(6, 128);
        gfx->printf("max %4d   heard %5lu ms ago   %6lu adv  ",
                    (int)d.rssiMax, (unsigned long)age, (unsigned long)d.advCount);
        gfx->setCursor(6, 140);
        gfx->print(d.addrType == BLE_ADDR_RANDOM ? "random address (may rotate)" : "public address             ");
    }
    tembed_input_flush();
}

bool startScan() {
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init("");
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return false;
    if (scan->isScanning()) scan->stop();
    scan->setAdvertisedDeviceCallbacks(&g_cb, true);   // want duplicates = live RSSI
    scan->setActiveScan(true);                         // ask for scan responses (names)
    scan->setInterval(100);
    scan->setWindow(90);
    scan->setMaxResults(0);                            // callbacks only, keep no list
    return scan->start(0, nullptr, false);             // 0 = until stopped
}

void stopScan() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return;
    scan->stop();
    scan->setAdvertisedDeviceCallbacks(nullptr, false);
    scan->clearResults();
}

} // namespace

void tembed_ble_radar_run() {
    header("BLE RADAR");
    footer("TURN=select  PUSH=track  SIDE=back");
    tembed_input_flush();

    portENTER_CRITICAL(&g_mux);
    for (int i = 0; i < kMaxDev; i++) g_dev[i].used = false;
    portEXIT_CRITICAL(&g_mux);
    g_sel = g_top = 0; g_haveSel = false;

    if (!startScan()) {
        gfx->setTextSize(2); gfx->setTextColor(BAD);
        gfx->setCursor(6, 70); gfx->print("BLE scan failed to start");
        footer("SIDE=back");
        while (!tembed_btn_back()) delay(10);
        return;
    }

    uint32_t lastDraw = 0;
    for (;;) {
        if (tembed_btn_back()) break;
        int d = tembed_encoder_delta();
        if (d != 0 && g_viewCount) {
            g_sel = ((g_sel + d) % g_viewCount + g_viewCount) % g_viewCount;
            memcpy(g_selMac, g_view[g_sel].mac, 6);
            drawList();
        }
        if (tembed_btn_select() && g_viewCount) {
            uint8_t mac[6]; memcpy(mac, g_view[g_sel].mac, 6);
            runTrack(mac);
            header("BLE RADAR");
            footer("TURN=select  PUSH=track  SIDE=back");
            lastDraw = 0;
        }
        if (millis() - lastDraw > 500) {
            lastDraw = millis();
            snapshot();
            drawList();
        }
        delay(5);
    }
    stopScan();
}

#endif // DEVICE_TEMBED_CC1101
