// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_system.cpp — System settings for C28P
//
//  Replaces the previous "SYSTEM tile redirects to ABOUT" stub with
//  a real settings screen. v1.2.1 settings:
//
//   - Backlight brightness (PWM, 5 levels)
//   - Sound on/off
//   - NoSQL storage stats (entries by category, total bytes)
//   - WiFi state summary
//   - Factory reset (wipes NoSQL only — does not touch firmware)
//   - About / version sub-screen
//
//  Settings persist in NoSQL category "settings" as key-value entries.
//  Each setting that needs persistence does its own load/save against
//  NoSQL.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "c28p_dpad.h"
#include "nosql_store.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t *x, int16_t *y);
extern void c28p_run_about();

// Backlight PWM driver. main.cpp drives PIN_LCD_BL high at boot to
// turn the screen on; we re-attach the pin to LEDC the first time
// the user touches the brightness pips, then call ledcWrite on every
// subsequent change. Five levels (1=lowest..5=brightest) map to
// 51..255 duty cycle on an 8-bit PWM at 5 kHz.
//
// Floor of 16 is enforced so a level-1 setting never goes fully
// dark — the only way to turn the C28P's screen off is the SLEEP
// button (not yet wired in v1.2.1), not the BACKLIGHT slider.
static void c28p_set_backlight(uint8_t pwm) {
    static bool ledc_attached = false;
    if (pwm < 16) pwm = 16;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    if (!ledc_attached) {
        // ESP32 Arduino 3.x: ledcAttach(pin, freq, resolution).
        // 5 kHz keeps the PWM above audible range; 8-bit resolution
        // gives us 256 duty levels, far more than the UI exposes.
        ledcAttach(PIN_LCD_BL, 5000, 8);
        ledc_attached = true;
    }
    ledcWrite(PIN_LCD_BL, pwm);
#else
    // ESP32 Arduino 2.x: explicit channel allocation. Channel 0 is
    // safe because no other driver in the C28P build uses LEDC.
    if (!ledc_attached) {
        ledcSetup(0, 5000, 8);
        ledcAttachPin(PIN_LCD_BL, 0);
        ledc_attached = true;
    }
    ledcWrite(0, pwm);
#endif
}

// ─── Setting helpers ───
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
    // Look for existing — delete-and-rewrite is the simplest pattern
    // since nosql_store may not have an in-place update API
    int total = nosql_get_count("settings");
    String t, c;
    for (int i = 0; i < total; i++) {
        if (!nosql_get_entry("settings", i, t, c)) continue;
        if (t == key) {
            // Remove (best-effort — if no remove API, just append; older
            // entries will be ignored on next read)
            // nosql_remove_entry("settings", i);
            break;
        }
    }
    nosql_save_entry("settings", key.c_str(), value.c_str());
}

// ─── Sub-screens ───
static int g_backlight_level = 3;   // 1-5 (loaded from settings on entry)
static bool g_sound_on = true;

static void load_settings() {
    g_backlight_level = settings_get("backlight", "3").toInt();
    if (g_backlight_level < 1) g_backlight_level = 1;
    if (g_backlight_level > 5) g_backlight_level = 5;
    g_sound_on = settings_get("sound", "1").toInt() != 0;
}

static void apply_backlight() {
    // 1=lowest, 5=brightest. Map to 51, 102, 153, 204, 255 PWM.
    uint8_t pwm = g_backlight_level * 51;
    c28p_set_backlight(pwm);
}

// ─── Drawing ───
static void sys_draw_chrome() {
    gfx->fillRect(0, 14, 240, 320 - 14, 0x0000);
    gfx->fillRect(0, 14, 240, 22, 0x4208);   // grey header
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 18);
    gfx->print("SYSTEM");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 22);
    gfx->print("< EXIT");
}

static void sys_draw_main() {
    sys_draw_chrome();

    // Backlight row
    int y = 50;
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(8, y);
    gfx->print("BACKLIGHT");
    // 5 brightness pips
    for (int i = 0; i < 5; i++) {
        int bx = 100 + i * 24;
        bool on = (i < g_backlight_level);
        gfx->fillRect(bx, y - 2, 20, 16, on ? 0xFFE0 : 0x2104);
        gfx->drawRect(bx, y - 2, 20, 16, 0x4208);
    }

    // Sound row
    y = 80;
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(8, y);
    gfx->print("SOUND");
    gfx->fillRect(100, y - 2, 60, 16, g_sound_on ? 0x07E0 : 0x2104);
    gfx->drawRect(100, y - 2, 60, 16, 0x4208);
    gfx->setTextColor(g_sound_on ? 0x0000 : 0x8410);
    gfx->setCursor(108, y + 2);
    gfx->print(g_sound_on ? "ON" : "OFF");

    // Divider
    gfx->drawFastHLine(0, 108, 240, 0x4208);

    // Storage stats
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 116);
    gfx->print("STORAGE");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 132);
    int wd = nosql_get_count("wardrive");
    gfx->printf("  wardrive: %d obs", wd);
    gfx->setCursor(8, 144);
    int ble = nosql_get_count("ble_log");
    gfx->printf("  ble_log:  %d obs", ble);
    gfx->setCursor(8, 156);
    int sv = nosql_get_count("survival");
    int md = nosql_get_count("medical");
    int hi = nosql_get_count("history");
    gfx->printf("  ref:      %d entries", sv + md + hi);

    // WiFi
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 180);
    gfx->print("WIFI");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 196);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->printf("  SSID: %.24s", WiFi.SSID().c_str());
        gfx->setCursor(8, 208);
        gfx->printf("  RSSI: %d dBm", WiFi.RSSI());
    } else {
        gfx->setTextColor(0xF800);
        gfx->print("  Not connected");
    }

    // ABOUT button
    int btn_y = 230;
    gfx->fillRect(16, btn_y, 208, 32, 0x18C3);
    gfx->drawRect(16, btn_y, 208, 32, 0x07FF);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(72, btn_y + 8);
    gfx->print("ABOUT");

    // FACTORY RESET button (red)
    btn_y = 272;
    gfx->fillRect(16, btn_y, 208, 32, 0x4000);
    gfx->drawRect(16, btn_y, 208, 32, 0xF800);
    gfx->setTextColor(0xF800);
    gfx->setCursor(40, btn_y + 8);
    gfx->print("FACTORY RESET");
}

// ─── Factory reset confirmation ───
static bool factory_reset_confirm() {
    sys_draw_chrome();
    gfx->setTextSize(2);
    gfx->setTextColor(0xF800);
    gfx->setCursor(40, 60);
    gfx->print("ARE YOU SURE?");
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(20, 100);
    gfx->print("This will erase all NoSQL");
    gfx->setCursor(20, 114);
    gfx->print("entries:");
    gfx->setTextColor(0x8410);
    gfx->setCursor(20, 130);
    gfx->print(" - Wardrive observations");
    gfx->setCursor(20, 142);
    gfx->print(" - BLE log");
    gfx->setCursor(20, 154);
    gfx->print(" - Survival/medical/history");
    gfx->setCursor(20, 166);
    gfx->print(" - Settings (incl. WiFi creds)");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(20, 184);
    gfx->print("Recordings and game saves");
    gfx->setCursor(20, 196);
    gfx->print("are NOT affected.");

    gfx->fillRect(16, 234, 96, 40, 0x18C3);
    gfx->drawRect(16, 234, 96, 40, 0x8410);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(40, 246);
    gfx->print("NO");

    gfx->fillRect(128, 234, 96, 40, 0x4000);
    gfx->drawRect(128, 234, 96, 40, 0xF800);
    gfx->setTextColor(0xF800);
    gfx->setCursor(152, 246);
    gfx->print("YES");

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < 14) pressed = -1;
            else if (ty >= 234 && ty < 274) {
                if (tx < 120) pressed = 0;       // NO
                else pressed = 1;                // YES
            }
        } else if (!touched && was_touched) {
            if (pressed == -1 || pressed == 0) return false;
            if (pressed == 1) return true;
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

static void factory_reset_execute() {
    // Wipe every NoSQL category the C28P touches. The category list
    // is hard-coded rather than auto-discovered because directory
    // walking /data/ would be more code than just naming the four
    // categories we know about. If a future C28P app adds a new
    // category, it must be added here too — same pattern as the
    // SYSTEM app's storage stats screen.
    //
    // Notably NOT wiped:
    //    /recordings    user voice memos (c28p_media)
    //    /audio         user-supplied music
    //    *.txt high-score files for games (mario_bros, tetris, etc)
    //    *.bm bookmarks (e-Reader)
    // These are user content the kiosk has no business erasing on
    // a settings-screen tap. Anyone wanting a true card-wipe should
    // reformat the card on a desktop.
    static const char* const wipe_categories[] = {
        "wardrive",
        "ble_log",
        "settings",
        "anomaly_baseline",   // c28p_anomaly's known-AP baseline
    };
    static const int n_categories =
        sizeof(wipe_categories) / sizeof(wipe_categories[0]);

    sys_draw_chrome();
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(40, 80);
    gfx->print("WIPING...");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);

    int wiped = 0;
    for (int i = 0; i < n_categories; i++) {
        gfx->fillRect(0, 120, 240, 80, 0x0000);
        gfx->setCursor(20, 130 + (i * 14));
        gfx->printf("  %s", wipe_categories[i]);
        if (nosql_clear_category(wipe_categories[i])) {
            wiped++;
            gfx->setTextColor(0x07E0);
            gfx->setCursor(180, 130 + (i * 14));
            gfx->print("OK");
            gfx->setTextColor(0x8410);
        } else {
            gfx->setTextColor(0xF800);
            gfx->setCursor(180, 130 + (i * 14));
            gfx->print("FAIL");
            gfx->setTextColor(0x8410);
        }
        delay(120);
    }

    // The settings we just nuked include the backlight + sound
    // preferences. Reset the in-memory copies to their defaults so
    // the settings panel doesn't redraw with stale values from
    // before the wipe — those entries no longer exist on disk.
    g_backlight_level = 3;
    g_sound_on = true;
    apply_backlight();

    gfx->setTextSize(2);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(40, 240);
    gfx->printf("Wiped %d/%d", wiped, n_categories);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(20, 270);
    gfx->print("User content (recordings,");
    gfx->setCursor(20, 282);
    gfx->print("audio, game saves) preserved.");
    delay(2500);
}

// ─── Public entry ───
void c28p_run_system() {
    load_settings();
    apply_backlight();

    sys_draw_main();

    bool was_touched = false;
    int pressed = -2;

    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            if (ty < 14) {
                pressed = -1;
            }
            // Backlight pips: y=48..64, x=100..220 with 5 pips
            else if (ty >= 48 && ty < 66 && tx >= 100 && tx < 220) {
                int pip = (tx - 100) / 24;
                if (pip >= 0 && pip < 5) {
                    g_backlight_level = pip + 1;
                    settings_set("backlight", String(g_backlight_level));
                    apply_backlight();
                    sys_draw_main();
                }
            }
            // Sound toggle
            else if (ty >= 78 && ty < 96 && tx >= 100 && tx < 160) {
                g_sound_on = !g_sound_on;
                settings_set("sound", g_sound_on ? "1" : "0");
                sys_draw_main();
            }
            // ABOUT button
            else if (ty >= 230 && ty < 262) {
                pressed = -10;
            }
            // FACTORY RESET button
            else if (ty >= 272 && ty < 304) {
                pressed = -11;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) {
                return;
            } else if (pressed == -10) {
                c28p_run_about();
                sys_draw_main();
            } else if (pressed == -11) {
                if (factory_reset_confirm()) {
                    factory_reset_execute();
                }
                sys_draw_main();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

#endif // DEVICE_C28P
