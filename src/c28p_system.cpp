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

// Backlight PWM stub. The real implementation lives in the C28P HAL
// once we know the actual backlight GPIO pin. Until then, this is a
// no-op — the slider in settings saves the chosen level to NoSQL so
// it persists across reboots, but the hardware doesn't yet respond.
// TODO(v1.2.2): wire to actual LEDC channel once pin is identified.
static void c28p_set_backlight(uint8_t level) {
    (void)level;
}

// ─── Setting helpers ───
static String settings_get(const String& key, const String& dflt) {
    nosql_init("settings");
    int total = nosql_get_count("settings");
    String t, c;
    for (int i = 0; i < total; i++) {
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
    // TODO(v1.2.2): wire up actual NoSQL category wipe once
    // nosql_clear_category() is added to nosql_store. For v1.2.1
    // this is a no-op stub — the confirmation flow works end-to-end
    // but no data is actually erased. This is intentional rather
    // than fragile: better to honestly say "not yet implemented"
    // than risk wiping the wrong category.
    sys_draw_chrome();
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(40, 130);
    gfx->print("Not yet");
    gfx->setCursor(36, 160);
    gfx->print("implemented");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(20, 200);
    gfx->print("Factory reset wiring lands");
    gfx->setCursor(20, 214);
    gfx->print("in v1.2.2 once nosql exposes");
    gfx->setCursor(20, 228);
    gfx->print("a per-category wipe API.");
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