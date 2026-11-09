// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com

#ifndef PM_LORA_LOCATOR_H
#define PM_LORA_LOCATOR_H

#include <Arduino.h>

// Motion-assisted LoRa location estimate. This is deliberately described as
// an estimate: RSSI has no bearing information and is strongly affected by
// terrain, buildings, antennas, and the vehicle itself.
typedef enum : uint8_t {
    PM_LORA_CONF_NONE   = 0,  // heard, but not enough separated observations
    PM_LORA_CONF_LOW    = 1,
    PM_LORA_CONF_MEDIUM = 2,
    PM_LORA_CONF_HIGH   = 3,
} pm_lora_confidence_t;

struct PMLoraLocationEstimate {
    uint32_t node_id;
    uint32_t last_seen_ms;
    uint32_t total_packets;

    float latitude;       // RSSI-weighted observer-position centroid
    float longitude;
    float radius_m;       // conservative uncertainty radius
    float route_span_m;   // spatial extent of accepted observations
    float geometry_score; // 0=straight-line geometry, 1=well distributed

    float strongest_latitude;
    float strongest_longitude;
    float last_snr;

    int16_t best_rssi;
    int16_t last_rssi;
    uint16_t direct_samples;
    uint8_t min_hops;
    pm_lora_confidence_t confidence;
    bool position_valid;
};

// Clear all accumulated node estimates. The locator owns only fixed-size
// static storage; reset performs no allocation.
void pm_lora_locator_reset();

// Feed one heard Meshtastic packet into the locator. Relayed packets update
// node presence metadata but are excluded from position fitting because RSSI
// describes the final relay hop, not the origin node.
void pm_lora_locator_observe(uint32_t node_id,
                             double observer_latitude,
                             double observer_longitude,
                             int rssi,
                             float snr,
                             uint8_t hops,
                             uint32_t now_ms);

// Copy the newest estimates into caller storage, ordered by most recently
// heard. Safe to call from the UI core while Wardrive updates on Core 0.
size_t pm_lora_locator_snapshot(PMLoraLocationEstimate* out,
                                size_t max_estimates);

const char* pm_lora_confidence_label(pm_lora_confidence_t confidence);

#endif // PM_LORA_LOCATOR_H
