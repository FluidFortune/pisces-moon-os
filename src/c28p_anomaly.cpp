// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_anomaly.cpp — Stationary Wardrive Anomaly Detection
//
//  PURPOSE:
//
//  The C28P has no GPS. Traditional wardriving correlates each
//  observation with a location, building a map of where networks
//  live. We can't do that here. But a stationary monitor has a
//  different, complementary capability: it sees the SAME LOCATION
//  over TIME. That lets us detect things mobile wardriving can't:
//
//    1. Devices that appeared recently (new neighbor, intruder)
//    2. Devices that disappeared (someone moved out, device died)
//    3. Devices that come and go on a schedule (visitors, workers)
//    4. Devices that probe for known SSIDs (wardriving in the area)
//    5. Devices that masquerade (same OUI prefix, different MAC)
//
//  ARCHITECTURE:
//
//    Baseline set: networks observed >= 3 times in the past
//                  24 hours (lives in NoSQL under "wd_baseline")
//
//    Current set: networks observed in the most recent scan
//
//    Each scan, compute deltas:
//      NEW       = current - baseline
//      VANISHED  = baseline - current (only after persistent absence)
//      KNOWN     = current ∩ baseline (normal)
//
//    NEW networks get logged to the "wd_anomaly" category with
//    timestamp. The UI can show them as alerts.
//
//  PREVIOUS-LOCATION LOOKUP:
//
//    When the C28P collects networks without GPS, it captures
//    only BSSID + signal strength. Users can later correlate
//    those BSSIDs against the Wigle.net database (or against
//    their own previously-collected mobile wardrive data) to
//    figure out where the network was originally seen. That
//    correlation happens in a separate analysis pass — this
//    file just provides the lookup function.
//
//  v1.2.1 SCOPE:
//
//    - Baseline maintenance (insert/decay)
//    - Anomaly detection on each scan
//    - Local lookup against NoSQL-stored prior data
//    - NO Wigle integration yet (planned v1.3 with API key)
//    - NO MAC-randomization detection yet (planned v1.3)
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <WiFi.h>
#include "nosql_store.h"

// ─────────────────────────────────────────────
//  Baseline state — small in-RAM cache backed by NoSQL
//
//  We keep up to 64 baseline networks in RAM. Each entry has
//  a BSSID (6 bytes), an observation count, and a last-seen
//  timestamp. On startup the baseline is loaded from NoSQL.
//  Periodically (every 100 scans, or when count > threshold)
//  the RAM cache is flushed back to NoSQL.
//
//  This is RAM-friendly: 64 * 16 bytes = 1KB. Cardputer-safe.
// ─────────────────────────────────────────────

#define BASELINE_MAX        64
#define BASELINE_THRESHOLD   3   // observations to be baseline
#define VANISH_THRESHOLD    10   // scans without seeing → vanished
#define BASELINE_FLUSH_SCANS 100

struct BaselineEntry {
    uint8_t  bssid[6];
    uint16_t obs_count;
    uint16_t miss_count;
    uint32_t last_seen_ms;
};

static BaselineEntry s_baseline[BASELINE_MAX];
static int s_baseline_count = 0;
static int s_scan_count = 0;
static bool s_baseline_loaded = false;

// ─────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────
static int find_baseline(const uint8_t bssid[6]) {
    for (int i = 0; i < s_baseline_count; i++) {
        if (memcmp(s_baseline[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void bssid_str(const uint8_t bssid[6], char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
}

// ─────────────────────────────────────────────
//  Persistence — flush in-RAM baseline to NoSQL
// ─────────────────────────────────────────────
static void baseline_flush() {
    // Encode each baseline entry as a small JSON record. NoSQL store
    // handles the file IO. We use the BSSID as the title (unique key).
    for (int i = 0; i < s_baseline_count; i++) {
        char title[18];
        bssid_str(s_baseline[i].bssid, title);

        char content[96];
        snprintf(content, sizeof(content),
                 "{\"obs\":%u,\"miss\":%u,\"last\":%lu}",
                 s_baseline[i].obs_count,
                 s_baseline[i].miss_count,
                 (unsigned long)s_baseline[i].last_seen_ms);

        nosql_save_entry("wd_baseline", title, content);
    }
    Serial.printf("[C28P-Anom] Baseline flushed: %d entries\n", s_baseline_count);
}

// ─────────────────────────────────────────────
//  Log an anomaly to NoSQL "wd_anomaly"
// ─────────────────────────────────────────────
static void log_anomaly(const char* event, const uint8_t bssid[6], int rssi,
                        const char* ssid) {
    char title[40];
    char bs[18];
    bssid_str(bssid, bs);
    snprintf(title, sizeof(title), "%lu %s %s",
             (unsigned long)(millis() / 1000), event, bs);

    char content[160];
    snprintf(content, sizeof(content),
             "{\"event\":\"%s\",\"bssid\":\"%s\",\"rssi\":%d,\"ssid\":\"%s\",\"t_ms\":%lu}",
             event, bs, rssi, ssid ? ssid : "",
             (unsigned long)millis());

    nosql_save_entry("wd_anomaly", title, content);
    Serial.printf("[C28P-Anom] %s %s (%d dBm) %s\n", event, bs, rssi, ssid ? ssid : "");
}

// ─────────────────────────────────────────────
//  PUBLIC API
// ─────────────────────────────────────────────

// Called once at C28P boot, after NoSQL is ready
void c28p_anomaly_init() {
    if (s_baseline_loaded) return;
    nosql_init("wd_baseline");
    nosql_init("wd_anomaly");

    // Load up to BASELINE_MAX entries from NoSQL into RAM
    int total = nosql_get_count("wd_baseline");
    int load_n = (total > BASELINE_MAX) ? BASELINE_MAX : total;
    String title;
    String content;
    for (int i = 0; i < load_n; i++) {
        if (!nosql_get_entry("wd_baseline", i, title, content)) continue;

        // Title is the BSSID — parse it
        unsigned int b[6];
        if (sscanf(title.c_str(), "%X:%X:%X:%X:%X:%X",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) continue;

        BaselineEntry& e = s_baseline[s_baseline_count];
        for (int k = 0; k < 6; k++) e.bssid[k] = (uint8_t)b[k];

        // Parse the count/miss/last from the JSON content
        unsigned int obs = 0, miss = 0;
        unsigned long last = 0;
        sscanf(content.c_str(), "{\"obs\":%u,\"miss\":%u,\"last\":%lu}",
               &obs, &miss, &last);
        e.obs_count = (uint16_t)obs;
        e.miss_count = (uint16_t)miss;
        e.last_seen_ms = (uint32_t)last;
        s_baseline_count++;
    }
    Serial.printf("[C28P-Anom] Baseline loaded: %d entries from NoSQL\n",
                  s_baseline_count);
    s_baseline_loaded = true;
}

// Process a single scan result. Call once per network per scan.
// Returns true if this observation is an anomaly (new network).
bool c28p_anomaly_observe(const uint8_t bssid[6], int rssi, const char* ssid) {
    if (!s_baseline_loaded) c28p_anomaly_init();

    int idx = find_baseline(bssid);
    if (idx < 0) {
        // Brand new BSSID — add to baseline cache
        if (s_baseline_count < BASELINE_MAX) {
            BaselineEntry& e = s_baseline[s_baseline_count];
            memcpy(e.bssid, bssid, 6);
            e.obs_count = 1;
            e.miss_count = 0;
            e.last_seen_ms = millis();
            s_baseline_count++;
        }
        // It's only an ANOMALY if we've been running long enough
        // to have a meaningful baseline. First 10 scans = warm-up.
        if (s_scan_count > 10) {
            log_anomaly("NEW", bssid, rssi, ssid);
            return true;
        }
        return false;
    }

    // Existing entry — update
    BaselineEntry& e = s_baseline[idx];
    e.obs_count++;
    e.miss_count = 0;
    e.last_seen_ms = millis();
    return false;
}

// Called at the end of each scan to age the baseline.
// Networks not seen for VANISH_THRESHOLD scans get logged as VANISHED
// and removed from the cache (will reappear as NEW if they come back).
void c28p_anomaly_scan_complete(const uint8_t seen_bssids[][6], int seen_count) {
    s_scan_count++;

    for (int i = 0; i < s_baseline_count; ) {
        // Check if this baseline entry was seen this scan
        bool seen = false;
        for (int j = 0; j < seen_count; j++) {
            if (memcmp(s_baseline[i].bssid, seen_bssids[j], 6) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            i++;
            continue;
        }

        s_baseline[i].miss_count++;

        if (s_baseline[i].miss_count > VANISH_THRESHOLD &&
            s_baseline[i].obs_count >= BASELINE_THRESHOLD) {
            // Was an established baseline entry, now gone too long
            log_anomaly("VANISHED", s_baseline[i].bssid, 0, "");
            // Remove from cache
            if (i < s_baseline_count - 1) {
                s_baseline[i] = s_baseline[s_baseline_count - 1];
            }
            s_baseline_count--;
        } else {
            i++;
        }
    }

    if (s_scan_count % BASELINE_FLUSH_SCANS == 0) {
        baseline_flush();
    }
}

// ─────────────────────────────────────────────
//  PREVIOUS-LOCATION LOOKUP
//
//  Search prior wardrive data (mobile or stationary) for any
//  observation of this BSSID. Returns the most recent location
//  if found, else empty string.
//
//  Lookups happen against the "wardrive" category in NoSQL —
//  the same store that the mobile-wardrive devices write to.
//  If the user has previously wardriven this network on a
//  T-Deck or T-LoraPager and synced the data, the C28P can
//  recover the location.
// ─────────────────────────────────────────────
String c28p_lookup_prior_location(const uint8_t bssid[6]) {
    char target[18];
    bssid_str(bssid, target);

    int total = nosql_get_count("wardrive");
    String title;
    String content;
    String best;
    uint32_t best_age = UINT32_MAX;

    // Walk newest-first, but the NoSQL store doesn't guarantee order.
    // For v1.2.1 we scan everything and pick the most recent match.
    // Performance: 1000 entries × ~10ms each = 10s. Acceptable for
    // user-initiated lookup; not for per-scan correlation.
    for (int i = 0; i < total; i++) {
        if (!nosql_get_entry("wardrive", i, title, content)) continue;
        if (content.indexOf(target) < 0) continue;

        // Found a hit. Extract lat/lon/timestamp from JSON if present.
        // Format expected: {"bssid":"...","lat":N,"lon":N,"t_ms":N}
        int t_ms = 0;
        int idx = content.indexOf("\"t_ms\":");
        if (idx >= 0) t_ms = content.substring(idx + 7).toInt();

        uint32_t age = (uint32_t)(millis() - (uint32_t)t_ms);
        if (age < best_age) {
            best_age = age;
            best = content;
        }
    }
    return best;
}

// Diagnostic — return number of baseline entries currently tracked
int c28p_anomaly_baseline_size() {
    return s_baseline_count;
}

int c28p_anomaly_recent_alerts() {
    return nosql_get_count("wd_anomaly");
}

#endif // DEVICE_C28P || DEVICE_MAXINE