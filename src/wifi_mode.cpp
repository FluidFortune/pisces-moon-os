// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com

#include "wifi_mode.h"
#include "wardrive.h"
#include "wifi_manager.h"
#include "keyboard.h"
#include "pm_input.h"
#include <WiFi.h>
#include <Arduino.h>

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
extern PMDispTLoRaPager *gfx;
#else
#include <Arduino_GFX_Library.h>
extern Arduino_GFX *gfx;
#endif

// ─────────────────────────────────────────────────────────────────
// Mode state — read by every WiFi-using app before claiming the radio
// ─────────────────────────────────────────────────────────────────
pm_wifi_mode_t g_wifi_mode = WIFI_MODE_PM_OFF;

const char* pm_wifi_mode_str(pm_wifi_mode_t mode) {
    switch (mode) {
        case WIFI_MODE_PM_OFF:     return "OFF";
        case WIFI_MODE_PM_CLIENT:  return "CLIENT";
        case WIFI_MODE_PM_SCANNER: return "SCANNER";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────
// Modal confirmation prompt — used to ask before destructive switches
//
// Renders a centered dialog with title, message, [Y]es / [N]o hints.
// Blocks until user presses Y/y/Enter (confirm) or N/n/Q/Esc (decline).
//
// Cardputer (240x135) and T-Deck Plus (320x240) share a layout that
// works on both — the dialog auto-sizes to about 80% of screen width
// and centers vertically.
// ─────────────────────────────────────────────────────────────────
static bool show_confirm_modal(const char* title, const char* line1,
                               const char* line2) {
#ifdef DEVICE_TLORAPAGER
    const int W = 480;
    const int H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
    const int W = 240;
    const int H = 135;
#else
    const int W = 320;
    const int H = 240;
#endif

    const int boxW = (int)(W * 0.85);
    const int boxH = (H < 160) ? 100 : 130;
    const int boxX = (W - boxW) / 2;
    const int boxY = (H - boxH) / 2;

    // Dim background
    gfx->fillRect(0, 0, W, H, 0x0000);
    // Box
    gfx->fillRect(boxX, boxY, boxW, boxH, 0x18C3);
    gfx->fillRect(boxX, boxY, boxW, 18, 0x4208);

    // Title bar
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(boxX + 6, boxY + 5);
    gfx->print(title);

    // Body
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(boxX + 6, boxY + 26);
    gfx->print(line1);
    if (line2 && line2[0]) {
        gfx->setCursor(boxX + 6, boxY + 40);
        gfx->print(line2);
    }

    // Y/N hints
    gfx->setTextColor(0x07E0);
    gfx->setCursor(boxX + 6, boxY + boxH - 16);
    gfx->print("[Y] Switch");
    gfx->setTextColor(0xF800);
    gfx->setCursor(boxX + boxW - 70, boxY + boxH - 16);
    gfx->print("[N] Cancel");

    // Wait for keypress
    while (true) {
        char k = get_keypress();
        if (k == 'y' || k == 'Y' || k == '\r' || k == '\n') return true;
        if (k == 'n' || k == 'N' || pm_is_exit_key(k))     return false;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────────────────────────
// Teardown — drops current WiFi state entirely, returns to OFF.
// ─────────────────────────────────────────────────────────────────
bool teardown_wifi_mode() {
    Serial.printf("[WIFI-MODE] Teardown from %s\n", pm_wifi_mode_str(g_wifi_mode));

    if (g_wifi_mode == WIFI_MODE_PM_OFF) {
        return true;
    }

    if (g_wifi_mode == WIFI_MODE_PM_SCANNER) {
        // The wardrive task owns scanner-mode WiFi. Tearing it down
        // also frees its 48KB NimBLE allocation and ~13KB internal
        // task state, ~61KB total reclaimed.
        bool ok = wardrive_teardown(3000);
        if (!ok) {
            Serial.println("[WIFI-MODE] wardrive_teardown failed");
            return false;
        }
    }

    if (g_wifi_mode == WIFI_MODE_PM_CLIENT) {
        // Client mode — disconnect from AP, drop driver
        WiFi.disconnect(true, true);  // eraseAP=true, eraseAll=true
        WiFi.mode(WIFI_OFF);
    }

    // Belt-and-suspenders: ensure driver is fully off
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));

    g_wifi_mode = WIFI_MODE_PM_OFF;
    Serial.printf("[WIFI-MODE] Teardown complete, free heap: %u\n",
                  (unsigned)ESP.getFreeHeap());
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Initialize WiFi in CLIENT mode and try to associate.
// ─────────────────────────────────────────────────────────────────
bool init_wifi_client() {
    if (g_wifi_mode != WIFI_MODE_PM_OFF) {
        Serial.printf("[WIFI-MODE] init_wifi_client called in mode %s — refusing\n",
                      pm_wifi_mode_str(g_wifi_mode));
        return false;
    }
    Serial.println("[WIFI-MODE] Initializing CLIENT mode");
    WiFi.mode(WIFI_STA);
    // auto_connect_wifi() reads /wifi.conf from SD and calls WiFi.begin().
    // It is non-blocking. Caller can poll WiFi.status() if it needs to
    // wait for association.
    auto_connect_wifi();
    g_wifi_mode = WIFI_MODE_PM_CLIENT;
    Serial.printf("[WIFI-MODE] CLIENT mode active, free heap: %u\n",
                  (unsigned)ESP.getFreeHeap());
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Initialize WiFi in SCANNER mode and spawn wardrive_task.
// ─────────────────────────────────────────────────────────────────
bool init_wifi_scanner() {
    if (g_wifi_mode != WIFI_MODE_PM_OFF) {
        Serial.printf("[WIFI-MODE] init_wifi_scanner called in mode %s — refusing\n",
                      pm_wifi_mode_str(g_wifi_mode));
        return false;
    }
    Serial.println("[WIFI-MODE] Initializing SCANNER mode");
    // wardrive_task calls WiFi.mode(WIFI_STA) + WiFi.disconnect(false)
    // internally as part of its scan loop setup. We just need to spawn
    // the task; it does the rest.
    init_wardrive_core();
    g_wifi_mode = WIFI_MODE_PM_SCANNER;
    Serial.printf("[WIFI-MODE] SCANNER mode active, free heap: %u\n",
                  (unsigned)ESP.getFreeHeap());
    return true;
}

// ─────────────────────────────────────────────────────────────────
// Public entry point: every WiFi-using app calls this before
// touching the WiFi radio.
// ─────────────────────────────────────────────────────────────────
bool request_wifi_mode(pm_wifi_mode_t mode, const char* app_name) {
    Serial.printf("[WIFI-MODE] %s requests %s (current: %s)\n",
                  app_name ? app_name : "?",
                  pm_wifi_mode_str(mode),
                  pm_wifi_mode_str(g_wifi_mode));

    // Already in the right mode — fast path
    if (g_wifi_mode == mode) return true;

    // Currently OFF — bring up requested mode, no prompt needed
    if (g_wifi_mode == WIFI_MODE_PM_OFF) {
        if (mode == WIFI_MODE_PM_CLIENT)  return init_wifi_client();
        if (mode == WIFI_MODE_PM_SCANNER) return init_wifi_scanner();
        return false;  // OFF requested while OFF was already weirdly handled
    }

    // Mode conflict — need user consent on Cardputer; permissive elsewhere
#ifdef DEVICE_CARDPUTER_ADV
    const char* line1;
    const char* line2;
    if (g_wifi_mode == WIFI_MODE_PM_SCANNER) {
        line1 = "Stop wardrive";
        line2 = "to use this app?";
    } else {
        line1 = "Disconnect WiFi";
        line2 = "to use this app?";
    }
    bool consent = show_confirm_modal(app_name ? app_name : "WiFi Mode Switch",
                                      line1, line2);
    if (!consent) {
        Serial.println("[WIFI-MODE] User declined switch");
        return false;
    }
#endif
    // On non-Cardputer or after consent: tear down + reinit
    if (!teardown_wifi_mode()) return false;

    if (mode == WIFI_MODE_PM_OFF)     return true;
    if (mode == WIFI_MODE_PM_CLIENT)  return init_wifi_client();
    if (mode == WIFI_MODE_PM_SCANNER) return init_wifi_scanner();
    return false;
}