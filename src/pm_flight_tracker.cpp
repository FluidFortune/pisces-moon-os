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
 * FLIGHT TRACKER — live aircraft on the moving map
 *
 * Pulls the OpenSky Network free REST API (no key, anonymous) for all
 * aircraft inside a lat/lon bounding box around the current viewport,
 * and plots each one as a heading arrow with callsign + altitude on the
 * shared map engine. Renders on every WiFi-capable device by drawing
 * through pm_map_engine (runtime-sized from gfx).
 *
 *   API: https://opensky-network.org/api/states/all?lamin=&lomin=&lamax=&lomax=
 *   Response: {"time":..,"states":[[icao24,callsign,origin_country,
 *              time_position,last_contact,lon,lat,baro_alt,on_ground,
 *              velocity,true_track,vertical_rate,...], ...]}
 *
 * We hand-scan the JSON array rather than pulling in a JSON lib: the
 * fields we need (lon idx5, lat idx6, alt idx7, on_ground idx8,
 * velocity idx9, true_track idx10, callsign idx1) are positional, and
 * the response can be large, so a streaming positional scan keeps RAM
 * flat. Anonymous OpenSky is rate-limited (~10s resolution), so we poll
 * every 12s and animate nothing in between — aircraft jump on refresh.
 *
 * Offline / no WiFi: shows a banner and still lets you pan the basemap.
 * Direct 1090 MHz ADS-B reception (RTL-SDR / companion board) is the
 * future hardware path; this is the API path that works on any unit
 * with WiFi today.
 */

#include "pm_map_apps.h"
#include "pm_map_engine.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
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

#define FT_HDR_H 16

#ifdef DEVICE_NO_PSRAM
  #define FT_MAX_AC 48
#else
  #define FT_MAX_AC 160
#endif

struct Aircraft {
    float    lat, lon;
    float    track;      // degrees, true
    int      altM;       // barometric altitude (m), -1 if unknown
    bool     onGround;
    char     call[10];
};
static Aircraft s_ac[FT_MAX_AC];
static int      s_acCount = 0;
static uint32_t s_lastFetch = 0;
static char     s_status[40] = "starting";

// ── tiny positional JSON scan over the OpenSky states array ──
// We don't parse the whole document; we walk it, and for each inner
// "[" ... "]" state vector we pull the fields by comma index.
static void parseStates(const String& body) {
    s_acCount = 0;
    int i = body.indexOf("\"states\":");
    if (i < 0) { strncpy(s_status, "no states", sizeof(s_status)); return; }
    i = body.indexOf('[', i);          // outer array
    if (i < 0) return;
    int depth = 0;
    int len = body.length();

    while (i < len && s_acCount < FT_MAX_AC) {
        char c = body[i];
        if (c == '[') {
            depth++;
            if (depth == 2) {
                // start of one state vector; scan to matching ']'
                int j = i + 1;
                int field = 0;
                // field accumulators
                String call = "";
                double lon = 0, lat = 0, vel = 0, trk = 0;
                int alt = -1; bool ground = false;
                bool haveLat = false, haveLon = false;
                String cur = "";
                bool inStr = false;
                while (j < len) {
                    char d = body[j];
                    if (d == '"') { inStr = !inStr; j++; continue; }
                    if (!inStr && (d == ',' || d == ']')) {
                        // commit field `field` from cur
                        cur.trim();
                        switch (field) {
                            case 1: call = cur; break;                 // callsign
                            case 5: if (cur.length() && cur != "null") { lon = cur.toDouble(); haveLon = true; } break;
                            case 6: if (cur.length() && cur != "null") { lat = cur.toDouble(); haveLat = true; } break;
                            case 7: if (cur.length() && cur != "null") alt = (int)cur.toDouble(); break;
                            case 8: ground = (cur == "true"); break;
                            case 9: if (cur.length() && cur != "null") vel = cur.toDouble(); break;
                            case 10: if (cur.length() && cur != "null") trk = cur.toDouble(); break;
                            default: break;
                        }
                        cur = "";
                        field++;
                        if (d == ']') { j++; break; }
                        j++;
                        continue;
                    }
                    if (d != '"') cur += d;
                    j++;
                }
                (void)vel;
                if (haveLat && haveLon && s_acCount < FT_MAX_AC) {
                    Aircraft& a = s_ac[s_acCount++];
                    a.lat = (float)lat; a.lon = (float)lon;
                    a.track = (float)trk; a.altM = alt; a.onGround = ground;
                    call.replace("\"", ""); call.trim();
                    strncpy(a.call, call.c_str(), sizeof(a.call) - 1);
                    a.call[sizeof(a.call) - 1] = '\0';
                }
                i = j;
                depth--;
                continue;
            }
        } else if (c == ']') {
            depth--;
            if (depth <= 0) break;
        }
        i++;
    }
    snprintf(s_status, sizeof(s_status), "%d aircraft", s_acCount);
}

static void fetchAircraft() {
    if (WiFi.status() != WL_CONNECTED) {
        strncpy(s_status, "no WiFi", sizeof(s_status));
        return;
    }
    // Bounding box ~ +/- 1.2 deg around viewport center (~130km tall).
    double clat = pm_map_center_lat(), clon = pm_map_center_lon();
    double dLat = 1.2, dLon = 1.6;
    char url[200];
    snprintf(url, sizeof(url),
        "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
        clat - dLat, clon - dLon, clat + dLat, clon + dLon);

    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(8000);
    if (!http.begin(url)) { strncpy(s_status, "http begin fail", sizeof(s_status)); return; }
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        parseStates(body);
    } else {
        snprintf(s_status, sizeof(s_status), "HTTP %d", code);
    }
    http.end();
}

static void ftHeader() {
    int w = gfx->width();
    gfx->fillRect(0, 0, w, FT_HDR_H, 0x0000);
    gfx->drawFastHLine(0, FT_HDR_H - 1, w, 0x07FF);
    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(4, 4);
    gfx->print("FLIGHTS");
    int rw = (int)strlen(s_status) * 6;
    gfx->setTextColor(WiFi.status() == WL_CONNECTED ? 0x07E0 : 0xFD20);
    gfx->setCursor(w - rw - 4, 4);
    gfx->print(s_status);
}

static void drawAircraft() {
    for (int i = 0; i < s_acCount; i++) {
        Aircraft& a = s_ac[i];
        uint16_t col = a.onGround ? 0x4208 : 0x07FF;
        // altitude tints: low=amber, high=cyan-white
        if (!a.onGround) {
            if (a.altM >= 0 && a.altM < 3000) col = 0xFD20;
            else if (a.altM >= 9000) col = 0xFFFF;
        }
        pm_map_arrow(a.lat, a.lon, a.track, 5, col);
        if (a.call[0] && a.call[0] != ' ')
            pm_map_label(a.lat, a.lon, 7, -3, a.call, col);
    }
}

void run_flight_tracker() {
    pm_map_begin("osm");
    int w = gfx->width(), h = gfx->height();
    pm_map_set_content_rect(0, FT_HDR_H, w, h - FT_HDR_H);

    if (gps.location.isValid())
        pm_map_set_view(gps.location.lat(), gps.location.lng(), 8);
    else
        pm_map_set_view(pm_map_center_lat(), pm_map_center_lon(), 8);

    s_acCount = 0;
    s_lastFetch = 0;
    strncpy(s_status, WiFi.status() == WL_CONNECTED ? "fetching" : "no WiFi", sizeof(s_status));

    gfx->fillScreen(0x0000);
    bool running = true, needRedraw = true;
    int16_t dragLastX = -1, dragLastY = -1; bool dragging = false;

    while (running) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty; bool touched = get_touch(&tx, &ty);

        if (touched && ty < FT_HDR_H) { while (get_touch(&tx, &ty)) delay(8); running = false; continue; }
        if (touched) {
            if (!dragging) { dragging = true; dragLastX = tx; dragLastY = ty; }
            else { int dx = tx - dragLastX, dy = ty - dragLastY;
                   if (dx || dy) { pm_map_handle_pan(dx, dy); dragLastX = tx; dragLastY = ty; needRedraw = true; } }
        } else dragging = false;

        if (tb.x || tb.y) { pm_map_handle_pan(-tb.x * 24, -tb.y * 24); needRedraw = true; }
        if (tb.clicked)   { pm_map_recenter_gps(); needRedraw = true; }

        if (k) switch (k) {
            case 'q': case 'Q': case 27: running = false; break;
            case '+': case '=': case '.': pm_map_zoom_in();  needRedraw = true; break;
            case '-': case '_': case ',': pm_map_zoom_out(); needRedraw = true; break;
            case 'w': case 'W': pm_map_handle_pan(0,  40); needRedraw = true; break;
            case 's': case 'S': pm_map_handle_pan(0, -40); needRedraw = true; break;
            case 'a': case 'A': pm_map_handle_pan( 40, 0); needRedraw = true; break;
            case 'd': case 'D': pm_map_handle_pan(-40, 0); needRedraw = true; break;
            case 'g': case 'G': pm_map_recenter_gps(); needRedraw = true; break;
            case 'r': case 'R': s_lastFetch = 0; break;       // force refresh
            default: break;
        }

        // poll every 12s (OpenSky anon rate limit friendly)
        if (millis() - s_lastFetch > 12000) {
            strncpy(s_status, "fetching...", sizeof(s_status));
            ftHeader();
            fetchAircraft();
            s_lastFetch = millis();
            needRedraw = true;
        }

        if (pm_map_follow() && gps.location.isValid()) needRedraw = true;

        if (needRedraw) {
            pm_map_draw_basemap();
            drawAircraft();
            pm_map_draw_hud("FLIGHTS");
            ftHeader();
            needRedraw = false;
        }

        delay(15);
        yield();
    }

    pm_map_end();
    gfx->fillScreen(0x0000);
}
