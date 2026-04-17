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
 * PISCES MOON OS — BASEBALL REFERENCE v1.0
 * Searchable player card database for T-Deck Plus
 *
 * Database format: /data/baseball/
 *   index.json     ← {entries:[{id,name,pos,years},...]}
 *   entry_NNN.json ← full player card
 *
 * Player card JSON fields:
 *   name, position, bats, throws, born, birthplace, debut, retired (or "Active")
 *   teams[]        ← array of team strings
 *   bio            ← 2-3 sentence summary
 *   career_stats:
 *     (hitters) G, AB, R, H, 2B, 3B, HR, RBI, BB, SO, SB, AVG, OBP, SLG, OPS, WAR
 *     (pitchers) G, GS, W, L, SV, IP, H, ER, BB, SO, ERA, WHIP, WAR, K9, BB9, FIP
 *   notable        ← awards/honors string
 *
 * Run baseball_db_builder.py on host to populate from Baseball Reference data.
 *
 * Controls:
 *   Trackball up/down = scroll card
 *   Trackball click   = return to search
 *   Keyboard          = search input
 *   Q / header tap    = exit
 */

#include <Arduino.h>
#include <FS.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include <ArduinoJson.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "baseball.h"
#include "baseball_fetch.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG        0x0000
#define COL_HEADER    0x9800   // Baseball brown/red
#define COL_CARD      0x0841   // Dark card background
#define COL_NAME      0xFFE0   // Gold for name
#define COL_LABEL     0x07FF   // Cyan labels
#define COL_VALUE     0xFFFF   // White values
#define COL_STAT_H    0x07E0   // Green for good stats
#define COL_ACCENT    0xFD20   // Orange accent
#define COL_DIM       0x4208   // Dim grey

#define DB_PATH "/data/baseball"

// ─────────────────────────────────────────────
//  SEARCH STATE
// ─────────────────────────────────────────────
struct IndexEntry {
    char id[16];
    char name[48];
    char pos[8];
    char years[16];
};

static IndexEntry* searchResults = nullptr;
static int resultCount = 0;
static int selectedResult = 0;
static int resultScroll = 0;
#define MAX_RESULTS 50
#define RESULTS_PER_PAGE 7

// ─────────────────────────────────────────────
//  DRAW HEADER BAR
// ─────────────────────────────────────────────
static void drawHeader(const char* title, bool showFetch = false) {
    gfx->fillRect(0, 0, 320, 24, COL_HEADER);
    gfx->drawFastHLine(0, 23, 320, COL_ACCENT);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_NAME);
    gfx->setCursor(10, 8);
    gfx->print(title);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(252, 8);
    gfx->print("[TAP EXIT]");
    if (showFetch) {
        gfx->drawRect(192, 2, 56, 19, 0x07FF);
        gfx->fillRect(193, 3, 54, 17, 0x0010);
        gfx->setTextColor(0x07FF);
        gfx->setCursor(198, 8);
        gfx->print("+FETCH");
    }
}

// ─────────────────────────────────────────────
//  LOAD INDEX
// ─────────────────────────────────────────────
static int loadIndex(const char* query) {
    char idxPath[64];
    snprintf(idxPath, sizeof(idxPath), "%s/index.json", DB_PATH);

    if (!sd.exists(idxPath)) return 0;
    FsFile f = sd.open(idxPath, O_READ);
    if (!f) return 0;

    // Stream parse — only load entries matching query
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return 0;

    JsonArray entries = doc["entries"].as<JsonArray>();
    int count = 0;
    String q = String(query);
    q.toLowerCase();

    if (!searchResults) searchResults = new IndexEntry[MAX_RESULTS];

    for (JsonObject e : entries) {
        if (count >= MAX_RESULTS) break;
        String n = e["name"] | "";
        String nl = n;
        nl.toLowerCase();
        if (q.length() == 0 || nl.indexOf(q) >= 0) {
            strncpy(searchResults[count].id,    (e["id"] | ""),    15);
            strncpy(searchResults[count].name,  n.c_str(),          47);
            strncpy(searchResults[count].pos,   (e["pos"] | ""),    7);
            strncpy(searchResults[count].years, (e["years"] | ""),  15);
            count++;
        }
    }
    return count;
}

// ─────────────────────────────────────────────
//  DRAW SEARCH SCREEN
// ─────────────────────────────────────────────
static void drawSearchScreen(const char* query, int results, int selected, int scroll) {
    gfx->fillScreen(COL_BG);
    drawHeader("BASEBALL REFERENCE", true);

    // Search box
    gfx->fillRect(5, 28, 310, 18, 0x18C3);
    gfx->drawRect(5, 28, 310, 18, COL_ACCENT);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_VALUE);
    gfx->setCursor(10, 33);
    if (strlen(query) > 0) gfx->print(query);
    else { gfx->setTextColor(COL_DIM); gfx->print("TYPE TO SEARCH PLAYERS..."); }

    // Result count
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(10, 50);
    if (results == 0) gfx->print("No results. Try a last name.");
    else gfx->printf("%d player%s found", results, results==1?"":"s");

    // Result list
    int displayEnd = min(scroll + RESULTS_PER_PAGE, results);
    for (int i = scroll; i < displayEnd; i++) {
        int y = 62 + (i - scroll) * 24;
        bool sel = (i == selected);
        gfx->fillRect(5, y, 310, 22, sel ? COL_HEADER : COL_CARD);
        gfx->drawRect(5, y, 310, 22, sel ? COL_ACCENT : 0x2104);

        gfx->setTextColor(sel ? COL_NAME : COL_VALUE);
        gfx->setTextSize(1);
        gfx->setCursor(12, y+4);
        // Truncate name to fit
        char displayName[32];
        strncpy(displayName, searchResults[i].name, 31);
        if (strlen(displayName) > 22) displayName[22] = '\0';
        gfx->print(displayName);

        // Position + years on right
        gfx->setTextColor(sel ? COL_ACCENT : COL_LABEL);
        gfx->setCursor(200, y+4);
        gfx->print(searchResults[i].pos);
        gfx->setCursor(230, y+4);
        gfx->print(searchResults[i].years);
    }

    // Scroll hint
    if (results > RESULTS_PER_PAGE) {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(10, 232);
        gfx->printf("Scroll: %d-%d of %d", scroll+1, displayEnd, results);
    }
}

// ─────────────────────────────────────────────
//  DRAW PLAYER CARD
// ─────────────────────────────────────────────
static void drawPlayerCard(const char* entryId, int scrollY) {
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.json", DB_PATH, entryId);

    if (!sd.exists(path)) {
        gfx->fillScreen(COL_BG);
        drawHeader("PLAYER NOT FOUND");
        gfx->setCursor(20, 80); gfx->setTextColor(0xF800);
        gfx->print("Card missing from SD.");
        gfx->setCursor(20, 96); gfx->setTextColor(COL_DIM);
        gfx->print("Run baseball_db_builder.py");
        return;
    }

    FsFile f = sd.open(path, O_READ);
    if (!f) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;

    gfx->fillScreen(COL_BG);
    drawHeader("PLAYER CARD");

    // Clip region — everything below header
    int y = 28 - scrollY;

    // ── NAME PLATE ──
    { int py = y; y += 28;
      if (py + 28 >= 28 && py < 240) {
        gfx->fillRect(0, py, 320, 26, 0x18C3);
        gfx->setTextSize(2); gfx->setTextColor(COL_NAME);
        gfx->setCursor(8, py+5);
        gfx->print(doc["name"] | "Unknown");
      }
    }

    // ── POSITION / BATS / THROWS ──
    { int py = y; y += 12;
      if (py + 12 >= 28 && py < 240) {
        gfx->setTextSize(1); gfx->setTextColor(COL_LABEL);
        gfx->setCursor(8, py+2);
        gfx->printf("%s  B:%s T:%s",
            (const char*)(doc["position"] | ""),
            (const char*)(doc["bats"]     | ""),
            (const char*)(doc["throws"]   | ""));
      }
    }

    // ── BORN ──
    { int py = y; y += 12;
      if (py + 12 >= 28 && py < 240) {
        gfx->setTextSize(1); gfx->setTextColor(COL_DIM);
        gfx->setCursor(8, py+2);
        gfx->printf("Born: %s  %s",
            (const char*)(doc["born"]       | ""),
            (const char*)(doc["birthplace"] | ""));
      }
    }

    // ── CAREER SPAN ──
    { int py = y; y += 12;
      if (py + 12 >= 28 && py < 240) {
        gfx->setTextSize(1); gfx->setTextColor(COL_DIM);
        gfx->setCursor(8, py+2);
        gfx->printf("Career: %s to %s",
            (const char*)(doc["debut"]   | ""),
            (const char*)(doc["retired"] | "Active"));
      }
    }

    // ── TEAMS ──
    { int py = y; y += 12;
      if (py + 12 >= 28 && py < 240) {
        gfx->setTextColor(COL_ACCENT);
        gfx->setCursor(8, py+2);
        String teams = "";
        JsonArray ta = doc["teams"].as<JsonArray>();
        for (const char* t : ta) { if (teams.length()) teams += ", "; teams += t; }
        if (teams.length() > 42) teams = teams.substring(0,42) + "...";
        gfx->print(teams);
      }
    }

    // ── DIVIDER ──
    { int py = y; y += 8;
      if (py + 8 >= 28 && py < 240)
        gfx->drawFastHLine(0, py+4, 320, COL_ACCENT);
    }

    // ── STATS HEADER ──
    bool isPitcher = (String(doc["position"] | "").indexOf("P") >= 0 &&
                      String(doc["position"] | "") != "PH");
    { int py = y; y += 12;
      if (py + 12 >= 28 && py < 240) {
        gfx->setTextSize(1); gfx->setTextColor(COL_LABEL);
        gfx->setCursor(8, py+2); gfx->print("CAREER STATISTICS");
      }
    }

    JsonObject cs = doc["career_stats"].as<JsonObject>();

    if (!isPitcher) {
        struct StatDef { const char* label; const char* key; bool highlight; };
        StatDef stats[] = {
            {"G","G",false},{"AB","AB",false},{"R","R",false},{"H","H",false},
            {"2B","2B",false},{"3B","3B",false},{"HR","HR",true},{"RBI","RBI",true},
            {"BB","BB",false},{"SO","SO",false},{"SB","SB",false},{"AVG","AVG",true},
            {"OBP","OBP",true},{"SLG","SLG",true},{"OPS","OPS",true},{"WAR","WAR",true},
        };
        for (int i = 0; i < 16; i += 2) {
            int py = y; y += 14;
            if (py + 14 < 28 || py >= 240) continue;
            for (int col = 0; col < 2 && i+col < 16; col++) {
                int x = col == 0 ? 8 : 165;
                gfx->setTextColor(stats[i+col].highlight ? COL_STAT_H : COL_LABEL);
                gfx->setCursor(x, py+2);
                gfx->print(stats[i+col].label); gfx->print(":");
                gfx->setTextColor(COL_VALUE);
                gfx->print(cs[stats[i+col].key] | "--");
            }
        }
    } else {
        struct StatDef { const char* label; const char* key; bool highlight; };
        StatDef stats[] = {
            {"G","G",false},{"GS","GS",false},{"W","W",true},{"L","L",false},
            {"SV","SV",true},{"IP","IP",false},{"ERA","ERA",true},{"WHIP","WHIP",true},
            {"SO","SO",true},{"BB","BB",false},{"K/9","K9",true},{"BB/9","BB9",false},
            {"FIP","FIP",true},{"WAR","WAR",true},
        };
        for (int i = 0; i < 14; i += 2) {
            int py = y; y += 14;
            if (py + 14 < 28 || py >= 240) continue;
            for (int col = 0; col < 2 && i+col < 14; col++) {
                int x = col == 0 ? 8 : 165;
                gfx->setTextColor(stats[i+col].highlight ? COL_STAT_H : COL_LABEL);
                gfx->setCursor(x, py+2);
                gfx->print(stats[i+col].label); gfx->print(":");
                gfx->setTextColor(COL_VALUE);
                gfx->print(cs[stats[i+col].key] | "--");
            }
        }
    }

    // ── DIVIDER ──
    { int py = y; y += 8;
      if (py + 8 >= 28 && py < 240)
        gfx->drawFastHLine(0, py+4, 320, 0x2104);
    }

    // ── BIO LABEL ──
    { int py = y; y += 12;
      if (py + 12 >= 28 && py < 240) {
        gfx->setTextColor(COL_LABEL);
        gfx->setCursor(8, py+2); gfx->print("BIO:");
      }
    }

    // ── BIO TEXT (word-wrapped) ──
    { String bio = doc["bio"] | "";
      while (bio.length() > 0) {
        int cut = min((int)bio.length(), 52);
        if (cut < (int)bio.length()) {
            int sp = bio.lastIndexOf(' ', cut);
            if (sp > 0) cut = sp;
        }
        String line = bio.substring(0, cut);
        bio = bio.substring(cut);
        if (bio.startsWith(" ")) bio = bio.substring(1);
        int py = y; y += 11;
        if (py + 11 >= 28 && py < 240) {
            gfx->setTextColor(COL_VALUE);
            gfx->setCursor(8, py+1);
            gfx->print(line);
        }
      }
    }

    // ── NOTABLE ──
    { int py = y; y += 10;
      if (py + 10 >= 28 && py < 240)
        gfx->drawFastHLine(0, py, 320, 0x2104);
    }
    { int py = y; y += 11;
      if (py + 11 >= 28 && py < 240) {
        gfx->setTextColor(COL_ACCENT);
        gfx->setCursor(8, py+1);
        const char* notable = doc["notable"] | "";
        char buf[53]; strncpy(buf, notable, 52); buf[52] = '\0';
        gfx->print(buf);
      }
    }

    // Scroll indicator
    int totalH = y - 28 + scrollY;
    if (totalH > 212) {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(260, 230);
        gfx->printf("SCR:%d", scrollY);
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_baseball() {
    if (!searchResults) searchResults = new IndexEntry[MAX_RESULTS];

    char query[32] = "";
    resultCount = loadIndex(query); // Load all on start
    selectedResult = 0; resultScroll = 0;
    drawSearchScreen(query, resultCount, selectedResult, resultScroll);

    bool inCard = false;
    int cardScroll = 0;

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        // Header tap
        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            if (inCard) {
                // Any header tap exits card view
                inCard = false; cardScroll = 0;
                drawSearchScreen(query, resultCount, selectedResult, resultScroll);
            } else if (tx >= 192 && tx <= 248) {
                // FETCH button tapped
                run_baseball_fetch();
                // Reload index after fetch — new player may have been added
                resultCount = loadIndex(query);
                selectedResult = 0; resultScroll = 0;
                drawSearchScreen(query, resultCount, selectedResult, resultScroll);
            } else {
                break; // Exit app
            }
            continue;
        }

        if (k == 'q' || k == 'Q') {
            if (inCard) {
                inCard = false; cardScroll = 0;
                drawSearchScreen(query, resultCount, selectedResult, resultScroll);
            } else break;
            continue;
        }

        if (inCard) {
            // Card scroll
            bool changed = false;
            if (tb.y == -1 && cardScroll > 0) { cardScroll -= 11; if (cardScroll<0) cardScroll=0; changed=true; }
            if (tb.y ==  1) { cardScroll += 11; changed = true; }
            if (tb.clicked || k == 13) { inCard = false; cardScroll = 0;
                drawSearchScreen(query, resultCount, selectedResult, resultScroll); }
            if (changed) drawPlayerCard(searchResults[selectedResult].id, cardScroll);
        } else {
            // Search mode
            bool queryChanged = false;

            // Keyboard input
            if (k >= 'a' && k <= 'z') { 
                int ql = strlen(query);
                if (ql < 30) { query[ql] = k; query[ql+1] = '\0'; queryChanged = true; }
            } else if (k >= 'A' && k <= 'Z') {
                int ql = strlen(query);
                if (ql < 30) { query[ql] = k - 'A' + 'a'; query[ql+1] = '\0'; queryChanged = true; }
            } else if ((k == 8 || k == 127) && strlen(query) > 0) {
                query[strlen(query)-1] = '\0'; queryChanged = true;
            }

            if (queryChanged) {
                resultCount = loadIndex(query);
                selectedResult = 0; resultScroll = 0;
                drawSearchScreen(query, resultCount, selectedResult, resultScroll);
            }

            // Trackball navigation
            bool navChanged = false;
            if (tb.y == -1 && selectedResult > 0) {
                selectedResult--;
                if (selectedResult < resultScroll) resultScroll--;
                navChanged = true;
            }
            if (tb.y ==  1 && selectedResult < resultCount - 1) {
                selectedResult++;
                if (selectedResult >= resultScroll + RESULTS_PER_PAGE)
                    resultScroll++;
                navChanged = true;
            }
            if (navChanged) drawSearchScreen(query, resultCount, selectedResult, resultScroll);

            // F key = open fetch screen
            if (k == 'f' || k == 'F') {
                run_baseball_fetch();
                resultCount = loadIndex(query);
                selectedResult = 0; resultScroll = 0;
                drawSearchScreen(query, resultCount, selectedResult, resultScroll);
                continue;
            }

            // Open card
            if ((tb.clicked || k == 13) && resultCount > 0) {
                inCard = true; cardScroll = 0;
                drawPlayerCard(searchResults[selectedResult].id, cardScroll);
            }
        }

        delay(40);
        yield();
    }

    if (searchResults) { delete[] searchResults; searchResults = nullptr; }
    gfx->fillScreen(0x0000);
}