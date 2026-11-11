// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// tembed_wifi.cpp — WiFi Join for the T-Embed (encoder only)
//
// Scan, pick a network with the wheel, type the password on the encoder
// keyboard (tembed_textin.cpp), connect. Credentials go into the shared
// /wifi.cfg keyring on SD (wifi_manager.cpp), so known networks are
// starred and rejoin without retyping, and other apps (Clock's sync) can
// reconnect to the last network. On success the clock is synced by NTP.
//
// pm_storage doesn't take the Bus Treaty mutex itself, and the SD card
// shares the SPI bus with the LCD and radios, so every keyring access
// here holds spi_mutex.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "tembed_ui.h"
#include "tembed_input.h"
#include "wifi_manager.h"
#include "wifi_mode.h"
#include "pm_storage.h"

using namespace tui;

extern volatile bool g_sd_ready;
extern bool isWiFiConnected;

namespace {

struct Net { char ssid[33]; int8_t rssi; uint8_t ch; bool open; bool known; };
constexpr int kMaxNets = 40;
Net  g_nets[kMaxNets];
int  g_netCount = 0;

// Read the /wifi.cfg keyring into `doc`. Returns false if absent/unreadable.
bool loadKeyring(JsonDocument& doc) {
    if (!g_sd_ready) return false;
    bool ok = false;
    if (busTake(1000)) {
        if (pm_storage::exists("/wifi.cfg")) {
            pm_storage::File f = pm_storage::open("/wifi.cfg", pm_storage::Mode::Read);
            if (f) {
                char buf[1024];
                size_t got = f.read((uint8_t*)buf, sizeof(buf) - 1);
                buf[got] = 0;
                f.close();
                ok = !deserializeJson(doc, buf);
            }
        }
        busGive();
    }
    return ok;
}

bool saveCreds(const char* ssid, const char* pass) {
    if (!g_sd_ready) return false;
    if (!busTake(1000)) return false;
    save_wifi_config(ssid, pass);
    bool ok = pm_storage::exists("/wifi.cfg");
    busGive();
    return ok;
}

// Wait for association, drawing a progress line. Side key cancels.
// Returns the final WiFi status.
wl_status_t waitConnect(uint32_t timeout_ms, bool draw) {
    uint32_t t0 = millis();
    int dots = 0;
    while (millis() - t0 < timeout_ms) {
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED || st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) return st;
        if (tembed_btn_back()) { WiFi.disconnect(); return WL_DISCONNECTED; }
        if (draw && (millis() - t0) / 300 != (uint32_t)dots) {
            dots = (millis() - t0) / 300;
            gfx->fillRect(6, 92, 308, 12, BG);
            gfx->setTextSize(1); gfx->setTextColor(DIM);
            gfx->setCursor(6, 94);
            gfx->printf("%lus ", (unsigned long)((millis() - t0) / 1000));
            for (int i = 0; i < dots % 20; i++) gfx->print('.');
        }
        delay(20);
    }
    return WiFi.status();
}

void beginJoin(const char* ssid, const char* pass) {
    WiFi.persistent(false);              // keyring lives on SD, not NVS
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    delay(100);
    WiFi.begin(ssid, (pass && pass[0]) ? pass : nullptr);
}

const char* statusText(wl_status_t st) {
    switch (st) {
        case WL_NO_SSID_AVAIL:  return "network not found";
        case WL_CONNECT_FAILED: return "rejected (wrong password?)";
        case WL_DISCONNECTED:   return "timed out / cancelled";
        default:                return "failed";
    }
}

// ── Scan ─────────────────────────────────────────────────────────
bool doScan() {
    header("WIFI JOIN");
    footer("Scanning...   SIDE=cancel");
    gfx->setTextSize(2); gfx->setTextColor(HI);
    gfx->setCursor(6, 70); gfx->print("Scanning");

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true /*async*/, false /*hidden*/);
    uint32_t t0 = millis();
    int n = WIFI_SCAN_RUNNING;
    while (true) {
        n = WiFi.scanComplete();
        if (n >= 0 || n == WIFI_SCAN_FAILED) break;
        if (tembed_btn_back() || millis() - t0 > 12000) { WiFi.scanDelete(); return false; }
        gfx->setCursor(102 + ((millis() - t0) / 400 % 4) * 12, 70);
        gfx->print('.');
        if ((millis() - t0) / 400 % 4 == 0) gfx->fillRect(102, 70, 60, 16, BG);
        delay(30);
    }

    JsonDocument keyring;
    bool haveKeys = loadKeyring(keyring);

    g_netCount = 0;
    for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (s.length() == 0) continue;
        int rssi = WiFi.RSSI(i);
        int dup = -1;
        for (int k = 0; k < g_netCount; k++)
            if (strcmp(g_nets[k].ssid, s.c_str()) == 0) { dup = k; break; }
        if (dup >= 0) {
            if (rssi > g_nets[dup].rssi) { g_nets[dup].rssi = rssi; g_nets[dup].ch = WiFi.channel(i); }
            continue;
        }
        if (g_netCount >= kMaxNets) continue;
        Net& e = g_nets[g_netCount++];
        strncpy(e.ssid, s.c_str(), 32); e.ssid[32] = 0;
        e.rssi  = rssi;
        e.ch    = WiFi.channel(i);
        e.open  = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        e.known = haveKeys && keyring[e.ssid].is<const char*>();
    }
    WiFi.scanDelete();
    // Strongest first.
    for (int a = 1; a < g_netCount; a++)
        for (int b = a; b > 0 && g_nets[b].rssi > g_nets[b - 1].rssi; b--) {
            Net t = g_nets[b]; g_nets[b] = g_nets[b - 1]; g_nets[b - 1] = t;
        }
    return true;
}

// ── List UI ──────────────────────────────────────────────────────
// Rows: [Rescan] [Disconnect (if connected)] then networks.
constexpr int kRowY = 48, kRowH = 21, kVisible = 5;
int g_sel = 0, g_top = 0;

int extraRows() { return WiFi.status() == WL_CONNECTED ? 2 : 1; }
int rowCount()  { return extraRows() + g_netCount; }

void drawStatus() {
    clearBand(28, 18);
    gfx->setTextSize(1);
    gfx->setCursor(6, 32);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(GOOD);
        gfx->printf("Connected: %s  %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else {
        gfx->setTextColor(DIM);
        gfx->printf("Not connected.  %d networks", g_netCount);
    }
    if (!g_sd_ready) { gfx->setTextColor(WARN); gfx->setCursor(230, 32); gfx->print("no SD:won't save"); }
}

void drawList() {
    int n = rowCount();
    if (g_sel >= n) g_sel = n - 1;
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + kVisible) g_top = g_sel - kVisible + 1;
    clearBand(kRowY - 2, kVisible * kRowH + 4);
    for (int r = 0; r < kVisible; r++) {
        int idx = g_top + r;
        if (idx >= n) break;
        int y = kRowY + r * kRowH;
        bool sel = idx == g_sel;
        if (sel) gfx->fillRect(2, y - 2, 316, kRowH - 1, SEL);
        gfx->setTextSize(2);
        if (idx == 0) {
            gfx->setTextColor(sel ? TXT : WARN); gfx->setCursor(10, y); gfx->print("[ Rescan ]");
            continue;
        }
        if (idx == 1 && extraRows() == 2) {
            gfx->setTextColor(sel ? TXT : WARN); gfx->setCursor(10, y); gfx->print("[ Disconnect ]");
            continue;
        }
        const Net& e = g_nets[idx - extraRows()];
        gfx->setTextColor(sel ? TXT : (e.known ? HI : HDR));
        gfx->setCursor(10, y);
        char name[19]; strncpy(name, e.ssid, 18); name[18] = 0;
        gfx->print(name);
        gfx->setTextSize(1);
        gfx->setTextColor(sel ? TXT : DIM);
        gfx->setCursor(238, y + 4);
        gfx->print(e.known ? "*" : (e.open ? "o" : " "));
        gfx->setCursor(246, y + 4);
        gfx->printf("%3d", e.rssi);
        rssiBars(270, y + 2, e.rssi);
        gfx->setCursor(292, y + 4);
        gfx->printf("%2d", e.ch);
    }
    gfx->setTextSize(1); gfx->setTextColor(DIM);
    if (g_top > 0)                 { gfx->setCursor(310, kRowY);              gfx->print("^"); }
    if (g_top + kVisible < n)      { gfx->setCursor(310, kRowY + 4 * kRowH);  gfx->print("v"); }
}

void drawListScreen() {
    header("WIFI JOIN");
    drawStatus();
    drawList();
    footer("TURN=pick  PUSH=join  SIDE=back   *=saved o=open");
}

// Connect flow for one network. Returns when the user dismisses the result.
void joinNetwork(const Net& net) {
    char pass[65] = {0};
    bool fromKeyring = false;

    if (!net.open) {
        JsonDocument keyring;
        if (net.known && loadKeyring(keyring)) {
            const char* p = keyring[net.ssid] | "";
            strncpy(pass, p, 64);
            fromKeyring = pass[0] != 0;
        }
        if (!fromKeyring) {
            if (!tembed_text_input("WIFI PASSWORD", net.ssid, pass, sizeof(pass), nullptr)) return;
        }
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        header("WIFI JOIN");
        gfx->setTextSize(2); gfx->setTextColor(HI);
        gfx->setCursor(6, 40); gfx->print("Connecting to");
        gfx->setTextColor(TXT);
        gfx->setCursor(6, 62); gfx->print(net.ssid);
        footer("SIDE=cancel");
        tembed_input_flush();

        beginJoin(net.ssid, pass);
        wl_status_t st = waitConnect(15000, true);

        if (st == WL_CONNECTED) {
            isWiFiConnected = true;
            g_wifi_mode = WIFI_MODE_PM_CLIENT;
            bool saved = saveCreds(net.ssid, pass);
            clearBand(88, 66);
            gfx->setTextSize(2); gfx->setTextColor(GOOD);
            gfx->setCursor(6, 90); gfx->print("Connected");
            gfx->setTextSize(1); gfx->setTextColor(TXT);
            gfx->setCursor(6, 112);
            gfx->printf("IP %s   RSSI %d dBm", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
            gfx->setCursor(6, 124);
            gfx->setTextColor(saved ? GOOD : WARN);
            gfx->print(saved ? "Saved to SD keyring" : "Not saved (no SD card)");
            gfx->setCursor(6, 136); gfx->setTextColor(DIM); gfx->print("Syncing clock...");
            bool t = tembed_time_sync(6000);
            gfx->fillRect(6, 136, 300, 10, BG);
            gfx->setCursor(6, 136);
            gfx->setTextColor(t ? GOOD : WARN);
            gfx->print(t ? "Clock synced (NTP)" : "Clock sync failed (try Clock > PUSH)");
            footer("PUSH or SIDE = done");
            tembed_input_flush();
            while (!tembed_btn_select() && !tembed_btn_back()) delay(10);
            return;
        }

        // Failed. If a saved password was rejected, offer to type a new one.
        clearBand(88, 66);
        gfx->setTextSize(2); gfx->setTextColor(BAD);
        gfx->setCursor(6, 90); gfx->print("Failed");
        gfx->setTextSize(1); gfx->setTextColor(TXT);
        gfx->setCursor(6, 112); gfx->print(statusText(st));
        if (fromKeyring && attempt == 0 && st != WL_NO_SSID_AVAIL && st != WL_DISCONNECTED) {
            gfx->setCursor(6, 126); gfx->setTextColor(WARN);
            gfx->print("Saved password didn't work.");
            footer("PUSH=type new password   SIDE=back");
            tembed_input_flush();
            for (;;) {
                if (tembed_btn_back()) return;
                if (tembed_btn_select()) break;
                delay(10);
            }
            pass[0] = 0;
            fromKeyring = false;
            if (!tembed_text_input("WIFI PASSWORD", net.ssid, pass, sizeof(pass), nullptr)) return;
            continue;
        }
        footer("PUSH or SIDE = back");
        tembed_input_flush();
        while (!tembed_btn_select() && !tembed_btn_back()) delay(10);
        return;
    }
}

void showConnectedInfo() {
    header("WIFI JOIN");
    gfx->setTextSize(2); gfx->setTextColor(GOOD);
    gfx->setCursor(6, 36); gfx->print(WiFi.SSID());
    gfx->setTextSize(1); gfx->setTextColor(TXT);
    gfx->setCursor(6, 62);  gfx->printf("IP       %s", WiFi.localIP().toString().c_str());
    gfx->setCursor(6, 76);  gfx->printf("Gateway  %s", WiFi.gatewayIP().toString().c_str());
    gfx->setCursor(6, 90);  gfx->printf("DNS      %s", WiFi.dnsIP().toString().c_str());
    gfx->setCursor(6, 104); gfx->printf("RSSI     %d dBm  ch %d", (int)WiFi.RSSI(), (int)WiFi.channel());
    gfx->setCursor(6, 118); gfx->printf("MAC      %s", WiFi.macAddress().c_str());
    footer("PUSH or SIDE = back");
    tembed_input_flush();
    while (!tembed_btn_select() && !tembed_btn_back()) delay(10);
}

} // namespace

// Reconnect to the last saved network if not already connected. Used by
// Clock's sync. Side key cancels. No UI of its own.
bool tembed_wifi_ensure(uint32_t timeout_ms) {
    if (WiFi.status() == WL_CONNECTED) return true;
    JsonDocument keyring;
    if (!loadKeyring(keyring)) return false;
    const char* ssid = keyring["ssid"] | "";
    const char* pass = keyring["password"] | "";
    if (!ssid[0]) return false;
    beginJoin(ssid, pass);
    bool ok = waitConnect(timeout_ms, false) == WL_CONNECTED;
    if (ok) { isWiFiConnected = true; g_wifi_mode = WIFI_MODE_PM_CLIENT; }
    tembed_input_flush();
    return ok;
}

void tembed_wifi_run() {
    tembed_input_flush();
    if (!doScan()) g_netCount = 0;
    g_sel = 0; g_top = 0;
    drawListScreen();

    for (;;) {
        if (tembed_btn_back()) break;
        int d = tembed_encoder_delta();
        if (d != 0) {
            int n = rowCount();
            g_sel = ((g_sel + d) % n + n) % n;
            drawList();
        }
        if (tembed_btn_select()) {
            int extra = extraRows();
            if (g_sel == 0) {
                doScan();
            } else if (g_sel == 1 && extra == 2) {
                WiFi.disconnect(false, true);
                isWiFiConnected = false;
                g_wifi_mode = WIFI_MODE_PM_OFF;
                delay(200);
            } else {
                const Net& net = g_nets[g_sel - extra];
                if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == net.ssid) showConnectedInfo();
                else joinNetwork(net);
            }
            drawListScreen();
            tembed_input_flush();
        }
        delay(5);
    }
}

#endif // DEVICE_TEMBED_CC1101
