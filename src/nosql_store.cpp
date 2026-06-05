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

#include "nosql_store.h"
#include <ArduinoJson.h>
#include "pm_storage.h"

// ─────────────────────────────────────────────
//  STORAGE BACKEND (v1.3 — via pm_storage HAL)
//
//  The four SPI-SD devices (T-Deck Plus, T-LoRa Pager, Cardputer ADV,
//  Maxine) all mount their cards through SdFat. The C28P uses Arduino's
//  SD_MMC over 4-bit SDIO. v1.2.x handled this with a per-file macro
//  adapter; v1.3 hides it inside the pm_storage HAL so every call here
//  reads as plain C++ without device branching.
//
//  ArduinoJson 7 still wants a Stream-compatible target for
//  serialize/deserialize, but pm_storage::File is not derived from
//  Stream (deliberate — we don't want to inherit the Arduino fs::FS
//  baggage). To bridge, we serialize/deserialize against a String
//  buffer and shuttle bytes through pm_storage::File ourselves. The
//  file sizes here are small (an index.json + per-entry JSON, both
//  well under 16 KB even at the v1.2 max-entries setting), so the
//  intermediate String buffer is cheap.
// ─────────────────────────────────────────────

// ─────────────────────────────────────────────
//  INTERNAL HELPERS
// ─────────────────────────────────────────────

// Builds the base path for a category
//   "medical" -> "/data/medical"
String nosql_category_path(const char* category) {
    return String("/data/") + String(category);
}

// Index file path
//   "medical" -> "/data/medical/index.json"
static String index_path(const char* category) {
    return nosql_category_path(category) + "/index.json";
}

// Entry file path
//   category="gemini", id=1 -> "/data/gemini/entry_001.json"
static String entry_path(const char* category, int id) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%s/entry_%03d.json",
             nosql_category_path(category).c_str(), id);
    return String(buf);
}

// Read the entire contents of a pm_storage::File into a String.
// Returns "" if the file is closed or empty.
static String read_all(pm_storage::File& f) {
    String out;
    if (!f) return out;
    size_t sz = f.size();
    if (sz == 0) return out;
    out.reserve(sz + 1);
    uint8_t chunk[256];
    while (true) {
        size_t n = f.read(chunk, sizeof(chunk));
        if (n == 0) break;
        for (size_t i = 0; i < n; i++) out += (char)chunk[i];
    }
    return out;
}

// Serialize a JsonDocument pretty-printed into a pm_storage::File.
static bool write_doc(pm_storage::File& f, JsonDocument& doc) {
    String s;
    serializeJsonPretty(doc, s);
    return f.write(reinterpret_cast<const uint8_t*>(s.c_str()), s.length()) == s.length();
}

// ─────────────────────────────────────────────
//  INIT
//  Creates /data/<category>/ and a blank index if absent.
// ─────────────────────────────────────────────
bool nosql_init(const char* category) {
    if (!pm_storage::exists("/data")) {
        if (!pm_storage::mkdir("/data")) {
            Serial.println("[NOSQL] ERROR: Cannot create /data/");
            return false;
        }
        Serial.println("[NOSQL] Created /data/");
    }

    String catPath = nosql_category_path(category);
    if (!pm_storage::exists(catPath.c_str())) {
        if (!pm_storage::mkdir(catPath.c_str())) {
            Serial.printf("[NOSQL] ERROR: Cannot create %s\n", catPath.c_str());
            return false;
        }
        Serial.printf("[NOSQL] Created %s\n", catPath.c_str());
    }

    String idxPath = index_path(category);
    if (!pm_storage::exists(idxPath.c_str())) {
        pm_storage::File f = pm_storage::open(idxPath.c_str(),
                                              pm_storage::Mode::Write);
        if (!f) {
            Serial.printf("[NOSQL] ERROR: Cannot create %s\n", idxPath.c_str());
            return false;
        }
        f.printf("{\n  \"category\": \"%s\",\n  \"count\": 0,\n  \"entries\": []\n}\n",
                 category);
        f.close();
        Serial.printf("[NOSQL] Created blank index: %s\n", idxPath.c_str());
    }

    Serial.printf("[NOSQL] Category '%s' ready.\n", category);
    return true;
}

// ─────────────────────────────────────────────
//  GET COUNT
//  Reads "count" field from index.json
// ─────────────────────────────────────────────
int nosql_get_count(const char* category) {
    String idxPath = index_path(category);
    pm_storage::File f = pm_storage::open(idxPath.c_str(),
                                          pm_storage::Mode::Read);
    if (!f) return 0;

    String body = read_all(f);
    f.close();

    JsonDocument filter;
    filter["count"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body,
                                   DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[NOSQL] Count parse error: %s\n", err.c_str());
        return 0;
    }
    return doc["count"] | 0;
}

// ─────────────────────────────────────────────
//  SAVE ENTRY
//  1. Write the entry JSON file
//  2. Append a record to index.json
// ─────────────────────────────────────────────
bool nosql_save_entry(const char* category,
                      const char* title,
                      const char* content,
                      const char* tags) {

    if (!nosql_init(category)) return false;

    int newId = nosql_get_count(category) + 1;
    if (newId > NOSQL_MAX_ENTRIES) {
        Serial.println("[NOSQL] ERROR: Max entries reached.");
        return false;
    }

    // ── Step 1: Write the entry file ──
    String ePath = entry_path(category, newId);
    pm_storage::File entryFile = pm_storage::open(ePath.c_str(),
                                                  pm_storage::Mode::Write);
    if (!entryFile) {
        Serial.printf("[NOSQL] ERROR: Cannot write %s\n", ePath.c_str());
        return false;
    }

    JsonDocument entryDoc;
    entryDoc["id"]      = newId;
    entryDoc["title"]   = title;
    entryDoc["tags"]    = tags;
    entryDoc["content"] = content;
    if (!write_doc(entryFile, entryDoc)) {
        Serial.printf("[NOSQL] ERROR: Write failed: %s\n", ePath.c_str());
        return false;
    }
    entryFile.close();
    Serial.printf("[NOSQL] Wrote entry: %s\n", ePath.c_str());

    // ── Step 2: Update index.json ──
    String idxPath = index_path(category);
    String idxBody;
    {
        pm_storage::File idxRead = pm_storage::open(idxPath.c_str(),
                                                    pm_storage::Mode::Read);
        if (!idxRead) {
            Serial.println("[NOSQL] ERROR: Cannot read index for update.");
            return false;
        }
        idxBody = read_all(idxRead);
    }

    JsonDocument idxDoc;
    DeserializationError err = deserializeJson(idxDoc, idxBody);
    if (err) {
        Serial.printf("[NOSQL] Index parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray entries = idxDoc["entries"].as<JsonArray>();
    JsonObject newRecord = entries.add<JsonObject>();
    newRecord["id"]    = newId;
    newRecord["title"] = title;
    newRecord["tags"]  = tags;
    newRecord["file"]  = String("entry_") +
                         (newId < 10  ? "00" :
                          newId < 100 ? "0"  : "") +
                         String(newId) + ".json";

    idxDoc["count"] = newId;

    pm_storage::File idxWrite = pm_storage::open(idxPath.c_str(),
                                                 pm_storage::Mode::Write);
    if (!idxWrite) {
        Serial.println("[NOSQL] ERROR: Cannot write updated index.");
        return false;
    }
    if (!write_doc(idxWrite, idxDoc)) {
        Serial.println("[NOSQL] ERROR: Index write failed.");
        return false;
    }
    idxWrite.close();

    Serial.printf("[NOSQL] Index updated. Total entries: %d\n", newId);
    return true;
}

// ─────────────────────────────────────────────
//  GET ENTRY
//  Loads entry by 0-based index.
// ─────────────────────────────────────────────
bool nosql_get_entry(const char* category,
                     int index,
                     String &title,
                     String &content) {

    // ── Step 1: Get filename from index ──
    String idxPath = index_path(category);
    String idxBody;
    {
        pm_storage::File idxFile = pm_storage::open(idxPath.c_str(),
                                                    pm_storage::Mode::Read);
        if (!idxFile) {
            Serial.println("[NOSQL] ERROR: Cannot open index.");
            return false;
        }
        idxBody = read_all(idxFile);
    }

    JsonDocument filter;
    filter["entries"][0]["file"]  = true;
    filter["entries"][0]["title"] = true;

    JsonDocument idxDoc;
    DeserializationError err = deserializeJson(idxDoc, idxBody,
                                   DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[NOSQL] Index parse error: %s\n", err.c_str());
        return false;
    }

    JsonArray entries = idxDoc["entries"].as<JsonArray>();
    if (index < 0 || index >= (int)entries.size()) {
        Serial.println("[NOSQL] ERROR: Index out of range.");
        return false;
    }

    String filename = entries[index]["file"].as<String>();
    title           = entries[index]["title"].as<String>();

    // ── Step 2: Load the entry file for full content ──
    String ePath = nosql_category_path(category) + "/" + filename;
    String entryBody;
    {
        pm_storage::File entryFile = pm_storage::open(ePath.c_str(),
                                                      pm_storage::Mode::Read);
        if (!entryFile) {
            Serial.printf("[NOSQL] ERROR: Cannot open %s\n", ePath.c_str());
            return false;
        }
        entryBody = read_all(entryFile);
    }

    JsonDocument entryFilter;
    entryFilter["content"] = true;

    JsonDocument entryDoc;
    err = deserializeJson(entryDoc, entryBody,
              DeserializationOption::Filter(entryFilter));
    if (err) {
        Serial.printf("[NOSQL] Entry parse error: %s\n", err.c_str());
        return false;
    }

    content = entryDoc["content"].as<String>();
    return true;
}

// ─────────────────────────────────────────────
//  SEARCH
//  Walks index.json entries for a keyword match
//  (case-insensitive) in title or tags.
// ─────────────────────────────────────────────
bool nosql_search(const char* category,
                  const char* keyword,
                  String &result_title,
                  String &result_content) {

    String idxPath = index_path(category);
    String idxBody;
    {
        pm_storage::File idxFile = pm_storage::open(idxPath.c_str(),
                                                    pm_storage::Mode::Read);
        if (!idxFile) return false;
        idxBody = read_all(idxFile);
    }

    JsonDocument filter;
    filter["entries"][0]["title"] = true;
    filter["entries"][0]["tags"]  = true;
    filter["entries"][0]["file"]  = true;

    JsonDocument idxDoc;
    DeserializationError err = deserializeJson(idxDoc, idxBody,
                                   DeserializationOption::Filter(filter));
    if (err) return false;

    String kw = String(keyword);
    kw.toLowerCase();

    JsonArray entries = idxDoc["entries"].as<JsonArray>();
    for (JsonObject entry : entries) {
        String entryTitle = entry["title"].as<String>();
        String entryTags  = entry["tags"].as<String>();
        entryTitle.toLowerCase();
        entryTags.toLowerCase();

        if (entryTitle.indexOf(kw) != -1 || entryTags.indexOf(kw) != -1) {
            String filename = entry["file"].as<String>();
            String ePath = nosql_category_path(category) + "/" + filename;

            String entryBody;
            {
                pm_storage::File entryFile = pm_storage::open(ePath.c_str(),
                                                              pm_storage::Mode::Read);
                if (!entryFile) return false;
                entryBody = read_all(entryFile);
            }

            JsonDocument entryFilter;
            entryFilter["title"]   = true;
            entryFilter["content"] = true;

            JsonDocument entryDoc;
            err = deserializeJson(entryDoc, entryBody,
                      DeserializationOption::Filter(entryFilter));
            if (err) return false;

            result_title   = entryDoc["title"].as<String>();
            result_content = entryDoc["content"].as<String>();
            return true;
        }
    }

    Serial.printf("[NOSQL] No match for '%s' in '%s'\n", keyword, category);
    return false;
}

// ─────────────────────────────────────────────
//  CLEAR CATEGORY (DESTRUCTIVE)
//
//  Walks /data/<category>/ deleting every file, then removes the
//  category folder. After this call the category appears as if
//  never initialized; the next nosql_init recreates it.
//
//  v1.3 — uses pm_storage's iteration API and is now identical
//  for both backends. The SdFat/fs::FS difference (open_next vs
//  openNextFile, getName vs name) is folded into pm_storage::File.
// ─────────────────────────────────────────────
bool nosql_clear_category(const char* category) {
    String catPath = nosql_category_path(category);
    if (!pm_storage::exists(catPath.c_str())) {
        Serial.printf("[NOSQL] clear: '%s' didn't exist, nothing to do\n",
                      category);
        return true;
    }

    int removed = 0;
    pm_storage::File dir = pm_storage::openDir(catPath.c_str());
    if (!dir) {
        Serial.printf("[NOSQL] clear: cannot open %s for walk\n",
                      catPath.c_str());
        return false;
    }
    while (true) {
        pm_storage::File entry = dir.openNextEntry();
        if (!entry) break;
        if (entry.isDirectory()) continue;
        String full = entry.name();
        entry.close();
        if (pm_storage::remove(full.c_str())) {
            removed++;
        } else {
            Serial.printf("[NOSQL] clear: failed to remove %s\n",
                          full.c_str());
        }
    }
    dir.close();
    pm_storage::rmdir(catPath.c_str());

    Serial.printf("[NOSQL] clear: category '%s' wiped (%d files)\n",
                  category, removed);
    return true;
}
