// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ────────────────────────────────────────────────────────────────
//  wifi_share_qr.cpp — captive-portal phone bridge via QR code
//
//  Problem this solves:
//    The keyboard-equipped devices (T-Deck Plus, T-LoRa Pager,
//    Cardputer ADV) can JOIN an open WiFi network from their
//    physical keyboard just fine via wifi_connect.cpp — but for
//    networks behind a captive portal (Starbucks, hotels, airports
//    — the kind that pop up a browser login page) the device has
//    no browser to fill the form. The user is stuck on the open
//    SSID with all HTTP requests redirected to a portal they
//    can't reach.
//
//  The fix:
//    Show the currently-connected (or last-saved) WiFi credentials
//    as a STANDARD WiFi-config QR code. The user scans it on their
//    phone, the phone joins the same network independently, the
//    phone handles the captive portal in its own browser, and the
//    user can then either:
//      a) just use the phone for whatever they actually needed the
//         internet for (the device stays offline but the user is
//         unblocked); or
//      b) turn on their phone's hotspot and re-point the device to
//         the phone's hotspot SSID — which the device can join
//         directly with no captive portal in the way.
//
//  QR payload format (industry standard, recognized by iOS Camera
//  and Android Settings):
//    WIFI:S:<ssid>;T:<WPA|WEP|nopass>;P:<password>;H:<true|false>;;
//
//  Implementation:
//    - Uses ricmoo/QRCode 0.0.1 (header-only C lib, ~106 bytes per
//      v3 QR — pulled in via platformio.ini lib_deps).
//    - Version 4 QR (33×33 modules) gives ~114-char capacity at
//      ECC_LOW, which comfortably handles the longest typical WiFi
//      config payload (32-char SSID + 63-char password + format
//      overhead = ~110 chars).
//    - Per-device module size: 5 px on T-Deck Plus (320×240),
//      5 px on T-LoRa Pager (480×222), 3 px on Cardputer ADV
//      (240×135 — tighter but still scannable from a phone held
//      ~6 inches away).
//
//  NOT included on touch-only kiosks (C5/C28P/Maxine) — those have
//  their own MANUAL on-screen-keyboard flow via kiosk_wifi_setup.cpp,
//  which handles home-network setup directly; captive portals on
//  those devices remain a v1.3 problem to solve.
// ────────────────────────────────────────────────────────────────

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)

#include <Arduino.h>
#include <WiFi.h>
#include "qrcode.h"
#include "wifi_manager.h"
#include "keyboard.h"
#include "pm_input.h"
#include "theme.h"
#include "apps.h"

#if defined(DEVICE_TLORAPAGER)
  #include "pm_disp_tlorapager.h"
  extern PMDispTLoRaPager *gfx;
  static constexpr int DISP_W = 480;
  static constexpr int DISP_H = 222;
  static constexpr int QR_PX  = 5;
#elif defined(DEVICE_CARDPUTER_ADV)
  #include <Arduino_GFX_Library.h>
  extern Arduino_GFX *gfx;
  static constexpr int DISP_W = 240;
  static constexpr int DISP_H = 135;
  static constexpr int QR_PX  = 3;
#else  // DEVICE_TDECK_PLUS
  #include <Arduino_GFX_Library.h>
  extern Arduino_GFX *gfx;
  static constexpr int DISP_W = 320;
  static constexpr int DISP_H = 240;
  static constexpr int QR_PX  = 5;
#endif

// Build the standard WIFI:S:...;T:...;P:...;; payload from the live
// WiFi state plus the saved keyring. Returns empty string if we can't
// figure out a useful payload (no current SSID, no saved creds).
static String build_payload(String &out_ssid, bool &out_has_password) {
    String ssid     = WiFi.SSID();
    String password;

    if (ssid.length() > 0) {
        // Connected — try the keyring for the password
        password = get_known_password(ssid);
    } else {
        // Not connected — fall back to the last-saved primary cred so
        // the user can still share a known-good network even when the
        // radio happens to be off.
        // wifi_manager.cpp stores doc["ssid"]/doc["password"] as the
        // primary; reading via get_known_password requires knowing the
        // SSID. Without a primary-read API, the simpler signal is that
        // the keyring lookup of WiFi.SSID() returns empty — at which
        // point we just can't help.
        out_ssid = "";
        out_has_password = false;
        return "";
    }

    out_ssid = ssid;
    out_has_password = (password.length() > 0);

    // T:WPA covers WPA/WPA2/WPA3 — phones figure out which actual
    // protocol to use during the join. T:nopass marks open networks.
    const char *type = out_has_password ? "WPA" : "nopass";

    String payload = "WIFI:S:";
    payload += ssid;
    payload += ";T:";
    payload += type;
    payload += ";";
    if (out_has_password) {
        payload += "P:";
        payload += password;
        payload += ";";
    }
    payload += ";";
    return payload;
}

static void draw_header() {
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
    gfx->print("SHARE WIFI QR | " PM_EXIT_SHORT_COPY);
}

static void draw_error(const char *line1, const char *line2) {
    gfx->fillScreen(C_BLACK);
    draw_header();
    gfx->setTextSize(2);
    gfx->setTextColor(C_RED);
    gfx->setCursor(10, 50);
    gfx->print(line1);
    if (line2) {
        gfx->setTextSize(1);
        gfx->setTextColor(C_GREY);
        gfx->setCursor(10, 80);
        gfx->print(line2);
    }
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, DISP_H - 24);
    gfx->print("Any key to exit.");
}

void run_wifi_share_qr() {
    String ssid;
    bool   has_password = false;
    String payload = build_payload(ssid, has_password);

    if (payload.length() == 0) {
        draw_error("No WiFi.",
                   "Connect to a network first,");
        // No second-line variant of message API \u2014 print a third line manually.
        gfx->setCursor(10, 96);
        gfx->setTextColor(C_GREY);
        gfx->print("then revisit this screen.");
        while (true) {
            char k = get_keypress();
            if (k != 0) return;
            delay(40); yield();
        }
    }

    // Version 4 QR — 33×33 modules — fits up to 114 alphanumeric chars
    // at ECC_LOW, which is the comfortable ceiling for WIFI: payloads
    // with both a long SSID (32 chars) and a long WPA password (63
    // chars). Higher versions get tight on the Cardputer's 135-tall
    // panel; lower versions can fail on long credentials.
    QRCode qrcode;
    constexpr uint8_t QR_VERSION = 4;
    uint8_t qrcodeBuf[qrcode_getBufferSize(QR_VERSION)];
    qrcode_initText(&qrcode, qrcodeBuf, QR_VERSION, ECC_LOW, payload.c_str());

    gfx->fillScreen(C_BLACK);
    draw_header();

    // Centered QR on white quiet-zone background. The 4-module
    // quiet-zone padding is part of the spec — scanners need it to
    // delimit the symbol from the surrounding screen.
    const int qr_total = qrcode.size * QR_PX;
    const int quiet_px = 4 * QR_PX;
    const int frame_px = qr_total + 2 * quiet_px;
    const int qr_x = (DISP_W - qr_total) / 2;
    const int qr_y = 28;
    gfx->fillRect(qr_x - quiet_px, qr_y - quiet_px,
                  frame_px, frame_px, C_WHITE);

    for (int yy = 0; yy < qrcode.size; yy++) {
        for (int xx = 0; xx < qrcode.size; xx++) {
            if (qrcode_getModule(&qrcode, xx, yy)) {
                gfx->fillRect(qr_x + xx * QR_PX,
                              qr_y + yy * QR_PX,
                              QR_PX, QR_PX, C_BLACK);
            }
        }
    }

    // Caption: SSID + how-to text. Layout adapts per device.
    int caption_y = qr_y + qr_total + quiet_px + 4;
#if defined(DEVICE_CARDPUTER_ADV)
    // Cardputer is tight — single line, only SSID. The on-screen QR
    // already implies "scan me with your phone".
    if (caption_y > DISP_H - 12) caption_y = DISP_H - 12;
    gfx->setTextSize(1);
    gfx->setTextColor(C_CYAN);
    gfx->setCursor(2, caption_y);
    String label = ssid;
    if ((int)label.length() > 26) label = label.substring(0, 25) + "~";
    gfx->printf("SSID: %s", label.c_str());
#else
    // T-Deck Plus + T-LoRa Pager have room for a small how-to.
    gfx->setTextSize(1);
    gfx->setTextColor(C_CYAN);
    gfx->setCursor(10, caption_y);
    String label = ssid;
    if ((int)label.length() > 36) label = label.substring(0, 35) + "~";
    gfx->printf("SSID: %s", label.c_str());

    gfx->setTextColor(C_GREY);
    gfx->setCursor(10, caption_y + 14);
    gfx->print("Scan on phone -> joins same network.");
    gfx->setCursor(10, caption_y + 28);
    gfx->print("Phone handles captive portal in browser,");
    gfx->setCursor(10, caption_y + 42);
    gfx->print("then tether the device via hotspot.");
#endif

    // Block until any exit key (Q on T-Deck/T-LoRa, ESC on Cardputer).
    // Any non-zero keypress dismisses the screen.
    while (true) {
        char k = get_keypress();
        if (pm_is_exit_key(k) || k == 'q' || k == 'Q' || k == 27) return;
        delay(40);
        yield();
    }
}

#endif  // keyboard-device guards
