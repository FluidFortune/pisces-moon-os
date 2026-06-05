// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_rss_cache.cpp — Persistent RSS headline cache (SD-backed)
//
//  Implements pm_rss_cache.h. Backend selection mirrors nosql_store
//  and c28p_wardrive_engine: SD_MMC on the C28P (SDIO 4-bit), SdFat
//  on every other device (SPI). Same RC_FS / RC_FILE macro pattern
//  used by the other SD-touching files.
//
//  JSON is parsed and written via ArduinoJson. We size the static
//  document at 6 KB which comfortably holds 20 cached items at
//  ~250 chars each (typical RSS title+description footprint). If
//  a feed has shorter items we just use less of the buffer.
// ─────────────────────────────────────────────

#include "pm_rss_cache.h"
#include <ArduinoJson.h>

// ── SD backend ───
#ifdef DEVICE_C28P
  #include <FS.h>
  #include <SD_MMC.h>
  #define RC_FS         SD_MMC
  #define RC_FILE       fs::File
  #define RC_OPEN_READ  FILE_READ
  #define RC_OPEN_WRITE FILE_WRITE          // truncates by default
#else
  #include "SdFat.h"
  extern SdFat sd;
  #define RC_FS         sd
  #define RC_FILE       FsFile
  #define RC_OPEN_READ  O_READ
  #define RC_OPEN_WRITE (O_WRITE | O_CREAT | O_TRUNC)
#endif

// ── Path helpers ───
//
// Sanitize a feed name into a safe filename: alnum chars pass
// through, everything else becomes '_'. Truncate to 28 chars so
// the full path stays well under typical FAT limits (8.3 isn't
// required on SD ExFAT/FAT32 but stay polite).
static void sanitize_name(const char* in, char* out, size_t outsz) {
    size_t w = 0;
    for (size_t i = 0; in[i] && w < outsz - 1 && w < 28; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) {
            out[w++] = c;
        } else if (c == ' ' || c == '-' || c == '_') {
            out[w++] = '_';
        }
        // Everything else dropped silently.
    }
    out[w] = 0;
    if (w == 0) {           // pathological empty name
        strncpy(out, "feed", outsz);
        out[outsz - 1] = 0;
    }
}

static void cache_path(const char* feed_name, char* path, size_t path_sz) {
    char san[32];
    sanitize_name(feed_name, san, sizeof(san));
    snprintf(path, path_sz, "/rss_cache/%s.json", san);
}

static bool ensure_dir() {
    // The /rss_cache directory must exist before writing files into
    // it. SD_MMC's mkdir is fs::FS-style; SdFat's is too. Both return
    // true if the dir already exists or was created successfully.
    if (RC_FS.exists("/rss_cache")) return true;
    return RC_FS.mkdir("/rss_cache");
}

// ─────────────────────────────────────────────
//  pm_rss_cache_save
// ─────────────────────────────────────────────
bool pm_rss_cache_save(const char* feed_name,
                       const PmRssCachedItem* items, int count) {
    if (!feed_name || !items || count <= 0) return false;
    if (!ensure_dir()) {
        Serial.println("[RSS-CACHE] mkdir /rss_cache failed");
        return false;
    }

    char path[64];
    cache_path(feed_name, path, sizeof(path));

    // Build the JSON doc. 6 KB is a comfortable cap for 20 items;
    // we'll cap at 20 here too to match MAX_HEADLINES in rss.cpp.
    if (count > 20) count = 20;
    StaticJsonDocument<6144> doc;
    doc["name"] = feed_name;
    doc["fetched_uptime_s"] = (uint32_t)(millis() / 1000);
    JsonArray arr = doc.createNestedArray("items");
    for (int i = 0; i < count; i++) {
        JsonObject o = arr.createNestedObject();
        // Truncate aggressively to keep total size under the doc cap.
        // Titles past 200 chars and descriptions past 400 are noise
        // at the kiosk's display resolution anyway.
        String t = items[i].title;
        String d = items[i].description;
        if (t.length() > 200) t = t.substring(0, 200);
        if (d.length() > 400) d = d.substring(0, 400);
        o["t"] = t;
        o["d"] = d;
    }

    RC_FILE f = RC_FS.open(path, RC_OPEN_WRITE);
    if (!f) {
        Serial.printf("[RSS-CACHE] open(write) failed: %s\n", path);
        return false;
    }
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) {
        Serial.printf("[RSS-CACHE] write 0 bytes (doc too big?): %s\n", path);
        return false;
    }
    Serial.printf("[RSS-CACHE] saved %d items (%u bytes) -> %s\n",
                  count, (unsigned)written, path);
    return true;
}

// ─────────────────────────────────────────────
//  pm_rss_cache_load
// ─────────────────────────────────────────────
int pm_rss_cache_load(const char* feed_name,
                      PmRssCachedItem* items, int max_items) {
    if (!feed_name || !items || max_items <= 0) return -1;

    char path[64];
    cache_path(feed_name, path, sizeof(path));
    if (!RC_FS.exists(path)) {
        return 0;   // no cache yet — not an error
    }

    RC_FILE f = RC_FS.open(path, RC_OPEN_READ);
    if (!f) {
        Serial.printf("[RSS-CACHE] open(read) failed: %s\n", path);
        return -1;
    }
    StaticJsonDocument<6144> doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[RSS-CACHE] parse failed (%s): %s\n",
                      err.c_str(), path);
        return -1;
    }

    JsonArray arr = doc["items"].as<JsonArray>();
    int loaded = 0;
    for (JsonObject o : arr) {
        if (loaded >= max_items) break;
        items[loaded].title       = String((const char*)(o["t"] | ""));
        items[loaded].description = String((const char*)(o["d"] | ""));
        if (items[loaded].title.length() > 0) loaded++;
    }
    Serial.printf("[RSS-CACHE] loaded %d items <- %s\n", loaded, path);
    return loaded;
}

// ─────────────────────────────────────────────
//  pm_rss_cache_exists
// ─────────────────────────────────────────────
bool pm_rss_cache_exists(const char* feed_name) {
    if (!feed_name) return false;
    char path[64];
    cache_path(feed_name, path, sizeof(path));
    return RC_FS.exists(path);
}
