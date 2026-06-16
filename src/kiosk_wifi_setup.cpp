// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ────────────────────────────────────────────────────────────────
//  kiosk_wifi_setup.cpp — touch-only WiFi setup with on-screen keyboard
//
//  Started as c5_wifi_setup.cpp (NM-CYD-C5 specific) and was promoted
//  in v1.2.1 to a fleet-wide kiosk helper. All three touch-only kiosks
//  in the Pisces Moon fleet now share this file:
//
//    DEVICE_C5     240×320, XPT2046 resistive  (K_SCALE = 1)
//    DEVICE_C28P   240×320, FT6336G capacitive (K_SCALE = 1)
//    DEVICE_MAXINE 480×800, GT911 capacitive   (K_SCALE = 2)
//
//  Differences are absorbed by two device-dispatch blocks at the top:
//
//    1. The touch reader (each board has its own driver, all returning
//       portrait pixel coords via a (int16_t*,int16_t*)→bool signature).
//
//    2. A K_SCALE multiplier applied to every geometry constant and
//       every gfx->setTextSize() call. C5/C28P keep the original 240-
//       wide layout; Maxine doubles everything so the keyboard fills
//       its 480px width and stays comfortable to tap on the 5" panel.
//
//  Two screens, same on every device:
//
//    1. SCAN LIST
//         • Top banner shows the currently associated SSID (if any).
//         • Dual-band scan on the C5 (its signature trick — both 2.4
//           and 5 GHz APs in one list); single-band 2.4 GHz scan on
//           the C28P and Maxine (their radios only see 2.4).
//         • Asterisk (*) prefix marks SSIDs the /wifi.cfg keyring
//           already knows the password for.
//         • Tap a row → password entry with the saved password pre-
//           filled if there is one, so reconnecting an old network
//           is one tap of DONE.
//         • SCAN AGAIN refreshes; DISCONNECT/FORGET appear when
//           associated to the currently active SSID.
//
//    2. PASSWORD ENTRY (on-screen keyboard)
//         • Five-row keyboard, ABC + SYM pages, SHIFT for one-shot
//           uppercase. Numbers row stays put across both pages.
//         • SHOW/HIDE toggle on the password field — defaults to
//           masked dots so a shoulder-surfer can't read it.
//         • DONE attempts WiFi.begin() with up to 12 s timeout. On
//           success, save_wifi_config() persists the credential via
//           the device-agnostic wifi_manager API and the flow
//           returns to the scan list with the new network active.
//
//  Captive-portal networks (Starbucks-style HTTP redirects) aren't
//  handled here — touch kiosks have no browser to fill the portal
//  form. The QR-bridge feature for that lives in wifi_share_qr.cpp
//  on keyboard-equipped devices; touch kiosks rely on the WiFiManager
//  AP-portal flow (separate menu item on C28P/Maxine).
//
//  All persistence goes through wifi_manager.cpp's pm_storage-backed
//  API. No SD bus details in this file.
// ────────────────────────────────────────────────────────────────

#if defined(DEVICE_C5) || defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <ctype.h>
#include "wifi_manager.h"

#if defined(DEVICE_C5)
  #include <esp_wifi.h>
  #include "hal_c5.h"
  extern bool c5_touch_read(int16_t* x, int16_t* y);
  static inline bool kiosk_touch_read(int16_t* x, int16_t* y) {
      return c5_touch_read(x, y);
  }
  #define KIOSK_W   SCREEN_W            // 240
  #define KIOSK_H   SCREEN_H            // 320
  #define K_SCALE   1
  #define KIOSK_BANNER "2.4G + 5G"
  #define KIOSK_DUAL_BAND 1
#elif defined(DEVICE_C28P)
  extern bool c28p_touch_read(int16_t* x, int16_t* y);
  static inline bool kiosk_touch_read(int16_t* x, int16_t* y) {
      return c28p_touch_read(x, y);
  }
  #define KIOSK_W   240
  #define KIOSK_H   320
  #define K_SCALE   1
  #define KIOSK_BANNER "2.4 GHz"
  #define KIOSK_DUAL_BAND 0
#elif defined(DEVICE_MAXINE)
  extern bool maxine_touch_read(int16_t* x, int16_t* y);
  static inline bool kiosk_touch_read(int16_t* x, int16_t* y) {
      return maxine_touch_read(x, y);
  }
  #define KIOSK_W   480
  #define KIOSK_H   800
  #define K_SCALE   2
  #define KIOSK_BANNER "2.4 GHz"
  #define KIOSK_DUAL_BAND 0
#endif

extern Arduino_GFX *gfx;

namespace {

// ──── Color palette ────
constexpr uint16_t COL_BG         = 0x0000;
constexpr uint16_t COL_HEADER_BG  = 0x031F;   // blue
constexpr uint16_t COL_TEXT       = 0xFFFF;
constexpr uint16_t COL_DIM        = 0x8410;
constexpr uint16_t COL_ACCENT     = 0x07FF;   // cyan
constexpr uint16_t COL_OK         = 0x07E0;   // green
constexpr uint16_t COL_WARN       = 0xFFE0;   // yellow
constexpr uint16_t COL_AMBER      = 0xFD20;
constexpr uint16_t COL_ERR        = 0xF800;
constexpr uint16_t COL_PANEL      = 0x18C3;
constexpr uint16_t COL_PANEL_HI   = 0x2104;
constexpr uint16_t COL_STRIPE     = 0x0841;
constexpr uint16_t COL_KEY_BG     = 0x2104;
constexpr uint16_t COL_KEY_BORDER = 0x4208;
constexpr uint16_t COL_KEY_ACT_BG = 0x0260;

// ──── Geometry — multiplied by K_SCALE for Maxine ────
constexpr int CHAR_W       = 6  * K_SCALE;   // glyph width at base font size
constexpr int HEADER_H     = 28 * K_SCALE;
constexpr int MAX_SCAN_ROWS = 9;             // unchanged — row count is logical
constexpr int LIST_Y       = 60 * K_SCALE;
constexpr int LIST_ROW_H   = 22 * K_SCALE;
constexpr int FOOTER_H     = 32 * K_SCALE;
constexpr int FOOTER_Y     = LIST_Y + MAX_SCAN_ROWS * LIST_ROW_H + 4 * K_SCALE;
constexpr int DISC_BTN_H   = 24 * K_SCALE;
constexpr int DISC_BTN_Y   = FOOTER_Y + FOOTER_H + 4 * K_SCALE;
constexpr int PWD_Y        = 44 * K_SCALE;
constexpr int PWD_H        = 28 * K_SCALE;
constexpr int KBD_Y        = 100 * K_SCALE;
constexpr int KBD_ROW_H    = 28 * K_SCALE;
constexpr int KBD_ROW_GAP  = 2  * K_SCALE;
constexpr int KBD_ROWS     = 5;

constexpr int KEY_W_NUM    = 24 * K_SCALE;       // 10 number keys
constexpr int KEY_W_ROW2   = 24 * K_SCALE;       // 10 letter keys
constexpr int KEY_W_ROW3   = 24 * K_SCALE;       // 9 letter keys, indented
constexpr int ROW3_INDENT  = 12 * K_SCALE;
constexpr int KEY_W_SHIFT  = 40 * K_SCALE;
constexpr int KEY_W_ROW4   = 22 * K_SCALE;       // 7 letter keys
constexpr int KEY_W_BKSP   = 46 * K_SCALE;
constexpr int KEY_W_SYM    = 40 * K_SCALE;
constexpr int KEY_W_DOT    = 24 * K_SCALE;
constexpr int KEY_W_SPACE  = 104 * K_SCALE;
constexpr int KEY_W_AT     = 24 * K_SCALE;
constexpr int KEY_W_DONE   = 48 * K_SCALE;

// ──── Scan result table ────
struct ScanRow {
    String           ssid;
    int              rssi;
    int              ch;
    wifi_auth_mode_t auth;
    bool             saved;
};
ScanRow scan_rows[MAX_SCAN_ROWS];
int     scan_row_count = 0;
bool    scan_truncated = false;

// ──── Keyboard state ────
bool kbd_shift = false;
bool kbd_sym   = false;

// ──── Generic touch helpers ────
void drain_touch() {
    int16_t tx, ty;
    while (kiosk_touch_read(&tx, &ty)) { delay(15); yield(); }
}

void wait_press() {
    drain_touch();
    int16_t tx, ty;
    while (!kiosk_touch_read(&tx, &ty)) { delay(15); yield(); }
}

// ──── Chrome ────
void draw_header(const char *title, const char *right_label) {
    gfx->fillRect(0, 0, KIOSK_W, HEADER_H, COL_HEADER_BG);
    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(8 * K_SCALE, 6 * K_SCALE);
    gfx->print(title);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8 * K_SCALE, 16 * K_SCALE);
    gfx->print(KIOSK_BANNER);
    if (right_label) {
        int rw = (int)strlen(right_label) * CHAR_W;
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(KIOSK_W - rw - 8 * K_SCALE, 10 * K_SCALE);
        gfx->print(right_label);
    }
}

void draw_status_line(int y, const char *msg, uint16_t color) {
    gfx->fillRect(0, y, KIOSK_W, 12 * K_SCALE, COL_BG);
    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(color);
    gfx->setCursor(8 * K_SCALE, y);
    gfx->print(msg);
}

void draw_button(int x, int y, int w, int h,
                 const char *label,
                 uint16_t bg, uint16_t fg, bool pressed) {
    uint16_t draw_bg = pressed ? fg : bg;
    uint16_t draw_fg = pressed ? bg : fg;
    gfx->fillRect(x, y, w, h, draw_bg);
    gfx->drawRect(x, y, w, h, fg);
    int label_w = (int)strlen(label) * CHAR_W;
    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(draw_fg);
    gfx->setCursor(x + (w - label_w) / 2, y + (h - 8 * K_SCALE) / 2);
    gfx->print(label);
}

// ──── Scan list helpers ────
const char *auth_short(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA*";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA*";
        default:                        return "?";
    }
}

uint16_t rssi_color(int rssi) {
    if (rssi > -55) return COL_OK;
    if (rssi > -70) return COL_WARN;
    if (rssi > -82) return COL_AMBER;
    return COL_ERR;
}

// Populate scan_rows[] sorted by RSSI desc. Skips hidden SSIDs (empty
// name) — the user can't pick those from a list usefully. Returns the
// status string to display under the header.
const char *do_scan() {
    static char status_buf[48];

#if KIOSK_DUAL_BAND
    // C5 only: explicit dual-band so we pick up both 2.4 + 5 GHz APs.
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    WiFi.mode(WIFI_STA);
    delay(50);

    int n = WiFi.scanNetworks(false /*sync*/, false /*show_hidden*/);
    if (n < 0) {
        scan_row_count = 0;
        scan_truncated = false;
        snprintf(status_buf, sizeof(status_buf), "Scan failed (%d)", n);
        return status_buf;
    }

    // Insertion sort by RSSI desc into an index array.
    int idx[64];
    int count = (n > 64) ? 64 : n;
    for (int i = 0; i < count; i++) idx[i] = i;
    for (int i = 1; i < count; i++) {
        int key = idx[i];
        int key_rssi = WiFi.RSSI(key);
        int j = i - 1;
        while (j >= 0 && WiFi.RSSI(idx[j]) < key_rssi) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }

    scan_row_count = 0;
    scan_truncated = false;
    for (int i = 0; i < count && scan_row_count < MAX_SCAN_ROWS; i++) {
        String ssid = WiFi.SSID(idx[i]);
        if (ssid.length() == 0) continue;        // skip hidden
        ScanRow &r = scan_rows[scan_row_count++];
        r.ssid = ssid;
        r.rssi = WiFi.RSSI(idx[i]);
        r.ch   = WiFi.channel(idx[i]);
        r.auth = WiFi.encryptionType(idx[i]);
        r.saved = (get_known_password(ssid).length() > 0);
    }
    if (count > scan_row_count) scan_truncated = true;

    snprintf(status_buf, sizeof(status_buf), "Found %d AP%s%s",
             n, (n == 1) ? "" : "s",
             scan_truncated ? " (top 9 shown)" : "");
    WiFi.scanDelete();
    return status_buf;
}

int hit_scan_row(int16_t tx, int16_t ty) {
    (void)tx;
    if (ty < LIST_Y || ty >= LIST_Y + scan_row_count * LIST_ROW_H) return -1;
    return (ty - LIST_Y) / LIST_ROW_H;
}

void render_scan_list() {
    gfx->fillRect(0, LIST_Y, KIOSK_W, FOOTER_Y - LIST_Y, COL_BG);
    if (scan_row_count == 0) {
        gfx->setTextSize(K_SCALE);
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(8 * K_SCALE, LIST_Y + 8 * K_SCALE);
        gfx->print("No networks visible.");
        return;
    }
    for (int i = 0; i < scan_row_count; i++) {
        const ScanRow &r = scan_rows[i];
        int y = LIST_Y + i * LIST_ROW_H;
        if (i & 1) gfx->fillRect(0, y, KIOSK_W, LIST_ROW_H, COL_STRIPE);

        gfx->setTextSize(K_SCALE);
        if (r.saved) {
            gfx->setTextColor(COL_OK);
            gfx->setCursor(4 * K_SCALE, y + 4 * K_SCALE);
            gfx->print("*");
        }
        String ssid = r.ssid;
        if (ssid.length() > 17) ssid = ssid.substring(0, 16) + "~";
        gfx->setTextColor(COL_TEXT);
        gfx->setCursor(14 * K_SCALE, y + 4 * K_SCALE);
        gfx->print(ssid);

        char rline[32];
        snprintf(rline, sizeof(rline), "%4ddBm  ch%-3d  %s",
                 r.rssi, r.ch, auth_short(r.auth));
        gfx->setTextColor(rssi_color(r.rssi));
        gfx->setCursor(14 * K_SCALE, y + 13 * K_SCALE);
        gfx->print(rline);
    }
}

// ──── Keyboard ────
const char *ROW1     = "1234567890";
const char *ROW2_ABC = "qwertyuiop";
const char *ROW3_ABC = "asdfghjkl";       // 9 chars
const char *ROW4_ABC = "zxcvbnm";         // 7 chars
const char *ROW2_SYM = "!@#$%^&*()";
const char *ROW3_SYM = "-_=+;:'\"?";      // 9 chars
const char *ROW4_SYM = "\\|/{}[]";        // 7 chars

enum KeyKind : uint8_t { KK_NONE, KK_CHAR, KK_SHIFT, KK_BKSP, KK_SYM, KK_SPACE, KK_DONE };

struct KbdHit {
    KeyKind kind;
    char    c;
};

int row_y(int row) {
    return KBD_Y + row * (KBD_ROW_H + KBD_ROW_GAP);
}

void draw_key(int x, int y, int w, const char *label,
              bool active = false, bool dim = false) {
    uint16_t bg = active ? COL_KEY_ACT_BG : COL_KEY_BG;
    uint16_t border = dim ? COL_DIM
                          : (active ? COL_OK : COL_KEY_BORDER);
    uint16_t fg = dim ? COL_DIM : COL_TEXT;
    gfx->fillRect(x, y, w - 1, KBD_ROW_H, bg);
    gfx->drawRect(x, y, w - 1, KBD_ROW_H, border);
    int lw = (int)strlen(label) * CHAR_W;
    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(fg);
    gfx->setCursor(x + (w - lw) / 2, y + (KBD_ROW_H - 8 * K_SCALE) / 2);
    gfx->print(label);
}

void draw_keyboard() {
    gfx->fillRect(0, KBD_Y, KIOSK_W,
                  (KBD_ROW_H + KBD_ROW_GAP) * KBD_ROWS, COL_BG);

    // Row 1 — numbers
    {
        int y = row_y(0), x = 0;
        for (int i = 0; i < 10; i++) {
            char buf[2] = { ROW1[i], 0 };
            draw_key(x, y, KEY_W_NUM, buf);
            x += KEY_W_NUM;
        }
    }
    // Row 2 — top letters / first symbols
    {
        int y = row_y(1), x = 0;
        const char *src = kbd_sym ? ROW2_SYM : ROW2_ABC;
        for (int i = 0; i < 10; i++) {
            char ch = src[i];
            if (!kbd_sym && kbd_shift) ch = toupper((unsigned char)ch);
            char buf[2] = { ch, 0 };
            draw_key(x, y, KEY_W_ROW2, buf);
            x += KEY_W_ROW2;
        }
    }
    // Row 3 — middle letters / second symbols (9 keys, indented)
    {
        int y = row_y(2), x = ROW3_INDENT;
        const char *src = kbd_sym ? ROW3_SYM : ROW3_ABC;
        for (int i = 0; i < 9; i++) {
            char ch = src[i];
            if (!kbd_sym && kbd_shift) ch = toupper((unsigned char)ch);
            char buf[2] = { ch, 0 };
            draw_key(x, y, KEY_W_ROW3, buf);
            x += KEY_W_ROW3;
        }
    }
    // Row 4 — SHIFT + 7 mid keys + BKSP
    {
        int y = row_y(3), x = 0;
        draw_key(x, y, KEY_W_SHIFT,
                 kbd_shift ? "SHFT" : "shft",
                 kbd_shift, kbd_sym);
        x += KEY_W_SHIFT;
        const char *src = kbd_sym ? ROW4_SYM : ROW4_ABC;
        for (int i = 0; i < 7; i++) {
            char ch = src[i];
            if (!kbd_sym && kbd_shift) ch = toupper((unsigned char)ch);
            char buf[2] = { ch, 0 };
            draw_key(x, y, KEY_W_ROW4, buf);
            x += KEY_W_ROW4;
        }
        draw_key(x, y, KEY_W_BKSP, "BKSP");
    }
    // Row 5 — SYM/ABC + . + SPACE + @ + DONE
    {
        int y = row_y(4), x = 0;
        draw_key(x, y, KEY_W_SYM, kbd_sym ? "ABC" : "SYM", kbd_sym);
        x += KEY_W_SYM;
        draw_key(x, y, KEY_W_DOT, ".");
        x += KEY_W_DOT;
        draw_key(x, y, KEY_W_SPACE, "space");
        x += KEY_W_SPACE;
        draw_key(x, y, KEY_W_AT, "@");
        x += KEY_W_AT;
        draw_key(x, y, KEY_W_DONE, "DONE", true /*highlight*/);
    }
}

KbdHit kbd_hit(int16_t tx, int16_t ty) {
    KbdHit r = { KK_NONE, 0 };
    auto in_row = [&](int row) {
        int y = row_y(row);
        return (ty >= y && ty < y + KBD_ROW_H);
    };

    if (in_row(0)) {
        int col = tx / KEY_W_NUM;
        if (col >= 0 && col < 10) {
            r.kind = KK_CHAR; r.c = ROW1[col];
        }
        return r;
    }
    if (in_row(1)) {
        int col = tx / KEY_W_ROW2;
        if (col >= 0 && col < 10) {
            char ch = (kbd_sym ? ROW2_SYM : ROW2_ABC)[col];
            if (!kbd_sym && kbd_shift) ch = toupper((unsigned char)ch);
            r.kind = KK_CHAR; r.c = ch;
        }
        return r;
    }
    if (in_row(2)) {
        if (tx >= ROW3_INDENT && tx < ROW3_INDENT + 9 * KEY_W_ROW3) {
            int col = (tx - ROW3_INDENT) / KEY_W_ROW3;
            char ch = (kbd_sym ? ROW3_SYM : ROW3_ABC)[col];
            if (!kbd_sym && kbd_shift) ch = toupper((unsigned char)ch);
            r.kind = KK_CHAR; r.c = ch;
        }
        return r;
    }
    if (in_row(3)) {
        if (tx < KEY_W_SHIFT) {
            if (!kbd_sym) r.kind = KK_SHIFT;
            return r;
        }
        if (tx < KEY_W_SHIFT + 7 * KEY_W_ROW4) {
            int col = (tx - KEY_W_SHIFT) / KEY_W_ROW4;
            char ch = (kbd_sym ? ROW4_SYM : ROW4_ABC)[col];
            if (!kbd_sym && kbd_shift) ch = toupper((unsigned char)ch);
            r.kind = KK_CHAR; r.c = ch;
            return r;
        }
        r.kind = KK_BKSP;
        return r;
    }
    if (in_row(4)) {
        int x = 0;
        if (tx < x + KEY_W_SYM)   { r.kind = KK_SYM;   return r; }
        x += KEY_W_SYM;
        if (tx < x + KEY_W_DOT)   { r.kind = KK_CHAR; r.c = '.'; return r; }
        x += KEY_W_DOT;
        if (tx < x + KEY_W_SPACE) { r.kind = KK_SPACE; return r; }
        x += KEY_W_SPACE;
        if (tx < x + KEY_W_AT)    { r.kind = KK_CHAR; r.c = '@'; return r; }
        r.kind = KK_DONE;
        return r;
    }
    return r;
}

void render_password_field(const char *buf, int plen, bool show) {
    gfx->fillRect(8 * K_SCALE, PWD_Y,
                  KIOSK_W - 16 * K_SCALE, PWD_H, COL_PANEL);
    gfx->drawRect(8 * K_SCALE, PWD_Y,
                  KIOSK_W - 16 * K_SCALE, PWD_H, COL_ACCENT);

    // Visible window — scrolled to tail on long passwords
    constexpr int MAX_VIS = 28;
    char visible[MAX_VIS + 1];
    int vis_n = (plen > MAX_VIS) ? MAX_VIS : plen;
    int offset = plen - vis_n;
    for (int i = 0; i < vis_n; i++) {
        visible[i] = show ? buf[offset + i] : '*';
    }
    visible[vis_n] = 0;

    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(14 * K_SCALE, PWD_Y + 6 * K_SCALE);
    gfx->print(visible);

    int caret_x = 14 * K_SCALE + vis_n * CHAR_W;
    if (caret_x < KIOSK_W - 64 * K_SCALE) {
        gfx->drawFastVLine(caret_x, PWD_Y + 5 * K_SCALE, 10 * K_SCALE, COL_ACCENT);
    }

    // SHOW/HIDE toggle on the right side of the field
    const int sw_x = KIOSK_W - 56 * K_SCALE;
    const int sw_w = 48 * K_SCALE;
    const int sw_y = PWD_Y + 4 * K_SCALE;
    const int sw_h = 20 * K_SCALE;
    gfx->fillRect(sw_x, sw_y, sw_w, sw_h, show ? COL_OK : COL_PANEL_HI);
    gfx->drawRect(sw_x, sw_y, sw_w, sw_h, COL_ACCENT);
    gfx->setTextColor(show ? COL_BG : COL_ACCENT);
    gfx->setCursor(sw_x + 10 * K_SCALE, sw_y + 6 * K_SCALE);
    gfx->print(show ? "HIDE" : "SHOW");
}

// Returns true if user pressed DONE. out receives the typed password.
// On cancel (header tap), returns false.
bool enter_password_for(const String &ssid,
                        const String &prefill,
                        String &out) {
    char buf[64];
    int  len = 0;
    if (prefill.length() > 0 && (int)prefill.length() < 64) {
        strncpy(buf, prefill.c_str(), 63);
        buf[63] = 0;
        len = (int)strlen(buf);
    } else {
        buf[0] = 0;
    }
    bool show = false;
    kbd_shift = false;
    kbd_sym   = false;

    auto repaint_all = [&]() {
        gfx->fillScreen(COL_BG);
        draw_header("WIFI SETUP", "<CANCEL");
        gfx->setTextSize(K_SCALE);
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(8 * K_SCALE, HEADER_H + 4 * K_SCALE);
        gfx->print("Connect to:");
        gfx->setTextSize(2 * K_SCALE);
        gfx->setTextColor(COL_ACCENT);
        String label = ssid;
        if (label.length() > 17) label = label.substring(0, 16) + "~";
        gfx->setCursor(8 * K_SCALE, HEADER_H + 14 * K_SCALE);
        gfx->print(label);

        render_password_field(buf, len, show);
        draw_keyboard();

        int hint_y = row_y(KBD_ROWS - 1) + KBD_ROW_H + 6 * K_SCALE;
        gfx->setTextSize(K_SCALE);
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(8 * K_SCALE, hint_y);
        gfx->print("DONE = connect + save");
    };
    repaint_all();

    drain_touch();
    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = kiosk_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            // CANCEL via header tap
            if (ty < HEADER_H) {
                drain_touch();
                out = "";
                return false;
            }
            // SHOW/HIDE toggle in password field
            const int sw_x = KIOSK_W - 56 * K_SCALE;
            const int sw_w = 48 * K_SCALE;
            const int sw_y = PWD_Y + 4 * K_SCALE;
            const int sw_h = 20 * K_SCALE;
            if (ty >= sw_y && ty < sw_y + sw_h &&
                tx >= sw_x && tx < sw_x + sw_w) {
                show = !show;
                render_password_field(buf, len, show);
                was_touched = touched;
                delay(15);
                continue;
            }
            // Keyboard
            if (ty >= KBD_Y &&
                ty < row_y(KBD_ROWS - 1) + KBD_ROW_H) {
                KbdHit h = kbd_hit(tx, ty);
                switch (h.kind) {
                    case KK_CHAR:
                        if (h.c && len < 63) {
                            buf[len++] = h.c;
                            buf[len] = 0;
                            if (!kbd_sym && kbd_shift) {
                                kbd_shift = false;
                                draw_keyboard();
                            }
                            render_password_field(buf, len, show);
                        }
                        break;
                    case KK_SHIFT:
                        kbd_shift = !kbd_shift;
                        draw_keyboard();
                        break;
                    case KK_SYM:
                        kbd_sym = !kbd_sym;
                        if (kbd_sym) kbd_shift = false;
                        draw_keyboard();
                        break;
                    case KK_BKSP:
                        if (len > 0) {
                            buf[--len] = 0;
                            render_password_field(buf, len, show);
                        }
                        break;
                    case KK_SPACE:
                        if (len < 63) {
                            buf[len++] = ' ';
                            buf[len] = 0;
                            render_password_field(buf, len, show);
                        }
                        break;
                    case KK_DONE:
                        drain_touch();
                        out = String(buf);
                        return true;
                    default:
                        break;
                }
            }
        }
        was_touched = touched;
        delay(15);
        yield();
    }
}

bool try_connect_with_ui(const String &ssid, const String &password) {
    gfx->fillScreen(COL_BG);
    draw_header("WIFI SETUP", nullptr);

    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8 * K_SCALE, HEADER_H + 8 * K_SCALE);
    gfx->print("Connecting to:");
    gfx->setTextSize(2 * K_SCALE);
    gfx->setTextColor(COL_ACCENT);
    String label = ssid;
    if (label.length() > 17) label = label.substring(0, 16) + "~";
    gfx->setCursor(8 * K_SCALE, HEADER_H + 22 * K_SCALE);
    gfx->print(label);

    int spin_y = HEADER_H + 70 * K_SCALE;
    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(COL_WARN);
    gfx->setCursor(8 * K_SCALE, spin_y);
    gfx->print("Working...");

#if KIOSK_DUAL_BAND
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    WiFi.disconnect(true, true);
    delay(150);
    WiFi.mode(WIFI_STA);
    delay(150);
    WiFi.begin(ssid.c_str(), password.c_str());

    const int max_iters = 60;       // 60 × 200 ms = 12 s
    for (int i = 0; i < max_iters; i++) {
        wl_status_t st = WiFi.status();

        gfx->fillRect(0, spin_y + 16 * K_SCALE,
                      KIOSK_W, 12 * K_SCALE, COL_BG);
        gfx->setTextSize(K_SCALE);
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(8 * K_SCALE, spin_y + 16 * K_SCALE);
        int ndots = (i % 4);
        char dots[5] = {0};
        for (int d = 0; d < ndots; d++) dots[d] = '.';
        gfx->printf("status %d%s", (int)st, dots);

        if (st == WL_CONNECTED) {
            save_wifi_config(ssid.c_str(), password.c_str());

            gfx->fillScreen(COL_BG);
            draw_header("WIFI SETUP", nullptr);
            gfx->setTextSize(2 * K_SCALE);
            gfx->setTextColor(COL_OK);
            gfx->setCursor(8 * K_SCALE, HEADER_H + 16 * K_SCALE);
            gfx->print("Connected!");
            gfx->setTextSize(K_SCALE);
            gfx->setTextColor(COL_TEXT);
            gfx->setCursor(8 * K_SCALE, HEADER_H + 44 * K_SCALE);
            gfx->print(ssid);
            gfx->setCursor(8 * K_SCALE, HEADER_H + 60 * K_SCALE);
            gfx->printf("IP: %s", WiFi.localIP().toString().c_str());
            gfx->setTextColor(COL_DIM);
            gfx->setCursor(8 * K_SCALE, HEADER_H + 84 * K_SCALE);
            gfx->print("Credentials saved.");
            gfx->setTextColor(COL_OK);
            gfx->setCursor(8 * K_SCALE, HEADER_H + 110 * K_SCALE);
            gfx->print("Tap to continue.");
            wait_press();
            drain_touch();
            return true;
        }
        if (st == WL_NO_SSID_AVAIL) break;
        delay(200);
        yield();
    }

    WiFi.disconnect(false, false);
    gfx->fillScreen(COL_BG);
    draw_header("WIFI SETUP", nullptr);
    gfx->setTextSize(2 * K_SCALE);
    gfx->setTextColor(COL_ERR);
    gfx->setCursor(8 * K_SCALE, HEADER_H + 16 * K_SCALE);
    gfx->print("Failed.");
    gfx->setTextSize(K_SCALE);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(8 * K_SCALE, HEADER_H + 44 * K_SCALE);
    gfx->print(ssid);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8 * K_SCALE, HEADER_H + 60 * K_SCALE);
    gfx->print("Wrong password or out of range.");
    gfx->setCursor(8 * K_SCALE, HEADER_H + 74 * K_SCALE);
    gfx->printf("WiFi.status() = %d", (int)WiFi.status());
    gfx->setTextColor(COL_WARN);
    gfx->setCursor(8 * K_SCALE, HEADER_H + 100 * K_SCALE);
    gfx->print("Tap to retry, or CANCEL");
    gfx->setCursor(8 * K_SCALE, HEADER_H + 112 * K_SCALE);
    gfx->print("on the next screen.");
    wait_press();
    drain_touch();
    return false;
}

}  // anonymous namespace

// ────────────────────────────────────────────────────────────────
//  Public entry — kiosk_run_wifi_setup()
//
//  Called from each kiosk's SYSTEM/WIFI menu. The C5 wires it as
//  the only WIFI option; the C28P and Maxine offer it as MANUAL
//  alongside AUTO (known-network reconnect) and PORTAL (phone bridge).
// ────────────────────────────────────────────────────────────────
void kiosk_run_wifi_setup() {
    wifi_mode_t prev_mode = WiFi.getMode();

    bool keep_running = true;
    while (keep_running) {
        gfx->fillScreen(COL_BG);
        draw_header("WIFI SETUP", "<EXIT");

        bool   connected   = (WiFi.status() == WL_CONNECTED);
        String active_ssid = connected ? WiFi.SSID() : String();
        if (connected) {
            String label = active_ssid;
            if (label.length() > 22) label = label.substring(0, 21) + "~";
            char banner[40];
            snprintf(banner, sizeof(banner), "Active: %s", label.c_str());
            draw_status_line(HEADER_H + 4 * K_SCALE, banner, COL_OK);
        } else {
            draw_status_line(HEADER_H + 4 * K_SCALE, "Not connected.", COL_DIM);
        }

        draw_status_line(HEADER_H + 18 * K_SCALE, "Scanning...", COL_WARN);
        const char *scan_status = do_scan();
        draw_status_line(HEADER_H + 18 * K_SCALE, scan_status,
                         scan_row_count > 0 ? COL_ACCENT : COL_DIM);
        render_scan_list();

        draw_button(8 * K_SCALE, FOOTER_Y, KIOSK_W - 16 * K_SCALE, FOOTER_H,
                    "SCAN AGAIN", COL_PANEL, COL_ACCENT, false);
        if (connected) {
            int half_w = (KIOSK_W - 24 * K_SCALE) / 2;
            draw_button(8 * K_SCALE, DISC_BTN_Y, half_w, DISC_BTN_H,
                        "DISCONNECT", COL_PANEL, COL_WARN, false);
            draw_button(KIOSK_W - 8 * K_SCALE - half_w, DISC_BTN_Y,
                        half_w, DISC_BTN_H,
                        "FORGET", COL_PANEL, COL_ERR, false);
        } else {
            gfx->setTextSize(K_SCALE);
            gfx->setTextColor(COL_DIM);
            gfx->setCursor(8 * K_SCALE, DISC_BTN_Y + 4 * K_SCALE);
            gfx->print("* = saved password");
        }

        bool was_touched = false;
        bool need_repaint = false;
        while (!need_repaint && keep_running) {
            int16_t tx, ty;
            bool touched = kiosk_touch_read(&tx, &ty);

            if (touched && !was_touched) {
                if (ty < HEADER_H) {
                    keep_running = false;
                    break;
                }
                if (ty >= FOOTER_Y && ty < FOOTER_Y + FOOTER_H) {
                    drain_touch();
                    need_repaint = true;
                    break;
                }
                if (connected && ty >= DISC_BTN_Y && ty < DISC_BTN_Y + DISC_BTN_H) {
                    int half_w = (KIOSK_W - 24 * K_SCALE) / 2;
                    if (tx >= 8 * K_SCALE && tx < 8 * K_SCALE + half_w) {
                        WiFi.disconnect(false, false);
                        delay(200);
                    } else if (tx >= KIOSK_W - 8 * K_SCALE - half_w &&
                               tx < KIOSK_W - 8 * K_SCALE) {
                        if (active_ssid.length() > 0) {
                            save_wifi_config(active_ssid.c_str(), "");
                        }
                        WiFi.disconnect(true, true);
                        delay(200);
                    }
                    drain_touch();
                    need_repaint = true;
                    break;
                }
                int row = hit_scan_row(tx, ty);
                if (row >= 0 && row < scan_row_count) {
                    drain_touch();
                    ScanRow &r = scan_rows[row];

                    if (r.auth == WIFI_AUTH_OPEN) {
                        try_connect_with_ui(r.ssid, "");
                        need_repaint = true;
                        break;
                    }

                    String saved_pw = r.saved ? get_known_password(r.ssid)
                                              : String();
                    String typed;
                    if (!enter_password_for(r.ssid, saved_pw, typed)) {
                        need_repaint = true;
                        break;
                    }

                    String pw = typed;
                    while (true) {
                        if (try_connect_with_ui(r.ssid, pw)) break;
                        if (!enter_password_for(r.ssid, pw, pw)) break;
                    }
                    need_repaint = true;
                    break;
                }
            }
            was_touched = touched;
            delay(20);
            yield();
        }
    }

    WiFi.mode((prev_mode == WIFI_OFF) ? WIFI_STA : prev_mode);
}

// Backwards-compatible alias so c5_boot.cpp's existing call site stays
// valid without an edit. v1.3 can drop this once all callers are
// updated to use the kiosk_ name.
#if defined(DEVICE_C5)
void c5_run_wifi_setup() {
    kiosk_run_wifi_setup();
}
#endif

#endif  // DEVICE_C5 || DEVICE_C28P || DEVICE_MAXINE
