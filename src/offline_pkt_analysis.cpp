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
 * PISCES MOON OS — OFFLINE PACKET ANALYSIS v1.0
 * Post-session analysis engine for /cyber_logs/ data.
 *
 * Reads beacon spotter JSON and packet sniffer CSV files from the SD card
 * and runs a rules engine against the collected data to surface anomalies.
 *
 * Detection rules:
 *   [DEAUTH FLOOD]   Source MAC sent >10 deauth/disassoc frames in session
 *   [EVIL TWIN]      Same SSID seen with 2+ different BSSIDs on different channels
 *   [ENC DOWNGRADE]  Same SSID seen as both encrypted and OPEN (possible stripping)
 *   [HIDDEN BEACON]  Hidden SSID with >500 beacons (high rate = potential spoofing)
 *   [CHANNEL ANOMALY] AP on non-standard channel (1/6/11 for 2.4GHz)
 *   [PROBE PATTERN]  Device probing for corporate/sensitive SSID names
 *
 * File browsing:
 *   Lists all beacon_*.json and pkt_*.csv in /cyber_logs/
 *   User selects file, engine runs analysis, findings displayed with severity
 *
 * Output:
 *   On-screen findings list with CRITICAL/WARNING/INFO levels
 *   Saved to /cyber_logs/analysis_NNNN.txt for later reference
 *
 * SPI Bus Treaty: SD read only. No WiFi. Safe to run alongside wardrive.
 *
 * Controls:
 *   Trackball U/D = scroll file list / findings list
 *   Trackball CLICK = select file / run analysis
 *   Q / tap header  = exit / back
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "offline_pkt_analysis.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define OA_BG      0x0000
#define OA_HDR     0x0010
#define OA_CYAN    0x07FF
#define OA_GREEN   0x07E0
#define OA_YELLOW  0xFFE0
#define OA_ORANGE  0xFD20
#define OA_RED     0xF800
#define OA_WHITE   0xFFFF
#define OA_DIM     0x4208
#define OA_SEL     0x0018

#define LOG_DIR    "/cyber_logs"

// ─────────────────────────────────────────────
//  SEVERITY LEVELS
// ─────────────────────────────────────────────
#define SEV_INFO     0
#define SEV_WARNING  1
#define SEV_CRITICAL 2

static uint16_t sevColor(int sev) {
    switch(sev) {
        case SEV_CRITICAL: return OA_RED;
        case SEV_WARNING:  return OA_YELLOW;
        default:           return OA_CYAN;
    }
}
static const char* sevLabel(int sev) {
    switch(sev) {
        case SEV_CRITICAL: return "CRIT";
        case SEV_WARNING:  return "WARN";
        default:           return "INFO";
    }
}

// ─────────────────────────────────────────────
//  FINDING STORAGE
// ─────────────────────────────────────────────
#define MAX_FINDINGS 48
#define FINDING_MSG_LEN 64

struct OAFinding {
    int  severity;
    char rule[16];
    char msg[FINDING_MSG_LEN];
};

// Declared after struct definitions below — PSRAM allocated on launch
static OAFinding* findings    = nullptr;
static int        findingCount = 0;

static void oaAddFinding(int sev, const char* rule, const char* msg) {
    if (findingCount >= MAX_FINDINGS) return;
    OAFinding& f = findings[findingCount++];
    f.severity = sev;
    strncpy(f.rule, rule, 15); f.rule[15] = '\0';
    strncpy(f.msg,  msg,  FINDING_MSG_LEN - 1); f.msg[FINDING_MSG_LEN-1] = '\0';
    Serial.printf("[OA] [%s] %s\n", rule, msg);
}

// ─────────────────────────────────────────────
//  FILE LIST
// ─────────────────────────────────────────────
#define MAX_LOG_FILES 32

struct OAFile {
    char name[48];
    bool isBeacon;   // true=JSON, false=CSV
    uint32_t size;
};

// PSRAM pointers — allocated in run_offline_pkt_analysis(), freed on exit
static OAFile* oaFiles    = nullptr;
static int     oaFileCount = 0;

// oaFiles and findings declared as PSRAM pointers above

static void oaScanFiles() {
    oaFileCount = 0;
    if (!sd.exists(LOG_DIR)) return;
    FsFile dir = sd.open(LOG_DIR);
    if (!dir) return;
    FsFile f;
    while (f.openNext(&dir, O_RDONLY) && oaFileCount < MAX_LOG_FILES) {
        char nm[48]; f.getName(nm, sizeof(nm));
        bool beacon = (strncmp(nm, "beacon_", 7) == 0 && strstr(nm, ".json"));
        bool pkt    = (strncmp(nm, "pkt_",    4) == 0 && strstr(nm, ".csv"));
        bool probe  = (strncmp(nm, "probe_",  6) == 0 && strstr(nm, ".json"));
        if (beacon || pkt || probe) {
            OAFile& of = oaFiles[oaFileCount++];
            snprintf(of.name, sizeof(of.name), "%s/%s", LOG_DIR, nm);
            of.isBeacon = beacon || probe;
            of.size     = f.fileSize();
        }
        f.close();
    }
    dir.close();
}

// ─────────────────────────────────────────────
//  KNOWN SENSITIVE SSID PATTERNS
//  Probing for these warrants a PROBE PATTERN finding
// ─────────────────────────────────────────────
static const char* SENSITIVE_PATTERNS[] = {
    "corp", "vpn", "internal", "employee", "staff", "office",
    "admin", "secure", "private", "guest", "iot", "camera",
    "mgmt", "management", "infrastructure", "prod", "dev",
    nullptr
};

static bool oaIsSensitive(const char* ssid) {
    if (!ssid || ssid[0] == '\0') return false;
    char lower[33]; int i = 0;
    while (ssid[i] && i < 32) { lower[i] = tolower(ssid[i]); i++; }
    lower[i] = '\0';
    for (int j = 0; SENSITIVE_PATTERNS[j]; j++) {
        if (strstr(lower, SENSITIVE_PATTERNS[j])) return true;
    }
    return false;
}

// ─────────────────────────────────────────────
//  ANALYSIS ENGINE — BEACON JSON
// ─────────────────────────────────────────────

// AP shadow table for cross-AP analysis
struct OAApEntry { char ssid[33]; char bssid[18]; int channel; bool enc; };

// AP shadow table — PSRAM allocated
static OAApEntry* oaAps    = nullptr;
static int        oaApCount = 0;

static bool oaIsStandardChannel(int ch) {
    return (ch == 1 || ch == 6 || ch == 11 || ch == 13 ||
            ch == 36 || ch == 40 || ch == 44 || ch == 48);
}

static void oaAnalyzeBeacon(const char* path) {
    oaApCount = 0;

    // Stream parse the JSON — ArduinoJson with PSRAM-backed doc
    FsFile f = sd.open(path, O_RDONLY);
    if (!f) { oaAddFinding(SEV_WARNING, "FILE", "Cannot open log file"); return; }

    uint32_t fsize = f.fileSize();
    if (fsize > 48000) {
        oaAddFinding(SEV_INFO, "SIZE", "File >48KB — truncating parse to first 48KB");
        fsize = 48000;
    }

    char* buf = (char*)ps_malloc(fsize + 1);
    if (!buf) { f.close(); oaAddFinding(SEV_WARNING, "MEM", "Out of PSRAM for parse buffer"); return; }
    f.read(buf, fsize);
    buf[fsize] = '\0';
    f.close();

    // Parse with ArduinoJson
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    free(buf);

    if (err) {
        char msg[48]; snprintf(msg, sizeof(msg), "JSON parse error: %s", err.c_str());
        oaAddFinding(SEV_WARNING, "PARSE", msg); return;
    }

    // ── Process APs ──────────────────────────────────────────
    JsonArray aps = doc["aps"];
    for (JsonObject ap : aps) {
        if (oaApCount >= 64) break;
        OAApEntry& e = oaAps[oaApCount++];
        const char* ssid  = ap["ssid"] | "";
        const char* bssid = ap["bssid"] | "";
        int channel    = ap["channel"] | 0;
        bool enc       = ap["enc"] | false;
        bool hidden    = ap["hidden"] | false;
        long beacons   = ap["beacons"] | 0;

        strncpy(e.ssid,  ssid,  32); e.ssid[32]  = '\0';
        strncpy(e.bssid, bssid, 17); e.bssid[17] = '\0';
        e.channel = channel;
        e.enc     = enc;

        // Rule: HIDDEN BEACON with high rate
        if (hidden && beacons > 500) {
            char msg[FINDING_MSG_LEN];
            snprintf(msg, sizeof(msg), "Hidden AP %s: %ld beacons (high rate)", bssid, beacons);
            oaAddFinding(SEV_WARNING, "HIDDEN AP", msg);
        }

        // Rule: CHANNEL ANOMALY
        if (channel > 0 && !oaIsStandardChannel(channel)) {
            char msg[FINDING_MSG_LEN];
            snprintf(msg, sizeof(msg), "AP %s on non-std CH%d (%.12s)", bssid, channel, ssid);
            oaAddFinding(SEV_INFO, "CH ANOMALY", msg);
        }
    }

    // Rule: EVIL TWIN — same SSID, different BSSID, different channel
    for (int i = 0; i < oaApCount; i++) {
        for (int j = i + 1; j < oaApCount; j++) {
            if (oaAps[i].ssid[0] == '\0') continue;
            if (strcmp(oaAps[i].ssid, oaAps[j].ssid) != 0) continue;
            if (strcmp(oaAps[i].bssid, oaAps[j].bssid) == 0) continue;
            // Same SSID, different BSSID
            if (abs(oaAps[i].channel - oaAps[j].channel) > 2) {
                char msg[FINDING_MSG_LEN];
                snprintf(msg, sizeof(msg), "EVIL TWIN? '%.14s' on CH%d+CH%d diff BSSID",
                    oaAps[i].ssid, oaAps[i].channel, oaAps[j].channel);
                oaAddFinding(SEV_CRITICAL, "EVIL TWIN", msg);
            }
            // Rule: ENCRYPTION DOWNGRADE — same SSID, different encryption
            if (oaAps[i].enc != oaAps[j].enc) {
                char msg[FINDING_MSG_LEN];
                snprintf(msg, sizeof(msg), "ENC MISMATCH: '%.14s' seen WPA+OPEN",
                    oaAps[i].ssid);
                oaAddFinding(SEV_CRITICAL, "ENC DNGRD", msg);
            }
        }
    }

    // Rule: DEAUTH FLOOD
    JsonArray deauths = doc["deauths"];
    for (JsonObject d : deauths) {
        long count = d["count"] | 0;
        if (count > 10) {
            char msg[FINDING_MSG_LEN];
            const char* src = d["src"] | "??:??:??:??:??:??";
            snprintf(msg, sizeof(msg), "DEAUTH FLOOD: %s sent %ld frames", src, count);
            oaAddFinding(count > 50 ? SEV_CRITICAL : SEV_WARNING, "DEAUTH FLD", msg);
        }
    }

    // Rule: PROBE PATTERN — sensitive SSIDs
    JsonArray probes = doc["probes"];
    for (JsonObject p : probes) {
        const char* ssid = p["ssid"] | "";
        if (oaIsSensitive(ssid)) {
            char msg[FINDING_MSG_LEN];
            const char* src = p["src"] | "??:??:??:??:??:??";
            snprintf(msg, sizeof(msg), "SENSITIVE PROBE: '%.16s' from %.11s", ssid, src);
            oaAddFinding(SEV_WARNING, "PROBE PTTRN", msg);
        }
    }

    if (findingCount == 0) {
        oaAddFinding(SEV_INFO, "CLEAN", "No anomalies detected in this session.");
    }
}

// ─────────────────────────────────────────────
//  ANALYSIS ENGINE — PACKET CSV
// ─────────────────────────────────────────────
// CSV format: uptime_ms,type,src_mac,rssi_dbm,channel
// We look for deauth floods and channel anomalies

static void oaAnalyzeCSV(const char* path) {
    FsFile f = sd.open(path, O_RDONLY);
    if (!f) { oaAddFinding(SEV_WARNING, "FILE", "Cannot open CSV"); return; }

    // Track deauth counts per source MAC
    struct { char mac[18]; uint32_t count; } deauthSrcs[32];
    int deauthSrcCount = 0;

    // Track frame type counts
    uint32_t totalFrames = 0, deauthTotal = 0, dataTotal = 0;

    // Skip header line
    char line[128];
    f.fgets(line, sizeof(line));

    while (f.fgets(line, sizeof(line))) {
        totalFrames++;
        // Parse: uptime_ms,type,src_mac,rssi_dbm,channel
        char* tok = strtok(line, ",");
        if (!tok) continue;
        tok = strtok(nullptr, ","); if (!tok) continue;  // type
        char type[16]; strncpy(type, tok, 15); type[15] = '\0';

        tok = strtok(nullptr, ","); if (!tok) continue;  // src_mac
        char mac[18]; strncpy(mac, tok, 17); mac[17] = '\0';

        bool isDeauth = (strcmp(type, "DEAUTH") == 0 || strcmp(type, "DISASSOC") == 0);
        if (isDeauth) {
            deauthTotal++;
            bool found = false;
            for (int i = 0; i < deauthSrcCount; i++) {
                if (strcmp(deauthSrcs[i].mac, mac) == 0) { deauthSrcs[i].count++; found = true; break; }
            }
            if (!found && deauthSrcCount < 32) {
                strncpy(deauthSrcs[deauthSrcCount].mac, mac, 17);
                deauthSrcs[deauthSrcCount].count = 1;
                deauthSrcCount++;
            }
        }
        if (strncmp(type, "DATA", 4) == 0 || strncmp(type, "QOS", 3) == 0) dataTotal++;

        yield();
    }
    f.close();

    // Evaluate deauth sources
    for (int i = 0; i < deauthSrcCount; i++) {
        if (deauthSrcs[i].count > 10) {
            char msg[FINDING_MSG_LEN];
            snprintf(msg, sizeof(msg), "DEAUTH FLOOD: %.11s sent %lu frames",
                deauthSrcs[i].mac, deauthSrcs[i].count);
            oaAddFinding(deauthSrcs[i].count > 50 ? SEV_CRITICAL : SEV_WARNING,
                         "DEAUTH FLD", msg);
        }
    }

    // Summary findings
    char msg[FINDING_MSG_LEN];
    snprintf(msg, sizeof(msg), "Total frames: %lu  Deauth: %lu  Data: %lu",
             totalFrames, deauthTotal, dataTotal);
    oaAddFinding(SEV_INFO, "SUMMARY", msg);

    float deauthRatio = totalFrames > 0 ? (float)deauthTotal / totalFrames : 0;
    if (deauthRatio > 0.05f) {
        snprintf(msg, sizeof(msg), "%.1f%% of frames are deauth/disassoc (>5%% is suspicious)",
                 deauthRatio * 100.0f);
        oaAddFinding(SEV_WARNING, "HIGH DEAUTH", msg);
    }

    if (findingCount == 0) {
        oaAddFinding(SEV_INFO, "CLEAN", "No anomalies detected in CSV session.");
    }
}

// ─────────────────────────────────────────────
//  SAVE ANALYSIS REPORT
// ─────────────────────────────────────────────
static void oaSaveReport(const char* sourceFile) {
    char fname[64];
    snprintf(fname, sizeof(fname), "%s/analysis_%010lu.txt", LOG_DIR, millis());
    FsFile f = sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return;

    f.printf("PISCES MOON OS — OFFLINE PACKET ANALYSIS\n");
    f.printf("Source: %s\n", sourceFile);
    f.printf("Findings: %d\n\n", findingCount);

    int crit = 0, warn = 0, info = 0;
    for (int i = 0; i < findingCount; i++) {
        if (findings[i].severity == SEV_CRITICAL) crit++;
        else if (findings[i].severity == SEV_WARNING) warn++;
        else info++;
        f.printf("[%s] %-12s %s\n",
            sevLabel(findings[i].severity), findings[i].rule, findings[i].msg);
    }
    f.printf("\nSummary: %d CRITICAL  %d WARNING  %d INFO\n", crit, warn, info);
    f.close();
    Serial.printf("[OA] Report: %s\n", fname);
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void oaDrawHeader(const char* sub) {
    gfx->fillRect(0, 0, 320, 24, OA_HDR);
    gfx->drawFastHLine(0, 23, 320, OA_CYAN);
    gfx->setTextSize(1);
    gfx->setTextColor(OA_CYAN);
    gfx->setCursor(6, 4);
    gfx->print("OFFLINE ANALYSIS");
    gfx->setTextColor(OA_DIM);
    gfx->setCursor(6, 14);
    gfx->print(sub);
    gfx->setTextColor(OA_DIM);
    gfx->setCursor(270, 8);
    gfx->print("[Q=EXIT]");
}

static void oaDrawFileList(int scroll, int selected) {
    gfx->fillRect(0, 26, 320, 182, OA_BG);
    gfx->setTextSize(1);

    if (oaFileCount == 0) {
        gfx->setTextColor(OA_DIM);
        gfx->setCursor(10, 80); gfx->print("No session logs found in /cyber_logs/");
        gfx->setCursor(10, 96); gfx->print("Run Beacon Spotter or PKT Sniffer first.");
        return;
    }

    gfx->setTextColor(OA_DIM);
    gfx->setCursor(4, 28);
    gfx->printf("%-28s  TYPE   SIZE", "FILENAME");
    gfx->drawFastHLine(0, 37, 320, 0x0018);

    int show = min(8, oaFileCount);
    for (int i = scroll; i < scroll + show && i < oaFileCount; i++) {
        int ry  = 40 + (i - scroll) * 21;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, 20, sel ? OA_SEL : (i%2==0 ? 0x0821 : OA_BG));

        // Filename (just basename)
        const char* slash = strrchr(oaFiles[i].name, '/');
        const char* fname = slash ? slash + 1 : oaFiles[i].name;
        gfx->setTextColor(sel ? OA_WHITE : 0xC618);
        gfx->setCursor(4, ry + 5);
        char nb[28]; strncpy(nb, fname, 27); nb[27] = '\0';
        gfx->print(nb);

        // Type
        gfx->setTextColor(sel ? OA_CYAN : OA_DIM);
        gfx->setCursor(228, ry + 5);
        gfx->print(oaFiles[i].isBeacon ? "JSON" : "CSV ");

        // Size
        gfx->setTextColor(sel ? OA_YELLOW : OA_DIM);
        gfx->setCursor(266, ry + 5);
        if (oaFiles[i].size < 1024) gfx->printf("%4luB", oaFiles[i].size);
        else gfx->printf("%3luK", oaFiles[i].size / 1024);
    }
}

static void oaDrawFindings(int scroll, int selected) {
    gfx->fillRect(0, 26, 320, 182, OA_BG);
    gfx->setTextSize(1);

    // Summary bar
    int crit = 0, warn = 0, info = 0;
    for (int i = 0; i < findingCount; i++) {
        if (findings[i].severity == SEV_CRITICAL) crit++;
        else if (findings[i].severity == SEV_WARNING) warn++;
        else info++;
    }
    gfx->setCursor(4, 28);
    gfx->setTextColor(OA_RED);    gfx->printf("CRIT:%d  ", crit);
    gfx->setTextColor(OA_YELLOW); gfx->printf("WARN:%d  ", warn);
    gfx->setTextColor(OA_CYAN);   gfx->printf("INFO:%d  ", info);
    gfx->setTextColor(OA_GREEN);  gfx->printf("TOTAL:%d", findingCount);
    gfx->drawFastHLine(0, 37, 320, 0x0018);

    int show = min(7, findingCount);
    for (int i = scroll; i < scroll + show && i < findingCount; i++) {
        OAFinding& fi = findings[i];
        int ry = 40 + (i - scroll) * 24;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, 23, sel ? OA_SEL : (i%2==0 ? 0x0821 : OA_BG));

        // Severity badge
        gfx->setTextColor(sevColor(fi.severity));
        gfx->setCursor(4, ry + 2);
        gfx->printf("%-4s", sevLabel(fi.severity));

        // Rule tag
        gfx->setTextColor(OA_DIM);
        gfx->setCursor(34, ry + 2);
        gfx->printf("%-12s", fi.rule);

        // Message (line 2)
        gfx->setTextColor(sel ? OA_WHITE : 0xC618);
        gfx->setCursor(4, ry + 13);
        char mb[50]; strncpy(mb, fi.msg, 49); mb[49] = '\0';
        gfx->print(mb);
    }
}

static void oaDrawStatus(const char* msg, uint16_t col = OA_DIM) {
    gfx->fillRect(0, 210, 320, 30, 0x0008);
    gfx->drawFastHLine(0, 210, 320, OA_CYAN);
    gfx->setTextSize(1);
    gfx->setTextColor(col);
    gfx->setCursor(4, 220);
    gfx->print(msg);
}

// ─────────────────────────────────────────────
//  PROGRESS BAR
// ─────────────────────────────────────────────
static void oaProgress(const char* label, int pct) {
    gfx->fillRect(10, 130, 300, 40, OA_BG);
    gfx->setTextColor(OA_DIM); gfx->setTextSize(1);
    gfx->setCursor(10, 130); gfx->print(label);
    gfx->fillRect(10, 145, 300, 12, 0x0821);
    gfx->fillRect(10, 145, (300 * pct) / 100, 12, OA_CYAN);
    gfx->drawRect(10, 145, 300, 12, OA_DIM);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_offline_pkt_analysis() {
    // Allocate working buffers in PSRAM
    findings = (OAFinding*)ps_malloc(MAX_FINDINGS * sizeof(OAFinding));
    oaFiles  = (OAFile*)ps_malloc(MAX_LOG_FILES  * sizeof(OAFile));
    oaAps    = (OAApEntry*)ps_malloc(64 * sizeof(OAApEntry));
    if (!findings || !oaFiles || !oaAps) {
        gfx->fillScreen(OA_BG);
        oaDrawHeader("OUT OF PSRAM");
        gfx->setTextColor(OA_RED); gfx->setTextSize(1);
        gfx->setCursor(6, 50); gfx->print("Cannot allocate analysis buffers.");
        gfx->setTextColor(OA_DIM); gfx->setCursor(6, 70);
        gfx->print("Tap or Q to exit.");
        while (true) {
            if (get_keypress()) break;
            int16_t tx, ty; if (get_touch(&tx, &ty)) break;
            delay(50);
        }
        free(findings); free(oaFiles); free(oaAps);
        findings = nullptr; oaFiles = nullptr; oaAps = nullptr;
        return;
    }
    findingCount = 0; oaFileCount = 0; oaApCount = 0;

    gfx->fillScreen(OA_BG);
    oaDrawHeader("Scanning /cyber_logs/...");
    gfx->setTextColor(OA_DIM); gfx->setTextSize(1);
    gfx->setCursor(6, 50); gfx->print("Loading session file index...");

    oaScanFiles();

    if (oaFileCount == 0) {
        gfx->fillRect(6, 50, 300, 12, OA_BG);
        gfx->setTextColor(OA_ORANGE);
        gfx->setCursor(6, 50); gfx->print("No logs found. Run Beacon/PKT Sniffer first.");
        delay(3000);
        return;
    }

    int scroll = 0, selected = 0;
    bool inResults = false;
    char currentFile[48] = "";

    gfx->fillScreen(OA_BG);
    oaDrawHeader("Select session log to analyze");
    oaDrawFileList(scroll, selected);
    oaDrawStatus("CLICK=analyze  BALL=scroll  Q=exit", OA_DIM);

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (inResults) {
                inResults = false;
                gfx->fillScreen(OA_BG);
                oaDrawHeader("Select session log to analyze");
                oaDrawFileList(scroll, selected);
                oaDrawStatus("CLICK=analyze  BALL=scroll  Q=exit", OA_DIM);
            } else break;
            continue;
        }

        if (k == 'q' || k == 'Q') {
            if (inResults) {
                inResults = false;
                gfx->fillScreen(OA_BG);
                oaDrawHeader("Select session log to analyze");
                oaDrawFileList(scroll, selected);
                oaDrawStatus("CLICK=analyze  BALL=scroll  Q=exit", OA_DIM);
            } else break;
            continue;
        }

        if (!inResults) {
            // Scroll file list
            int maxScroll = max(0, oaFileCount - 8);
            if (tb.y == -1 && scroll > 0)         { scroll--; if (selected > 0) selected--; }
            if (tb.y ==  1 && scroll < maxScroll)  { scroll++; if (selected < oaFileCount-1) selected++; }

            // Select and analyze
            if ((tb.clicked || k == 13) && oaFileCount > 0) {
                strncpy(currentFile, oaFiles[selected].name, 47);
                findingCount = 0;

                // Progress screen
                gfx->fillScreen(OA_BG);
                oaDrawHeader("Analyzing...");
                gfx->setTextColor(OA_WHITE); gfx->setTextSize(1);
                gfx->setCursor(6, 50);
                const char* slash = strrchr(currentFile, '/');
                gfx->print(slash ? slash + 1 : currentFile);

                oaProgress("Parsing session data...", 20);

                if (oaFiles[selected].isBeacon) {
                    oaProgress("Analyzing beacon data...", 40);
                    oaAnalyzeBeacon(currentFile);
                } else {
                    oaProgress("Analyzing packet CSV...", 40);
                    oaAnalyzeCSV(currentFile);
                }

                oaProgress("Saving report...", 80);
                oaSaveReport(currentFile);
                oaProgress("Complete.", 100);
                delay(500);

                // Show findings
                inResults = true;
                scroll = 0; selected = 0;
                gfx->fillScreen(OA_BG);

                char subTitle[48];
                const char* slash2 = strrchr(currentFile, '/');
                snprintf(subTitle, sizeof(subTitle), "%.28s", slash2 ? slash2+1 : currentFile);
                oaDrawHeader(subTitle);
                oaDrawFindings(scroll, selected);

                int crit = 0;
                for (int i = 0; i < findingCount; i++)
                    if (findings[i].severity == SEV_CRITICAL) crit++;

                if (crit > 0) {
                    oaDrawStatus("! CRITICAL findings detected. Report saved.", OA_RED);
                } else {
                    oaDrawStatus("Analysis complete. Report saved to /cyber_logs/", OA_GREEN);
                }
            }

            if (!inResults) oaDrawFileList(scroll, selected);

        } else {
            // Browse findings
            int maxScroll = max(0, findingCount - 7);
            if (tb.y == -1 && scroll > 0)         { scroll--; if (selected > 0) selected--; }
            if (tb.y ==  1 && scroll < maxScroll)  { scroll++; if (selected < findingCount-1) selected++; }
            oaDrawFindings(scroll, selected);
        }

        delay(20); yield();
    }

    free(findings); findings = nullptr;
    free(oaFiles);  oaFiles  = nullptr;
    free(oaAps);    oaAps    = nullptr;
    gfx->fillScreen(OA_BG);
}
