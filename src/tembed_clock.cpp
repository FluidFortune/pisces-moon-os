// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// tembed_clock.cpp — Clock + battery for the T-Embed, and the shared
// time service (timezone + NTP) other T-Embed apps use.
//
// TIME: there is no RTC on this board. The clock is set by SNTP once
// WiFi is up (WiFi Join syncs automatically; PUSH here syncs on demand,
// reconnecting to the last saved network if needed). Until then the face
// shows uptime. The timezone is a POSIX TZ preset (so DST is handled by
// libc), chosen with the wheel and saved in NVS.
//
// BATTERY: read straight from the BQ27220 fuel gauge (I2C 0x55) using its
// standard commands — no library. Current is signed: + charging, - draining.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "hal_pins.h"
#include "tembed_ui.h"
#include "tembed_input.h"

using namespace tui;

bool tembed_wifi_ensure(uint32_t timeout_ms);   // tembed_wifi.cpp

namespace {

// ── Timezone presets ──────────────────────────────────────────────
struct Zone { const char* name; const char* tz; bool h12; };
const Zone kZones[] = {
    { "UTC",            "UTC0",                              false },
    { "US Pacific",     "PST8PDT,M3.2.0,M11.1.0",            true  },
    { "US Mountain",    "MST7MDT,M3.2.0,M11.1.0",            true  },
    { "Arizona",        "MST7",                              true  },
    { "US Central",     "CST6CDT,M3.2.0,M11.1.0",            true  },
    { "US Eastern",     "EST5EDT,M3.2.0,M11.1.0",            true  },
    { "Alaska",         "AKST9AKDT,M3.2.0,M11.1.0",          true  },
    { "Hawaii",         "HST10",                             true  },
    { "London",         "GMT0BST,M3.5.0/1,M10.5.0",          false },
    { "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3",        false },
    { "Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4",      false },
    { "India",          "IST-5:30",                          false },
    { "China",          "CST-8",                             false },
    { "Japan",          "JST-9",                             false },
    { "Sydney",         "AEST-10AEDT,M10.1.0,M4.1.0/3",      false },
    { "Auckland",       "NZST-12NZDT,M9.5.0,M4.1.0/3",       false },
};
constexpr int kZoneCount = sizeof(kZones) / sizeof(kZones[0]);

int  g_zone = -1;          // -1 = not loaded yet
uint32_t g_lastSyncMs = 0; // millis() of the last successful NTP sync

int loadZone() {
    if (g_zone >= 0) return g_zone;
    Preferences p;
    g_zone = 0;
    if (p.begin("pm_tembed", true)) { g_zone = p.getUChar("tz", 0); p.end(); }
    if (g_zone < 0 || g_zone >= kZoneCount) g_zone = 0;
    return g_zone;
}

void saveZone(int z) {
    Preferences p;
    if (p.begin("pm_tembed", false)) { p.putUChar("tz", (uint8_t)z); p.end(); }
}

// ── BQ27220 fuel gauge ─────────────────────────────────────────────
struct Batt {
    bool ok = false;
    int  soc = 0;          // %
    int  mv = 0;           // mV
    int  ma = 0;           // mA, + charging / - discharging
    int  tteMin = -1;      // time to empty, minutes (-1 = n/a)
    int  ttfMin = -1;      // time to full,  minutes (-1 = n/a)
    float tempC = 0;
};

int gaugeRead16(uint8_t reg) {
    Wire.beginTransmission(I2C_ADDR_GAUGE);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)I2C_ADDR_GAUGE, (uint8_t)2) != 2) return -1;
    int lo = Wire.read(), hi = Wire.read();
    return lo | (hi << 8);
}

Batt readBattery() {
    Batt b;
    int soc = gaugeRead16(0x2C);
    int mv  = gaugeRead16(0x08);
    if (soc < 0 || mv < 0) return b;
    b.ok  = true;
    b.soc = soc > 100 ? 100 : soc;
    b.mv  = mv;
    int ma = gaugeRead16(0x0C);
    b.ma  = (ma < 0) ? 0 : (int16_t)ma;
    int tte = gaugeRead16(0x16), ttf = gaugeRead16(0x18);
    b.tteMin = (tte < 0 || tte == 0xFFFF) ? -1 : tte;
    b.ttfMin = (ttf < 0 || ttf == 0xFFFF) ? -1 : ttf;
    int t = gaugeRead16(0x06);
    b.tempC = (t < 0) ? 0 : t * 0.1f - 273.15f;
    return b;
}

// ── Drawing ───────────────────────────────────────────────────────
void drawZoneTag() {
    headerTag(kZones[loadZone()].name, HI);
}

void drawTime() {
    char hms[12], ampm[4] = "", date[32];
    uint16_t col = TXT;
    if (tembed_time_valid()) {
        time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        if (kZones[loadZone()].h12) {
            int h = lt.tm_hour % 12; if (h == 0) h = 12;
            snprintf(hms, sizeof(hms), "%2d:%02d:%02d", h, lt.tm_min, lt.tm_sec);
            strcpy(ampm, lt.tm_hour < 12 ? "AM" : "PM");
        } else {
            snprintf(hms, sizeof(hms), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
        }
        strftime(date, sizeof(date), "%a %b %d %Y", &lt);
    } else {
        uint32_t s = millis() / 1000;
        snprintf(hms, sizeof(hms), "%02lu:%02lu:%02lu",
                 (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
        strcpy(date, "UPTIME - clock not set");
        col = SUBT;
    }
    gfx->setTextSize(5);
    gfx->setTextColor(col, BG);
    gfx->setCursor(28, 36);
    gfx->print(hms);
    gfx->setTextSize(2);
    gfx->setTextColor(WARN, BG);
    gfx->setCursor(274, 36);
    gfx->print(ampm[0] ? ampm : "  ");

    gfx->fillRect(0, 82, W, 18, BG);
    gfx->setTextSize(2);
    gfx->setTextColor(col == TXT ? HI : DIM);
    int tw = (int)strlen(date) * 12;
    gfx->setCursor((W - tw) / 2, 84);
    gfx->print(date);
}

void drawSyncLine(const char* override_msg = nullptr) {
    clearBand(102, 12);
    gfx->setTextSize(1);
    if (override_msg) {
        gfx->setTextColor(WARN);
        gfx->setCursor(6, 104);
        gfx->print(override_msg);
        return;
    }
    if (tembed_time_valid() && g_lastSyncMs) {
        uint32_t ago = (millis() - g_lastSyncMs) / 60000;
        gfx->setTextColor(GOOD);
        gfx->setCursor(6, 104);
        gfx->printf("NTP synced %lu min ago", (unsigned long)ago);
    } else if (tembed_time_valid()) {
        gfx->setTextColor(GOOD);
        gfx->setCursor(6, 104);
        gfx->print("Clock set");
    } else {
        gfx->setTextColor(DIM);
        gfx->setCursor(6, 104);
        gfx->print("Not synced. PUSH to sync over saved WiFi.");
    }
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(DIM);
        gfx->setCursor(220, 104);
        gfx->print("WiFi up");
    }
}

void drawBattery(const Batt& b) {
    clearBand(116, 38);
    gfx->setTextSize(1);
    if (!b.ok) {
        gfx->setTextColor(BAD);
        gfx->setCursor(6, 122);
        gfx->print("Battery gauge not responding (BQ27220 @0x55)");
        return;
    }
    uint16_t c = b.soc > 40 ? GOOD : (b.soc > 15 ? WARN : BAD);
    meter(6, 118, 180, 14, (float)b.soc, 0, 100, c);
    gfx->setTextSize(2);
    gfx->setTextColor(TXT);
    gfx->setCursor(194, 118);
    gfx->printf("%3d%%", b.soc);
    gfx->setTextSize(1);
    gfx->setTextColor(HDR);
    gfx->setCursor(250, 122);
    gfx->printf("%d.%02dV", b.mv / 1000, (b.mv % 1000) / 10);

    gfx->setCursor(6, 140);
    if (b.ma > 5) {
        gfx->setTextColor(GOOD);
        gfx->printf("+%d mA charging", b.ma);
        if (b.ttfMin > 0) gfx->printf("  full in %dh%02dm", b.ttfMin / 60, b.ttfMin % 60);
    } else if (b.ma < -5) {
        gfx->setTextColor(TXT);
        gfx->printf("%d mA", b.ma);
        if (b.tteMin > 0) gfx->printf("  ~%dh%02dm left", b.tteMin / 60, b.tteMin % 60);
    } else {
        gfx->setTextColor(DIM);
        gfx->print("idle / full");
    }
    gfx->setTextColor(DIM);
    gfx->setCursor(262, 140);
    gfx->printf("%.0fC", b.tempC);
}

} // namespace

// Hardware Test hook: one gauge reading.
bool tembed_battery_read(int* soc, int* mv, int* ma) {
    Batt b = readBattery();
    if (soc) *soc = b.soc;
    if (mv)  *mv  = b.mv;
    if (ma)  *ma  = b.ma;
    return b.ok;
}

// ── Shared time service ───────────────────────────────────────────
void tembed_time_apply_tz() {
    setenv("TZ", kZones[loadZone()].tz, 1);
    tzset();
}

bool tembed_time_valid() {
    return time(nullptr) > 1700000000;     // later than Nov 2023
}

bool tembed_time_sync(uint32_t timeout_ms) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // configTzTime restarts SNTP, which triggers a fresh sync. The IDF
    // reports COMPLETED once per sync (reading it resets it), so this only
    // succeeds on a sync that happened now — not on a clock set earlier.
    configTzTime(kZones[loadZone()].tz, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && tembed_time_valid()) {
            g_lastSyncMs = millis();
            return true;
        }
        delay(100);
    }
    return false;
}

// ── App ───────────────────────────────────────────────────────────
void tembed_clock_run() {
    tembed_time_apply_tz();
    header("CLOCK");
    drawZoneTag();
    footer("TURN=timezone   PUSH=sync time   SIDE=back");
    tembed_input_flush();

    Batt batt = readBattery();
    drawTime();
    drawSyncLine();
    drawBattery(batt);

    uint32_t lastSec = 0, lastBatt = millis();
    for (;;) {
        if (tembed_btn_back()) break;

        int d = tembed_encoder_delta();
        if (d != 0) {
            g_zone = ((loadZone() + d) % kZoneCount + kZoneCount) % kZoneCount;
            saveZone(g_zone);
            tembed_time_apply_tz();
            drawZoneTag();
            drawTime();
        }

        if (tembed_btn_select()) {
            drawSyncLine(WiFi.status() == WL_CONNECTED ? "Syncing with NTP..."
                                                       : "Connecting to saved WiFi...");
            bool ok = false;
            if (tembed_wifi_ensure(12000)) {
                drawSyncLine("Syncing with NTP...");
                ok = tembed_time_sync(8000);
            }
            drawSyncLine(ok ? nullptr : "Sync failed: no saved WiFi or no network. Use WiFi Join.");
            if (!ok) delay(1500);
            drawSyncLine();
            drawTime();
            tembed_input_flush();
        }

        uint32_t now = millis();
        if (now / 1000 != lastSec) { lastSec = now / 1000; drawTime(); }
        if (now - lastBatt > 5000) {
            lastBatt = now;
            batt = readBattery();
            drawBattery(batt);
            drawSyncLine();
        }
        delay(10);
    }
}

#endif // DEVICE_TEMBED_CC1101
