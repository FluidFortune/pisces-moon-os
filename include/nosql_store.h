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

#ifndef NOSQL_STORE_H
#define NOSQL_STORE_H

#include <Arduino.h>

// ─────────────────────────────────────────────
//  PISCES MOON NoSQL STORE v1.0
//  A lightweight JSON document store built on
//  SdFat. Each category is a folder containing
//  an index.json and numbered entry files.
//
//  Directory layout on SD card:
//    /data/
//      gemini/
//        index.json
//        entry_001.json
//        entry_002.json
//      medical/
//        index.json
//        burns.json
//      survival/
//        index.json
//        fire.json
// ─────────────────────────────────────────────

// Maximum lengths — kept small to respect PSRAM
#define NOSQL_MAX_TITLE     64
#define NOSQL_MAX_CATEGORY  32
#define NOSQL_MAX_ENTRIES   999

// ── Core Lifecycle ──────────────────────────

// Call once per category before using it.
// Creates /data/<category>/ and index.json if missing.
bool nosql_init(const char* category);

// ── Write ────────────────────────────────────

// Save a new entry. Auto-increments entry ID.
// title    : short label shown in the browser (max 63 chars)
// content  : full text body of the entry
// tags     : comma-separated keywords e.g. "burn,injury,fire"
// Returns true on success.
bool nosql_save_entry(const char* category,
                      const char* title,
                      const char* content,
                      const char* tags = "");

// ── Read ─────────────────────────────────────

// Returns total number of entries in a category.
int nosql_get_count(const char* category);

// Load a specific entry by its 0-based index.
// Populates title and content Strings.
// Returns true on success.
bool nosql_get_entry(const char* category,
                     int index,
                     String &title,
                     String &content);

// ── Search ───────────────────────────────────

// Searches index.json for a keyword match in title or tags.
// Populates result_title and result_content if found.
// Returns true if at least one match found (returns first match).
bool nosql_search(const char* category,
                  const char* keyword,
                  String &result_title,
                  String &result_content);

// ── Utility ──────────────────────────────────

// Returns the full SD path for a category folder.
// e.g. nosql_category_path("medical") -> "/data/medical"
String nosql_category_path(const char* category);

#endif