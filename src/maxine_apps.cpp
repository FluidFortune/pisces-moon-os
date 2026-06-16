// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  maxine_apps.cpp — Maxine kiosk apps (480x800 portrait)
//
//  Implements the Maxine-scaled equivalents of the C28P touch apps:
//    - About (info screen)
//    - System (settings: backlight, sound, storage stats, factory reset)
//    - WiFi setup (connect / portal / forget)
//    - Data reader (survival / medical / history)
//    - Weather (Open-Meteo, lazy WiFi)
//    - RSS (feed picker → headlines, lazy WiFi)
//    - Wardrive (stationary RF monitor + anomaly detection)
//
//  Architecture: these are NOT modifications of the C28P apps. They
//  are standalone Maxine UIs that delegate to the SAME underlying
//  engines (nosql_store, wardrive task, anomaly detector). Zero risk
//  to the working C28P build.
//
//  Layout philosophy: top-anchored at y=0..720 (game viewport stays
//  at 0..520, but full-screen apps use 0..720 with a 80px footer area
//  reserved for the BACK affordance). Wide touch targets (≥56px) for
//  easy tapping on the big panel. Size-2 and size-3 text throughout.
//
//  Touch dispatch: maxine_touch_read() returns logical portrait
//  coords (0..479, 0..799). All hit-testing is in those coords.
// ─────────────────────────────────────────────

#ifdef DEVICE_MAXINE

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "maxine_apps.h"
#include "nosql_store.h"
#include "wifi_manager.h"
#include "wardrive.h"
#include "pm_rss_cache.h"

extern Arduino_GFX *gfx;
extern bool maxine_touch_read(int16_t* x, int16_t* y);
extern void kiosk_run_wifi_setup();  // src/kiosk_wifi_setup.cpp — 2× scaled on Maxine

// Forward decls from c28p_anomaly.cpp (now compiled for Maxine too)
extern void c28p_anomaly_init();
extern int  c28p_anomaly_baseline_size();
extern int  c28p_anomaly_recent_alerts();

// ── Traffic-light globals normally owned by gemini_client.cpp ────────────
// gemini_client.cpp defines `wifi_in_use` and `sd_in_use` and is the
// reason they exist: it sets them around HTTPS calls so the wardrive
// task knows to pause WiFi scans / SD writes during AI requests. The
// Maxine kiosk has no Gemini integration (no AI), so gemini_client.cpp
// is not in the Maxine build filter — but c28p_wardrive_engine.cpp
// (which IS in the Maxine build) declares them as externs and reads
// them at the top of every scan cycle. Define them here, defaulted to
// false (no contention), so the engine never blocks. If a future
// Maxine app needs to gate the wardrive task (e.g. a Gemini-style
// HTTPS client), it can flip these flags around its critical section.
volatile bool wifi_in_use = false;
volatile bool sd_in_use   = false;

// ─── Layout constants ───
static constexpr int MAX_W       = 480;
static constexpr int MAX_H       = 800;
static constexpr int APP_TOP_H   = 56;     // title bar
static constexpr int APP_BACK_H  = 72;     // bottom BACK button area
static constexpr int APP_BACK_Y  = MAX_H - APP_BACK_H - 16;
static constexpr int APP_BODY_Y  = APP_TOP_H + 8;

// ─── Common chrome helpers ───
static void app_chrome(const char* title, uint16_t accent_color) {
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, MAX_W, APP_TOP_H, 0x18C3);
    gfx->drawFastHLine(0, APP_TOP_H, MAX_W, accent_color);
    gfx->setTextSize(3);
    gfx->setTextColor(accent_color);
    gfx->setCursor(16, 14);
    gfx->print(title);
}

static void app_back_button(uint16_t color) {
    gfx->fillRect(40, APP_BACK_Y, MAX_W - 80, APP_BACK_H, 0x18C3);
    gfx->drawRect(40, APP_BACK_Y, MAX_W - 80, APP_BACK_H, color);
    gfx->setTextSize(3);
    gfx->setTextColor(color);
    int tw = (int)strlen("< BACK") * 18;
    gfx->setCursor((MAX_W - tw) / 2, APP_BACK_Y + (APP_BACK_H - 24) / 2);
    gfx->print("< BACK");
}

static bool hit_back(int16_t tx, int16_t ty) {
    return (ty >= APP_BACK_Y && ty < APP_BACK_Y + APP_BACK_H &&
            tx >= 40 && tx < MAX_W - 40);
}

// Wait for current touch to release before returning to caller, so the
// launcher doesn't immediately re-dispatch on the same press.
static void wait_release() {
    int16_t tx, ty;
    while (maxine_touch_read(&tx, &ty)) { delay(20); yield(); }
}

// ════════════════════════════════════════════════════════════
//  ABOUT
// ════════════════════════════════════════════════════════════
void maxine_run_about() {
    app_chrome("ABOUT", 0x07FF);

    // Big version block
    gfx->setTextSize(4);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(24, 88);
    gfx->print("Pisces Moon");
    gfx->setTextSize(3);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(24, 132);
    gfx->print("Maxine Edition");
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor(24, 168);
    gfx->print("Version 1.2.1");

    // Specs
    gfx->drawFastHLine(24, 208, MAX_W - 48, 0x4208);
    int y = 224;
    auto line = [&](const char* label, const char* value) {
        gfx->setTextSize(2);
        gfx->setTextColor(0x8410);
        gfx->setCursor(24, y);
        gfx->print(label);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(180, y);
        gfx->print(value);
        y += 28;
    };
    line("Device:",   "Maxine (5\")");
    line("Display:",  "480x800 RGB");
    line("Touch:",    "GT911");
    line("Audio:",    "Out only");
    line("Storage:",  "MicroSD");
    line("Form:",     "Desk kiosk");

    // Authorship
    y += 16;
    gfx->drawFastHLine(24, y, MAX_W - 48, 0x4208);
    y += 16;
    gfx->setTextSize(2);
    gfx->setTextColor(0xFC00);
    gfx->setCursor(24, y);
    gfx->print("By Eric Becker");
    y += 26;
    gfx->setTextColor(0x8410);
    gfx->setCursor(24, y);
    gfx->print("Fluid Fortune");
    y += 22;
    gfx->setTextColor(0x4208);
    gfx->setCursor(24, y);
    gfx->print("Court Jester of Vibe Code");
    y += 24;
    gfx->setTextColor(0x07FF);
    gfx->setCursor(24, y);
    gfx->print("fluidfortune.com");

    y += 32;
    gfx->setTextSize(1);
    gfx->setTextColor(0x4208);
    gfx->setCursor(24, y);
    gfx->print("Licensed AGPL-3.0-or-later");

    app_back_button(0xFC00);

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            wait_release();
            return;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ════════════════════════════════════════════════════════════
//  WIFI SETUP
//
//  Three actions: CONNECT (try known networks), PORTAL (captive),
//  FORGET (clear saved creds). Status panel at the top.
// ════════════════════════════════════════════════════════════
static void wifi_draw_status_panel() {
    // Clear status area (below title, above buttons)
    gfx->fillRect(0, APP_TOP_H + 4, MAX_W, 140, 0x0000);

    gfx->setTextSize(2);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(0x07E0);
        gfx->setCursor(24, 80);
        gfx->print("CONNECTED");
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(24, 110);
        String ssid = WiFi.SSID();
        if (ssid.length() > 28) ssid = ssid.substring(0, 28);
        gfx->print(ssid);
        gfx->setTextSize(1);
        gfx->setTextColor(0x8410);
        gfx->setCursor(24, 140);
        gfx->printf("IP: %s", WiFi.localIP().toString().c_str());
        gfx->setCursor(24, 158);
        gfx->printf("RSSI: %d dBm", WiFi.RSSI());
    } else {
        gfx->setTextColor(0xF800);
        gfx->setCursor(24, 80);
        gfx->print("NOT CONNECTED");
        gfx->setTextSize(1);
        gfx->setTextColor(0x8410);
        gfx->setCursor(24, 116);
        gfx->print("Tap CONNECT to try saved networks,");
        gfx->setCursor(24, 132);
        gfx->print("or PORTAL for phone-driven setup.");
    }
}

static void wifi_draw_main() {
    app_chrome("WIFI", 0x07FF);
    wifi_draw_status_panel();

    // Four big buttons (CONNECT / MANUAL / PORTAL / FORGET), 60 tall
    // with 10px gaps so they fit comfortably between status panel and
    // BACK affordance.
    struct { const char* label; uint16_t color; int y; } btn[] = {
        { "CONNECT",     0x07E0, 200 },
        { "MANUAL",      0x07FF, 270 },
        { "OPEN PORTAL", 0xFC00, 340 },
        { "FORGET",      0xF800, 410 },
    };
    for (int i = 0; i < 4; i++) {
        gfx->fillRect(40, btn[i].y, MAX_W - 80, 60, 0x18C3);
        gfx->drawRect(40, btn[i].y, MAX_W - 80, 60, btn[i].color);
        gfx->setTextSize(3);
        gfx->setTextColor(btn[i].color);
        int tw = (int)strlen(btn[i].label) * 18;
        gfx->setCursor((MAX_W - tw) / 2, btn[i].y + (60 - 24) / 2);
        gfx->print(btn[i].label);
    }

    app_back_button(0xFFFF);
}

static void wifi_show_message(const char* line1, const char* line2,
                              uint16_t color, int hold_ms) {
    gfx->fillRect(0, 200, MAX_W, 280, 0x0000);
    gfx->setTextSize(3);
    gfx->setTextColor(color);
    if (line1) {
        int w = (int)strlen(line1) * 18;
        gfx->setCursor((MAX_W - w) / 2, 240);
        gfx->print(line1);
    }
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    if (line2) {
        int w = (int)strlen(line2) * 12;
        gfx->setCursor((MAX_W - w) / 2, 300);
        gfx->print(line2);
    }
    if (hold_ms > 0) delay(hold_ms);
}

static void wifi_portal_screen() {
    app_chrome("WIFI PORTAL", 0xFC00);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(24, 96);
    gfx->print("On your phone:");
    gfx->setTextColor(0xFFFF);
    int y = 140;
    auto step = [&](const char* s) {
        gfx->setCursor(24, y);
        gfx->print(s);
        y += 32;
    };
    step("1. Open WiFi settings");
    step("2. Join \"PiscesMoon-Setup\"");
    step("3. Wait for browser to open");
    step("4. Choose your home network");
    step("5. Enter password and save");

    y += 24;
    gfx->setTextColor(0xFC00);
    gfx->setCursor(24, y);
    gfx->print("Portal active (3 min timeout)");
}

void maxine_run_wifi_setup() {
    wifi_draw_main();

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            // CONNECT (y=200..260) / MANUAL (y=270..330) /
            // PORTAL  (y=340..400) / FORGET (y=410..470)
            if      (ty >= 200 && ty < 260) pressed = 0;
            else if (ty >= 270 && ty < 330) pressed = 3;   // MANUAL
            else if (ty >= 340 && ty < 400) pressed = 1;   // PORTAL
            else if (ty >= 410 && ty < 470) pressed = 2;   // FORGET
            else if (hit_back(tx, ty))      pressed = -1;
        } else if (!touched && was_touched) {
            if (pressed == -1) return;
            else if (pressed == 0) {
                wifi_show_message("Scanning...", "Trying saved networks", 0xFFE0, 0);
                auto_connect_wifi();
                if (WiFi.status() == WL_CONNECTED) {
                    wifi_show_message("Connected!", WiFi.SSID().c_str(), 0x07E0, 1800);
                } else {
                    wifi_show_message("Failed", "Try MANUAL or PORTAL", 0xF800, 2000);
                }
                wifi_draw_main();
            } else if (pressed == 3) {
                // MANUAL — kiosk_wifi_setup with 2× scaled keyboard
                kiosk_run_wifi_setup();
                wifi_draw_main();
            } else if (pressed == 1) {
                wifi_portal_screen();
                WiFiManager wm;
                wm.setConfigPortalTimeout(180);
                wm.autoConnect("PiscesMoon-Setup");
                wifi_draw_main();
            } else if (pressed == 2) {
                WiFi.disconnect(true, true);
                wifi_show_message("Forgotten", "Stored credentials cleared", 0xFC00, 1500);
                wifi_draw_main();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// Bring WiFi up on-demand. Returns true if connected within the timeout.
// Used by weather and RSS so the user gets a fast-launching kiosk but
// network-using apps still work when invoked.
static bool maxine_wifi_ondemand() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.println("[MAXINE] On-demand WiFi bring-up");
    auto_connect_wifi();
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 6000) {
        delay(100);
    }
    return WiFi.status() == WL_CONNECTED;
}

// ════════════════════════════════════════════════════════════
//  SYSTEM (settings)
// ════════════════════════════════════════════════════════════
static String settings_get(const String& key, const String& dflt) {
    nosql_init("settings");
    int total = nosql_get_count("settings");
    String t, c;
    for (int i = total - 1; i >= 0; i--) {
        if (!nosql_get_entry("settings", i, t, c)) continue;
        if (t == key) return c;
    }
    return dflt;
}

static void settings_set(const String& key, const String& value) {
    nosql_init("settings");
    nosql_save_entry("settings", key.c_str(), value.c_str());
}

static int  g_max_backlight  = 4;       // 1..5
static bool g_max_sound      = true;

static void maxine_set_backlight(uint8_t pwm) {
    static bool ledc_attached = false;
    if (pwm < 16) pwm = 16;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    if (!ledc_attached) {
        ledcAttach(PIN_RGB_BL, 5000, 8);
        ledc_attached = true;
    }
    ledcWrite(PIN_RGB_BL, pwm);
#else
    if (!ledc_attached) {
        ledcSetup(1, 5000, 8);
        ledcAttachPin(PIN_RGB_BL, 1);
        ledc_attached = true;
    }
    ledcWrite(1, pwm);
#endif
}

static void maxine_apply_backlight() {
    maxine_set_backlight((uint8_t)(g_max_backlight * 51));
}

static void sys_draw_main() {
    app_chrome("SYSTEM", 0x8410);

    int y = 88;
    // Backlight
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(24, y);
    gfx->print("BACKLIGHT");
    for (int i = 0; i < 5; i++) {
        int bx = 200 + i * 52;
        bool on = (i < g_max_backlight);
        gfx->fillRect(bx, y - 4, 44, 32, on ? 0xFFE0 : 0x2104);
        gfx->drawRect(bx, y - 4, 44, 32, 0x4208);
    }

    y += 64;
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(24, y);
    gfx->print("SOUND");
    gfx->fillRect(200, y - 4, 120, 32, g_max_sound ? 0x07E0 : 0x2104);
    gfx->drawRect(200, y - 4, 120, 32, 0x4208);
    gfx->setTextColor(g_max_sound ? 0x0000 : 0x8410);
    gfx->setCursor(218, y);
    gfx->print(g_max_sound ? "ON" : "OFF");

    y += 64;
    gfx->drawFastHLine(24, y, MAX_W - 48, 0x4208);
    y += 16;

    // Storage stats
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(24, y);
    gfx->print("STORAGE");
    y += 32;
    gfx->setTextColor(0xFFFF);
    int wd  = nosql_get_count("wardrive");
    int ble = nosql_get_count("ble_log");
    int sv  = nosql_get_count("survival");
    int md  = nosql_get_count("medical");
    int hi  = nosql_get_count("history");
    int an  = nosql_get_count("wd_anomaly");
    gfx->setCursor(24, y); gfx->printf("  wardrive: %d obs",   wd);  y += 26;
    gfx->setCursor(24, y); gfx->printf("  ble_log:  %d obs",   ble); y += 26;
    gfx->setCursor(24, y); gfx->printf("  anomaly:  %d alerts", an); y += 26;
    gfx->setCursor(24, y); gfx->printf("  ref:      %d entries", sv + md + hi); y += 26;

    y += 8;
    gfx->drawFastHLine(24, y, MAX_W - 48, 0x4208);
    y += 16;

    // WiFi status
    gfx->setTextColor(0x07FF);
    gfx->setCursor(24, y);
    gfx->print("WIFI");
    y += 32;
    gfx->setTextSize(2);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(24, y);
        String ssid = WiFi.SSID();
        if (ssid.length() > 22) ssid = ssid.substring(0, 22);
        gfx->printf("  SSID: %s", ssid.c_str());
        y += 26;
        gfx->setCursor(24, y);
        gfx->printf("  RSSI: %d dBm", WiFi.RSSI());
    } else {
        gfx->setTextColor(0xF800);
        gfx->setCursor(24, y);
        gfx->print("  Not connected");
    }

    app_back_button(0xFFFF);
}

void maxine_run_system() {
    g_max_backlight = settings_get("backlight", "4").toInt();
    if (g_max_backlight < 1) g_max_backlight = 1;
    if (g_max_backlight > 5) g_max_backlight = 5;
    g_max_sound = settings_get("sound", "1").toInt() != 0;
    maxine_apply_backlight();

    sys_draw_main();

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            // Backlight pips: y=84..120, x=200..460 with 5 pips of 52px stride
            if (ty >= 84 && ty < 120 && tx >= 200 && tx < 460) {
                int pip = (tx - 200) / 52;
                if (pip >= 0 && pip < 5) {
                    g_max_backlight = pip + 1;
                    settings_set("backlight", String(g_max_backlight));
                    maxine_apply_backlight();
                    sys_draw_main();
                }
            }
            // Sound: y=148..184, x=200..320
            else if (ty >= 148 && ty < 184 && tx >= 200 && tx < 320) {
                g_max_sound = !g_max_sound;
                settings_set("sound", g_max_sound ? "1" : "0");
                sys_draw_main();
            }
            else if (hit_back(tx, ty)) pressed = -1;
        } else if (!touched && was_touched) {
            if (pressed == -1) return;
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ════════════════════════════════════════════════════════════
//  DATA READER (survival / medical / history)
// ════════════════════════════════════════════════════════════
static constexpr int DR_ROW_H    = 64;
static constexpr int DR_LIST_TOP = 80;
static constexpr int DR_LIST_ROWS = 9;     // 9 rows of 64px = 576px

static void dr_chrome(const char* display_name, int total) {
    app_chrome(display_name, 0x07FF);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor(MAX_W - 160, 20);
    gfx->printf("%d entries", total);
}

static void dr_controls(int scroll_top, int total) {
    int btn_y = APP_BACK_Y - 80;
    int btn_w = 120;
    int gap = (MAX_W - 3 * btn_w) / 4;

    // UP
    gfx->fillRect(gap, btn_y, btn_w, 64, 0x18C3);
    gfx->drawRect(gap, btn_y, btn_w, 64, 0xFFE0);
    gfx->setTextSize(3);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(gap + 36, btn_y + 20);
    gfx->print("UP");
    // DOWN
    gfx->fillRect(gap*2 + btn_w, btn_y, btn_w, 64, 0x18C3);
    gfx->drawRect(gap*2 + btn_w, btn_y, btn_w, 64, 0xFFE0);
    gfx->setCursor(gap*2 + btn_w + 18, btn_y + 20);
    gfx->print("DOWN");
    // PAGE indicator
    gfx->fillRect(gap*3 + btn_w*2, btn_y, btn_w, 64, 0x18C3);
    gfx->drawRect(gap*3 + btn_w*2, btn_y, btn_w, 64, 0x4208);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    int first = scroll_top + 1;
    int last  = min(scroll_top + DR_LIST_ROWS, total);
    char buf[20];
    snprintf(buf, sizeof(buf), "%d-%d/%d", first, last, total);
    int tw = (int)strlen(buf) * 12;
    gfx->setCursor(gap*3 + btn_w*2 + (btn_w - tw)/2, btn_y + 24);
    gfx->print(buf);
}

static void dr_list_page(const char* category, int scroll_top, int total) {
    // Clear list area
    gfx->fillRect(0, DR_LIST_TOP, MAX_W, DR_LIST_ROWS * DR_ROW_H, 0x0000);
    String title, content;
    for (int i = 0; i < DR_LIST_ROWS; i++) {
        int idx = scroll_top + i;
        if (idx >= total) break;
        if (!nosql_get_entry(category, idx, title, content)) continue;
        int y = DR_LIST_TOP + i * DR_ROW_H;
        gfx->fillRect(16, y + 4, MAX_W - 32, DR_ROW_H - 12, (i % 2) ? 0x1082 : 0x0841);
        gfx->drawRect(16, y + 4, MAX_W - 32, DR_ROW_H - 12, 0x4208);
        gfx->setTextSize(2);
        gfx->setTextColor(0xFFFF);
        // Show entry number + truncated title
        char buf[80];
        snprintf(buf, sizeof(buf), "%2d  %s", idx + 1, title.c_str());
        if (strlen(buf) > 36) buf[36] = '\0';
        gfx->setCursor(28, y + 18);
        gfx->print(buf);
    }
}

static void dr_detail(const char* title, const String& content, int line_offset) {
    app_chrome("READ", 0x07FF);
    // Title (size 2 wrapped)
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    int title_y = 76;
    int cursor_x = 24;
    int col = 0;
    int max_cols = 36;
    int y = title_y;
    for (size_t i = 0; i < strlen(title); i++) {
        if (col >= max_cols) { y += 22; col = 0; }
        gfx->setCursor(cursor_x + col * 12, y);
        gfx->write(title[i]);
        col++;
    }
    y += 32;
    gfx->drawFastHLine(16, y, MAX_W - 32, 0x4208);
    y += 12;

    // Body — word-wrapped, size 2, paginated by `line_offset` lines
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    int body_top = y;
    int body_bottom = APP_BACK_Y - 100;
    int line_h = 22;
    int chars_per_line = 36;
    int line_no = 0;
    int rendered_lines = 0;
    int max_lines = (body_bottom - body_top) / line_h;
    int start = 0;
    for (size_t i = 0; i <= content.length() && rendered_lines < max_lines; i++) {
        bool is_break = (i == content.length()) ||
                        (content[i] == '\n') ||
                        (i - start >= (size_t)chars_per_line);
        if (is_break) {
            if (line_no >= line_offset) {
                String chunk = content.substring(start, i);
                chunk.trim();
                gfx->setCursor(24, body_top + rendered_lines * line_h);
                gfx->print(chunk);
                rendered_lines++;
            }
            line_no++;
            start = (i < content.length() && content[i] == '\n') ? (int)i + 1 : (int)i;
        }
    }

    // Controls: UP / DOWN + line position
    int btn_y = APP_BACK_Y - 80;
    int btn_w = 120;
    int gap = (MAX_W - 3 * btn_w) / 4;
    gfx->fillRect(gap, btn_y, btn_w, 64, 0x18C3);
    gfx->drawRect(gap, btn_y, btn_w, 64, 0xFFE0);
    gfx->setTextSize(3);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(gap + 36, btn_y + 20);
    gfx->print("UP");

    gfx->fillRect(gap*2 + btn_w, btn_y, btn_w, 64, 0x18C3);
    gfx->drawRect(gap*2 + btn_w, btn_y, btn_w, 64, 0xFFE0);
    gfx->setCursor(gap*2 + btn_w + 18, btn_y + 20);
    gfx->print("DOWN");

    gfx->fillRect(gap*3 + btn_w*2, btn_y, btn_w, 64, 0x18C3);
    gfx->drawRect(gap*3 + btn_w*2, btn_y, btn_w, 64, 0x4208);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    char buf[16];
    snprintf(buf, sizeof(buf), "L %d", line_offset);
    gfx->setCursor(gap*3 + btn_w*2 + 30, btn_y + 24);
    gfx->print(buf);

    app_back_button(0xF800);
}

static void dr_detail_view(const char* category, int entry_idx) {
    String title, content;
    if (!nosql_get_entry(category, entry_idx, title, content)) return;
    int line_offset = 0;
    int approx_total = (content.length() / 36) + 1;
    int max_lines_per_page = (APP_BACK_Y - 200) / 22;
    dr_detail(title.c_str(), content, line_offset);

    int btn_y = APP_BACK_Y - 80;
    int btn_w = 120;
    int gap = (MAX_W - 3 * btn_w) / 4;

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (hit_back(tx, ty)) pressed = -1;
            else if (ty >= btn_y && ty < btn_y + 64) {
                if (tx >= gap && tx < gap + btn_w) pressed = 1;        // UP
                else if (tx >= gap*2 + btn_w && tx < gap*2 + btn_w*2) pressed = 2; // DOWN
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) { wait_release(); return; }
            else if (pressed == 1) {
                if (line_offset > 0) {
                    line_offset = max(0, line_offset - max_lines_per_page);
                    dr_detail(title.c_str(), content, line_offset);
                }
            } else if (pressed == 2) {
                if (line_offset + max_lines_per_page < approx_total) {
                    line_offset += max_lines_per_page;
                    dr_detail(title.c_str(), content, line_offset);
                }
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

void maxine_run_data_reader(const char* category, const char* display_name) {
    nosql_init(category);
    int total = nosql_get_count(category);

    if (total == 0) {
        app_chrome(display_name, 0x07FF);
        gfx->setTextSize(4);
        gfx->setTextColor(0xF800);
        gfx->setCursor(48, 240);
        gfx->print("EMPTY");
        gfx->setTextSize(2);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(24, 320);
        gfx->printf("No %s entries.", display_name);
        gfx->setCursor(24, 352);
        gfx->print("Add JSON files to");
        gfx->setCursor(24, 384);
        gfx->printf("/data/%s/ on SD.", category);
        app_back_button(0xFC00);

        bool was_touched = false;
        while (true) {
            int16_t tx, ty;
            bool touched = maxine_touch_read(&tx, &ty);
            if (touched && !was_touched) { wait_release(); return; }
            was_touched = touched;
            delay(20); yield();
        }
    }

    int scroll_top = 0;
    dr_chrome(display_name, total);
    dr_list_page(category, scroll_top, total);
    dr_controls(scroll_top, total);
    app_back_button(0xF800);

    bool was_touched = false;
    int pressed = -2;
    int btn_y = APP_BACK_Y - 80;
    int btn_w = 120;
    int gap = (MAX_W - 3 * btn_w) / 4;

    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (hit_back(tx, ty)) pressed = -1;
            // Controls row
            else if (ty >= btn_y && ty < btn_y + 64) {
                if (tx >= gap && tx < gap + btn_w) pressed = -2;       // UP
                else if (tx >= gap*2 + btn_w && tx < gap*2 + btn_w*2) pressed = -3; // DOWN
            }
            // List rows
            else if (ty >= DR_LIST_TOP && ty < DR_LIST_TOP + DR_LIST_ROWS * DR_ROW_H) {
                int row = (ty - DR_LIST_TOP) / DR_ROW_H;
                int idx = scroll_top + row;
                if (idx < total) pressed = 1000 + idx;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) { wait_release(); return; }
            else if (pressed == -2) {
                if (scroll_top > 0) {
                    scroll_top = max(0, scroll_top - DR_LIST_ROWS);
                    dr_list_page(category, scroll_top, total);
                    dr_controls(scroll_top, total);
                }
            } else if (pressed == -3) {
                if (scroll_top + DR_LIST_ROWS < total) {
                    scroll_top += DR_LIST_ROWS;
                    dr_list_page(category, scroll_top, total);
                    dr_controls(scroll_top, total);
                }
            } else if (pressed >= 1000) {
                int idx = pressed - 1000;
                dr_detail_view(category, idx);
                dr_chrome(display_name, total);
                dr_list_page(category, scroll_top, total);
                dr_controls(scroll_top, total);
                app_back_button(0xF800);
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ════════════════════════════════════════════════════════════
//  WEATHER  (lazy WiFi, local Open-Meteo fetch)
//
//  This is a standalone implementation duplicating weather.cpp's
//  fetch logic rather than reaching into its statics. weather.cpp
//  is excluded from the Maxine build to avoid pulling in its
//  c28p_run_weather alias and 320x240-shaped render code.
// ════════════════════════════════════════════════════════════
static constexpr float MAX_WX_LAT = 34.1478f;
static constexpr float MAX_WX_LON = -118.1445f;
static constexpr const char* MAX_WX_LOC = "PASADENA, CA";

struct MaxWeather {
    bool        valid = false;
    char        location[32];
    float       lat = MAX_WX_LAT;
    float       lon = MAX_WX_LON;
    float       current_temp_f = 0.0f;
    int         current_code = 0;
    float       wind_mph = 0.0f;
    int         day_codes[3] = {0,0,0};
    float       day_highs[3] = {0,0,0};
    float       day_lows[3]  = {0,0,0};
    char        day_names[3][12];
    uint32_t    fetched_at = 0;
};
static MaxWeather mwx;

struct MaxWCode { int code; const char* text; uint16_t color; };
static const MaxWCode MAX_W_CODES[] = {
    {  0, "CLEAR",         0xFFE0 },
    {  1, "MOSTLY CLEAR",  0xFFE0 },
    {  2, "PARTLY CLOUDY", 0xC618 },
    {  3, "OVERCAST",      0x8410 },
    { 45, "FOG",           0xC618 },
    { 48, "RIME FOG",      0xC618 },
    { 51, "LT DRIZZLE",    0x07FF },
    { 53, "DRIZZLE",       0x07FF },
    { 55, "HVY DRIZZLE",   0x07FF },
    { 61, "LIGHT RAIN",    0x041F },
    { 63, "RAIN",          0x041F },
    { 65, "HEAVY RAIN",    0x041F },
    { 71, "LIGHT SNOW",    0xFFFF },
    { 73, "SNOW",          0xFFFF },
    { 75, "HEAVY SNOW",    0xFFFF },
    { 80, "SHOWERS",       0x041F },
    { 81, "SHOWERS",       0x041F },
    { 82, "HVY SHOWERS",   0x041F },
    { 95, "THUNDERSTORM",  0xF800 },
    { 96, "THUNDER+HAIL",  0xF800 },
    { 99, "THUNDER+HAIL",  0xF800 },
};
static constexpr int MAX_W_CODES_N = sizeof(MAX_W_CODES) / sizeof(MaxWCode);

static const char* mwx_text(int c) {
    for (int i = 0; i < MAX_W_CODES_N; i++) if (MAX_W_CODES[i].code == c) return MAX_W_CODES[i].text;
    return "UNKNOWN";
}
static uint16_t mwx_color(int c) {
    for (int i = 0; i < MAX_W_CODES_N; i++) if (MAX_W_CODES[i].code == c) return MAX_W_CODES[i].color;
    return 0xFFFF;
}

static bool mwx_fetch() {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,wind_speed_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&temperature_unit=fahrenheit&wind_speed_unit=mph"
        "&forecast_days=3&timezone=auto",
        mwx.lat, mwx.lon);
    http.begin(client, url);
    http.setTimeout(8000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject current = doc["current"];
    if (!current.isNull()) {
        mwx.current_temp_f = current["temperature_2m"] | 0.0f;
        mwx.current_code = current["weather_code"] | 0;
        mwx.wind_mph = current["wind_speed_10m"] | 0.0f;
    }
    JsonObject daily = doc["daily"];
    if (!daily.isNull()) {
        JsonArray codes = daily["weather_code"];
        JsonArray highs = daily["temperature_2m_max"];
        JsonArray lows  = daily["temperature_2m_min"];
        for (int i = 0; i < 3; i++) {
            mwx.day_codes[i] = (codes && i < (int)codes.size()) ? codes[i].as<int>() : 0;
            mwx.day_highs[i] = (highs && i < (int)highs.size()) ? highs[i].as<float>() : 0.0f;
            mwx.day_lows[i]  = (lows  && i < (int)lows.size())  ? lows[i].as<float>()  : 0.0f;
        }
    }
    snprintf(mwx.day_names[0], 12, "TODAY");
    snprintf(mwx.day_names[1], 12, "TMRW");
    snprintf(mwx.day_names[2], 12, "+2");
    mwx.valid = true;
    mwx.fetched_at = millis();
    return true;
}

static void mwx_load_loc() {
    snprintf(mwx.location, sizeof(mwx.location), "%s", MAX_WX_LOC);
    mwx.lat = MAX_WX_LAT;
    mwx.lon = MAX_WX_LON;
    nosql_init("settings");
    int total = nosql_get_count("settings");
    String t, c;
    for (int i = 0; i < total; i++) {
        if (!nosql_get_entry("settings", i, t, c)) continue;
        if (t == "weather_location") {
            int p1 = c.indexOf('|');
            int p2 = c.indexOf('|', p1 + 1);
            if (p1 > 0 && p2 > p1) {
                String name = c.substring(0, p1);
                snprintf(mwx.location, sizeof(mwx.location), "%s", name.c_str());
                mwx.lat = c.substring(p1 + 1, p2).toFloat();
                mwx.lon = c.substring(p2 + 1).toFloat();
            }
            break;
        }
    }
}

static void mwx_draw_loading() {
    gfx->fillRect(0, APP_TOP_H + 4, MAX_W, APP_BACK_Y - APP_TOP_H - 4, 0x0000);
    gfx->setTextSize(3);
    gfx->setTextColor(0xFFE0);
    int w = (int)strlen("Fetching...") * 18;
    gfx->setCursor((MAX_W - w) / 2, 380);
    gfx->print("Fetching...");
}

static void mwx_draw_error(const char* msg) {
    gfx->fillRect(0, APP_TOP_H + 4, MAX_W, APP_BACK_Y - APP_TOP_H - 4, 0x0000);
    gfx->setTextSize(4);
    gfx->setTextColor(0xF800);
    gfx->setCursor(24, 200);
    gfx->print("ERROR");
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(24, 260);
    gfx->print(msg);
    gfx->setTextColor(0x8410);
    gfx->setCursor(24, 292);
    gfx->print("Check WiFi connection.");
}

static void mwx_draw_data() {
    gfx->fillRect(0, APP_TOP_H + 4, MAX_W, APP_BACK_Y - APP_TOP_H - 4, 0x0000);

    // Location
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(24, 80);
    gfx->print(mwx.location);

    // Big temperature
    gfx->setTextSize(8);    // ~48px chars
    gfx->setTextColor(0xFFE0);
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%d", (int)round(mwx.current_temp_f));
    gfx->setCursor(40, 116);
    gfx->print(tbuf);
    gfx->setTextSize(4);
    gfx->setCursor(40 + (int)strlen(tbuf) * 48 + 8, 136);
    gfx->print("F");

    // Condition + wind
    gfx->setTextSize(3);
    gfx->setTextColor(mwx_color(mwx.current_code));
    gfx->setCursor(24, 232);
    gfx->print(mwx_text(mwx.current_code));
    gfx->setTextSize(2);
    gfx->setTextColor(0xC618);
    gfx->setCursor(24, 272);
    gfx->printf("WIND: %.0f MPH", mwx.wind_mph);

    // Divider
    gfx->drawFastHLine(16, 308, MAX_W - 32, 0x4208);
    gfx->setTextColor(0x8410);
    gfx->setCursor(24, 318);
    gfx->print("3-DAY FORECAST");

    // 3 forecast cards stacked
    for (int i = 0; i < 3; i++) {
        int y = 348 + i * 92;
        gfx->fillRect(16, y, MAX_W - 32, 80, 0x18C3);
        gfx->drawRect(16, y, MAX_W - 32, 80, 0x4208);
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(28, y + 14);
        gfx->print(mwx.day_names[i]);
        gfx->setTextSize(2);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(28, y + 48);
        gfx->printf("%d / %d F",
                    (int)round(mwx.day_highs[i]),
                    (int)round(mwx.day_lows[i]));
        gfx->setTextSize(2);
        gfx->setTextColor(mwx_color(mwx.day_codes[i]));
        int w = (int)strlen(mwx_text(mwx.day_codes[i])) * 12;
        gfx->setCursor(MAX_W - 32 - w, y + 32);
        gfx->print(mwx_text(mwx.day_codes[i]));
    }

    // REFRESH button at the bottom of body, above BACK
    int rb_y = APP_BACK_Y - 88;
    gfx->fillRect(40, rb_y, MAX_W - 80, 72, 0x0260);
    gfx->drawRect(40, rb_y, MAX_W - 80, 72, 0x07E0);
    gfx->setTextSize(3);
    gfx->setTextColor(0x07E0);
    int tw = (int)strlen("REFRESH") * 18;
    gfx->setCursor((MAX_W - tw) / 2, rb_y + (72 - 24) / 2);
    gfx->print("REFRESH");
}

void maxine_run_weather() {
    app_chrome("WEATHER", 0x07FF);
    mwx_load_loc();

    if (!maxine_wifi_ondemand()) {
        mwx_draw_error("WiFi not connected.");
        app_back_button(0xFC00);
        bool wt = false;
        while (true) {
            int16_t tx, ty;
            bool touched = maxine_touch_read(&tx, &ty);
            if (touched && !wt) { wait_release(); return; }
            wt = touched; delay(20); yield();
        }
    }

    mwx_draw_loading();
    if (!mwx_fetch()) mwx_draw_error("Fetch failed");
    else mwx_draw_data();
    app_back_button(0xFC00);

    int rb_y = APP_BACK_Y - 88;
    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (hit_back(tx, ty)) pressed = -1;
            else if (ty >= rb_y && ty < rb_y + 72 && tx >= 40 && tx < MAX_W - 40) pressed = 0;
        } else if (!touched && was_touched) {
            if (pressed == -1) return;
            else if (pressed == 0) {
                mwx_draw_loading();
                if (!mwx_fetch()) mwx_draw_error("Fetch failed");
                else mwx_draw_data();
                app_back_button(0xFC00);
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

// ════════════════════════════════════════════════════════════
//  RSS  (lazy WiFi, standalone implementation)
//
//  Same model as weather: NoSQL feed list seeded from a small
//  default set, fetch + parse on demand. Article view tap-anywhere
//  to return to list.
// ════════════════════════════════════════════════════════════
static constexpr int MAX_RSS_ITEMS = 20;
struct MaxRssItem { String title; String description; };
static MaxRssItem mrss[MAX_RSS_ITEMS];
static int mrss_count = 0;

struct MaxDefaultFeed { const char* name; const char* url; };
static const MaxDefaultFeed MAX_DEFAULT_FEEDS[] = {
    { "FLUID FORTUNE", "https://blog.fluidfortune.com/feed" },
    { "BBC WORLD",    "https://feeds.bbci.co.uk/news/world/rss.xml" },
    { "NPR",          "https://feeds.npr.org/1001/rss.xml" },
    { "AP TOP",       "https://rsshub.app/apnews/topics/apf-topnews" },
    { "HACKER NEWS",  "https://hnrss.org/frontpage" },
};
static constexpr int MAX_DEFAULT_FEEDS_N = sizeof(MAX_DEFAULT_FEEDS) / sizeof(MaxDefaultFeed);

static void mrss_seed_defaults() {
    nosql_init("rss_feeds");
    // Idempotent seed — mirrors rss.cpp::seed_defaults_if_empty().
    // Each default is added only if its URL isn't already present, so
    // an already-seeded device picks up newly-introduced defaults
    // (like FLUID FORTUNE) on the next launch.
    int existing = nosql_get_count("rss_feeds");
    String et, eu;
    for (int i = 0; i < MAX_DEFAULT_FEEDS_N; i++) {
        bool already = false;
        for (int j = 0; j < existing; j++) {
            if (!nosql_get_entry("rss_feeds", j, et, eu)) continue;
            if (eu == MAX_DEFAULT_FEEDS[i].url) { already = true; break; }
        }
        if (already) continue;
        nosql_save_entry("rss_feeds",
                         MAX_DEFAULT_FEEDS[i].name,
                         MAX_DEFAULT_FEEDS[i].url);
    }
}

static int mrss_parse(const String& body) {
    mrss_count = 0;
    int cursor = 0;
    while (mrss_count < MAX_RSS_ITEMS) {
        int item_start = body.indexOf("<item>", cursor);
        if (item_start < 0) item_start = body.indexOf("<item ", cursor);
        if (item_start < 0) break;
        int item_end = body.indexOf("</item>", item_start);
        if (item_end < 0) break;
        String item_xml = body.substring(item_start, item_end);

        String title = "";
        int t_start = item_xml.indexOf("<title>");
        if (t_start >= 0) {
            t_start += 7;
            int t_end = item_xml.indexOf("</title>", t_start);
            if (t_end > t_start) {
                title = item_xml.substring(t_start, t_end);
                if (title.startsWith("<![CDATA[")) {
                    title = title.substring(9);
                    int cd = title.indexOf("]]>");
                    if (cd >= 0) title = title.substring(0, cd);
                }
                title.trim();
            }
        }

        String desc = "";
        int d_start = item_xml.indexOf("<description>");
        if (d_start >= 0) {
            d_start += 13;
            int d_end = item_xml.indexOf("</description>", d_start);
            if (d_end > d_start) {
                desc = item_xml.substring(d_start, d_end);
                if (desc.startsWith("<![CDATA[")) {
                    desc = desc.substring(9);
                    int cd = desc.indexOf("]]>");
                    if (cd >= 0) desc = desc.substring(0, cd);
                }
                while (true) {
                    int lt = desc.indexOf('<');
                    if (lt < 0) break;
                    int gt = desc.indexOf('>', lt);
                    if (gt < 0) break;
                    desc = desc.substring(0, lt) + desc.substring(gt + 1);
                }
                desc.trim();
            }
        }

        if (title.length() > 0) {
            mrss[mrss_count].title = title;
            mrss[mrss_count].description = desc;
            mrss_count++;
        }
        cursor = item_end + 7;
    }
    return mrss_count;
}

static bool mrss_fetch(const String& url) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    if (url.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure();
        http.begin(client, url);
    } else {
        http.begin(url);
    }
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    mrss_parse(body);
    return mrss_count > 0;
}

static void mrss_draw_wrapped(const String& s, int x0, int y0,
                              int chars_per_line, int line_h, int y_max) {
    int col = 0;
    int y = y0;
    gfx->setCursor(x0, y);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (y > y_max) break;
        if (c == '\n' || col >= chars_per_line) {
            y += line_h;
            col = 0;
            gfx->setCursor(x0, y);
            if (c == '\n') continue;
        }
        gfx->write(c);
        col++;
    }
}

static void mrss_show_article(int idx) {
    if (idx < 0 || idx >= mrss_count) return;
    app_chrome("ARTICLE", 0x07FF);

    // Title (size 3 wrapped)
    gfx->setTextSize(3);
    gfx->setTextColor(0x07FF);
    mrss_draw_wrapped(mrss[idx].title, 16, 80, 24, 34, 200);

    gfx->drawFastHLine(16, 220, MAX_W - 32, 0x4208);

    // Body (size 2)
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    mrss_draw_wrapped(mrss[idx].description, 16, 240, 36, 22, APP_BACK_Y - 16);

    app_back_button(0xFFE0);

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) { wait_release(); return; }
        was_touched = touched;
        delay(20); yield();
    }
}

static constexpr int MRSS_PER_PAGE = 6;
static constexpr int MRSS_ROW_H    = 96;

static void mrss_show_headlines(const String& feed_name) {
    int page = 0;
    int pages = (mrss_count + MRSS_PER_PAGE - 1) / MRSS_PER_PAGE;
    if (pages < 1) pages = 1;

    auto draw_page = [&]() {
        app_chrome(feed_name.c_str(), 0xFFE0);
        if (mrss_count == 0) {
            gfx->setTextSize(2);
            gfx->setTextColor(0x8410);
            gfx->setCursor(120, 400);
            gfx->print("(no items)");
            return;
        }
        int start = page * MRSS_PER_PAGE;
        int end = min(mrss_count, start + MRSS_PER_PAGE);
        for (int i = start; i < end; i++) {
            int row = i - start;
            int y = APP_BODY_Y + row * MRSS_ROW_H;
            gfx->fillRect(16, y, MAX_W - 32, MRSS_ROW_H - 8, 0x18C3);
            gfx->drawRect(16, y, MAX_W - 32, MRSS_ROW_H - 8, 0x4208);
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFFF);
            mrss_draw_wrapped(mrss[i].title, 24, y + 10, 36, 22, y + MRSS_ROW_H - 16);
        }
        if (pages > 1) {
            int pb_y = APP_BACK_Y - 80;
            gfx->fillRect(40,         pb_y, 120, 64, 0x18C3);
            gfx->drawRect(40,         pb_y, 120, 64, 0x07FF);
            gfx->fillRect(MAX_W - 160, pb_y, 120, 64, 0x18C3);
            gfx->drawRect(MAX_W - 160, pb_y, 120, 64, 0x07FF);
            gfx->setTextSize(3);
            gfx->setTextColor(0x07FF);
            gfx->setCursor(64, pb_y + 20);
            gfx->print("PREV");
            gfx->setCursor(MAX_W - 134, pb_y + 20);
            gfx->print("NEXT");
            gfx->setTextSize(2);
            gfx->setTextColor(0x8410);
            char buf[12];
            snprintf(buf, sizeof(buf), "%d/%d", page + 1, pages);
            gfx->setCursor(MAX_W/2 - 24, pb_y + 24);
            gfx->print(buf);
        }
        app_back_button(0xF800);
    };

    draw_page();
    int pb_y = APP_BACK_Y - 80;
    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (hit_back(tx, ty)) pressed = -1;
            else if (pages > 1 && ty >= pb_y && ty < pb_y + 64) {
                if (tx >= 40 && tx < 160) pressed = -3;
                else if (tx >= MAX_W - 160 && tx < MAX_W - 40) pressed = -4;
            } else if (ty >= APP_BODY_Y && ty < APP_BODY_Y + MRSS_PER_PAGE * MRSS_ROW_H) {
                int row = (ty - APP_BODY_Y) / MRSS_ROW_H;
                int idx = page * MRSS_PER_PAGE + row;
                if (idx < mrss_count) pressed = 1000 + idx;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) return;
            else if (pressed == -3) { if (page > 0) { page--; draw_page(); } }
            else if (pressed == -4) { if (page < pages - 1) { page++; draw_page(); } }
            else if (pressed >= 1000) {
                int idx = pressed - 1000;
                mrss_show_article(idx);
                draw_page();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

static bool mrss_pick_feed(String& url_out, String& name_out) {
    String titles[16], urls[16];
    int feed_count = 0;
    int total = nosql_get_count("rss_feeds");
    if (total > 16) total = 16;
    String t, u;
    for (int i = 0; i < total; i++) {
        if (nosql_get_entry("rss_feeds", i, t, u)) {
            titles[feed_count] = t;
            urls[feed_count] = u;
            feed_count++;
        }
    }
    app_chrome("RSS FEEDS", 0xFFE0);

    int row_h = 88;
    for (int i = 0; i < feed_count; i++) {
        int y = APP_BODY_Y + i * row_h;
        if (y + row_h > APP_BACK_Y - 16) break;
        gfx->fillRect(24, y, MAX_W - 48, row_h - 12, 0x18C3);
        gfx->drawRect(24, y, MAX_W - 48, row_h - 12, 0x4208);
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(40, y + 18);
        gfx->print(titles[i]);
    }
    app_back_button(0xF800);

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (hit_back(tx, ty)) pressed = -1;
            else if (ty >= APP_BODY_Y) {
                int row = (ty - APP_BODY_Y) / row_h;
                if (row >= 0 && row < feed_count) pressed = row;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) return false;
            if (pressed >= 0 && pressed < feed_count) {
                url_out = urls[pressed];
                name_out = titles[pressed];
                return true;
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

void maxine_run_rss() {
    mrss_seed_defaults();

    // WiFi may be down — the cache fallback below will still let the
    // user read recently-fetched headlines. So we DON'T bail early
    // on "no WiFi"; we let the user pick a feed and try.
    maxine_wifi_ondemand();

    while (true) {
        String url, name;
        if (!mrss_pick_feed(url, name)) return;
        app_chrome(name.c_str(), 0xFFE0);
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(120, 400);
        gfx->print("Loading...");

        bool fetched_ok = mrss_fetch(url);
        bool from_cache = false;

        if (fetched_ok) {
            PmRssCachedItem cache_buf[MAX_RSS_ITEMS];
            for (int i = 0; i < mrss_count; i++) {
                cache_buf[i].title       = mrss[i].title;
                cache_buf[i].description = mrss[i].description;
            }
            pm_rss_cache_save(name.c_str(), cache_buf, mrss_count);
        } else {
            // Try cached fallback.
            PmRssCachedItem cache_buf[MAX_RSS_ITEMS];
            int loaded = pm_rss_cache_load(name.c_str(),
                                           cache_buf, MAX_RSS_ITEMS);
            if (loaded > 0) {
                mrss_count = loaded;
                for (int i = 0; i < loaded; i++) {
                    mrss[i].title       = cache_buf[i].title;
                    mrss[i].description = cache_buf[i].description;
                }
                from_cache = true;
            } else {
                gfx->setTextColor(0xF800);
                gfx->setCursor(120, 460);
                gfx->print("Fetch failed");
                gfx->setTextSize(2);
                gfx->setTextColor(0x8410);
                gfx->setCursor(120, 500);
                gfx->print("No cached copy on SD");
                delay(2000);
                continue;
            }
        }

        String display_name = from_cache ? (name + " (cached)") : name;
        mrss_show_headlines(display_name);
    }
}

// ════════════════════════════════════════════════════════════
//  WARDRIVE  (stationary RF monitor + anomaly detection)
//
//  Uses the same engines as the C28P wardrive: init_wardrive_core()
//  spawns the scan task (on Core 0), and c28p_anomaly_* (now compiled
//  for Maxine too) tracks the baseline and logs anomalies. The UI
//  here is just a window into the scan state.
// ════════════════════════════════════════════════════════════
static uint32_t g_max_wd_refresh = 0;
static int g_max_last_wifi   = -1;
static int g_max_last_ble    = -1;
static int g_max_last_alerts = -1;
static int g_max_last_base   = -1;

static void wd_chrome() {
    app_chrome("WARDRIVE", 0xFC00);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor(MAX_W - 200, 22);
    gfx->print("STATIONARY");

    gfx->setTextSize(2);
    gfx->setTextColor(0x4208);
    gfx->setCursor(16, APP_TOP_H + 12);
    gfx->print("Passive RF monitor + anomaly detect");

    // Big number panel
    gfx->drawFastHLine(0, 112, MAX_W, 0x4208);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor(24, 124);
    gfx->print("NOW SEEING");

    gfx->setTextSize(3);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(24, 156);
    gfx->print("WIFI");
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(180, 156);
    gfx->print("BLE");
    gfx->setTextColor(0xFC00);
    gfx->setCursor(336, 156);
    gfx->print("ALRT");

    // Ghost engine + session totals area
    gfx->drawFastHLine(0, 304, MAX_W, 0x4208);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor(16, 316);
    gfx->print("GHOST ENGINE");

    gfx->drawFastHLine(0, 432, MAX_W, 0x4208);
    gfx->setTextColor(0x8410);
    gfx->setCursor(16, 444);
    gfx->print("SESSION TOTALS");
}

static void wd_buttons() {
    // PAUSE/RESUME button (left)  + BACK (right)
    int by = APP_BACK_Y;
    int bh = APP_BACK_H;
    gfx->fillRect(24, by, 200, bh, 0x18C3);
    gfx->drawRect(24, by, 200, bh, wardrive_active ? 0x07E0 : 0xF800);
    gfx->setTextSize(3);
    gfx->setTextColor(wardrive_active ? 0x07E0 : 0xF800);
    const char* lbl = wardrive_active ? "PAUSE" : "RESUME";
    int tw = (int)strlen(lbl) * 18;
    gfx->setCursor(24 + (200 - tw)/2, by + (bh - 24)/2);
    gfx->print(lbl);

    gfx->fillRect(MAX_W - 224, by, 200, bh, 0x18C3);
    gfx->drawRect(MAX_W - 224, by, 200, bh, 0xF800);
    gfx->setTextColor(0xF800);
    int bw = (int)strlen("< BACK") * 18;
    gfx->setCursor(MAX_W - 224 + (200 - bw)/2, by + (bh - 24)/2);
    gfx->print("< BACK");
}

static void wd_stats() {
    // Big numbers update
    if (g_max_last_wifi != networks_found) {
        gfx->fillRect(24, 200, 140, 60, 0x0000);
        gfx->setTextSize(6);
        gfx->setTextColor(0x07FF);
        gfx->setCursor(24, 200);
        gfx->printf("%d", networks_found);
        g_max_last_wifi = networks_found;
    }
    if (g_max_last_ble != bt_found) {
        gfx->fillRect(180, 200, 140, 60, 0x0000);
        gfx->setTextSize(6);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(180, 200);
        gfx->printf("%d", bt_found);
        g_max_last_ble = bt_found;
    }
    int alerts = c28p_anomaly_recent_alerts();
    if (g_max_last_alerts != alerts) {
        gfx->fillRect(336, 200, 120, 60, 0x0000);
        gfx->setTextSize(6);
        gfx->setTextColor(alerts > 0 ? 0xF800 : 0x4208);
        gfx->setCursor(336, 200);
        gfx->printf("%d", alerts);
        g_max_last_alerts = alerts;
    }

    // Baseline size
    int base = c28p_anomaly_baseline_size();
    if (g_max_last_base != base) {
        gfx->fillRect(24, 268, MAX_W - 48, 28, 0x0000);
        gfx->setTextSize(2);
        gfx->setTextColor(0x8410);
        gfx->setCursor(24, 268);
        gfx->printf("Baseline: %d known networks", base);
        g_max_last_base = base;
    }

    // Ghost engine status
    gfx->fillRect(16, 348, MAX_W - 32, 60, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(wardrive_active ? 0x07E0 : 0xF800);
    gfx->setCursor(16, 348);
    gfx->print(wardrive_active ? "Scanning WiFi + BLE" : "Idle (Paused)");
    gfx->setTextColor(0x8410);
    gfx->setCursor(16, 380);
    uint32_t age = (millis() - last_scan_ms) / 1000;
    gfx->printf("Last scan: %lus ago", age);

    // Session totals
    gfx->fillRect(16, 472, MAX_W - 32, 100, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(16, 476);
    gfx->printf("  WiFi:   %d obs", networks_total);
    gfx->setCursor(16, 504);
    gfx->printf("  BLE:    %d obs", ble_total);
    gfx->setCursor(16, 532);
    gfx->printf("  ESP:    %d devices", esp_found);

    gfx->setTextColor(0x4208);
    gfx->setCursor(16, 568);
    gfx->print("Tap BACK -- scan persists in background.");
}

void maxine_run_wardrive() {
    Serial.println("[MAXINE-WD] Wardrive UI starting");
    c28p_anomaly_init();
    if (!wardrive_active) {
        Serial.println("[MAXINE-WD] Spawning wardrive core");
        init_wardrive_core();
    }

    wd_chrome();
    g_max_last_wifi = -1;
    g_max_last_ble  = -1;
    g_max_last_alerts = -1;
    g_max_last_base = -1;
    wd_stats();
    wd_buttons();

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            // PAUSE/RESUME left
            if (ty >= APP_BACK_Y && ty < APP_BACK_Y + APP_BACK_H &&
                tx >= 24 && tx < 224) pressed = 0;
            // BACK right
            else if (ty >= APP_BACK_Y && ty < APP_BACK_Y + APP_BACK_H &&
                     tx >= MAX_W - 224 && tx < MAX_W - 24) pressed = -1;
        } else if (!touched && was_touched) {
            if (pressed == -1) return;
            else if (pressed == 0) {
                if (wardrive_active) {
                    Serial.println("[MAXINE-WD] Pausing wardrive");
                    wardrive_teardown(3000);
                } else {
                    Serial.println("[MAXINE-WD] Resuming wardrive");
                    init_wardrive_core();
                }
                delay(200);
                wd_chrome();
                g_max_last_wifi = -1; g_max_last_ble = -1;
                g_max_last_alerts = -1; g_max_last_base = -1;
                wd_stats();
                wd_buttons();
            }
            pressed = -2;
        }

        if (millis() - g_max_wd_refresh > 500) {
            wd_stats();
            g_max_wd_refresh = millis();
        }
        was_touched = touched;
        delay(20); yield();
    }
}

#endif // DEVICE_MAXINE
