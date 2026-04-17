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
 * PISCES MOON OS — BASEBALL FETCHER v1.1
 * On-device player data retrieval via WiFi
 *
 * Two fetch modes:
 *
 * MODE 1 — MLB Stats API (current/active players, free, no key)
 *   Endpoint: https://statsapi.mlb.com/api/v1/people/search?names=<n>
 *   Returns: player ID, then fetches full career stats + bio
 *   Best for: active roster players, real-time season stats
 *
 * MODE 2 — Gemini Proxy (historical/retired players)
 *   Sends structured prompt to pisces-proxy VM asking Gemini to
 *   return a player card in our exact JSON schema.
 *   Best for: Babe Ruth, Ted Williams, anyone not in active MLB API
 *
 * Both modes write directly to /data/baseball/ on the SD card
 * and update index.json so the player appears in search immediately.
 *
 * v1.1: Removed WiFi.setSleep(false) — crashes ESP32-S3 when BT is active.
 *
 * Controls (fetch screen):
 *   Keyboard    = type player name
 *   Tab / F key = toggle mode (MLB API / Gemini lookup)
 *   Enter/Click = fetch
 *   Q / header  = cancel
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
#include "baseball_fetch.h"
#include "gemini_client.h"   // For ask_gemini() — historical player lookup

extern Arduino_GFX *gfx;
extern SdFat sd;
extern volatile bool wifi_in_use;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG      0x0000
#define COL_HDR     0x9800
#define COL_TEXT    0xFFFF
#define COL_LABEL   0x07FF
#define COL_VALUE   0xFFE0
#define COL_OK      0x07E0
#define COL_ERR     0xF800
#define COL_DIM     0x4208
#define COL_MODE_A  0x07FF   // MLB API mode color
#define COL_MODE_G  0xFD20   // Gemini mode color

#define DB_PATH     "/data/baseball"

// ─────────────────────────────────────────────
//  STATUS DISPLAY
// ─────────────────────────────────────────────
static int statusY = 80;

static void fetchStatus(const char* msg, uint16_t col = 0xFFFF) {
    gfx->fillRect(0, statusY, 320, 12, COL_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(col);
    gfx->setCursor(8, statusY);
    gfx->print(msg);
    statusY += 13;
    if (statusY > 220) statusY = 80; // Wrap
    Serial.printf("[BASEBALL FETCH] %s\n", msg);
}

static void fetchClearStatus() {
    gfx->fillRect(0, 80, 320, 160, COL_BG);
    statusY = 80;
}

// ─────────────────────────────────────────────
//  GET NEXT ENTRY ID
// ─────────────────────────────────────────────
static void getNextEntryId(char* out, int outLen) {
    int maxNum = 0;
    if (sd.exists(DB_PATH)) {
        FsFile dir = sd.open(DB_PATH);
        if (dir) {
            FsFile f;
            while (f.openNext(&dir, O_READ)) {
                char name[32];
                f.getName(name, sizeof(name));
                f.close();
                if (strncmp(name, "entry_", 6) == 0) {
                    int n = atoi(name + 6);
                    if (n > maxNum) maxNum = n;
                }
            }
            dir.close();
        }
    }
    snprintf(out, outLen, "entry_%03d", maxNum + 1);
}

// ─────────────────────────────────────────────
//  UPDATE INDEX.JSON
// ─────────────────────────────────────────────
static bool updateIndex(const char* id, const char* name,
                        const char* pos, const char* years) {
    char idxPath[64];
    snprintf(idxPath, sizeof(idxPath), "%s/index.json", DB_PATH);

    JsonDocument doc;
    if (sd.exists(idxPath)) {
        FsFile f = sd.open(idxPath, O_READ);
        if (f) { deserializeJson(doc, f); f.close(); }
    }

    if (doc["entries"].isNull()) {
        doc.to<JsonObject>();
        doc["category"] = "baseball";
        doc["version"]  = "1.0";
        doc["entries"].to<JsonArray>();
    }

    JsonArray arr = doc["entries"].as<JsonArray>();
    for (JsonObject e : arr) {
        if (strcmp(e["id"] | "", id) == 0) {
            e["name"]  = name;
            e["pos"]   = pos;
            e["years"] = years;
            FsFile f = sd.open(idxPath, O_WRITE | O_CREAT | O_TRUNC);
            if (!f) return false;
            serializeJson(doc, f); f.close();
            return true;
        }
    }

    JsonObject newEntry = arr.add<JsonObject>();
    newEntry["id"]    = id;
    newEntry["name"]  = name;
    newEntry["pos"]   = pos;
    newEntry["years"] = years;
    doc["count"]      = arr.size();

    if (!sd.exists(DB_PATH)) sd.mkdir(DB_PATH);
    FsFile f = sd.open(idxPath, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

// ─────────────────────────────────────────────
//  SAVE PLAYER CARD TO SD
// ─────────────────────────────────────────────
static bool savePlayerCard(const char* id, JsonDocument& doc) {
    if (!sd.exists(DB_PATH)) sd.mkdir(DB_PATH);
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.json", DB_PATH, id);
    FsFile f = sd.open(path, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    return true;
}

// ─────────────────────────────────────────────
//  MLB STATS API FETCH
// ─────────────────────────────────────────────
static bool fetchFromMLBApi(const char* playerName, const char* entryId) {
    fetchStatus("Searching MLB API...", COL_MODE_A);

    // NOTE: Do NOT call WiFi.setSleep(false) — crashes ESP32-S3 when BT active
    wifi_in_use = true;

    // ── Step 1: Player search ──
    HTTPClient http;
    char searchUrl[256];
    String nameEnc = String(playerName);
    nameEnc.replace(" ", "%20");
    snprintf(searchUrl, sizeof(searchUrl),
        "http://statsapi.mlb.com/api/v1/people/search?names=%s&sportId=1",
        nameEnc.c_str());

    http.begin(searchUrl);
    http.setTimeout(15000);
    int code = http.GET();
    String raw = http.getString();
    http.end();

    if (code != 200) {
        char msg[48]; snprintf(msg, 48, "Search failed: HTTP %d", code);
        fetchStatus(msg, COL_ERR);
        wifi_in_use = false;
        return false;
    }

    JsonDocument searchDoc;
    if (deserializeJson(searchDoc, raw)) {
        fetchStatus("Search parse error", COL_ERR);
        wifi_in_use = false;
        return false;
    }

    JsonArray people = searchDoc["people"].as<JsonArray>();
    if (people.size() == 0) {
        fetchStatus("Player not found in MLB API.", COL_ERR);
        fetchStatus("Try Gemini mode for retired players.", COL_DIM);
        wifi_in_use = false;
        return false;
    }

    // Take first result
    JsonObject person = people[0];
    int mlbId   = person["id"] | 0;
    String name = person["fullName"] | playerName;

    char idMsg[48]; snprintf(idMsg, 48, "Found: %s (ID %d)", name.c_str(), mlbId);
    fetchStatus(idMsg, COL_OK);
    fetchStatus("Fetching career stats...", COL_MODE_A);

    // ── Step 2: Full player details + career stats ──
    char detailUrl[256];
    snprintf(detailUrl, sizeof(detailUrl),
        "http://statsapi.mlb.com/api/v1/people/%d"
        "?hydrate=stats(group=[hitting,pitching],type=career),"
        "currentTeam,team,education,draft",
        mlbId);

    http.begin(detailUrl);
    http.setTimeout(20000);
    code = http.GET();
    raw = http.getString();
    http.end();

    wifi_in_use = false;

    if (code != 200) {
        char msg[48]; snprintf(msg, 48, "Detail fetch failed: HTTP %d", code);
        fetchStatus(msg, COL_ERR);
        return false;
    }

    JsonDocument detDoc;
    DeserializationError err = deserializeJson(detDoc, raw);
    if (err) {
        fetchStatus("Detail parse error (response too large?)", COL_ERR);
        return false;
    }

    fetchStatus("Parsing stats...", COL_MODE_A);

    JsonObject p = detDoc["people"][0].as<JsonObject>();

    // ── Build our player card schema ──
    JsonDocument card;
    card["id"]          = entryId;
    card["name"]        = p["fullName"] | name.c_str();
    card["position"]    = p["primaryPosition"]["abbreviation"] | "?";
    card["bats"]        = p["batSide"]["code"] | "?";
    card["throws"]      = p["pitchHand"]["code"] | "?";
    card["born"]        = p["birthDate"] | "";
    card["birthplace"]  = String(p["birthCity"] | "") + ", " +
                          String(p["birthStateProvince"] | "") +
                          String(p["birthCountry"] | "");
    card["debut"]       = p["mlbDebutDate"] | "";
    card["retired"]     = p["active"].as<bool>() ? "Active" : (p["lastPlayedDate"] | "");

    JsonArray teams = card["teams"].to<JsonArray>();
    if (p["currentTeam"]["name"]) {
        teams.add(p["currentTeam"]["name"].as<String>());
    }

    String bio = name + " is a " +
                 String(p["primaryPosition"]["name"] | "player") +
                 " who " + (p["active"].as<bool>() ? "plays" : "played") +
                 " in Major League Baseball.";
    if (p["currentTeam"]["name"]) {
        bio += " Currently with the " + String(p["currentTeam"]["name"] | "") + ".";
    }
    card["bio"] = bio;

    // ── Parse career stats ──
    JsonArray statsArr = p["stats"].as<JsonArray>();
    bool isPitcher = (strcmp(p["primaryPosition"]["abbreviation"] | "", "P") == 0 ||
                      strcmp(p["primaryPosition"]["abbreviation"] | "", "SP") == 0 ||
                      strcmp(p["primaryPosition"]["abbreviation"] | "", "RP") == 0);

    for (JsonObject statGroup : statsArr) {
        String groupType = statGroup["type"]["displayName"] | "";
        String groupName = statGroup["group"]["displayName"] | "";

        if (groupType != "career") continue;

        JsonObject splits = statGroup["splits"][0]["stat"].as<JsonObject>();
        if (splits.isNull()) continue;

        JsonObject cs = card["career_stats"].to<JsonObject>();

        if (groupName == "hitting" && !isPitcher) {
            cs["G"]   = splits["gamesPlayed"]   | "--";
            cs["AB"]  = splits["atBats"]         | "--";
            cs["R"]   = splits["runs"]            | "--";
            cs["H"]   = splits["hits"]            | "--";
            cs["2B"]  = splits["doubles"]         | "--";
            cs["3B"]  = splits["triples"]         | "--";
            cs["HR"]  = splits["homeRuns"]        | "--";
            cs["RBI"] = splits["rbi"]             | "--";
            cs["BB"]  = splits["baseOnBalls"]     | "--";
            cs["SO"]  = splits["strikeOuts"]      | "--";
            cs["SB"]  = splits["stolenBases"]     | "--";
            cs["AVG"] = splits["avg"]             | "--";
            cs["OBP"] = splits["obp"]             | "--";
            cs["SLG"] = splits["slg"]             | "--";
            cs["OPS"] = splits["ops"]             | "--";
            cs["WAR"] = "--";
        } else if (groupName == "pitching" && isPitcher) {
            cs["G"]    = splits["gamesPitched"]   | "--";
            cs["GS"]   = splits["gamesStarted"]   | "--";
            cs["W"]    = splits["wins"]            | "--";
            cs["L"]    = splits["losses"]          | "--";
            cs["SV"]   = splits["saves"]           | "--";
            cs["IP"]   = splits["inningsPitched"]  | "--";
            cs["H"]    = splits["hits"]            | "--";
            cs["ER"]   = splits["earnedRuns"]      | "--";
            cs["BB"]   = splits["baseOnBalls"]     | "--";
            cs["SO"]   = splits["strikeOuts"]      | "--";
            cs["ERA"]  = splits["era"]             | "--";
            cs["WHIP"] = splits["whip"]            | "--";
            cs["WAR"]  = "--";
            cs["K9"]   = splits["strikeoutsPer9Inn"] | "--";
            cs["BB9"]  = splits["walksPer9Inn"]    | "--";
            cs["FIP"]  = "--";
        }
    }

    card["notable"] = String("MLB player. Stats current as of ") +
                      String(p["active"].as<bool>() ? "active season." : "retirement.");

    if (!savePlayerCard(entryId, card)) {
        fetchStatus("SD write failed!", COL_ERR);
        return false;
    }

    String pos   = p["primaryPosition"]["abbreviation"] | "?";
    String debut = p["mlbDebutDate"] | "?";
    String retd  = p["active"].as<bool>() ? "Now" : String(p["lastPlayedDate"] | "?").substring(0,4);
    String years = debut.substring(0,4) + "-" + retd;

    if (!updateIndex(entryId, name.c_str(), pos.c_str(), years.c_str())) {
        fetchStatus("Index update failed!", COL_ERR);
        return false;
    }

    char doneMsg[48];
    snprintf(doneMsg, 48, "Saved: %s", name.c_str());
    fetchStatus(doneMsg, COL_OK);
    return true;
}

// ─────────────────────────────────────────────
//  GEMINI FETCH (historical / retired players)
// ─────────────────────────────────────────────
static bool fetchFromGemini(const char* playerName, const char* entryId) {
    fetchStatus("Asking Gemini...", COL_MODE_G);

    String prompt =
        String("Return a JSON player card for baseball player: ") + playerName +
        "\n\nReturn ONLY valid JSON, no markdown, no explanation. Use exactly this schema:\n"
        "{\n"
        "  \"name\": \"Full Name\",\n"
        "  \"position\": \"RF\",\n"
        "  \"bats\": \"R\",\n"
        "  \"throws\": \"R\",\n"
        "  \"born\": \"Month DD, YYYY\",\n"
        "  \"birthplace\": \"City, State\",\n"
        "  \"debut\": \"YYYY\",\n"
        "  \"retired\": \"YYYY or Active\",\n"
        "  \"teams\": [\"Team1\", \"Team2\"],\n"
        "  \"bio\": \"2-3 sentence biography.\",\n"
        "  \"career_stats\": {\n"
        "    \"G\":\"...\",\"AB\":\"...\",\"R\":\"...\",\"H\":\"...\","
        "\"2B\":\"...\",\"3B\":\"...\",\"HR\":\"...\",\"RBI\":\"...\","
        "\"BB\":\"...\",\"SO\":\"...\",\"SB\":\"...\","
        "\"AVG\":\".XXX\",\"OBP\":\".XXX\",\"SLG\":\".XXX\","
        "\"OPS\":\".XXX\",\"WAR\":\"XX.X\"\n"
        "  },\n"
        "  \"notable\": \"Awards and honors as a single string.\"\n"
        "}\n"
        "For pitchers use: G,GS,W,L,SV,IP,H,ER,BB,SO,ERA,WHIP,WAR,K9,BB9,FIP\n"
        "Use career totals. Return ONLY the JSON object.";

    fetchStatus("Waiting for response...", COL_MODE_G);

    String response = ask_gemini(prompt);

    if (response.startsWith("ERROR:")) {
        fetchStatus(response.c_str(), COL_ERR);
        return false;
    }

    fetchStatus("Parsing response...", COL_MODE_G);

    response.trim();
    if (response.startsWith("```")) {
        int start = response.indexOf('\n') + 1;
        int end   = response.lastIndexOf("```");
        if (end > start) response = response.substring(start, end);
        response.trim();
    }

    int jsonStart = response.indexOf('{');
    int jsonEnd   = response.lastIndexOf('}');
    if (jsonStart < 0 || jsonEnd < 0 || jsonEnd <= jsonStart) {
        fetchStatus("No valid JSON in response", COL_ERR);
        return false;
    }
    String jsonStr = response.substring(jsonStart, jsonEnd + 1);

    JsonDocument card;
    DeserializationError err = deserializeJson(card, jsonStr);
    if (err) {
        char msg[48]; snprintf(msg, 48, "JSON parse error: %s", err.c_str());
        fetchStatus(msg, COL_ERR);
        return false;
    }

    card["id"] = entryId;

    const char* name = card["name"] | "";
    if (strlen(name) == 0) {
        fetchStatus("Response missing player name", COL_ERR);
        return false;
    }

    if (!savePlayerCard(entryId, card)) {
        fetchStatus("SD write failed!", COL_ERR);
        return false;
    }

    const char* pos    = card["position"] | "?";
    const char* debut  = card["debut"]    | "?";
    const char* retd   = card["retired"]  | "?";
    String years = String(debut) + "-" +
                   (strcmp(retd,"Active")==0 ? "Now" : String(retd).substring(0,4));

    if (!updateIndex(entryId, name, pos, years.c_str())) {
        fetchStatus("Index update failed!", COL_ERR);
        return false;
    }

    char doneMsg[48];
    snprintf(doneMsg, 48, "Saved: %s", name);
    fetchStatus(doneMsg, COL_OK);
    return true;
}

// ─────────────────────────────────────────────
//  FETCH SCREEN UI
// ─────────────────────────────────────────────
static void drawFetchScreen(const char* query, int mode, const char* hint = nullptr) {
    gfx->fillScreen(COL_BG);

    gfx->fillRect(0, 0, 320, 24, 0x9800);
    gfx->drawFastHLine(0, 23, 320, COL_VALUE);
    gfx->setTextSize(1); gfx->setTextColor(COL_VALUE);
    gfx->setCursor(8, 8); gfx->print("FETCH PLAYER DATA");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(252, 8); gfx->print("[TAP=BACK]");

    gfx->fillRect(0, 26, 320, 18, 0x0821);
    gfx->drawFastHLine(0, 43, 320, 0x2104);

    uint16_t modeACol = (mode == 0) ? COL_MODE_A : COL_DIM;
    gfx->drawRect(4, 28, 80, 14, modeACol);
    if (mode == 0) gfx->fillRect(5, 29, 78, 12, 0x0010);
    gfx->setTextColor(modeACol); gfx->setCursor(8, 31);
    gfx->print("MLB API");

    uint16_t modeGCol = (mode == 1) ? COL_MODE_G : COL_DIM;
    gfx->drawRect(90, 28, 100, 14, modeGCol);
    if (mode == 1) gfx->fillRect(91, 29, 98, 12, 0x1800);
    gfx->setTextColor(modeGCol); gfx->setCursor(94, 31);
    gfx->print("GEMINI LOOKUP");

    gfx->setTextColor(COL_DIM); gfx->setCursor(196, 31);
    if (mode == 0) gfx->print("Active players");
    else           gfx->print("Any era");

    gfx->setTextColor(0x2104); gfx->setCursor(4, 44);
    gfx->print("TAB=switch mode");

    gfx->setTextColor(COL_LABEL); gfx->setCursor(8, 56);
    gfx->print("Player name:");
    gfx->fillRect(5, 66, 310, 16, 0x18C3);
    gfx->drawRect(5, 66, 310, 16, mode == 0 ? COL_MODE_A : COL_MODE_G);
    gfx->setTextColor(strlen(query) > 0 ? COL_TEXT : COL_DIM);
    gfx->setCursor(10, 70);
    if (strlen(query) > 0) { gfx->print(query); gfx->print("_"); }
    else gfx->print("Type player name, press ENTER...");

    gfx->drawFastHLine(0, 83, 320, 0x2104);
    gfx->setTextColor(COL_DIM); gfx->setCursor(8, 85);

    if (hint) {
        gfx->setTextColor(COL_VALUE);
        gfx->setCursor(8, 85);
        gfx->print(hint);
    }

    statusY = 100;
}

// ─────────────────────────────────────────────
//  MAIN FETCH ENTRY POINT
// ─────────────────────────────────────────────
void run_baseball_fetch() {
    if (WiFi.status() != WL_CONNECTED) {
        gfx->fillScreen(COL_BG);
        gfx->setTextColor(COL_ERR); gfx->setTextSize(1);
        gfx->setCursor(20, 100); gfx->print("WiFi not connected.");
        gfx->setCursor(20, 116); gfx->print("Connect via WIFI JOIN first.");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(20, 136); gfx->print("Tap header to go back.");
        while (true) {
            int16_t tx, ty; TrackballState tb = update_trackball();
            if (get_touch(&tx,&ty) && ty<24) { while(get_touch(&tx,&ty)){delay(10);} break; }
            if (tb.clicked || get_keypress() == 'q') break;
            delay(50);
        }
        return;
    }

    char query[48] = "";
    int mode = 0;
    drawFetchScreen(query, mode);

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        if (k == 'q' || k == 'Q') break;

        if (k == 9) {
            mode = 1 - mode;
            drawFetchScreen(query, mode);
            continue;
        }

        bool changed = false;
        if (k >= 'a' && k <= 'z') {
            int ql = strlen(query);
            if (ql < 46) { query[ql] = k; query[ql+1] = '\0'; changed = true; }
        } else if (k >= 'A' && k <= 'Z') {
            int ql = strlen(query);
            if (ql < 46) { query[ql] = k; query[ql+1] = '\0'; changed = true; }
        } else if ((k == 8 || k == 127) && strlen(query) > 0) {
            query[strlen(query)-1] = '\0'; changed = true;
        } else if ((k == 13 || tb.clicked) && strlen(query) > 0) {
            drawFetchScreen(query, mode);
            fetchClearStatus();

            char entryId[16];
            getNextEntryId(entryId, sizeof(entryId));

            bool ok = false;
            if (mode == 0) ok = fetchFromMLBApi(query, entryId);
            else           ok = fetchFromGemini(query, entryId);

            if (ok) {
                fetchStatus("", 0);
                fetchStatus("Done! Returning to search...", COL_OK);
                delay(1500);
                break;
            } else {
                fetchStatus("Fetch failed. Try other mode or check spelling.", COL_ERR);
                delay(2000);
                query[0] = '\0';
                drawFetchScreen(query, mode, "Try again or switch modes.");
                continue;
            }
        }

        if (changed) drawFetchScreen(query, mode);
        delay(40);
        yield();
    }

    gfx->fillScreen(COL_BG);
}