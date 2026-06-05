// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_rss_cache.h — Persistent RSS headline cache
//
//  Stores the most recent parse of each RSS feed to SD so the
//  reader can fall back to cached headlines when WiFi is down
//  or a fetch fails. Without this, Pisces Moon's RSS reader
//  was effectively useless offline: every visit re-fetched
//  from the network and a fetch failure meant a blank screen.
//
//  ONE FILE PER FEED:
//    /rss_cache/<sanitized_name>.json
//
//    where <sanitized_name> is the feed's display name with
//    spaces replaced by underscores and any non-alphanumeric
//    characters dropped — e.g. "FLUID FORTUNE" → "FLUID_FORTUNE".
//
//  CONTENT FORMAT (JSON, parsed by ArduinoJson):
//    {
//      "name": "FLUID FORTUNE",
//      "fetched_uptime_s": 1234,
//      "items": [
//        { "t": "Headline 1", "d": "Description 1" },
//        { "t": "Headline 2", "d": "Description 2" }
//      ]
//    }
//
//  "fetched_uptime_s" is millis()/1000 at save time — useful only
//  WITHIN the same boot (it resets on reboot, so it can't tell
//  you the cache is stale across reboots, only within a session).
//  That's a known limitation of cacheing without an RTC; v1.3
//  can extend this with the time service once that's wired up
//  on the kiosks.
//
//  Both rss.cpp (T-Deck/Pager/Cardputer/C28P) and maxine_apps.cpp
//  call into this module. SD backend selection (SD_MMC vs SdFat)
//  is handled internally via the same macro pattern as nosql_store
//  and the wardrive engine.
// ─────────────────────────────────────────────

#pragma once

#include <Arduino.h>

// One cached headline. Used as the wire-type between the RSS UI
// code (which has its own RssItem/MaxRssItem structs internally)
// and the cache module. Caller is responsible for converting to/
// from its native struct before/after the call.
struct PmRssCachedItem {
    String title;
    String description;
};

// Save the most recently fetched headlines for a feed to SD.
// Overwrites any existing cache file for the same feed.
//
//   feed_name : the human-readable feed name (e.g. "FLUID FORTUNE").
//               Used to derive the on-disk filename.
//   items     : array of cached items to save (oldest-first or
//               newest-first — preserved as given).
//   count     : number of valid entries in `items`.
//
// Returns true on success, false on SD or write failure. On
// failure the cache file may be partial or absent — readers
// should tolerate a missing file as "no cache available".
bool pm_rss_cache_save(const char* feed_name,
                       const PmRssCachedItem* items, int count);

// Load the cached headlines for a feed from SD into the provided
// array. Returns the number of items loaded (0 if no cache file
// exists or it was empty), or -1 on a hard I/O error.
//
//   feed_name : as for save.
//   items     : caller-provided output array.
//   max_items : size of `items`. Older cache files with more
//               entries than `max_items` are truncated to fit.
int pm_rss_cache_load(const char* feed_name,
                      PmRssCachedItem* items, int max_items);

// Returns true if a cache file exists for the given feed, without
// reading its contents. Cheap header presence check.
bool pm_rss_cache_exists(const char* feed_name);
