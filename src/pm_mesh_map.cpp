// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com

/**
 * MESH MAP — motion-assisted Meshtastic/LoRa location estimates
 *
 * Mesh Map is a read-only consumer of Wardrive's shared SX1262 receiver.
 * It never creates a second RadioLib Module, installs another DIO1 ISR, or
 * touches LoRa SPI itself. This is especially important on Cardputer ADV,
 * where LoRa and SD share one bus and internal SRAM is limited.
 *
 * Direct packets feed a fixed-memory streaming estimator in
 * pm_lora_locator.cpp. Repeated GPS-tagged RSSI observations are rendered as
 * an estimated center plus an uncertainty ring. Relayed packets are counted
 * as heard nodes but excluded from the position fit because their RSSI
 * belongs to the final relay hop.
 */

#include "pm_map_apps.h"

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)

#include "pm_map_engine.h"
#include "pm_lora_locator.h"
#include "wardrive.h"
#include "wifi_mode.h"
#include <Arduino.h>
#include <TinyGPSPlus.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"

#if defined(DEVICE_TLORAPAGER)
#include "pm_disp_tlorapager.h"
extern PMDispTLoRaPager *gfx;
#else
#include <Arduino_GFX_Library.h>
extern Arduino_GFX *gfx;
#endif

extern TinyGPSPlus  gps;

#define MM_HDR_H 16
#define MM_INFO_H 12

#ifdef DEVICE_NO_PSRAM
  #define MM_MAX_NODES 12
#else
  #define MM_MAX_NODES 32
#endif

// Static snapshot buffer: deterministic memory, no per-frame heap activity.
static PMLoraLocationEstimate s_estimates[MM_MAX_NODES];
static size_t s_estimate_count = 0;
static size_t s_position_count = 0;

static uint16_t rssiColor(int rssi) {
    if (rssi >= -70) return 0x07E0;
    if (rssi >= -95) return 0xFD20;
    return 0xF800;
}

static uint16_t confidenceColor(pm_lora_confidence_t confidence) {
    switch (confidence) {
        case PM_LORA_CONF_HIGH:   return 0x07E0;
        case PM_LORA_CONF_MEDIUM: return 0xFD20;
        case PM_LORA_CONF_LOW:    return 0xFBE0;
        default:                  return 0x8410;
    }
}

static void refreshEstimates() {
    s_estimate_count =
        pm_lora_locator_snapshot(s_estimates, MM_MAX_NODES);
    s_position_count = 0;
    for (size_t i = 0; i < s_estimate_count; ++i) {
        if (s_estimates[i].position_valid) ++s_position_count;
    }
}

static void formatRadius(float radius_m, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    if (radius_m < 1000.0f) {
        snprintf(out, out_len, "%.0fm", radius_m);
    } else {
        snprintf(out, out_len, "%.1fkm", radius_m / 1000.0f);
    }
}

static void drawHeader() {
    int w = gfx->width();
    gfx->fillRect(0, 0, w, MM_HDR_H, 0x0000);
    gfx->drawFastHLine(0, MM_HDR_H - 1, w, 0x07E0);
    gfx->setTextSize(1);
    gfx->setTextColor(wardrive_lora_is_ready() ? 0x07E0 : 0xFD20);
    gfx->setCursor(4, 4);
    gfx->print("MESH LOCATOR");

    char status[40];
    snprintf(status, sizeof(status), "%u/%u %s",
             (unsigned)s_position_count,
             (unsigned)s_estimate_count,
             wardrive_lora_is_ready() ? "RX" : "WAIT");
    int rw = (int)strlen(status) * 6;
    gfx->setCursor(w - rw - 4, 4);
    gfx->print(status);
}

static void drawNodes() {
    uint32_t now = millis();
    for (size_t i = 0; i < s_estimate_count; ++i) {
        const PMLoraLocationEstimate& e = s_estimates[i];
        if (!e.position_valid) continue;

        // Fade stale nodes after five minutes, but leave their last estimate
        // visible so a completed pass can still be inspected.
        bool stale = (now - e.last_seen_ms) > 300000UL;
        uint16_t marker = stale ? 0x4208 : rssiColor(e.best_rssi);
        uint16_t ring = stale ? 0x2104 : confidenceColor(e.confidence);

        pm_map_ring(e.latitude, e.longitude, e.radius_m, ring);
        pm_map_marker(e.latitude, e.longitude, 4, marker, 0xFFFF);

        char label[16];
        const char* confidence =
            (e.confidence == PM_LORA_CONF_HIGH) ? "H" :
            (e.confidence == PM_LORA_CONF_MEDIUM) ? "M" :
            (e.confidence == PM_LORA_CONF_LOW) ? "L" : "?";
        snprintf(label, sizeof(label), "%04x%s",
                 (unsigned)(e.node_id & 0xFFFF), confidence);
        pm_map_label(e.latitude, e.longitude, 6, -3, label, marker);

        // Mark the strongest actual observation. The line to the estimate
        // makes it clear that the center is inferred, not a decoded GPS fix.
        if (e.direct_samples > 1) {
            pm_map_line(e.strongest_latitude, e.strongest_longitude,
                        e.latitude, e.longitude, 0x4208);
            pm_map_marker(e.strongest_latitude, e.strongest_longitude,
                          2, 0x07FF, 0x07FF);
        }
    }
}

static const PMLoraLocationEstimate* freshestPosition() {
    for (size_t i = 0; i < s_estimate_count; ++i) {
        if (s_estimates[i].position_valid) return &s_estimates[i];
    }
    return nullptr;
}

static void drawInfoBar() {
    int w = gfx->width();
    int h = gfx->height();
    int y = h - MM_INFO_H;
    gfx->fillRect(0, y, w, MM_INFO_H, 0x0000);
    gfx->drawFastHLine(0, y, w, 0x4208);
    gfx->setTextSize(1);
    gfx->setCursor(3, y + 3);

    const PMLoraLocationEstimate* e = freshestPosition();
    if (!e) {
        gfx->setTextColor(0x8410);
        if (!wardrive_lora_is_ready()) {
            gfx->print("Starting shared Wardrive LoRa receiver...");
        } else if (!gps.location.isValid()) {
            gfx->print("Waiting for GPS fix...");
        } else {
            gfx->print("Collecting direct node packets...");
        }
        return;
    }

    char radius[16];
    formatRadius(e->radius_m, radius, sizeof(radius));
    gfx->setTextColor(confidenceColor(e->confidence));
    gfx->printf("!%08x +/- %s %s n=%u",
                (unsigned)e->node_id,
                radius,
                pm_lora_confidence_label(e->confidence),
                (unsigned)e->direct_samples);
}

void run_mesh_map() {
#ifdef DEVICE_CARDPUTER_ADV
    // Cardputer cannot afford a client WiFi stack beside scanner/NimBLE.
    // Reuse the established mode-lock path; it also spawns Wardrive lazily.
    if (!request_wifi_mode(WIFI_MODE_PM_SCANNER, "Mesh Map")) return;
#else
    // T-Deck Plus and Pager normally spawn Wardrive at boot. Idempotent call
    // covers diagnostic builds that enter the map before the Ghost Engine.
    init_wardrive_core();
#endif

    bool was_active = wardrive_active;
    wardrive_active = true;

#ifdef DEVICE_CARDPUTER_ADV
    // No-PSRAM safety: tile PNGs are normally read into a temporary heap
    // buffer. Wardrive + WiFi + NimBLE need that headroom, so the locator
    // uses the allocation-free graticule basemap on Cardputer.
    pm_map_begin(nullptr);
#else
    pm_map_begin("osm");
#endif
    int w = gfx->width();
    int h = gfx->height();
    pm_map_set_content_rect(0, MM_HDR_H, w,
                            h - MM_HDR_H - MM_INFO_H);
    if (gps.location.isValid())
        pm_map_set_view(gps.location.lat(), gps.location.lng(), 14);
    pm_map_recenter_gps();

    refreshEstimates();
    gfx->fillScreen(0x0000);
    drawHeader();
    drawInfoBar();

    bool running = true;
    bool need_redraw = true;
    uint32_t last_ui = 0;
    int16_t drag_last_x = -1;
    int16_t drag_last_y = -1;
    bool dragging = false;

    while (running) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;
        bool touched = get_touch(&tx, &ty);

        if (touched && ty < MM_HDR_H) {
            while (get_touch(&tx, &ty)) delay(8);
            running = false;
            continue;
        }
        if (touched) {
            if (!dragging) {
                dragging = true;
                drag_last_x = tx;
                drag_last_y = ty;
            } else {
                int dx = tx - drag_last_x;
                int dy = ty - drag_last_y;
                if (dx || dy) {
                    pm_map_handle_pan(dx, dy);
                    drag_last_x = tx;
                    drag_last_y = ty;
                    need_redraw = true;
                }
            }
        } else {
            dragging = false;
        }

        if (tb.x || tb.y) {
            pm_map_handle_pan(-tb.x * 24, -tb.y * 24);
            need_redraw = true;
        }
        if (tb.clicked) {
            pm_map_recenter_gps();
            need_redraw = true;
        }

        if (k) switch (k) {
            case 'q': case 'Q': case 27:
                running = false;
                break;
            case '+': case '=': case '.':
                pm_map_zoom_in();
                need_redraw = true;
                break;
            case '-': case '_': case ',':
                pm_map_zoom_out();
                need_redraw = true;
                break;
            case 'w': case 'W':
                pm_map_handle_pan(0, 40);
                need_redraw = true;
                break;
            case 's': case 'S':
                pm_map_handle_pan(0, -40);
                need_redraw = true;
                break;
            case 'a': case 'A':
                pm_map_handle_pan(40, 0);
                need_redraw = true;
                break;
            case 'd': case 'D':
                pm_map_handle_pan(-40, 0);
                need_redraw = true;
                break;
            case 'g': case 'G':
                pm_map_recenter_gps();
                need_redraw = true;
                break;
            case 'c': case 'C':
                pm_lora_locator_reset();
                refreshEstimates();
                need_redraw = true;
                break;
            default:
                break;
        }

        if (pm_map_follow() && gps.location.isValid()) need_redraw = true;
        if (millis() - last_ui > 1000UL) {
            refreshEstimates();
            need_redraw = true;
        }

        if (need_redraw) {
            pm_map_draw_basemap();
            drawNodes();
            pm_map_draw_hud("MESH LOCATOR");
            drawHeader();
            drawInfoBar();
            need_redraw = false;
            last_ui = millis();
        }

        delay(15);
        yield();
    }

    wardrive_active = was_active;
    pm_map_end();
    gfx->fillScreen(0x0000);
}

#endif // SX1262 LoRa devices only
