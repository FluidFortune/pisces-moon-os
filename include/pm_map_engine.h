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

#ifndef PM_MAP_ENGINE_H
#define PM_MAP_ENGINE_H

#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  PM MAP ENGINE — shared moving-map core
//
//  One viewport/projection/render layer that every map-flavored app
//  draws through, on EVERY device (Cardputer ADV 240x135, T-Deck Plus
//  320x240, T-LoRa Pager 480x222, C28P 240x320, Maxine 480x800). The
//  engine reads gfx->width()/height() at runtime, so the same code
//  paints correctly on each panel with no per-device geometry forks.
//
//  WHAT IT IS
//    - A Web-Mercator (EPSG:3857) projection, identical to every web
//      map / OSM tile scheme, so SD tiles in standard z/x/y layout
//      and live API lat/lon both land in the same coordinate space.
//    - A GPS-anchored viewport: center lat/lon + integer zoom. The
//      viewport can follow the GPS fix ("north-up, you-centered") or
//      be panned/zoomed freely (touch / trackball / keys), then
//      re-locked to GPS.
//    - A raster basemap layer that blits 256x256 tiles from the SD
//      card (/maps/<set>/<z>/<x>/<y>.png) through the device's PNG
//      path. Missing tiles draw a graticule placeholder, so the app
//      is fully functional with no SD card or no tiles — you just get
//      a coordinate grid instead of imagery.
//    - World<->screen transforms (pm_map_world_to_screen /
//      pm_map_screen_to_world) so overlay apps (flight tracker,
//      Meshtastic live map, APRS, breadcrumb trail) only ever think
//      in lat/lon and let the engine place pixels.
//
//  WHAT IT IS NOT
//    - Not a vector renderer (yet). v1 is raster basemap + vector
//      OVERLAYS (tracks, markers, range rings) drawn by the engine's
//      primitive helpers on top. Vector basemaps (roads from a packed
//      OSM binary) are the planned v2 and slot in as an alternate
//      basemap layer behind the same projection.
//
//  DRAW MODEL
//    Apps don't subclass anything. They:
//      1. pm_map_begin()      once, to bind to the active gfx + GPS.
//      2. each frame: pm_map_draw_basemap()  to paint tiles/grid,
//         then their own overlay using pm_map_world_to_screen() +
//         the pm_map_draw_* primitives, then pm_map_draw_hud() for
//         the shared scale bar / center crosshair / zoom + GPS badge.
//      3. feed input via pm_map_handle_pan/zoom/recenter.
//      4. pm_map_end() on exit.
//
//  COORDINATES
//    Latitude  in degrees, +N  (range -85.0511..85.0511 in Mercator)
//    Longitude in degrees, +E  (range -180..180)
//    Zoom      integer 0..19, OSM convention (each +1 doubles scale)
// ─────────────────────────────────────────────────────────────

// RGB565 colors the engine uses for its built-in chrome. Apps may
// reuse these for overlay consistency.
#define PM_MAP_COL_GRID     0x10A2   // graticule / placeholder lines
#define PM_MAP_COL_GRID_DIM 0x0841
#define PM_MAP_COL_HUD_BG   0x0000
#define PM_MAP_COL_HUD_FG   0x07E0   // green
#define PM_MAP_COL_HUD_DIM  0x4208
#define PM_MAP_COL_CROSS    0xFD20   // center crosshair (amber)
#define PM_MAP_COL_SELF     0x07FF   // own-position marker (cyan)
#define PM_MAP_COL_SELF_RING 0x035F
#define PM_MAP_COL_WARN     0xF800

// Tile pixel size — OSM/web standard. Do not change without a tileset
// that matches.
#define PM_MAP_TILE_PX 256

// Maximum basemap tileset name length (folder under /maps/).
#define PM_MAP_SET_NAME_MAX 24

// ── Lifecycle ────────────────────────────────────────────────

// Bind the engine to the active display and start a session. `tileSet`
// is the subfolder under /maps/ to pull raster tiles from (e.g.
// "osm", "sat"); pass nullptr or "" to run grid-only (no tile reads).
// Safe to call with no SD card present. Returns true always (the
// engine degrades rather than fails).
bool pm_map_begin(const char* tileSet);

// End the session, freeing the tile cache. Idempotent.
void pm_map_end();

// ── Viewport control ─────────────────────────────────────────

// Hard-set the viewport center (degrees) and zoom (0..19).
void pm_map_set_view(double lat, double lon, int zoom);

// Read back current center / zoom.
double pm_map_center_lat();
double pm_map_center_lon();
int    pm_map_zoom();

// When true, every pm_map_draw_basemap() re-centers on the latest GPS
// fix before drawing (you-centered follow mode). Panning turns this
// off; pm_map_recenter_gps() turns it back on.
void pm_map_set_follow(bool follow);
bool pm_map_follow();

// Pan the viewport by a screen-space delta in pixels (e.g. from a
// drag or trackball). Disables follow mode.
void pm_map_handle_pan(int dxPixels, int dyPixels);

// Zoom in/out by one level, keeping the center fixed. Clamped 0..19.
void pm_map_zoom_in();
void pm_map_zoom_out();

// Snap the viewport back onto the current GPS fix and re-enable
// follow mode. No-op (keeps last center) if there's no valid fix.
void pm_map_recenter_gps();

// True if the GPS global currently holds a valid fix.
bool pm_map_have_fix();

// ── Projection: world <-> screen ─────────────────────────────
//
// world_to_screen returns true and fills *sx/*sy if (lat,lon) is on
// (or near) the visible viewport; returns false if it's far enough
// off-screen that the app can skip drawing it. The "near" margin lets
// markers that are just past the edge still draw their leading edge.
bool pm_map_world_to_screen(double lat, double lon, int* sx, int* sy);

// screen_to_world converts a pixel on the display back to lat/lon
// (used for tap-to-inspect, lasso, "what's here").
void pm_map_screen_to_world(int sx, int sy, double* lat, double* lon);

// Great-circle distance in meters between two lat/lon points
// (haversine). Handy for overlays computing range rings / labels.
double pm_map_haversine_m(double lat1, double lon1, double lat2, double lon2);

// Initial compass bearing in degrees (0=N,90=E) from p1 to p2.
double pm_map_bearing_deg(double lat1, double lon1, double lat2, double lon2);

// ── Rendering ────────────────────────────────────────────────

// Paint the basemap (tiles if available, else graticule) across the
// full content rectangle set by pm_map_set_content_rect(). If follow
// mode is on, re-centers on GPS first.
void pm_map_draw_basemap();

// Restrict drawing to a content rectangle (so an app can keep a
// header/footer band). Defaults to the full screen. Coordinates are
// device pixels. The basemap, HUD, and projection all respect this.
void pm_map_set_content_rect(int x, int y, int w, int h);
void pm_map_content_rect(int* x, int* y, int* w, int* h);

// Built-in chrome: center crosshair, own-position dot (drawn at the
// GPS fix if visible), scale bar, and a top-corner badge with zoom +
// lat/lon + fix state. Call after the app's overlay so it sits on top.
void pm_map_draw_hud(const char* title);

// ── Overlay primitives (lat/lon in, clipped to content rect) ──
//
// These are thin wrappers over gfx that take WORLD coordinates and
// handle the projection + clipping, so overlay apps never touch the
// transform directly.

// Filled marker dot with optional 1px outline.
void pm_map_marker(double lat, double lon, int radiusPx,
                   uint16_t fill, uint16_t outline);

// A small icon-triangle pointing along `headingDeg` (for aircraft,
// vehicles, APRS stations with course). size is the triangle "radius".
void pm_map_arrow(double lat, double lon, double headingDeg,
                  int sizePx, uint16_t color);

// Line segment between two world points (track legs, vectors).
void pm_map_line(double lat1, double lon1, double lat2, double lon2,
                 uint16_t color);

// Range ring: a circle of `radiusMeters` around a world point,
// approximated as a polyline. Good for RF coverage / "within range".
void pm_map_ring(double lat, double lon, double radiusMeters,
                 uint16_t color);

// A short text label anchored at a world point with a pixel offset.
void pm_map_label(double lat, double lon, int dxPx, int dyPx,
                  const char* text, uint16_t color);

// Append a point to the internal breadcrumb trail and (optionally)
// draw the whole trail. The trail is a ring buffer owned by the
// engine so multiple apps share one "where I've been" line. Capacity
// is device-tuned (smaller on no-PSRAM Cardputer).
void pm_map_trail_push(double lat, double lon);
void pm_map_trail_draw(uint16_t color);
void pm_map_trail_clear();

#endif // PM_MAP_ENGINE_H
