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
 * PISCES MOON OS — NETWORK SCANNER v1.0
 * Local subnet host discovery + port scanner
 *
 * USE ONLY ON NETWORKS YOU OWN OR HAVE EXPLICIT PERMISSION TO TEST.
 * This is the same functionality as 'nmap -sn' (ping sweep) and
 * 'nmap -p <ports>' (port scan) — standard network administration tools.
 *
 * Phase 1 — Host Discovery:
 *   Sends ICMP echo (ping) to each host on /24 subnet
 *   Displays responding IPs and response time
 *
 * Phase 2 — Port Scan (on selected host):
 *   TCP connect scan on common ports:
 *   21 FTP, 22 SSH, 23 Telnet, 25 SMTP, 53 DNS, 80 HTTP,
 *   110 POP3, 143 IMAP, 443 HTTPS, 445 SMB, 3389 RDP,
 *   8080 HTTP-alt, 8443 HTTPS-alt
 *
 * Controls:
 *   Trackball up/down = scroll host list / select host
 *   Trackball click   = scan selected host ports
 *   Q / header tap    = exit / back
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <lwip/icmp.h>
#include <lwip/inet_chksum.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <FS.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "net_scanner.h"

extern Arduino_GFX *gfx;
extern volatile bool wifi_in_use;
extern SdFat sd;

// scan_log_write() and supporting structs are defined after
// MAX_HOSTS, NUM_PORTS, SCAN_PORTS etc. — see just before run_net_scanner()

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG      0x0000
#define COL_HDR     0x0010
#define COL_UP      0x07E0   // Green — host up
#define COL_OPEN    0x07E0   // Green — port open
#define COL_CLOSED  0x4208   // Grey — port closed
#define COL_SCAN    0xFFE0   // Yellow — scanning
#define COL_TEXT    0xFFFF
#define COL_DIM     0x4208
#define COL_ACCENT  0xFD20
#define COL_SEL     0x001F

// ─────────────────────────────────────────────
//  HOST TABLE
// ─────────────────────────────────────────────
#define MAX_HOSTS 32

struct HostEntry {
    uint8_t  lastOctet;
    uint32_t pingMs;
};

static HostEntry hosts[MAX_HOSTS];
static int       hostCount = 0;
static int       selectedHost = 0;
static int       hostScroll = 0;

// Port scan results
static const int SCAN_PORTS[] = {21,22,23,25,53,80,110,143,443,445,3389,8080,8443};
static const char* PORT_NAMES[] = {"FTP","SSH","TELNET","SMTP","DNS","HTTP","POP3","IMAP","HTTPS","SMB","RDP","HTTP-A","HTTPS-A"};
#define NUM_PORTS 13
static bool portOpen[NUM_PORTS];
static bool portScanned[NUM_PORTS];

// ─────────────────────────────────────────────
//  PING (ICMP echo via raw socket)
// ─────────────────────────────────────────────
static bool pingHost(uint32_t ip, uint32_t* rttMs) {
    // Use WiFi.ping() wrapper if available in this SDK version,
    // otherwise TCP connect as a probe (more reliable on embedded)
    // We use a TCP connect to port 7 (echo) or 80 as a quick alive check.
    // Pure ICMP requires raw sockets which need extra IDF privileges.
    // Fallback: TCP connect probe to port 80/22/443 with 200ms timeout.

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;

    uint32_t start = millis();

    // Try ports that are likely open on common devices
    const int probePorts[] = {80, 443, 22, 8080, 21, 23, 7};
    for (int port : probePorts) {
        addr.sin_port = htons(port);
        int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        // Non-blocking with timeout
        struct timeval tv;
        tv.tv_sec = 0; tv.tv_usec = 150000; // 150ms
        lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        int result = lwip_connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        lwip_close(sock);

        if (result == 0 || errno == ECONNREFUSED) {
            // Either connected or port refused — host IS up
            if (rttMs) *rttMs = millis() - start;
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────
//  PORT SCAN
// ─────────────────────────────────────────────
static bool tcpConnect(uint32_t ip, int port, int timeoutMs) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ip;
    addr.sin_port = htons(port);

    int sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    lwip_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int result = lwip_connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    lwip_close(sock);
    // ECONNREFUSED means port closed but host up. 0 means open.
    return (result == 0);
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawHeader(const char* subtitle) {
    gfx->fillRect(0, 0, 320, 24, COL_HDR);
    gfx->drawFastHLine(0, 23, 320, COL_UP);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_UP);
    gfx->setCursor(8, 4);  gfx->print("NET SCANNER");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8, 13); gfx->print(subtitle);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(260, 8); gfx->print("[TAP=EXIT]");
}

static void drawHostList() {
    int lineH = 20;
    int startY = 26;
    int perPage = 9;

    gfx->fillRect(0, startY, 320, perPage * lineH, COL_BG);
    gfx->setTextSize(1);

    for (int i = 0; i < perPage; i++) {
        int idx = hostScroll + i;
        if (idx >= hostCount) break;
        int y = startY + i * lineH;
        bool sel = (idx == selectedHost);
        gfx->fillRect(0, y, 320, lineH - 1, sel ? COL_SEL : (i%2?COL_BG:0x0821));

        gfx->setTextColor(COL_UP);
        gfx->setCursor(6, y + 4);
        char buf[40];
        String base = WiFi.localIP().toString();
        // Replace last octet
        int lastDot = base.lastIndexOf('.');
        String prefix = base.substring(0, lastDot + 1);
        snprintf(buf, 40, "%s%d", prefix.c_str(), hosts[idx].lastOctet);
        gfx->print(buf);

        gfx->setTextColor(COL_DIM);
        gfx->setCursor(200, y + 4);
        gfx->printf("%3lums", hosts[idx].pingMs);

        if (sel) {
            gfx->setTextColor(COL_ACCENT);
            gfx->setCursor(260, y + 4);
            gfx->print("[CLK=SCAN]");
        }
    }

    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, 26 + perPage * lineH + 4);
    gfx->printf("%d hosts up. Up/dn=select, CLK=port scan", hostCount);
}

static void drawPortScan(uint8_t lastOctet) {
    gfx->fillRect(0, 26, 320, 214, COL_BG);
    gfx->setTextSize(1);

    String base = WiFi.localIP().toString();
    int lastDot = base.lastIndexOf('.');
    String ip = base.substring(0, lastDot + 1) + String(lastOctet);

    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(6, 30);
    gfx->printf("Port scan: %s", ip.c_str());
    gfx->drawFastHLine(0, 40, 320, 0x2104);

    int cols = 2;
    int lineH = 18;
    for (int i = 0; i < NUM_PORTS; i++) {
        int col = i % cols;
        int row = i / cols;
        int x = col == 0 ? 6 : 166;
        int y = 44 + row * lineH;

        if (!portScanned[i]) {
            gfx->setTextColor(COL_DIM);
            gfx->setCursor(x, y);
            gfx->printf("%-7s %-5d ---", PORT_NAMES[i], SCAN_PORTS[i]);
        } else {
            gfx->setTextColor(portOpen[i] ? COL_OPEN : COL_CLOSED);
            gfx->setCursor(x, y);
            gfx->printf("%-7s %-5d %s",
                PORT_NAMES[i], SCAN_PORTS[i],
                portOpen[i] ? "OPEN" : "----");
        }
    }
}

// ─────────────────────────────────────────────
//  SESSION LOG
//  Defined here — after MAX_HOSTS, NUM_PORTS,
//  SCAN_PORTS, PORT_NAMES, and hostCount are all
//  declared so the compiler can resolve them.
// ─────────────────────────────────────────────
#define SCAN_LOG_DIR "/cyber_logs"

struct HostScanResult {
    uint8_t  lastOctet;
    uint32_t pingMs;
    bool     portOpen[NUM_PORTS];
    bool     portScanned;
};
static HostScanResult _scanResults[MAX_HOSTS];
static String _scannedSubnet = "";

static void scan_log_write() {
    if (hostCount == 0) return;
    if (!sd.exists(SCAN_LOG_DIR)) sd.mkdir(SCAN_LOG_DIR);

    char fname[52];
    snprintf(fname, sizeof(fname), "%s/scan_%010lu.json", SCAN_LOG_DIR, millis());

    FsFile f = sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) {
        Serial.println("[SCAN] Log write FAILED");
        return;
    }

    f.printf("{\"subnet\":\"%s\",\"hosts\":[", _scannedSubnet.c_str());

    for (int i = 0; i < hostCount; i++) {
        if (i > 0) f.print(",");
        f.printf("{\"ip\":\"%s%d\",\"ping_ms\":%lu,\"ports\":[",
            _scannedSubnet.c_str(),
            _scanResults[i].lastOctet,
            _scanResults[i].pingMs);

        if (_scanResults[i].portScanned) {
            bool firstPort = true;
            for (int p = 0; p < NUM_PORTS; p++) {
                if (_scanResults[i].portOpen[p]) {
                    if (!firstPort) f.print(",");
                    f.printf("{\"port\":%d,\"name\":\"%s\"}",
                        SCAN_PORTS[p], PORT_NAMES[p]);
                    firstPort = false;
                }
            }
        }
        f.print("]}");
    }

    f.print("]}");
    f.flush();
    f.close();
    Serial.printf("[SCAN] Log saved: %s  Hosts:%d\n", fname, hostCount);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_net_scanner() {
    if (WiFi.status() != WL_CONNECTED) {
        gfx->fillScreen(COL_BG);
        gfx->setTextColor(0xF800);
        gfx->setTextSize(1);
        gfx->setCursor(20, 100); gfx->print("WiFi not connected.");
        gfx->setCursor(20, 116); gfx->print("Connect via WIFI JOIN first.");
        delay(3000); return;
    }

    wifi_in_use = true;
    hostCount = 0;
    selectedHost = 0;
    hostScroll = 0;
    memset(_scanResults, 0, sizeof(_scanResults));

    // Get our subnet
    IPAddress local = WiFi.localIP();
    String subnet = String(local[0]) + "." + String(local[1]) + "." + String(local[2]) + ".";
    _scannedSubnet = subnet;

    gfx->fillScreen(COL_BG);
    drawHeader("HOST DISCOVERY");

    gfx->setTextSize(1);
    gfx->setTextColor(COL_SCAN);
    gfx->setCursor(6, 30);
    gfx->printf("Scanning %s0/24...", subnet.c_str());

    // Ping sweep
    for (int i = 1; i <= 254; i++) {
        // Update progress
        if (i % 16 == 0) {
            gfx->fillRect(0, 42, 320, 10, COL_BG);
            gfx->setTextColor(COL_DIM);
            gfx->setCursor(6, 42);
            gfx->printf("Probing ...%d (%d found)", i, hostCount);

            // Check for header tap to abort
            int16_t tx, ty;
            if (get_touch(&tx, &ty) && ty < 24) {
                while(get_touch(&tx,&ty)){delay(5);}
                wifi_in_use = false;
                gfx->fillScreen(COL_BG); return;
            }
            yield();
        }

        // Skip our own IP and broadcast
        if (i == local[3] || i == 255 || i == 0) continue;

        IPAddress target(local[0], local[1], local[2], i);
        uint32_t rtt = 999;
        if (pingHost((uint32_t)target, &rtt) && hostCount < MAX_HOSTS) {
            hosts[hostCount].lastOctet = i;
            hosts[hostCount].pingMs    = rtt;
            _scanResults[hostCount].lastOctet  = i;
            _scanResults[hostCount].pingMs     = rtt;
            _scanResults[hostCount].portScanned = false;
            hostCount++;

            // Update live display
            gfx->setTextColor(COL_UP);
            gfx->setCursor(6, 56 + (hostCount % 6) * 18);
            gfx->printf("%s%d  %3lums  UP", subnet.c_str(), i, rtt);
        }
    }

    // Show results
    gfx->fillScreen(COL_BG);
    drawHeader("HOST DISCOVERY — DONE");
    drawHostList();

    bool inPortScan = false;

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (inPortScan) {
                inPortScan = false;
                gfx->fillScreen(COL_BG);
                drawHeader("HOST DISCOVERY — DONE");
                drawHostList();
            } else break;
            continue;
        }
        if (k == 'q' || k == 'Q') {
            if (inPortScan) {
                inPortScan = false;
                gfx->fillScreen(COL_BG);
                drawHeader("HOST DISCOVERY — DONE");
                drawHostList();
            } else break;
            continue;
        }

        if (!inPortScan) {
            bool changed = false;
            if (tb.y == -1 && selectedHost > 0) {
                selectedHost--;
                if (selectedHost < hostScroll) hostScroll--;
                changed = true;
            }
            if (tb.y == 1 && selectedHost < hostCount - 1) {
                selectedHost++;
                if (selectedHost >= hostScroll + 9) hostScroll++;
                changed = true;
            }
            if (changed) drawHostList();

            if (tb.clicked && hostCount > 0) {
                // Run port scan on selected host
                inPortScan = true;
                memset(portOpen, 0, sizeof(portOpen));
                memset(portScanned, 0, sizeof(portScanned));

                String ip = subnet + String(hosts[selectedHost].lastOctet);
                IPAddress target;
                target.fromString(ip);
                uint32_t targetIP = (uint32_t)target;

                drawHeader(("PORT SCAN: " + ip).c_str());
                drawPortScan(hosts[selectedHost].lastOctet);

                for (int p = 0; p < NUM_PORTS; p++) {
                    // Live update scanning indicator
                    gfx->setTextColor(COL_SCAN);
                    int col = p % 2;
                    int row = p / 2;
                    int x = col == 0 ? 6 : 166;
                    int y = 44 + row * 18;
                    gfx->setCursor(x, y);
                    gfx->printf("%-7s %-5d ...", PORT_NAMES[p], SCAN_PORTS[p]);

                    portOpen[p]    = tcpConnect(targetIP, SCAN_PORTS[p], 300);
                    portScanned[p] = true;

                    // Store in persistent results for log
                    _scanResults[selectedHost].portOpen[p]   = portOpen[p];
                    _scanResults[selectedHost].portScanned    = true;

                    drawPortScan(hosts[selectedHost].lastOctet);
                    yield();
                }
            }
        }

        delay(50);
        yield();
    }

    wifi_in_use = false;
    scan_log_write();   // Save all hosts + port results to SD
    gfx->fillScreen(COL_BG);
}