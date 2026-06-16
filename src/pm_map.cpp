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
 * MAP — offline moving map + GPS breadcrumb trail
 *
 * The anchor app for the map engine. Renders on every device by drawing
 * through pm_map_engine (which sizes from gfx at runtime). Raster tiles
 * load from /maps/<set>/<z>/<x>/<y>.png on the SD card; with no card or
 * no tiles you get a coordinate graticule and everything still works.
 *
 * Controls are unified across the fleet:
 *   touch drag / trackball roll / arrows / WASD   pan
 *   +/- (or , .)                                   zoom out / in
 *   g  or  center-tap  or  trackball-click         recenter on GPS, follow
 *   t                                              toggle trail logging
 *   c                                              clear trail
 *   q / back / tap-header                          exit
 *
 * The trail auto-logs the GPS fix once per second while logging is on,
 * giving a "where have I driven" breadcrumb that ties straight into the
 * wardriving workflow (did I already cover this street?).
 */

#include "pm_map_apps.h"
#include "pm_map_engine.h"
#include <Arduino.h>
#include <TinyGPSPlus.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"

#if defined(DEVICE_TLORAPAGER)
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif

extern Arduino_GFX *gfx;
extern TinyGPSPlus  gps;

// Header band height; the map fills the rest.
#define MAP_HDR_H 16

static void mapDrawHeader(bool trailOn) {
    int w = gfx->width();
    gfx->fillRect(0, 0, w, MAP_HDR_H, 0x0000);
    gfx->drawFastHLine(0, MAP_HDR_H - 1, w, 0x07E0);
    gfx->setTextSize(1);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(4, 4);
    gfx->print("MAP");
    // right side: trail + fix state
    char r[28];
    snprintf(r, sizeof(r), "%s  %s", trailOn ? "TRAIL" : "trail-off",
             gps.location.isValid() ? "FIX" : "no-fix");
    int rw = (int)strlen(r) * 6;
    gfx->setTextColor(gps.location.isValid() ? 0x07E0 : 0x4208);
    gfx->setCursor(w - rw - 4, 4);
    gfx->print(r);
}

void run_map() {
    pm_map_begin("osm");                 // tiles from /maps/osm/...
    int w = gfx->width(), h = gfx->height();
    pm_map_set_content_rect(0, MAP_HDR_H, w, h - MAP_HDR_H);

    // Seed the viewport on the GPS fix if we have one.
    if (gps.location.isValid())
        pm_map_set_view(gps.location.lat(), gps.location.lng(), 15);
    pm_map_recenter_gps();

    bool trailOn = true;
    uint32_t lastTrail = 0;
    bool running = true;
    bool needRedraw = true;

    int16_t dragLastX = -1, dragLastY = -1;
    bool dragging = false;

    gfx->fillScreen(0x0000);

    while (running) {
        // ── input ──
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;
        bool touched = get_touch(&tx, &ty);

        // header tap = exit
        if (touched && ty < MAP_HDR_H) {
            while (get_touch(&tx, &ty)) delay(8);
            running = false;
            continue;
        }

        // touch drag to pan
        if (touched) {
            if (!dragging) { dragging = true; dragLastX = tx; dragLastY = ty; }
            else {
                int dx = tx - dragLastX, dy = ty - dragLastY;
                if (dx || dy) {
                    pm_map_handle_pan(dx, dy);
                    dragLastX = tx; dragLastY = ty;
                    needRedraw = true;
                }
            }
        } else if (dragging) {
            dragging = false;
        }

        // trackball roll = pan; click = recenter
        if (tb.x || tb.y) {
            pm_map_handle_pan(-tb.x * 24, -tb.y * 24);
            needRedraw = true;
        }
        if (tb.clicked) { pm_map_recenter_gps(); needRedraw = true; }

        // keys
        if (k) {
            switch (k) {
                case 'q': case 'Q': case 27: running = false; break;
                case '+': case '=': case '.': pm_map_zoom_in();  needRedraw = true; break;
                case '-': case '_': case ',': pm_map_zoom_out(); needRedraw = true; break;
                case 'w': case 'W': pm_map_handle_pan(0,  40); needRedraw = true; break;
                case 's': case 'S': pm_map_handle_pan(0, -40); needRedraw = true; break;
                case 'a': case 'A': pm_map_handle_pan( 40, 0); needRedraw = true; break;
                case 'd': case 'D': pm_map_handle_pan(-40, 0); needRedraw = true; break;
                case 'g': case 'G': pm_map_recenter_gps(); needRedraw = true; break;
                case 't': case 'T': trailOn = !trailOn; needRedraw = true; break;
                case 'c': case 'C': pm_map_trail_clear(); needRedraw = true; break;
                default: break;
            }
        }

        // ── trail logging: 1 Hz while on and fixed ──
        if (trailOn && gps.location.isValid() && millis() - lastTrail > 1000) {
            pm_map_trail_push(gps.location.lat(), gps.location.lng());
            lastTrail = millis();
            needRedraw = true;
        }

        // follow mode keeps redrawing as the fix moves
        if (pm_map_follow() && gps.location.isValid()) needRedraw = true;

        if (needRedraw) {
            pm_map_draw_basemap();
            pm_map_trail_draw(0xFD20);          // amber breadcrumb
            pm_map_draw_hud("MAP");
            mapDrawHeader(trailOn);
            needRedraw = false;
        }

        delay(15);
        yield();
    }

    pm_map_end();
    gfx->fillScreen(0x0000);
}
