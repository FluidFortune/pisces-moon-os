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
 * PISCES MOON OS — SSH CLIENT v1.0
 * Minimal SSH terminal for homelab access
 *
 * Uses LibSSH-ESP32 (ewpa/LibSSH-ESP32) which ports libssh2
 * to the ESP32 Arduino framework.
 *
 * Controls:
 *   All keyboard keys pass through to SSH session
 *   CTRL-] (ASCII 29) = disconnect and return to host list
 *   Q on host list    = exit app
 *   Tap header        = exit app
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "ssh_client.h"

// LibSSH-ESP32: https://github.com/ewpa/LibSSH-ESP32
// Add to lib_deps: https://github.com/ewpa/LibSSH-ESP32
// When installed, uncomment:
// #include <libssh/libssh.h>
// For now the connection framework and UI are complete.

extern Arduino_GFX *gfx;
extern SdFat sd;
extern volatile bool wifi_in_use;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define SSH_BG      0x0000
#define SSH_HEADER  0x0841
#define SSH_GREEN   0x07E0
#define SSH_CYAN    0x07FF
#define SSH_WHITE   0xFFFF
#define SSH_DIM     0x4208
#define SSH_RED     0xF800
#define SSH_AMBER   0xFD20
#define SSH_TEXT    0x07E0   // Terminal text color — classic green-on-black

// ─────────────────────────────────────────────
//  TERMINAL DIMENSIONS
//  320x240 display, 6px per char at size 1
//  Header 22px, footer 16px → 202px for terminal
//  Cols: 320/6 = 53, Rows: 202/8 = ~25 (with line height)
// ─────────────────────────────────────────────
#define SSH_COLS       53
#define SSH_ROWS       10
#define SSH_CHAR_W     6
#define SSH_CHAR_H     9    // 8px + 1px spacing
#define SSH_TERM_TOP   24
#define SSH_TERM_LEFT  2

// ─────────────────────────────────────────────
//  SAVED HOSTS
// ─────────────────────────────────────────────
#define SSH_HOSTS_FILE "/ssh_hosts.json"
#define SSH_MAX_HOSTS  10

struct SSHHost {
    char label[32];
    char host[64];
    int  port;
    char user[32];
    char pass[64];   // Stored in plaintext for now — NVS encryption future work
};

static SSHHost sshHosts[SSH_MAX_HOSTS];
static int     sshHostCount = 0;

// ─────────────────────────────────────────────
//  TERMINAL BUFFER
// ─────────────────────────────────────────────
static char  sshTermBuf[SSH_ROWS][SSH_COLS + 1];
static int   sshCurRow = 0;
static int   sshCurCol = 0;

static void sshClearTerm() {
    for (int r = 0; r < SSH_ROWS; r++) {
        memset(sshTermBuf[r], 0, SSH_COLS + 1);
    }
    sshCurRow = 0;
    sshCurCol = 0;
}

static void sshScrollUp() {
    for (int r = 0; r < SSH_ROWS - 1; r++)
        memcpy(sshTermBuf[r], sshTermBuf[r + 1], SSH_COLS + 1);
    memset(sshTermBuf[SSH_ROWS - 1], 0, SSH_COLS + 1);
    sshCurRow = SSH_ROWS - 1;
    sshCurCol = 0;
}

static void sshPutChar(char c) {
    if (c == '\n' || c == '\r') {
        sshCurCol = 0;
        sshCurRow++;
        if (sshCurRow >= SSH_ROWS) sshScrollUp();
        return;
    }
    if (c == '\b' && sshCurCol > 0) {
        sshCurCol--;
        sshTermBuf[sshCurRow][sshCurCol] = 0;
        return;
    }
    if (c >= 32 && c < 127) {
        if (sshCurCol >= SSH_COLS) {
            sshCurCol = 0;
            sshCurRow++;
            if (sshCurRow >= SSH_ROWS) sshScrollUp();
        }
        sshTermBuf[sshCurRow][sshCurCol++] = c;
    }
}

static void sshPutString(const char* s) {
    while (*s) sshPutChar(*s++);
}

// ─────────────────────────────────────────────
//  DRAW TERMINAL
// ─────────────────────────────────────────────
static void sshDrawHeader(const char* title, bool connected) {
    gfx->fillRect(0, 0, 320, 22, SSH_HEADER);
    gfx->drawFastHLine(0, 22, 320, connected ? SSH_GREEN : SSH_DIM);
    gfx->setTextSize(1);
    gfx->setTextColor(connected ? SSH_GREEN : SSH_DIM);
    gfx->setCursor(6, 7);
    gfx->print(title);
    gfx->setTextColor(SSH_DIM);
    gfx->setCursor(240, 7);
    gfx->print("[TAP=EXIT]");
}

static void sshDrawTerm() {
    gfx->fillRect(0, SSH_TERM_TOP, 320, 240 - SSH_TERM_TOP, SSH_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(SSH_TEXT);
    for (int r = 0; r < SSH_ROWS; r++) {
        if (sshTermBuf[r][0] == 0) continue;
        gfx->setCursor(SSH_TERM_LEFT, SSH_TERM_TOP + r * SSH_CHAR_H);
        gfx->print(sshTermBuf[r]);
    }
    // Draw cursor
    int cy = SSH_TERM_TOP + sshCurRow * SSH_CHAR_H;
    int cx = SSH_TERM_LEFT + sshCurCol * SSH_CHAR_W;
    gfx->fillRect(cx, cy + 7, SSH_CHAR_W, 2, SSH_GREEN);
}

// ─────────────────────────────────────────────
//  LOAD / SAVE HOSTS
// ─────────────────────────────────────────────
static void sshLoadHosts() {
    sshHostCount = 0;
    if (!sd.exists(SSH_HOSTS_FILE)) return;

    FsFile f = sd.open(SSH_HOSTS_FILE, O_READ);
    if (!f) return;

    JsonDocument doc;
    if (deserializeJson(doc, f) != DeserializationError::Ok) {
        f.close();
        return;
    }
    f.close();

    JsonArray arr = doc["hosts"].as<JsonArray>();
    for (JsonObject h : arr) {
        if (sshHostCount >= SSH_MAX_HOSTS) break;
        SSHHost& host = sshHosts[sshHostCount++];
        strncpy(host.label, h["label"] | "Host", 31);
        strncpy(host.host,  h["host"]  | "",     63);
        host.port = h["port"] | 22;
        strncpy(host.user,  h["user"]  | "",     31);
        strncpy(host.pass,  h["pass"]  | "",     63);
    }
}

static void sshSaveHost(const SSHHost& h) {
    // Load existing, append, save
    sshLoadHosts();

    JsonDocument doc;
    JsonArray arr = doc["hosts"].to<JsonArray>();
    for (int i = 0; i < sshHostCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["label"] = sshHosts[i].label;
        o["host"]  = sshHosts[i].host;
        o["port"]  = sshHosts[i].port;
        o["user"]  = sshHosts[i].user;
        o["pass"]  = sshHosts[i].pass;
    }
    // Add new
    if (sshHostCount < SSH_MAX_HOSTS) {
        JsonObject o = arr.add<JsonObject>();
        o["label"] = h.label;
        o["host"]  = h.host;
        o["port"]  = h.port;
        o["user"]  = h.user;
        o["pass"]  = h.pass;
    }

    FsFile f = sd.open(SSH_HOSTS_FILE, O_WRITE | O_CREAT | O_TRUNC);
    if (f) {
        serializeJson(doc, f);
        f.flush();
        f.close();
    }
}

// ─────────────────────────────────────────────
//  HOST LIST SCREEN
// ─────────────────────────────────────────────
static int sshHostListScreen() {
    sshLoadHosts();

    gfx->fillScreen(SSH_BG);
    sshDrawHeader("SSH CLIENT — SELECT HOST", false);

    gfx->setTextSize(1);
    if (sshHostCount == 0) {
        gfx->setTextColor(SSH_DIM);
        gfx->setCursor(10, 50);
        gfx->print("No saved hosts. Press N to add new.");
    } else {
        for (int i = 0; i < sshHostCount; i++) {
            bool sel = (i == 0);  // TODO: trackball selection
            gfx->setTextColor(sel ? SSH_WHITE : SSH_DIM);
            gfx->setCursor(10, 30 + i * 18);
            gfx->printf("[%d] %s  %s@%s:%d",
                i + 1,
                sshHosts[i].label,
                sshHosts[i].user,
                sshHosts[i].host,
                sshHosts[i].port);
        }
    }

    gfx->setTextColor(SSH_DIM);
    gfx->setCursor(10, 220);
    gfx->print("1-9=connect  N=new host  Q=exit");

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) delay(10);
            return -1;  // exit
        }

        char k = get_keypress();
        if (k == 'q' || k == 'Q') return -1;

        // Number keys select host
        if (k >= '1' && k <= '9') {
            int idx = k - '1';
            if (idx < sshHostCount) return idx;
        }

        // N = new host
        if (k == 'n' || k == 'N') return -2;  // new host

        delay(30);
    }
}

// ─────────────────────────────────────────────
//  NEW HOST ENTRY
// ─────────────────────────────────────────────
static bool sshNewHostScreen(SSHHost& host) {
    gfx->fillScreen(SSH_BG);
    sshDrawHeader("SSH CLIENT — NEW HOST", false);

    struct Field {
        const char* label;
        char*       buf;
        int         maxLen;
        bool        isPass;
    } fields[] = {
        {"Label:",    host.label, 31,  false},
        {"Hostname:", host.host,  63,  false},
        {"Port:",     nullptr,    0,   false},
        {"Username:", host.user,  31,  false},
        {"Password:", host.pass,  63,  true},
    };

    char portStr[8] = "22";
    fields[2].buf    = portStr;
    fields[2].maxLen = 7;

    gfx->setTextSize(1);

    for (int f = 0; f < 5; f++) {
        int y = 35 + f * 38;
        gfx->setTextColor(SSH_CYAN);
        gfx->setCursor(10, y);
        gfx->print(fields[f].label);

        gfx->setTextColor(SSH_WHITE);
        gfx->setCursor(10, y + 14);
        gfx->print("> ");

        String input = "";
        gfx->setCursor(22, y + 14);

        while (true) {
            char c = get_keypress();
            if (c == 13 || c == 10) break;
            if (c == 'q' && input.length() == 0) return false;
            if ((c == 8 || c == 127) && input.length() > 0) {
                input.remove(input.length() - 1);
                gfx->fillRect(22, y + 14, 290, 10, SSH_BG);
                gfx->setCursor(22, y + 14);
                gfx->print(fields[f].isPass ? String(input.length(), '*') : input);
            } else if (c >= 32 && c <= 126 && (int)input.length() < fields[f].maxLen) {
                input += c;
                if (fields[f].isPass) {
                    gfx->fillRect(22, y + 14, 290, 10, SSH_BG);
                    gfx->setCursor(22, y + 14);
                    gfx->print(String(input.length(), '*'));
                } else {
                    gfx->print(c);
                }
            }
            delay(15);
        }

        strncpy(fields[f].buf, input.c_str(), fields[f].maxLen);
    }

    host.port = atoi(portStr);
    if (host.port <= 0 || host.port > 65535) host.port = 22;
    return true;
}

// ─────────────────────────────────────────────
//  SSH SESSION
//  Full session with LibSSH-ESP32.
//  Codec stub present — replace with real SSH calls when library installed.
// ─────────────────────────────────────────────
static void sshSession(const SSHHost& host) {
    gfx->fillScreen(SSH_BG);
    sshClearTerm();
    sshDrawHeader("SSH CLIENT — CONNECTING", false);

    // Check WiFi
    if (WiFi.status() != WL_CONNECTED) {
        sshPutString("ERROR: No WiFi connection.\r\nConnect via WIFI JOIN first.\r\n");
        sshDrawTerm();
        delay(2000);
        return;
    }

    sshPutString("Connecting to ");
    sshPutString(host.host);
    char portStr[16];
    snprintf(portStr, sizeof(portStr), ":%d\r\n", host.port);
    sshPutString(portStr);
    sshDrawTerm();

    // ── LibSSH-ESP32 session ──────────────────────────────────────
    // When library is installed, replace this stub with:
    //
    // ssh_session session = ssh_new();
    // ssh_options_set(session, SSH_OPTIONS_HOST, host.host);
    // ssh_options_set(session, SSH_OPTIONS_PORT, &host.port);
    // ssh_options_set(session, SSH_OPTIONS_USER, host.user);
    // ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY_STR, "0");
    //
    // if (ssh_connect(session) != SSH_OK) { ... }
    // if (ssh_userauth_password(session, nullptr, host.pass) != SSH_AUTH_SUCCESS) { ... }
    //
    // ssh_channel channel = ssh_channel_new(session);
    // ssh_channel_open_session(channel);
    // ssh_channel_request_pty(channel);
    // ssh_channel_request_shell(channel);
    //
    // Then in loop:
    //   char buf[512];
    //   int n = ssh_channel_read_nonblocking(channel, buf, sizeof(buf), 0);
    //   if (n > 0) { for(int i=0;i<n;i++) sshPutChar(buf[i]); sshDrawTerm(); }
    //   char k = get_keypress();
    //   if (k == 29) break;  // CTRL-]
    //   if (k != 0) ssh_channel_write(channel, &k, 1);
    //
    // ssh_channel_close(channel);
    // ssh_channel_free(channel);
    // ssh_disconnect(session);
    // ssh_free(session);
    // ─────────────────────────────────────────────────────────────

    // Demo mode: TCP connect only (no SSH crypto)
    WiFiClient client;
    wifi_in_use = true;

    sshPutString("Opening TCP connection...\r\n");
    sshDrawTerm();

    if (!client.connect(host.host, host.port)) {
        sshPutString("Connection FAILED.\r\n");
        sshPutString("Check hostname/port and network.\r\n");
        sshDrawTerm();
        wifi_in_use = false;
        delay(2000);
        return;
    }

    sshPutString("TCP connected. SSH handshake pending library install.\r\n");
    sshPutString("Add to platformio.ini lib_deps:\r\n");
    sshPutString("  https://github.com/ewpa/LibSSH-ESP32\r\n");
    sshPutString("Then rebuild to enable full SSH.\r\n");
    sshPutString("\r\nPress CTRL-] (type ] with CTRL held) to disconnect.\r\n");
    sshDrawTerm();

    sshDrawHeader(host.label, true);

    // Passthrough loop (TCP only until SSH library installed)
    while (client.connected()) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) delay(10);
            break;
        }

        // Read from TCP
        while (client.available()) {
            char c = client.read();
            sshPutChar(c);
        }

        // Read keyboard → send to TCP
        char k = get_keypress();
        if (k == 29) break;  // CTRL-] disconnect
        if (k != 0) client.write(k);

        if (millis() % 250 == 0) sshDrawTerm();

        delay(10);
        yield();
    }

    client.stop();
    wifi_in_use = false;

    sshPutString("\r\n[Disconnected]\r\n");
    sshDrawTerm();
    delay(1500);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_ssh_client() {
    while (true) {
        int hostIdx = sshHostListScreen();

        if (hostIdx == -1) break;  // Exit

        if (hostIdx == -2) {
            // New host
            SSHHost newHost = {};
            if (sshNewHostScreen(newHost)) {
                sshSaveHost(newHost);
                sshLoadHosts();
            }
            continue;
        }

        // Connect to selected host
        sshSession(sshHosts[hostIdx]);
    }

    gfx->fillScreen(SSH_BG);
}