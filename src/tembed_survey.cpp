// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// tembed_survey.cpp — Stationary Survey (T-Embed)
//
// Sit in one spot and characterize the RF around it. Each pass runs:
//   1. WiFi scan     (async, all channels)            ~2-4 s
//   2. BLE listen    (NimBLE scan, duplicates on)       4 s
//   3. Sub-GHz sweep (CC1101 RSSI at 315/390/433/868) ~2 s
// and folds the results into unique-device tables, so the totals show
// how busy the spot is and the per-pass numbers show what's here now.
//
// Receive only. With an SD card each pass is appended to
// /survey/<date>_<time>.csv (or boot_<ms>.csv before the clock is set):
//   pass,uptime_s,epoch,type,id,name,rssi,channel,detail
//
// UI: TURN = page (Summary / WiFi / BLE / Sub-GHz), PUSH = pause/resume,
// SIDE = stop (shows the totals and the log path; SIDE again exits).
//
// The loop is a small state machine so the wheel and buttons stay live
// during every phase; nothing blocks longer than a single RSSI read.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "tembed_ui.h"
#include "tembed_input.h"
#include "cc1101_analyzer.h"
#include "pm_storage.h"

using namespace tui;

extern volatile bool g_sd_ready;

namespace {

// ── Tables (PSRAM) ───────────────────────────────────────────────
struct Ap {
    uint8_t  bssid[6];
    char     ssid[33];
    int8_t   best, passBest;
    uint8_t  ch, enc;
    uint16_t seenPasses, lastPass;
};
struct BleE {
    uint8_t  mac[6];
    char     name[21];
    int8_t   best, passBest;
    uint8_t  addrType;
    uint16_t mfg;
    uint16_t seenPasses, lastPass;
};
struct Sub {
    float    passAvg, passPeak;          // this pass
    float    sumAvg;  int nPass;         // across passes
    float    peakMax, floorMin;
    uint16_t activePasses;               // passes with a burst well above floor
    // per-pass accumulators
    float    acc; int accN; float accPeak, accMin;
};

constexpr int kMaxAp  = 256;
constexpr int kMaxBle = 512;
constexpr int kMaxSub = 8;

Ap*   g_ap  = nullptr; int g_apN  = 0;
BleE* g_ble = nullptr; int g_bleN = 0;
Sub   g_sub[kMaxSub];  int g_subN = 0;
bool  g_subOk = false;

portMUX_TYPE      g_mux  = portMUX_INITIALIZER_UNLOCKED;
volatile uint16_t g_pass = 0;
volatile int      g_blePassCount = 0;     // devices seen this pass

template <typename T> T* allocTable(int n) {
    void* p = heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(n, sizeof(T));
    return (T*)p;
}

// ── BLE ingest (runs on the NimBLE host task) ────────────────────
class SurveyCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* d) override {
        uint8_t mac[6];
        memcpy(mac, d->getAddress().getNative(), 6);
        int rssi = d->getRSSI();
        uint8_t atype = d->getAddressType();
        char name[21] = {0};
        if (d->haveName()) { std::string n = d->getName(); strncpy(name, n.c_str(), 20); }
        uint16_t mfg = 0xFFFF;
        if (d->haveManufacturerData()) {
            std::string m = d->getManufacturerData();
            if (m.size() >= 2) mfg = (uint8_t)m[0] | ((uint8_t)m[1] << 8);
        }
        portENTER_CRITICAL(&g_mux);
        if (g_ble) {
            int slot = -1;
            for (int i = 0; i < g_bleN; i++)
                if (memcmp(g_ble[i].mac, mac, 6) == 0) { slot = i; break; }
            if (slot < 0 && g_bleN < kMaxBle) {
                slot = g_bleN++;
                BleE& n = g_ble[slot];
                memset(&n, 0, sizeof(n));
                memcpy(n.mac, mac, 6);
                n.best = -127; n.mfg = 0xFFFF; n.lastPass = 0xFFFF;
            }
            if (slot >= 0) {
                BleE& e = g_ble[slot];
                if (e.lastPass != g_pass) {
                    e.lastPass = g_pass; e.seenPasses++; e.passBest = -127; g_blePassCount++;
                }
                if (rssi > e.best)     e.best = rssi;
                if (rssi > e.passBest) e.passBest = rssi;
                e.addrType = atype;
                if (name[0]) memcpy(e.name, name, sizeof(e.name));
                if (mfg != 0xFFFF) e.mfg = mfg;
            }
        }
        portEXIT_CRITICAL(&g_mux);
    }
};
SurveyCB g_bleCb;

// ── WiFi ingest ──────────────────────────────────────────────────
int g_wifiPassCount = 0;

void ingestWifi(int n) {
    g_wifiPassCount = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t* b = WiFi.BSSID(i);
        if (!b) continue;
        int slot = -1;
        for (int k = 0; k < g_apN; k++)
            if (memcmp(g_ap[k].bssid, b, 6) == 0) { slot = k; break; }
        if (slot < 0) {
            if (g_apN >= kMaxAp) continue;
            slot = g_apN++;
            memset(&g_ap[slot], 0, sizeof(Ap));
            memcpy(g_ap[slot].bssid, b, 6);
            g_ap[slot].best = -127; g_ap[slot].lastPass = 0xFFFF;
        }
        Ap& e = g_ap[slot];
        int rssi = WiFi.RSSI(i);
        if (e.lastPass != g_pass) { e.lastPass = g_pass; e.seenPasses++; e.passBest = -127; g_wifiPassCount++; }
        if (rssi > e.best)     e.best = rssi;
        if (rssi > e.passBest) e.passBest = rssi;
        e.ch  = WiFi.channel(i);
        e.enc = (uint8_t)WiFi.encryptionType(i);
        String s = WiFi.SSID(i);
        if (s.length()) { strncpy(e.ssid, s.c_str(), 32); e.ssid[32] = 0; }
    }
}

const char* encName(uint8_t e) {
    switch ((wifi_auth_mode_t)e) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
        default:                        return "?";
    }
}

// ── Logging ──────────────────────────────────────────────────────
char     g_logPath[48] = {0};
bool     g_logOk = false;
uint32_t g_rows = 0;

void csvField(pm_storage::File& f, const char* s) {
    f.print("\"");
    for (const char* p = s; *p; p++) {
        if (*p == '"') f.print("\"\"");
        else if (*p == '\n' || *p == '\r') f.print(" ");
        else f.write((uint8_t)*p);
    }
    f.print("\"");
}

void openLog() {
    g_logOk = false; g_rows = 0; g_logPath[0] = 0;
    if (!g_sd_ready) return;
    if (time(nullptr) > 1700000000) {
        time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
        strftime(g_logPath, sizeof(g_logPath), "/survey/%Y%m%d_%H%M%S.csv", &lt);
    } else {
        snprintf(g_logPath, sizeof(g_logPath), "/survey/boot_%lu.csv", (unsigned long)millis());
    }
    if (!busTake(1000)) return;
    if (!pm_storage::exists("/survey")) pm_storage::mkdir("/survey");
    pm_storage::File f = pm_storage::open(g_logPath, pm_storage::Mode::Write);
    if (f) {
        f.println("pass,uptime_s,epoch,type,id,name,rssi,channel,detail");
        f.close();
        g_logOk = true;
    }
    busGive();
}

void macStr(const uint8_t* m, char* out, bool reversed) {
    if (reversed) sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[5], m[4], m[3], m[2], m[1], m[0]);
    else          sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

void logPass() {
    if (!g_logOk) return;
    if (!busTake(2000)) return;
    pm_storage::File f = pm_storage::open(g_logPath, pm_storage::Mode::Append);
    if (!f) { busGive(); return; }
    unsigned long up = millis() / 1000;
    time_t epoch = time(nullptr);
    char ep[16] = "";
    if (epoch > 1700000000) snprintf(ep, sizeof(ep), "%ld", (long)epoch);
    char id[18];
    for (int i = 0; i < g_apN; i++) {
        const Ap& e = g_ap[i];
        if (e.lastPass != g_pass) continue;
        macStr(e.bssid, id, false);
        f.printf("%u,%lu,%s,wifi,%s,", (unsigned)g_pass, up, ep, id);
        csvField(f, e.ssid);
        f.printf(",%d,%u,%s\n", e.passBest, e.ch, encName(e.enc));
        g_rows++;
    }
    // Copy BLE rows under the lock, write outside it.
    for (int i = 0; i < g_bleN; i++) {
        BleE e;
        portENTER_CRITICAL(&g_mux); e = g_ble[i]; portEXIT_CRITICAL(&g_mux);
        if (e.lastPass != g_pass) continue;
        macStr(e.mac, id, true);
        f.printf("%u,%lu,%s,ble,%s,", (unsigned)g_pass, up, ep, id);
        csvField(f, e.name);
        if (e.mfg != 0xFFFF) f.printf(",%d,,mfg=0x%04X\n", e.passBest, e.mfg);
        else                 f.printf(",%d,,\n", e.passBest);
        g_rows++;
    }
    for (int b = 0; b < g_subN; b++) {
        const Sub& s = g_sub[b];
        f.printf("%u,%lu,%s,subghz,%s,,%.0f,,peak=%.0f;floor=%.0f\n",
                 (unsigned)g_pass, up, ep, cc1101_survey_label(b), s.passAvg, s.passPeak, s.accMin);
        g_rows++;
    }
    f.close();
    busGive();
}

// ── State machine ────────────────────────────────────────────────
enum Phase { PH_WIFI_START, PH_WIFI_WAIT, PH_BLE_START, PH_BLE_WAIT, PH_SUB, PH_LOG };
Phase    g_phase = PH_WIFI_START;
uint32_t g_phaseT0 = 0;
int      g_subBand = 0;
bool     g_paused = false;
uint32_t g_startMs = 0;

constexpr uint32_t kBleMs     = 4000;
constexpr uint32_t kSubBandMs = 450;
constexpr uint32_t kWifiMaxMs = 9000;

const char* phaseName() {
    if (g_paused && g_phase == PH_WIFI_START) return "PAUSED";
    switch (g_phase) {
        case PH_WIFI_START: case PH_WIFI_WAIT: return "WiFi scan";
        case PH_BLE_START:  case PH_BLE_WAIT:  return "BLE listen";
        case PH_SUB:  return "Sub-GHz sweep";
        case PH_LOG:  return "logging";
    }
    return "";
}

void bleStart() {
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init("");
    NimBLEScan* s = NimBLEDevice::getScan();
    if (s->isScanning()) s->stop();
    g_blePassCount = 0;
    s->setAdvertisedDeviceCallbacks(&g_bleCb, true);
    s->setActiveScan(true);
    s->setInterval(100);
    s->setWindow(90);
    s->setMaxResults(0);
    s->start(0, nullptr, false);
}

void bleStop() {
    NimBLEScan* s = NimBLEDevice::getScan();
    if (!s) return;
    s->stop();
    s->setAdvertisedDeviceCallbacks(nullptr, false);
    s->clearResults();
}

void subPassBegin() {
    for (int b = 0; b < g_subN; b++) {
        Sub& s = g_sub[b];
        s.acc = 0; s.accN = 0; s.accPeak = -127; s.accMin = 0;
    }
}

void subBandFinish(int b) {
    Sub& s = g_sub[b];
    if (s.accN == 0) return;
    s.passAvg  = s.acc / s.accN;
    s.passPeak = s.accPeak;
    s.sumAvg  += s.passAvg; s.nPass++;
    if (s.passPeak > s.peakMax) s.peakMax = s.passPeak;
    if (s.nPass == 1 || s.accMin < s.floorMin) s.floorMin = s.accMin;
    if (s.passPeak - s.passAvg > 15) s.activePasses++;   // a burst well above the floor
}

// Advance the survey by one small step. Never blocks for long.
void step() {
    uint32_t now = millis();
    switch (g_phase) {
    case PH_WIFI_START:
        if (g_paused) return;
        g_pass++;
        WiFi.mode(WIFI_STA);
        WiFi.scanDelete();
        WiFi.scanNetworks(true, true, false, 120);
        g_phase = PH_WIFI_WAIT; g_phaseT0 = now;
        break;
    case PH_WIFI_WAIT: {
        int n = WiFi.scanComplete();
        if (n >= 0) { ingestWifi(n); WiFi.scanDelete(); g_phase = PH_BLE_START; }
        else if (n == WIFI_SCAN_FAILED || now - g_phaseT0 > kWifiMaxMs) { WiFi.scanDelete(); g_phase = PH_BLE_START; }
        break;
    }
    case PH_BLE_START:
        bleStart();
        g_phase = PH_BLE_WAIT; g_phaseT0 = now;
        break;
    case PH_BLE_WAIT:
        if (now - g_phaseT0 > kBleMs) {
            bleStop();
            g_subBand = 0;
            subPassBegin();
            if (g_subOk) cc1101_survey_tune(0);
            g_phase = PH_SUB; g_phaseT0 = now;
        }
        break;
    case PH_SUB: {
        if (!g_subOk) { g_phase = PH_LOG; break; }
        Sub& s = g_sub[g_subBand];
        if (now - g_phaseT0 > 3) {                 // let RSSI settle after tune
            float r = cc1101_survey_rssi();
            if (r > -126) {
                s.acc += r; s.accN++;
                if (r > s.accPeak) s.accPeak = r;
                if (s.accN == 1 || r < s.accMin) s.accMin = r;
            }
        }
        if (now - g_phaseT0 > kSubBandMs) {
            subBandFinish(g_subBand);
            if (++g_subBand >= g_subN) { g_phase = PH_LOG; break; }
            cc1101_survey_tune(g_subBand);
            g_phaseT0 = now;
        }
        break;
    }
    case PH_LOG:
        logPass();
        g_phase = PH_WIFI_START;
        break;
    }
}

// ── Rendering ────────────────────────────────────────────────────
int g_page = 0;
constexpr int kPages = 4;
const char* const kPageName[kPages] = { "SURVEY", "SURVEY: WIFI", "SURVEY: BLE", "SURVEY: SUB-GHZ" };

void drawPageChrome() {
    header(kPageName[g_page]);
    footer("TURN=page  PUSH=pause  SIDE=stop");
}

void drawTag() {
    char t[24];
    snprintf(t, sizeof(t), "pass %u", (unsigned)g_pass);
    headerTag(t, g_paused ? WARN : HI);
}

void drawSummary() {
    clearBand(28, 126);
    uint32_t el = (millis() - g_startMs) / 1000;
    gfx->setTextSize(1); gfx->setTextColor(SUBT);
    gfx->setCursor(6, 30);
    gfx->printf("%02lu:%02lu elapsed   now: %s", (unsigned long)(el / 60), (unsigned long)(el % 60), phaseName());

    // Strongest AP and busiest 2.4 GHz channel.
    int bestAp = -1; int chCount[15] = {0};
    for (int i = 0; i < g_apN; i++) {
        if (bestAp < 0 || g_ap[i].best > g_ap[bestAp].best) bestAp = i;
        if (g_ap[i].ch >= 1 && g_ap[i].ch <= 14) chCount[g_ap[i].ch]++;
    }
    int busyCh = 0; for (int c = 1; c <= 14; c++) if (chCount[c] > chCount[busyCh]) busyCh = c;

    gfx->setTextSize(2);
    gfx->setTextColor(HI);  gfx->setCursor(6, 44);  gfx->printf("WiFi %3d", g_apN);
    gfx->setTextColor(TXT); gfx->setTextSize(1);    gfx->setCursor(110, 48);
    gfx->printf("APs   +%d this pass", g_wifiPassCount);

    int bleN; portENTER_CRITICAL(&g_mux); bleN = g_bleN; portEXIT_CRITICAL(&g_mux);
    gfx->setTextSize(2);
    gfx->setTextColor(HI);  gfx->setCursor(6, 66);  gfx->printf("BLE  %3d", bleN);
    gfx->setTextColor(TXT); gfx->setTextSize(1);    gfx->setCursor(110, 70);
    gfx->printf("dev   +%d this pass", (int)g_blePassCount);

    gfx->setTextSize(2); gfx->setTextColor(HI);
    gfx->setCursor(6, 88); gfx->print("Sub");
    gfx->setTextSize(1);
    if (!g_subOk) { gfx->setTextColor(BAD); gfx->setCursor(60, 92); gfx->print("CC1101 not available"); }
    for (int b = 0; b < g_subN && g_subOk; b++) {
        const Sub& s = g_sub[b];
        bool act = s.nPass && (s.peakMax - s.sumAvg / s.nPass > 15);
        gfx->setTextColor(act ? WARN : DIM);
        gfx->setCursor(52 + b * 66, 88);  gfx->print(cc1101_survey_label(b));
        gfx->setCursor(52 + b * 66, 98);
        if (s.nPass) gfx->printf("pk %d", (int)s.peakMax); else gfx->print("--");
    }

    gfx->setTextColor(SUBT);
    gfx->setCursor(6, 114);
    if (bestAp >= 0) gfx->printf("strongest AP: %.18s %d", g_ap[bestAp].ssid[0] ? g_ap[bestAp].ssid : "(hidden)", g_ap[bestAp].best);
    gfx->setCursor(6, 126);
    if (busyCh) gfx->printf("busiest channel: %d (%d APs)", busyCh, chCount[busyCh]);
    gfx->setCursor(6, 140);
    if (g_logOk)          { gfx->setTextColor(GOOD);   gfx->printf("SD %s  %lu rows", g_logPath, (unsigned long)g_rows); }
    else if (g_sd_ready)  { gfx->setTextColor(BAD);  gfx->print("SD log could not be opened"); }
    else                  { gfx->setTextColor(WARN); gfx->print("No SD card: live view only, nothing saved"); }
}

void drawWifiPage() {
    clearBand(28, 126);
    // Top 9 by best RSSI (selection on a small index array).
    int idx[9]; int n = 0;
    for (int i = 0; i < g_apN; i++) {
        int pos = n < 9 ? n++ : 9;
        if (pos == 9 && g_ap[i].best <= g_ap[idx[8]].best) continue;
        if (pos == 9) pos = 8;
        idx[pos] = i;
        for (int k = pos; k > 0 && g_ap[idx[k]].best > g_ap[idx[k - 1]].best; k--) { int t = idx[k]; idx[k] = idx[k - 1]; idx[k - 1] = t; }
    }
    gfx->setTextSize(1);
    for (int r = 0; r < n; r++) {
        const Ap& e = g_ap[idx[r]];
        bool now = e.lastPass == g_pass;
        gfx->setTextColor(now ? TXT : DIM);
        gfx->setCursor(6, 32 + r * 13);
        gfx->printf("%-22.22s %4d  ch%-2u %-6s x%u", e.ssid[0] ? e.ssid : "(hidden)", e.best, e.ch, encName(e.enc), e.seenPasses);
    }
    if (!n) { gfx->setTextColor(DIM); gfx->setCursor(6, 70); gfx->print("No access points yet."); }
}

void drawBlePage() {
    clearBand(28, 126);
    int n = 0; BleE top[9];
    portENTER_CRITICAL(&g_mux);
    for (int i = 0; i < g_bleN; i++) {
        const BleE& e = g_ble[i];
        int pos = n < 9 ? n++ : 9;
        if (pos == 9 && e.best <= top[8].best) continue;
        if (pos == 9) pos = 8;
        top[pos] = e;
        for (int k = pos; k > 0 && top[k].best > top[k - 1].best; k--) { BleE t = top[k]; top[k] = top[k - 1]; top[k - 1] = t; }
    }
    portEXIT_CRITICAL(&g_mux);
    gfx->setTextSize(1);
    char m[18];
    for (int r = 0; r < n; r++) {
        const BleE& e = top[r];
        macStr(e.mac, m, true);
        gfx->setTextColor(e.lastPass == g_pass ? TXT : DIM);
        gfx->setCursor(6, 32 + r * 13);
        gfx->printf("%-18.18s %4d  %s  x%u", e.name[0] ? e.name : m, e.best,
                    e.addrType == BLE_ADDR_RANDOM ? "rnd" : "pub", e.seenPasses);
    }
    if (!n) { gfx->setTextColor(DIM); gfx->setCursor(6, 70); gfx->print("No BLE devices yet."); }
}

void drawSubPage() {
    clearBand(28, 126);
    if (!g_subOk) {
        gfx->setTextSize(2); gfx->setTextColor(BAD);
        gfx->setCursor(6, 70); gfx->print("CC1101 not available");
        return;
    }
    for (int b = 0; b < g_subN; b++) {
        const Sub& s = g_sub[b];
        int y = 32 + b * 30;
        gfx->setTextSize(2); gfx->setTextColor(HI);
        gfx->setCursor(6, y); gfx->print(cc1101_survey_label(b));
        if (!s.nPass) { gfx->setTextSize(1); gfx->setTextColor(DIM); gfx->setCursor(90, y + 4); gfx->print("waiting"); continue; }
        float avg = s.sumAvg / s.nPass;
        meter(90, y, 130, 8, avg, -110, -40, DIM);
        meter(90, y + 10, 130, 8, s.peakMax, -110, -40, s.peakMax - avg > 15 ? WARN : GOOD);
        gfx->setTextSize(1); gfx->setTextColor(TXT);
        gfx->setCursor(226, y);      gfx->printf("avg %4d", (int)avg);
        gfx->setCursor(226, y + 10); gfx->printf("pk  %4d", (int)s.peakMax);
        gfx->setTextColor(s.activePasses ? WARN : DIM);
        gfx->setCursor(278, y + 5);  gfx->printf("act%u", s.activePasses);
    }
}

void drawPage() {
    drawTag();
    switch (g_page) {
        case 0: drawSummary();  break;
        case 1: drawWifiPage(); break;
        case 2: drawBlePage();  break;
        case 3: drawSubPage();  break;
    }
}

void showFinal() {
    header("SURVEY DONE");
    int bleN; portENTER_CRITICAL(&g_mux); bleN = g_bleN; portEXIT_CRITICAL(&g_mux);
    uint32_t el = (millis() - g_startMs) / 1000;
    gfx->setTextSize(2); gfx->setTextColor(TXT);
    gfx->setCursor(6, 36);  gfx->printf("%u passes  %lu:%02lu", (unsigned)g_pass, (unsigned long)(el / 60), (unsigned long)(el % 60));
    gfx->setTextColor(HI);
    gfx->setCursor(6, 60);  gfx->printf("WiFi APs   %d", g_apN);
    gfx->setCursor(6, 82);  gfx->printf("BLE dev    %d", bleN);
    gfx->setTextSize(1);
    gfx->setCursor(6, 108);
    if (g_logOk) { gfx->setTextColor(GOOD); gfx->printf("Saved %lu rows to", (unsigned long)g_rows);
                   gfx->setCursor(6, 120); gfx->print(g_logPath); }
    else         { gfx->setTextColor(WARN); gfx->print("Not saved (no SD card)."); }
    footer("PUSH or SIDE = exit");
    tembed_input_flush();
    while (!tembed_btn_select() && !tembed_btn_back()) delay(10);
}

} // namespace

void tembed_survey_run() {
    tembed_input_flush();
    g_ap  = allocTable<Ap>(kMaxAp);
    g_ble = allocTable<BleE>(kMaxBle);
    if (!g_ap || !g_ble) {
        header("SURVEY");
        gfx->setTextSize(2); gfx->setTextColor(BAD);
        gfx->setCursor(6, 70); gfx->print("Out of memory");
        footer("SIDE=back");
        while (!tembed_btn_back()) delay(10);
        free(g_ap); free(g_ble); g_ap = nullptr; g_ble = nullptr;
        return;
    }
    g_apN = 0; g_bleN = 0; g_pass = 0; g_wifiPassCount = 0; g_blePassCount = 0;
    g_phase = PH_WIFI_START; g_paused = false; g_page = 0;

    header("SURVEY");
    gfx->setTextSize(2); gfx->setTextColor(HI);
    gfx->setCursor(6, 70); gfx->print("Starting radios...");
    g_subOk = cc1101_survey_begin();
    g_subN  = g_subOk ? min(cc1101_survey_bands(), kMaxSub) : 0;
    memset(g_sub, 0, sizeof(g_sub));
    openLog();
    g_startMs = millis();

    drawPageChrome();
    drawPage();

    uint32_t lastDraw = 0;
    for (;;) {
        if (tembed_btn_back()) break;
        int d = tembed_encoder_delta();
        if (d != 0) {
            g_page = ((g_page + d) % kPages + kPages) % kPages;
            drawPageChrome();
            drawPage();
        }
        if (tembed_btn_select()) { g_paused = !g_paused; drawTag(); }

        step();

        if (millis() - lastDraw > 700) { lastDraw = millis(); drawPage(); }
        delay(2);
    }

    // Wind down whatever phase we were in.
    bleStop();
    WiFi.scanDelete();
    if (g_subOk) cc1101_survey_end();
    if (g_phase == PH_LOG) logPass();

    showFinal();

    portENTER_CRITICAL(&g_mux);
    BleE* b = g_ble; g_ble = nullptr; g_bleN = 0;
    portEXIT_CRITICAL(&g_mux);
    free(b);
    free(g_ap); g_ap = nullptr; g_apN = 0;
}

#endif // DEVICE_TEMBED_CC1101
