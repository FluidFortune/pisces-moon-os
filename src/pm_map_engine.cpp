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
 * PM MAP ENGINE — shared moving-map core (implementation)
 *
 * Renders on every device by reading gfx->width()/height() at runtime.
 * Web-Mercator projection so SD raster tiles (/maps/<set>/<z>/<x>/<y>.png,
 * standard OSM layout) and live lat/lon share one coordinate space.
 *
 * Basemap path uses PNGdec to decode 256x256 tiles streamed from the SD
 * card through pm_storage (the unified SD HAL). Decoded scanlines are
 * pushed straight to the panel via gfx, tile by tile, so we never hold a
 * full framebuffer — critical on the no-PSRAM Cardputer. If a tile file
 * is missing or PNGdec/SD is unavailable, that tile draws a graticule
 * cell instead, so the app is fully usable with no card and no imagery.
 *
 * The Pager's gfx is the PMDispTLoRaPager driver (Print-derived) but is
 * driven here through `extern Arduino_GFX *gfx`, exactly as
 * mesh_messenger.cpp / rf_spectrum.cpp do across these devices. We keep
 * to the gfx method set BOTH that driver and Arduino_GFX implement with
 * matching signatures: fillRect, drawRect, drawFastHLine, drawFastVLine,
 * drawLine, drawPixel, fillCircle, drawCircle, setCursor, setTextColor,
 * setTextSize, print, printf, fillScreen, width, height, and crucially
 * draw16bitBeRGBBitmap for the tile blit (NOT setAddrWindow/writePixels,
 * which are Arduino_GFX-only and absent on the Pager driver).
 */

#include "pm_map_engine.h"
#include <Arduino.h>
#include <math.h>
#include <TinyGPSPlus.h>
#include "pm_storage.h"

#if defined(DEVICE_TLORAPAGER)
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif

// PNGdec is a tiny, dependency-free PNG decoder already suited to MCUs.
// If the library isn't present in a given build, the basemap silently
// falls back to graticule-only (PM_MAP_HAVE_PNGDEC guards the calls).
#if __has_include(<PNGdec.h>)
  #include <PNGdec.h>
  #define PM_MAP_HAVE_PNGDEC 1
#else
  #define PM_MAP_HAVE_PNGDEC 0
#endif

extern Arduino_GFX *gfx;
extern TinyGPSPlus  gps;

// ─────────────────────────────────────────────
//  DEVICE-TUNED LIMITS
// ─────────────────────────────────────────────
#ifdef DEVICE_NO_PSRAM
  #define PM_MAP_TRAIL_CAP   256    // breadcrumb ring (Cardputer)
  #define PM_MAP_TILE_BUFW   PM_MAP_TILE_PX  // one scanline width
#else
  #define PM_MAP_TRAIL_CAP   2048
  #define PM_MAP_TILE_BUFW   PM_MAP_TILE_PX
#endif

// ─────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────
static bool   s_active   = false;
static char   s_tileSet[PM_MAP_SET_NAME_MAX + 1] = "";
static bool   s_haveTiles = false;       // tileSet non-empty AND SD ready

static double s_centerLat = 34.0901;     // default: Pasadena-ish
static double s_centerLon = -118.1300;
static int    s_zoom      = 14;
static bool   s_follow    = true;

// Content rectangle (where the map draws; apps reserve header/footer).
static int s_cx = 0, s_cy = 0, s_cw = 0, s_ch = 0;

// Breadcrumb ring buffer.
struct TrailPt { float lat; float lon; };
static TrailPt  s_trail[PM_MAP_TRAIL_CAP];
static int      s_trailHead  = 0;        // next write index
static int      s_trailCount = 0;

#if PM_MAP_HAVE_PNGDEC
static PNG s_png;                        // decoder instance (reused)
// Per-tile blit context handed to the PNGdec line callback.
struct TileBlitCtx {
    int destX;        // top-left of this tile on screen
    int destY;
    int clipX0, clipY0, clipX1, clipY1;  // content-rect clip (inclusive-exclusive)
};
static TileBlitCtx s_blit;
static uint16_t    s_lineBuf[PM_MAP_TILE_BUFW];
#endif

// ─────────────────────────────────────────────
//  SMALL HELPERS
// ─────────────────────────────────────────────
static inline int dispW() { return gfx ? gfx->width()  : 240; }
static inline int dispH() { return gfx ? gfx->height() : 135; }

static inline double clampLat(double lat) {
    if (lat >  85.05112878) return  85.05112878;
    if (lat < -85.05112878) return -85.05112878;
    return lat;
}
static inline double wrapLon(double lon) {
    while (lon >  180.0) lon -= 360.0;
    while (lon < -180.0) lon += 360.0;
    return lon;
}
static inline int clampZoom(int z) { return z < 0 ? 0 : (z > 19 ? 19 : z); }

// World pixel coordinates at the current zoom (Web-Mercator). The world
// at zoom z is (256 * 2^z) pixels square. We use doubles for sub-pixel
// viewport math.
static double worldSizePx() {
    return (double)PM_MAP_TILE_PX * (double)(1u << s_zoom);
}
static void lonLatToWorldPx(double lat, double lon, double* wx, double* wy) {
    double ws = worldSizePx();
    lat = clampLat(lat);
    double sinLat = sin(lat * M_PI / 180.0);
    *wx = (lon + 180.0) / 360.0 * ws;
    *wy = (0.5 - log((1.0 + sinLat) / (1.0 - sinLat)) / (4.0 * M_PI)) * ws;
}
static void worldPxToLonLat(double wx, double wy, double* lat, double* lon) {
    double ws = worldSizePx();
    double n = M_PI - 2.0 * M_PI * wy / ws;
    *lon = wx / ws * 360.0 - 180.0;
    *lat = 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

// Top-left world-pixel of the content rectangle, given the center.
static void viewOriginWorldPx(double* ox, double* oy) {
    double cwx, cwy;
    lonLatToWorldPx(s_centerLat, s_centerLon, &cwx, &cwy);
    *ox = cwx - (double)s_cw * 0.5;
    *oy = cwy - (double)s_ch * 0.5;
}

// ─────────────────────────────────────────────
//  LIFECYCLE
// ─────────────────────────────────────────────
bool pm_map_begin(const char* tileSet) {
    s_active = true;
    s_trailHead = s_trailCount = 0;

    // Default content rect = whole screen until app overrides.
    s_cx = 0; s_cy = 0; s_cw = dispW(); s_ch = dispH();

    s_tileSet[0] = '\0';
    if (tileSet && tileSet[0]) {
        strncpy(s_tileSet, tileSet, PM_MAP_SET_NAME_MAX);
        s_tileSet[PM_MAP_SET_NAME_MAX] = '\0';
    }
    s_haveTiles = (s_tileSet[0] != '\0') && pm_storage::ready();

    return true;
}

void pm_map_end() {
    s_active = false;
    s_haveTiles = false;
    s_trailHead = s_trailCount = 0;
}

// ─────────────────────────────────────────────
//  VIEWPORT
// ─────────────────────────────────────────────
void pm_map_set_view(double lat, double lon, int zoom) {
    s_centerLat = clampLat(lat);
    s_centerLon = wrapLon(lon);
    s_zoom = clampZoom(zoom);
}
double pm_map_center_lat() { return s_centerLat; }
double pm_map_center_lon() { return s_centerLon; }
int    pm_map_zoom()       { return s_zoom; }

void pm_map_set_follow(bool follow) { s_follow = follow; }
bool pm_map_follow()                { return s_follow; }

void pm_map_handle_pan(int dxPixels, int dyPixels) {
    // Dragging the map right should move the view left (content moves
    // with the finger), so subtract the delta from the world origin.
    double ox, oy; viewOriginWorldPx(&ox, &oy);
    double newCx = ox + (double)s_cw * 0.5 - (double)dxPixels;
    double newCy = oy + (double)s_ch * 0.5 - (double)dyPixels;
    worldPxToLonLat(newCx, newCy, &s_centerLat, &s_centerLon);
    s_centerLat = clampLat(s_centerLat);
    s_centerLon = wrapLon(s_centerLon);
    s_follow = false;
}

void pm_map_zoom_in()  { s_zoom = clampZoom(s_zoom + 1); }
void pm_map_zoom_out() { s_zoom = clampZoom(s_zoom - 1); }

bool pm_map_have_fix() { return gps.location.isValid(); }

void pm_map_recenter_gps() {
    if (gps.location.isValid()) {
        s_centerLat = clampLat(gps.location.lat());
        s_centerLon = wrapLon(gps.location.lng());
    }
    s_follow = true;
}

// ─────────────────────────────────────────────
//  PROJECTION
// ─────────────────────────────────────────────
bool pm_map_world_to_screen(double lat, double lon, int* sx, int* sy) {
    double ox, oy; viewOriginWorldPx(&ox, &oy);
    double wx, wy; lonLatToWorldPx(lat, lon, &wx, &wy);
    int px = s_cx + (int)lround(wx - ox);
    int py = s_cy + (int)lround(wy - oy);
    if (sx) *sx = px;
    if (sy) *sy = py;
    // "near" margin = one tile, so partially-off markers still draw.
    const int m = PM_MAP_TILE_PX;
    if (px < s_cx - m || px > s_cx + s_cw + m) return false;
    if (py < s_cy - m || py > s_cy + s_ch + m) return false;
    return true;
}

void pm_map_screen_to_world(int sx, int sy, double* lat, double* lon) {
    double ox, oy; viewOriginWorldPx(&ox, &oy);
    double wx = ox + (double)(sx - s_cx);
    double wy = oy + (double)(sy - s_cy);
    worldPxToLonLat(wx, wy, lat, lon);
    *lat = clampLat(*lat);
    *lon = wrapLon(*lon);
}

double pm_map_haversine_m(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat/2)*sin(dLat/2) +
               cos(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*sin(dLon/2)*sin(dLon/2);
    return 2.0 * R * atan2(sqrt(a), sqrt(1.0 - a));
}

double pm_map_bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double y = sin((lon2-lon1)*M_PI/180.0) * cos(lat2*M_PI/180.0);
    double x = cos(lat1*M_PI/180.0)*sin(lat2*M_PI/180.0) -
               sin(lat1*M_PI/180.0)*cos(lat2*M_PI/180.0)*cos((lon2-lon1)*M_PI/180.0);
    double b = atan2(y, x) * 180.0 / M_PI;
    return fmod(b + 360.0, 360.0);
}

// ─────────────────────────────────────────────
//  CONTENT RECT
// ─────────────────────────────────────────────
void pm_map_set_content_rect(int x, int y, int w, int h) {
    s_cx = x; s_cy = y; s_cw = w; s_ch = h;
}
void pm_map_content_rect(int* x, int* y, int* w, int* h) {
    if (x) *x = s_cx; if (y) *y = s_cy; if (w) *w = s_cw; if (h) *h = s_ch;
}

// Simple per-pixel clip test against the content rect.
static inline bool inContent(int x, int y) {
    return x >= s_cx && x < s_cx + s_cw && y >= s_cy && y < s_cy + s_ch;
}

// ─────────────────────────────────────────────
//  BASEMAP — graticule fallback cell
// ─────────────────────────────────────────────
static void drawGraticuleCell(int dx, int dy) {
    // Fill the tile footprint with dim background + a grid, clipped to
    // the content rect. Drawn line-by-line so we honor the clip without
    // a framebuffer.
    int x0 = dx, y0 = dy, x1 = dx + PM_MAP_TILE_PX, y1 = dy + PM_MAP_TILE_PX;
    if (x0 < s_cx) x0 = s_cx; if (y0 < s_cy) y0 = s_cy;
    if (x1 > s_cx + s_cw) x1 = s_cx + s_cw;
    if (y1 > s_cy + s_ch) y1 = s_cy + s_ch;
    if (x0 >= x1 || y0 >= y1) return;
    gfx->fillRect(x0, y0, x1 - x0, y1 - y0, PM_MAP_COL_HUD_BG);

    // 64px sub-grid inside the tile.
    for (int gx = dx; gx <= dx + PM_MAP_TILE_PX; gx += 64) {
        if (gx < s_cx || gx >= s_cx + s_cw) continue;
        int gy0 = (dy < s_cy) ? s_cy : dy;
        int gy1 = (dy + PM_MAP_TILE_PX > s_cy + s_ch) ? s_cy + s_ch : dy + PM_MAP_TILE_PX;
        if (gy1 > gy0) gfx->drawFastVLine(gx, gy0, gy1 - gy0,
                                          (gx % PM_MAP_TILE_PX == 0) ? PM_MAP_COL_GRID : PM_MAP_COL_GRID_DIM);
    }
    for (int gy = dy; gy <= dy + PM_MAP_TILE_PX; gy += 64) {
        if (gy < s_cy || gy >= s_cy + s_ch) continue;
        int gx0 = (dx < s_cx) ? s_cx : dx;
        int gx1 = (dx + PM_MAP_TILE_PX > s_cx + s_cw) ? s_cx + s_cw : dx + PM_MAP_TILE_PX;
        if (gx1 > gx0) gfx->drawFastHLine(gx0, gy, gx1 - gx0,
                                          (gy % PM_MAP_TILE_PX == 0) ? PM_MAP_COL_GRID : PM_MAP_COL_GRID_DIM);
    }
}

#if PM_MAP_HAVE_PNGDEC
// PNGdec line callback: convert one decoded scanline to RGB565 and blit
// the in-bounds span to the panel. Clipped to the content rect.
static int tilePngDraw(PNGDRAW *pDraw) {
    int sy = s_blit.destY + pDraw->y;
    if (sy < s_blit.clipY0 || sy >= s_blit.clipY1) return 1;  // row off-screen

    int w = pDraw->iWidth;
    if (w > PM_MAP_TILE_BUFW) w = PM_MAP_TILE_BUFW;
    s_png.getLineAsRGB565(pDraw, s_lineBuf, PNG_RGB565_BIG_ENDIAN, 0xFFFFFFFF);

    // Compute the visible x-span for this row.
    int x0 = s_blit.destX;
    int x1 = s_blit.destX + w;
    if (x0 < s_blit.clipX0) x0 = s_blit.clipX0;
    if (x1 > s_blit.clipX1) x1 = s_blit.clipX1;
    if (x0 >= x1) return 1;

    int srcOff = x0 - s_blit.destX;           // first visible source pixel
    // Blit this 1-row span as a big-endian RGB565 bitmap. This method is
    // implemented by BOTH Arduino_GFX and the Pager's PMDispTLoRaPager
    // driver (unlike setAddrWindow/writePixels, which are Arduino_GFX
    // only), so the same path works on every device.
    gfx->draw16bitBeRGBBitmap(x0, sy, s_lineBuf + srcOff, x1 - x0, 1);
    return 1;
}

// Load + decode one tile from SD into its on-screen footprint. Returns
// true if the tile was drawn, false if missing/failed (caller draws the
// graticule fallback). Reads the whole PNG into a heap buffer first
// because PNGdec wants a contiguous source; tiles are small (~5-40 KB).
static bool drawTileFromSD(int z, int tx, int ty, int dx, int dy) {
    char path[80];
    snprintf(path, sizeof(path), "/maps/%s/%d/%d/%d.png", s_tileSet, z, tx, ty);
    if (!pm_storage::exists(path)) return false;

    pm_storage::File f = pm_storage::open(path, pm_storage::Mode::Read);
    if (!f) return false;
    size_t sz = f.size();
    if (sz == 0 || sz > 200000) { f.close(); return false; }  // sanity cap

    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) { f.close(); return false; }
    size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) { free(buf); return false; }

    int rc = s_png.openRAM(buf, sz, tilePngDraw);
    if (rc != PNG_SUCCESS) { free(buf); return false; }

    s_blit.destX = dx;
    s_blit.destY = dy;
    s_blit.clipX0 = s_cx;            s_blit.clipY0 = s_cy;
    s_blit.clipX1 = s_cx + s_cw;     s_blit.clipY1 = s_cy + s_ch;
    rc = s_png.decode(nullptr, 0);
    s_png.close();
    free(buf);
    return rc == PNG_SUCCESS;
}
#endif // PM_MAP_HAVE_PNGDEC

void pm_map_draw_basemap() {
    if (!s_active) return;
    if (s_follow && gps.location.isValid()) {
        s_centerLat = clampLat(gps.location.lat());
        s_centerLon = wrapLon(gps.location.lng());
    }

    int n = 1 << s_zoom;                 // tiles per axis at this zoom
    double ox, oy; viewOriginWorldPx(&ox, &oy);

    // Range of tile indices covering the content rect.
    int firstTileX = (int)floor(ox / PM_MAP_TILE_PX);
    int firstTileY = (int)floor(oy / PM_MAP_TILE_PX);
    int lastTileX  = (int)floor((ox + s_cw) / PM_MAP_TILE_PX);
    int lastTileY  = (int)floor((oy + s_ch) / PM_MAP_TILE_PX);

    for (int ty = firstTileY; ty <= lastTileY; ty++) {
        for (int tx = firstTileX; tx <= lastTileX; tx++) {
            // Screen position of this tile's top-left corner.
            int dx = s_cx + (int)lround((double)tx * PM_MAP_TILE_PX - ox);
            int dy = s_cy + (int)lround((double)ty * PM_MAP_TILE_PX - oy);

            // Wrap X around the world; clamp Y (no vertical wrap).
            int wrappedX = ((tx % n) + n) % n;
            bool valid = (ty >= 0 && ty < n);

            bool drawn = false;
#if PM_MAP_HAVE_PNGDEC
            if (valid && s_haveTiles) {
                drawn = drawTileFromSD(s_zoom, wrappedX, ty, dx, dy);
            }
#endif
            if (!drawn) drawGraticuleCell(dx, dy);
        }
    }
}

// ─────────────────────────────────────────────
//  OVERLAY PRIMITIVES
// ─────────────────────────────────────────────
void pm_map_marker(double lat, double lon, int radiusPx,
                   uint16_t fill, uint16_t outline) {
    int sx, sy;
    if (!pm_map_world_to_screen(lat, lon, &sx, &sy)) return;
    if (!inContent(sx, sy)) return;
    gfx->fillCircle(sx, sy, radiusPx, fill);
    if (outline != fill) gfx->drawCircle(sx, sy, radiusPx, outline);
}

void pm_map_arrow(double lat, double lon, double headingDeg,
                  int sizePx, uint16_t color) {
    int sx, sy;
    if (!pm_map_world_to_screen(lat, lon, &sx, &sy)) return;
    if (!inContent(sx, sy)) return;
    double h = headingDeg * M_PI / 180.0;
    // Tip ahead along heading; two tail corners behind.
    double tipX = sx + sin(h) * sizePx;
    double tipY = sy - cos(h) * sizePx;
    double leftX = sx + sin(h + 2.5) * sizePx;
    double leftY = sy - cos(h + 2.5) * sizePx;
    double rightX = sx + sin(h - 2.5) * sizePx;
    double rightY = sy - cos(h - 2.5) * sizePx;
    gfx->drawLine((int)tipX, (int)tipY, (int)leftX, (int)leftY, color);
    gfx->drawLine((int)tipX, (int)tipY, (int)rightX, (int)rightY, color);
    gfx->drawLine((int)leftX, (int)leftY, (int)rightX, (int)rightY, color);
}

void pm_map_line(double lat1, double lon1, double lat2, double lon2,
                 uint16_t color) {
    int x1, y1, x2, y2;
    bool a = pm_map_world_to_screen(lat1, lon1, &x1, &y1);
    bool b = pm_map_world_to_screen(lat2, lon2, &x2, &y2);
    if (!a && !b) return;          // both far off-screen
    gfx->drawLine(x1, y1, x2, y2, color);
}

void pm_map_ring(double lat, double lon, double radiusMeters, uint16_t color) {
    // Approximate a circle of given ground radius as a 24-gon. Meters
    // per degree latitude ~111320; longitude scaled by cos(lat).
    const int SEG = 24;
    double dLat = radiusMeters / 111320.0;
    double dLon = radiusMeters / (111320.0 * cos(lat * M_PI / 180.0));
    double prevLat = lat + dLat, prevLon = lon;
    for (int i = 1; i <= SEG; i++) {
        double a = (double)i / SEG * 2.0 * M_PI;
        double pl = lat + dLat * cos(a);
        double po = lon + dLon * sin(a);
        pm_map_line(prevLat, prevLon, pl, po, color);
        prevLat = pl; prevLon = po;
    }
}

void pm_map_label(double lat, double lon, int dxPx, int dyPx,
                  const char* text, uint16_t color) {
    int sx, sy;
    if (!pm_map_world_to_screen(lat, lon, &sx, &sy)) return;
    int tx = sx + dxPx, ty = sy + dyPx;
    if (!inContent(tx, ty)) return;
    gfx->setTextSize(1);
    gfx->setTextColor(color);
    gfx->setCursor(tx, ty);
    gfx->print(text);
}

// ─────────────────────────────────────────────
//  BREADCRUMB TRAIL
// ─────────────────────────────────────────────
void pm_map_trail_push(double lat, double lon) {
    s_trail[s_trailHead].lat = (float)lat;
    s_trail[s_trailHead].lon = (float)lon;
    s_trailHead = (s_trailHead + 1) % PM_MAP_TRAIL_CAP;
    if (s_trailCount < PM_MAP_TRAIL_CAP) s_trailCount++;
}

void pm_map_trail_draw(uint16_t color) {
    if (s_trailCount < 2) return;
    // Oldest index.
    int idx = (s_trailHead - s_trailCount + PM_MAP_TRAIL_CAP) % PM_MAP_TRAIL_CAP;
    double prevLat = s_trail[idx].lat, prevLon = s_trail[idx].lon;
    for (int i = 1; i < s_trailCount; i++) {
        int j = (idx + i) % PM_MAP_TRAIL_CAP;
        double la = s_trail[j].lat, lo = s_trail[j].lon;
        pm_map_line(prevLat, prevLon, la, lo, color);
        prevLat = la; prevLon = lo;
    }
}

void pm_map_trail_clear() { s_trailHead = s_trailCount = 0; }

// ─────────────────────────────────────────────
//  HUD — crosshair, own dot, scale bar, badge
// ─────────────────────────────────────────────
static void formatScale(double metersPerPx, char* out, size_t n) {
    // Pick a "nice" bar length ~ up to 80px wide.
    double targetM = metersPerPx * 80.0;
    double pow10 = pow(10.0, floor(log10(targetM)));
    double mant = targetM / pow10;
    double nice = (mant >= 5) ? 5 : (mant >= 2 ? 2 : 1);
    double barM = nice * pow10;
    if (barM >= 1000.0) snprintf(out, n, "%.0f km", barM / 1000.0);
    else                snprintf(out, n, "%.0f m", barM);
}

void pm_map_draw_hud(const char* title) {
    if (!s_active) return;

    // ── center crosshair ──
    int ccx = s_cx + s_cw / 2;
    int ccy = s_cy + s_ch / 2;
    gfx->drawFastHLine(ccx - 6, ccy, 12, PM_MAP_COL_CROSS);
    gfx->drawFastVLine(ccx, ccy - 6, 12, PM_MAP_COL_CROSS);

    // ── own-position marker (if fix is visible) ──
    if (gps.location.isValid()) {
        int sx, sy;
        if (pm_map_world_to_screen(gps.location.lat(), gps.location.lng(), &sx, &sy)
            && inContent(sx, sy)) {
            gfx->fillCircle(sx, sy, 3, PM_MAP_COL_SELF);
            gfx->drawCircle(sx, sy, 6, PM_MAP_COL_SELF_RING);
            // heading tick if we have course
            if (gps.course.isValid()) {
                double h = gps.course.deg() * M_PI / 180.0;
                gfx->drawLine(sx, sy, sx + (int)(sin(h)*10), sy - (int)(cos(h)*10),
                              PM_MAP_COL_SELF);
            }
        }
    }

    // ── scale bar (bottom-left of content) ──
    double mPerPxEquator = 156543.03392 * cos(s_centerLat * M_PI / 180.0)
                           / (double)(1u << s_zoom);
    char scaleBuf[16];
    formatScale(mPerPxEquator, scaleBuf, sizeof(scaleBuf));
    int barPx = 0;
    {
        // recompute exact bar pixel length for the label value
        double targetM = mPerPxEquator * 80.0;
        double pow10 = pow(10.0, floor(log10(targetM)));
        double mant = targetM / pow10;
        double nice = (mant >= 5) ? 5 : (mant >= 2 ? 2 : 1);
        double barM = nice * pow10;
        barPx = (int)(barM / mPerPxEquator);
        if (barPx < 8)  barPx = 8;
        if (barPx > s_cw - 20) barPx = s_cw - 20;
    }
    int barX = s_cx + 6;
    int barY = s_cy + s_ch - 8;
    gfx->drawFastHLine(barX, barY, barPx, PM_MAP_COL_HUD_FG);
    gfx->drawFastVLine(barX, barY - 3, 4, PM_MAP_COL_HUD_FG);
    gfx->drawFastVLine(barX + barPx, barY - 3, 4, PM_MAP_COL_HUD_FG);
    gfx->setTextSize(1);
    gfx->setTextColor(PM_MAP_COL_HUD_FG);
    gfx->setCursor(barX + 2, barY - 11);
    gfx->print(scaleBuf);

    // ── badge: title + zoom + center + fix (top-left) ──
    char line[40];
    gfx->setTextColor(gps.location.isValid() ? PM_MAP_COL_HUD_FG : PM_MAP_COL_HUD_DIM);
    gfx->setCursor(s_cx + 4, s_cy + 3);
    if (title && title[0]) { gfx->print(title); }
    gfx->setCursor(s_cx + 4, s_cy + 13);
    snprintf(line, sizeof(line), "z%d %s%s", s_zoom,
             s_follow ? "FOLLOW" : "PAN",
             s_haveTiles ? "" : " /grid");
    gfx->print(line);

    // center lat/lon, small, top-right
    char ll[28];
    snprintf(ll, sizeof(ll), "%.4f,%.4f", s_centerLat, s_centerLon);
    int llw = (int)strlen(ll) * 6;
    gfx->setCursor(s_cx + s_cw - llw - 4, s_cy + 3);
    gfx->setTextColor(PM_MAP_COL_HUD_DIM);
    gfx->print(ll);
}
