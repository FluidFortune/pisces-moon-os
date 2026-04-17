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
#include "SdFat.h"
#include <ArduinoJson.h>

extern SdFat sd;

// ─────────────────────────────────────────────
//  INTERNAL HELPERS
// ─────────────────────────────────────────────

// Builds the base path for a category
// e.g. "medical" -> "/data/medical"
String nosql_category_path(const char* category) {
    return String("/data/") + String(category);
}

// Builds the index file path for a category
// e.g. "medical" -> "/data/medical/index.json"
static String index_path(const char* category) {
    return nosql_category_path(category) + "/index.json";
}

// Builds the entry file path for a given ID
// e.g. category="gemini", id=1 -> "/data/gemini/entry_001.json"
static String entry_path(const char* category, int id) {
    char buf[80];
    snprintf(buf, sizeof(buf), "%s/entry_%03d.json",
             nosql_category_path(category).c_str(), id);
    return String(buf);
}

// ─────────────────────────────────────────────
//  INIT
//  Creates /data/<category>/ and a blank index
//  if they don't already exist.
// ─────────────────────────────────────────────
bool nosql_init(const char* category) {
    // Ensure /data/ root exists
    if (!sd.exists("/data")) {
        if (!sd.mkdir("/data")) {
            Serial.println("[NOSQL] ERROR: Cannot create /data/");
            return false;
        }
        Serial.println("[NOSQL] Created /data/");
    }

    // Ensure /data/<category>/ exists
    String catPath = nosql_category_path(category);
    if (!sd.exists(catPath.c_str())) {
        if (!sd.mkdir(catPath.c_str())) {
            Serial.printf("[NOSQL] ERROR: Cannot create %s\n", catPath.c_str());
            return false;
        }
        Serial.printf("[NOSQL] Created %s\n", catPath.c_str());
    }

    // Ensure index.json exists with a valid empty structure
    String idxPath = index_path(category);
    if (!sd.exists(idxPath.c_str())) {
        FsFile f = sd.open(idxPath.c_str(), O_WRITE | O_CREAT);
        if (!f) {
            Serial.printf("[NOSQL] ERROR: Cannot create %s\n", idxPath.c_str());
            return false;
        }
        // Write a blank but valid index
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
    FsFile f = sd.open(idxPath.c_str(), O_READ);
    if (!f) return 0;

    // Only parse what we need — filter for count field
    JsonDocument filter;
    filter["count"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f,
                                   DeserializationOption::Filter(filter));
    f.close();

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

    // Make sure the category is initialized
    if (!nosql_init(category)) return false;

    // Get the next entry ID
    int newId = nosql_get_count(category) + 1;
    if (newId > NOSQL_MAX_ENTRIES) {
        Serial.println("[NOSQL] ERROR: Max entries reached.");
        return false;
    }

    // ── Step 1: Write the entry file ──
    String ePath = entry_path(category, newId);
    FsFile entryFile = sd.open(ePath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (!entryFile) {
        Serial.printf("[NOSQL] ERROR: Cannot write %s\n", ePath.c_str());
        return false;
    }

    JsonDocument entryDoc;
    entryDoc["id"]       = newId;
    entryDoc["title"]    = title;
    entryDoc["tags"]     = tags;
    entryDoc["content"]  = content;

    serializeJsonPretty(entryDoc, entryFile);
    entryFile.close();
    Serial.printf("[NOSQL] Wrote entry: %s\n", ePath.c_str());

    // ── Step 2: Update index.json ──
    // Read existing index
    String idxPath = index_path(category);
    FsFile idxRead = sd.open(idxPath.c_str(), O_READ);
    if (!idxRead) {
        Serial.println("[NOSQL] ERROR: Cannot read index for update.");
        return false;
    }

    JsonDocument idxDoc;
    DeserializationError err = deserializeJson(idxDoc, idxRead);
    idxRead.close();

    if (err) {
        Serial.printf("[NOSQL] Index parse error: %s\n", err.c_str());
        return false;
    }

    // Append the new entry record to the entries array
    JsonArray entries = idxDoc["entries"].as<JsonArray>();
    JsonObject newRecord = entries.add<JsonObject>();
    newRecord["id"]    = newId;
    newRecord["title"] = title;
    newRecord["tags"]  = tags;
    newRecord["file"]  = String("entry_") +
                         (newId < 10  ? "00" :
                          newId < 100 ? "0"  : "") +
                         String(newId) + ".json";

    // Update the count
    idxDoc["count"] = newId;

    // Write the updated index back
    FsFile idxWrite = sd.open(idxPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (!idxWrite) {
        Serial.println("[NOSQL] ERROR: Cannot write updated index.");
        return false;
    }
    serializeJsonPretty(idxDoc, idxWrite);
    idxWrite.close();

    Serial.printf("[NOSQL] Index updated. Total entries: %d\n", newId);
    return true;
}

// ─────────────────────────────────────────────
//  GET ENTRY
//  Loads entry by 0-based index.
//  Reads the index to find the filename,
//  then loads that file for full content.
// ─────────────────────────────────────────────
bool nosql_get_entry(const char* category,
                     int index,
                     String &title,
                     String &content) {

    // ── Step 1: Get filename from index ──
    String idxPath = index_path(category);
    FsFile idxFile = sd.open(idxPath.c_str(), O_READ);
    if (!idxFile) {
        Serial.println("[NOSQL] ERROR: Cannot open index.");
        return false;
    }

    // Filter to only pull the entries array — saves RAM
    JsonDocument filter;
    filter["entries"][0]["file"]  = true;
    filter["entries"][0]["title"] = true;

    JsonDocument idxDoc;
    DeserializationError err = deserializeJson(idxDoc, idxFile,
                                   DeserializationOption::Filter(filter));
    idxFile.close();

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
    FsFile entryFile = sd.open(ePath.c_str(), O_READ);
    if (!entryFile) {
        Serial.printf("[NOSQL] ERROR: Cannot open %s\n", ePath.c_str());
        return false;
    }

    // Filter for just the content field
    JsonDocument entryFilter;
    entryFilter["content"] = true;

    JsonDocument entryDoc;
    err = deserializeJson(entryDoc, entryFile,
              DeserializationOption::Filter(entryFilter));
    entryFile.close();

    if (err) {
        Serial.printf("[NOSQL] Entry parse error: %s\n", err.c_str());
        return false;
    }

    content = entryDoc["content"].as<String>();
    return true;
}

// ─────────────────────────────────────────────
//  SEARCH
//  Walks index.json entries looking for keyword
//  match in title or tags (case-insensitive).
//  Returns the first match found.
// ─────────────────────────────────────────────
bool nosql_search(const char* category,
                  const char* keyword,
                  String &result_title,
                  String &result_content) {

    String idxPath = index_path(category);
    FsFile idxFile = sd.open(idxPath.c_str(), O_READ);
    if (!idxFile) return false;

    JsonDocument filter;
    filter["entries"][0]["title"] = true;
    filter["entries"][0]["tags"]  = true;
    filter["entries"][0]["file"]  = true;

    JsonDocument idxDoc;
    DeserializationError err = deserializeJson(idxDoc, idxFile,
                                   DeserializationOption::Filter(filter));
    idxFile.close();
    if (err) return false;

    // Case-insensitive search — lowercase both sides
    String kw = String(keyword);
    kw.toLowerCase();

    JsonArray entries = idxDoc["entries"].as<JsonArray>();
    for (JsonObject entry : entries) {
        String entryTitle = entry["title"].as<String>();
        String entryTags  = entry["tags"].as<String>();
        entryTitle.toLowerCase();
        entryTags.toLowerCase();

        if (entryTitle.indexOf(kw) != -1 || entryTags.indexOf(kw) != -1) {
            // Match found — load the full entry
            String filename = entry["file"].as<String>();
            String ePath = nosql_category_path(category) + "/" + filename;

            FsFile entryFile = sd.open(ePath.c_str(), O_READ);
            if (!entryFile) return false;

            JsonDocument entryFilter;
            entryFilter["title"]   = true;
            entryFilter["content"] = true;

            JsonDocument entryDoc;
            err = deserializeJson(entryDoc, entryFile,
                      DeserializationOption::Filter(entryFilter));
            entryFile.close();
            if (err) return false;

            result_title   = entryDoc["title"].as<String>();
            result_content = entryDoc["content"].as<String>();
            return true;
        }
    }

    Serial.printf("[NOSQL] No match for '%s' in '%s'\n", keyword, category);
    return false;
}