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
 * PISCES MOON OS — TRAIL DATABASE v1.0
 * Offline hiking trail reference with live WiFi fetch
 *
 * Storage: /data/trails/ on SD card (same NoSQL pattern as medical/baseball)
 *   index.json       ← {entries:[{id,name,region,difficulty},...]}
 *   entry_NNN.json   ← full trail card
 *
 * Trail card JSON schema:
 *   name, region, state, difficulty (Easy/Moderate/Hard/Expert)
 *   distance_miles, elevation_gain_ft, high_point_ft
 *   trailhead, permit_required, dogs_allowed, season
 *   description     ← 2-3 sentence overview
 *   waypoints[]     ← key landmarks with mile markers
 *   hazards         ← weather, wildlife, terrain notes
 *   emergency       ← nearest town, ranger station, cell coverage
 *   last_updated    ← date string
 *
 * Fetch modes (same as baseball):
 *   F key / +FETCH button → Gemini lookup by trail name + region
 *   Gemini returns structured JSON matching our schema
 *
 * Controls:
 *   Keyboard         = search/type
 *   Trackball up/dn  = scroll
 *   Trackball click  = open trail / back
 *   F key / +FETCH   = fetch new trail via Gemini
 *   Q / header tap   = exit
 */

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include <ArduinoJson.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "trails.h"
#include "gemini_client.h"

extern Arduino_GFX *gfx;
extern SdFat sd;
extern volatile bool wifi_in_use;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG       0x0000
#define COL_HDR      0x0340   // Trail green
#define COL_ACCENT   0x07E0   // Bright green
#define COL_NAME     0xFFE0   // Gold
#define COL_LABEL    0x07FF   // Cyan
#define COL_VALUE    0xFFFF
#define COL_EASY     0x07E0   // Green
#define COL_MOD      0xFFE0   // Yellow
#define COL_HARD     0xFD20   // Orange
#define COL_EXPERT   0xF800   // Red
#define COL_HAZARD   0xF800
#define COL_DIM      0x4208
#define COL_SEL      0x001F

#define DB_PATH      "/data/trails"
#define MAX_RESULTS  40
#define RESULTS_PER_PAGE 7

// ─────────────────────────────────────────────
//  INDEX
// ─────────────────────────────────────────────
struct TrailEntry {
    char id[16];
    char name[48];
    char region[32];
    char difficulty[10];
};

static TrailEntry* results   = nullptr;
static int         resCount  = 0;
static int         selResult = 0;
static int         resScroll = 0;

static uint16_t diffColor(const char* d) {
    if (strcasecmp(d, "Easy")     == 0) return COL_EASY;
    if (strcasecmp(d, "Moderate") == 0) return COL_MOD;
    if (strcasecmp(d, "Hard")     == 0) return COL_HARD;
    return COL_EXPERT;
}

static int loadTrailIndex(const char* query) {
    char idxPath[64];
    snprintf(idxPath, sizeof(idxPath), "%s/index.json", DB_PATH);
    if (!sd.exists(idxPath)) return 0;

    FsFile f = sd.open(idxPath, O_READ);
    if (!f) return 0;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return 0;

    if (!results) results = new TrailEntry[MAX_RESULTS];
    int count = 0;
    String q = String(query); q.toLowerCase();

    for (JsonObject e : doc["entries"].as<JsonArray>()) {
        if (count >= MAX_RESULTS) break;
        String n = e["name"] | ""; String nl = n; nl.toLowerCase();
        String r = e["region"] | ""; String rl = r; rl.toLowerCase();
        if (q.length() == 0 || nl.indexOf(q) >= 0 || rl.indexOf(q) >= 0) {
            strncpy(results[count].id,         (e["id"]         | ""), 15);
            strncpy(results[count].name,        n.c_str(),               47);
            strncpy(results[count].region,      r.c_str(),               31);
            strncpy(results[count].difficulty,  (e["difficulty"] | ""), 9);
            count++;
        }
    }
    return count;
}

// ─────────────────────────────────────────────
//  HEADER
// ─────────────────────────────────────────────
static void drawTrailHeader(const char* title, bool showFetch = false) {
    gfx->fillRect(0, 0, 320, 24, COL_HDR);
    gfx->drawFastHLine(0, 23, 320, COL_ACCENT);
    gfx->setTextSize(1); gfx->setTextColor(COL_NAME);
    gfx->setCursor(8, 8); gfx->print(title);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(252, 8); gfx->print("[TAP EXIT]");
    if (showFetch) {
        gfx->drawRect(192, 2, 56, 19, COL_ACCENT);
        gfx->fillRect(193, 3, 54, 17, 0x0020);
        gfx->setTextColor(COL_ACCENT);
        gfx->setCursor(198, 8); gfx->print("+FETCH");
    }
}

// ─────────────────────────────────────────────
//  SEARCH SCREEN
// ─────────────────────────────────────────────
static void drawSearchScreen(const char* query, int count, int sel, int scroll) {
    gfx->fillScreen(COL_BG);
    drawTrailHeader("TRAIL DATABASE", true);

    // Search box
    gfx->fillRect(5, 28, 310, 18, 0x0820);
    gfx->drawRect(5, 28, 310, 18, COL_ACCENT);
    gfx->setTextSize(1);
    gfx->setTextColor(strlen(query) > 0 ? COL_VALUE : COL_DIM);
    gfx->setCursor(10, 33);
    if (strlen(query) > 0) { gfx->print(query); gfx->print("_"); }
    else gfx->print("Type trail name or region...");

    // Count / hint
    gfx->setTextColor(COL_DIM); gfx->setCursor(10, 50);
    if (count == 0) gfx->print("No trails. Press F or +FETCH to add.");
    else gfx->printf("%d trail%s  |  F=fetch new trail", count, count==1?"":"s");

    // Results list
    int end = min(scroll + RESULTS_PER_PAGE, count);
    for (int i = scroll; i < end; i++) {
        int y = 62 + (i - scroll) * 24;
        bool s = (i == sel);
        gfx->fillRect(5, y, 310, 22, s ? 0x0820 : COL_BG);
        gfx->drawRect(5, y, 310, 22, s ? COL_ACCENT : 0x2104);

        gfx->setTextColor(s ? COL_NAME : COL_VALUE);
        gfx->setCursor(12, y+4);
        char nm[28]; strncpy(nm, results[i].name, 27); nm[27]='\0';
        gfx->print(nm);

        // Difficulty badge
        gfx->setTextColor(diffColor(results[i].difficulty));
        gfx->setCursor(200, y+4);
        char diff[4]; strncpy(diff, results[i].difficulty, 3); diff[3]='\0';
        gfx->print(diff);

        // Region
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(230, y+4);
        char reg[10]; strncpy(reg, results[i].region, 9); reg[9]='\0';
        gfx->print(reg);
    }
}

// ─────────────────────────────────────────────
//  TRAIL CARD
// ─────────────────────────────────────────────
static void drawTrailCard(const char* entryId, int scrollY) {
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.json", DB_PATH, entryId);
    if (!sd.exists(path)) {
        gfx->fillScreen(COL_BG); drawTrailHeader("NOT FOUND");
        gfx->setTextColor(COL_HAZARD); gfx->setCursor(10, 80);
        gfx->print("Trail card missing from SD.");
        return;
    }
    FsFile f = sd.open(path, O_READ);
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();

    gfx->fillScreen(COL_BG);
    drawTrailHeader("TRAIL CARD");

    int y = 28 - scrollY;

    // Name plate
    { if (y+28>=28 && y<240) {
        gfx->fillRect(0, y, 320, 26, 0x0820);
        gfx->setTextSize(2); gfx->setTextColor(COL_NAME);
        gfx->setCursor(8, y+5);
        // Truncate long names
        String n = doc["name"] | "Unknown Trail";
        if (n.length() > 18) n = n.substring(0,17) + "~";
        gfx->print(n);
    } y += 28; }

    // Region / State / Difficulty
    { if (y+12>=28 && y<240) {
        gfx->setTextSize(1);
        gfx->setTextColor(COL_LABEL); gfx->setCursor(8, y+2);
        gfx->printf("%s, %s",
            (const char*)(doc["region"] | ""),
            (const char*)(doc["state"]  | ""));
        const char* diff = doc["difficulty"] | "?";
        gfx->setTextColor(diffColor(diff));
        gfx->setCursor(240, y+2); gfx->print(diff);
    } y += 13; }

    // Stats row: distance / gain / high point
    { if (y+12>=28 && y<240) {
        gfx->setTextSize(1); gfx->setTextColor(COL_ACCENT);
        gfx->setCursor(8, y+2);
        gfx->printf("%.1fmi  +%dft  Hi:%dft",
            (float)(doc["distance_miles"] | 0.0f),
            (int)(doc["elevation_gain_ft"] | 0),
            (int)(doc["high_point_ft"]     | 0));
    } y += 13; }

    // Season / permits / dogs
    { if (y+12>=28 && y<240) {
        gfx->setTextSize(1); gfx->setTextColor(COL_DIM);
        gfx->setCursor(8, y+2);
        bool permit = doc["permit_required"] | false;
        bool dogs   = doc["dogs_allowed"]    | false;
        gfx->printf("Season:%s  %s  %s",
            (const char*)(doc["season"] | "Year-round"),
            permit ? "PERMIT REQ" : "No permit",
            dogs ? "Dogs OK" : "No dogs");
    } y += 13; }

    // Trailhead
    { if (y+12>=28 && y<240) {
        gfx->setTextColor(COL_DIM); gfx->setCursor(8, y+2);
        gfx->printf("TH: %s", (const char*)(doc["trailhead"] | "See map"));
    } y += 13; }

    // Divider
    { if (y+4>=28 && y<240) gfx->drawFastHLine(0, y+2, 320, COL_ACCENT); y += 8; }

    // Description
    { if (y+12>=28 && y<240) {
        gfx->setTextColor(COL_LABEL); gfx->setCursor(8, y+2);
        gfx->print("OVERVIEW:"); } y += 12;
      String desc = doc["description"] | "";
      while (desc.length() > 0) {
        int cut = min((int)desc.length(), 52);
        if (cut < (int)desc.length()) { int sp = desc.lastIndexOf(' ',cut); if(sp>0) cut=sp; }
        String line = desc.substring(0, cut);
        desc = desc.substring(cut); if (desc.startsWith(" ")) desc=desc.substring(1);
        if (y+11>=28 && y<240) { gfx->setTextColor(COL_VALUE); gfx->setCursor(8,y+1); gfx->print(line); }
        y += 11;
      }
    }

    // Waypoints
    { if (y+4>=28 && y<240) gfx->drawFastHLine(0, y+2, 320, 0x2104); y += 8;
      if (y+12>=28 && y<240) {
        gfx->setTextColor(COL_LABEL); gfx->setCursor(8, y+2); gfx->print("WAYPOINTS:");
      } y += 12;
      JsonArray wps = doc["waypoints"].as<JsonArray>();
      for (const char* wp : wps) {
        if (y+11>=28 && y<240) {
            gfx->setTextColor(COL_MOD); gfx->setCursor(8, y+1);
            char buf[53]; strncpy(buf, wp, 52); buf[52]='\0'; gfx->print(buf);
        }
        y += 11;
      }
    }

    // Hazards
    { if (y+4>=28 && y<240) gfx->drawFastHLine(0, y+2, 320, 0x2104); y += 8;
      if (y+12>=28 && y<240) {
        gfx->setTextColor(COL_HAZARD); gfx->setCursor(8, y+2); gfx->print("HAZARDS:");
      } y += 12;
      String haz = doc["hazards"] | "";
      while (haz.length() > 0) {
        int cut = min((int)haz.length(), 52);
        if (cut < (int)haz.length()) { int sp = haz.lastIndexOf(' ',cut); if(sp>0) cut=sp; }
        String line = haz.substring(0, cut);
        haz = haz.substring(cut); if (haz.startsWith(" ")) haz=haz.substring(1);
        if (y+11>=28 && y<240) { gfx->setTextColor(0xFD20); gfx->setCursor(8,y+1); gfx->print(line); }
        y += 11;
      }
    }

    // Emergency
    { if (y+4>=28 && y<240) gfx->drawFastHLine(0, y+2, 320, 0x2104); y += 8;
      if (y+12>=28 && y<240) {
        gfx->setTextColor(COL_HAZARD); gfx->setCursor(8, y+2); gfx->print("EMERGENCY:");
      } y += 12;
      if (y+11>=28 && y<240) {
        gfx->setTextColor(COL_VALUE); gfx->setCursor(8, y+1);
        char buf[53]; strncpy(buf, doc["emergency"] | "See park service", 52); buf[52]='\0';
        gfx->print(buf);
      } y += 11;
    }

    // Scroll indicator
    if (y - 28 + scrollY > 212) {
        gfx->setTextColor(COL_DIM); gfx->setCursor(265, 230);
        gfx->printf("^%d", scrollY);
    }
}

// ─────────────────────────────────────────────
//  GEMINI FETCH
// ─────────────────────────────────────────────
static int getNextTrailId() {
    int maxNum = 0;
    if (sd.exists(DB_PATH)) {
        FsFile dir = sd.open(DB_PATH);
        if (dir) {
            FsFile f;
            while (f.openNext(&dir, O_READ)) {
                char name[32]; f.getName(name, sizeof(name)); f.close();
                if (strncmp(name, "entry_", 6) == 0) {
                    int n = atoi(name + 6); if (n > maxNum) maxNum = n;
                }
            }
            dir.close();
        }
    }
    return maxNum + 1;
}

static bool updateTrailIndex(const char* id, const char* name,
                              const char* region, const char* diff) {
    char idxPath[64]; snprintf(idxPath, sizeof(idxPath), "%s/index.json", DB_PATH);
    JsonDocument doc;
    if (sd.exists(idxPath)) {
        FsFile f = sd.open(idxPath, O_READ);
        if (f) { deserializeJson(doc, f); f.close(); }
    }
    if (doc["entries"].isNull()) {
        doc.to<JsonObject>();
        doc["category"] = "trails"; doc["version"] = "1.0";
        doc["entries"].to<JsonArray>();
    }
    JsonArray arr = doc["entries"].as<JsonArray>();
    for (JsonObject e : arr) {
        if (strcmp(e["id"] | "", id) == 0) {
            e["name"]=name; e["region"]=region; e["difficulty"]=diff;
            FsFile f = sd.open(idxPath, O_WRITE|O_CREAT|O_TRUNC);
            if (!f) return false; serializeJson(doc,f); f.close(); return true;
        }
    }
    JsonObject ne = arr.add<JsonObject>();
    ne["id"]=id; ne["name"]=name; ne["region"]=region; ne["difficulty"]=diff;
    doc["count"] = arr.size();
    if (!sd.exists(DB_PATH)) sd.mkdir(DB_PATH);
    FsFile f = sd.open(idxPath, O_WRITE|O_CREAT|O_TRUNC);
    if (!f) return false; serializeJson(doc,f); f.close(); return true;
}

static void runTrailFetch() {
    if (WiFi.status() != WL_CONNECTED) {
        gfx->fillScreen(COL_BG); drawTrailHeader("FETCH TRAIL");
        gfx->setTextColor(COL_HAZARD); gfx->setCursor(10, 80);
        gfx->print("WiFi not connected.");
        gfx->setTextColor(COL_DIM); gfx->setCursor(10, 96);
        gfx->print("Connect via WIFI JOIN first.");
        delay(2500); return;
    }

    gfx->fillScreen(COL_BG); drawTrailHeader("FETCH TRAIL");
    gfx->setTextColor(COL_LABEL); gfx->setCursor(8, 32);
    gfx->print("Trail name (e.g. 'Half Dome Yosemite'):");
    gfx->fillRect(5, 44, 310, 16, 0x0820);
    gfx->drawRect(5, 44, 310, 16, COL_ACCENT);

    char query[64] = "";
    bool fetching = false;
    int statusY = 70;

    auto status = [&](const char* msg, uint16_t col = COL_VALUE) {
        gfx->fillRect(0, statusY, 320, 12, COL_BG);
        gfx->setTextColor(col); gfx->setCursor(8, statusY); gfx->print(msg);
        statusY += 13; if (statusY > 220) statusY = 70;
    };

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) { while(get_touch(&tx,&ty)){delay(10);} break; }
        if (k == 'q' || k == 'Q') break;

        bool changed = false;
        if (!fetching) {
            if (k >= 32 && k <= 126 && k != 'q' && k != 'Q') {
                int ql = strlen(query);
                if (ql < 62) { query[ql] = k; query[ql+1] = '\0'; changed = true; }
            } else if ((k == 8 || k == 127) && strlen(query) > 0) {
                query[strlen(query)-1] = '\0'; changed = true;
            } else if ((k == 13 || tb.clicked) && strlen(query) > 0) {
                fetching = true;
                status("Asking Gemini for trail data...", COL_ACCENT);

                String prompt =
                    String("Return a JSON trail card for hiking trail: ") + query +
                    "\n\nReturn ONLY valid JSON, no markdown. Schema:\n"
                    "{\n"
                    "  \"name\": \"Trail Name\",\n"
                    "  \"region\": \"Park or Area Name\",\n"
                    "  \"state\": \"CA\",\n"
                    "  \"difficulty\": \"Easy|Moderate|Hard|Expert\",\n"
                    "  \"distance_miles\": 8.2,\n"
                    "  \"elevation_gain_ft\": 2800,\n"
                    "  \"high_point_ft\": 8842,\n"
                    "  \"trailhead\": \"Happy Isles Trailhead\",\n"
                    "  \"permit_required\": true,\n"
                    "  \"dogs_allowed\": false,\n"
                    "  \"season\": \"May-Oct\",\n"
                    "  \"description\": \"2-3 sentence overview.\",\n"
                    "  \"waypoints\": [\"0.0mi: Trailhead\",\"3.2mi: Landmark\"],\n"
                    "  \"hazards\": \"Key hazards as a single string.\",\n"
                    "  \"emergency\": \"Nearest ranger station and town.\",\n"
                    "  \"last_updated\": \"2026\"\n"
                    "}\n"
                    "Use real data. Return ONLY the JSON object.";

                String resp = ask_gemini(prompt);

                if (resp.startsWith("ERROR:")) {
                    status(resp.c_str(), COL_HAZARD);
                    fetching = false;
                    delay(2000); continue;
                }

                // Strip markdown fences
                resp.trim();
                if (resp.startsWith("```")) {
                    int s = resp.indexOf('\n')+1;
                    int e2 = resp.lastIndexOf("```");
                    if (e2 > s) resp = resp.substring(s, e2);
                    resp.trim();
                }
                int js = resp.indexOf('{'), je = resp.lastIndexOf('}');
                if (js < 0 || je <= js) { status("No JSON in response", COL_HAZARD); fetching=false; delay(2000); continue; }
                String jsonStr = resp.substring(js, je+1);

                JsonDocument card;
                if (deserializeJson(card, jsonStr)) {
                    status("JSON parse error", COL_HAZARD); fetching=false; delay(2000); continue;
                }

                int num = getNextTrailId();
                char entryId[16]; snprintf(entryId, sizeof(entryId), "entry_%03d", num);
                card["id"] = entryId;

                if (!sd.exists(DB_PATH)) sd.mkdir(DB_PATH);
                char path[80]; snprintf(path, sizeof(path), "%s/%s.json", DB_PATH, entryId);
                FsFile f = sd.open(path, O_WRITE|O_CREAT|O_TRUNC);
                if (!f) { status("SD write failed", COL_HAZARD); fetching=false; delay(2000); continue; }
                serializeJson(card, f); f.close();

                const char* nm   = card["name"]       | query;
                const char* reg  = card["region"]     | "Unknown";
                const char* diff = card["difficulty"]  | "?";
                updateTrailIndex(entryId, nm, reg, diff);

                char msg[48]; snprintf(msg, 48, "Saved: %s", nm);
                status(msg, COL_EASY);
                delay(1500);
                break;
            }
        }

        if (changed) {
            gfx->fillRect(6, 45, 308, 14, 0x0820);
            gfx->setTextColor(COL_VALUE); gfx->setCursor(10, 48);
            gfx->print(query); if (strlen(query)) gfx->print("_");
        }
        delay(40); yield();
    }
    gfx->fillScreen(COL_BG);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_trails() {
    if (!results) results = new TrailEntry[MAX_RESULTS];
    char query[32] = "";
    resCount = loadTrailIndex(query);
    selResult = resScroll = 0;
    drawSearchScreen(query, resCount, selResult, resScroll);

    bool inCard = false;
    int  cardScroll = 0;

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (inCard) {
                inCard=false; cardScroll=0;
                drawSearchScreen(query,resCount,selResult,resScroll);
            } else if (tx >= 192 && tx <= 248) {
                runTrailFetch();
                resCount = loadTrailIndex(query); selResult=resScroll=0;
                drawSearchScreen(query,resCount,selResult,resScroll);
            } else break;
            continue;
        }
        if (k == 'q' || k == 'Q') {
            if (inCard) { inCard=false; cardScroll=0; drawSearchScreen(query,resCount,selResult,resScroll); }
            else break;
            continue;
        }

        if (inCard) {
            bool ch = false;
            if (tb.y==-1 && cardScroll>0) { cardScroll-=11; if(cardScroll<0)cardScroll=0; ch=true; }
            if (tb.y== 1) { cardScroll+=11; ch=true; }
            if (tb.clicked || k==13) { inCard=false; cardScroll=0; drawSearchScreen(query,resCount,selResult,resScroll); }
            if (ch) drawTrailCard(results[selResult].id, cardScroll);
        } else {
            if (k=='f'||k=='F') {
                runTrailFetch();
                resCount=loadTrailIndex(query); selResult=resScroll=0;
                drawSearchScreen(query,resCount,selResult,resScroll);
                continue;
            }
            bool qch = false;
            if (k>='a'&&k<='z') { int l=strlen(query); if(l<30){query[l]=k;query[l+1]='\0';qch=true;} }
            else if (k>='A'&&k<='Z') { int l=strlen(query); if(l<30){query[l]=k-'A'+'a';query[l+1]='\0';qch=true;} }
            else if ((k==8||k==127)&&strlen(query)>0) { query[strlen(query)-1]='\0'; qch=true; }
            if (qch) { resCount=loadTrailIndex(query); selResult=resScroll=0; drawSearchScreen(query,resCount,selResult,resScroll); }

            bool nav=false;
            if (tb.y==-1&&selResult>0) { selResult--; if(selResult<resScroll)resScroll--; nav=true; }
            if (tb.y== 1&&selResult<resCount-1) { selResult++; if(selResult>=resScroll+RESULTS_PER_PAGE)resScroll++; nav=true; }
            if (nav) drawSearchScreen(query,resCount,selResult,resScroll);
            if ((tb.clicked||k==13)&&resCount>0) { inCard=true; cardScroll=0; drawTrailCard(results[selResult].id,cardScroll); }
        }
        delay(40); yield();
    }

    if (results) { delete[] results; results=nullptr; }
    gfx->fillScreen(COL_BG);
}