// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com

/**
 * Motion-assisted LoRa node localization.
 *
 * The estimator intentionally uses streaming statistical moments instead of
 * retaining packet history. That keeps Cardputer ADV memory deterministic and
 * avoids heap pressure on its no-PSRAM build. Only direct packets contribute
 * to the fit; relay RSSI belongs to the relay, not the origin node.
 *
 * The reported point is an RSSI-weighted centroid of observer positions. The
 * uncertainty radius combines weighted spatial spread, an RSSI path-loss
 * bound, and a penalty for poor route geometry. It is an honest "likely area,"
 * not a claim of GPS-like transmitter precision.
 */

#include "pm_lora_locator.h"

#include <math.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifdef DEVICE_NO_PSRAM
// Cardputer keeps only the twelve freshest estimates. Raw Wardrive CSV
// logging is unaffected; this cap applies only to the live statistical fit.
// Float moments are ample for a drive-scale local tangent plane and save
// several kilobytes of internal SRAM versus double precision.
static constexpr size_t PM_LOCATOR_MAX_NODES = 12;
#else
// The raw CSV still records every node. The live fit keeps the freshest 32,
// which is ample for a moving local survey without spending ~20 KB of
// internal BSS on devices whose PSRAM is better reserved for app payloads.
static constexpr size_t PM_LOCATOR_MAX_NODES = 32;
#endif
using LocatorAccum = float;

static constexpr double METERS_PER_DEGREE = 111320.0;
static constexpr float  MIN_SAMPLE_MOVE_M = 8.0f;
static constexpr uint32_t STATIONARY_REFRESH_MS = 30000UL;

struct LocatorNode {
    uint32_t node_id;
    uint32_t last_seen_ms;
    uint32_t total_packets;

    double origin_lat;
    double origin_lon;
    double lon_scale;

    // RSSI-weighted moments in a local tangent plane (meters).
    LocatorAccum sum_w;
    LocatorAccum sum_wx;
    LocatorAccum sum_wy;
    LocatorAccum sum_wx2;
    LocatorAccum sum_wy2;
    LocatorAccum sum_wxy;

    // Unweighted route moments, used to measure observation geometry.
    LocatorAccum sum_x;
    LocatorAccum sum_y;
    LocatorAccum sum_x2;
    LocatorAccum sum_y2;
    LocatorAccum sum_xy;

    float min_x, max_x, min_y, max_y;
    float last_x, last_y;
    uint32_t last_sample_ms;

    float strongest_lat;
    float strongest_lon;
    float last_snr;
    int16_t best_rssi;
    int16_t weakest_rssi;
    int16_t last_rssi;
    uint16_t direct_samples;
    uint8_t min_hops;
    bool has_origin;

    PMLoraLocationEstimate estimate;
};

static LocatorNode s_nodes[PM_LOCATOR_MAX_NODES];
static size_t s_node_count = 0;

// Static mutex: no heap allocation, including on Cardputer ADV.
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex = nullptr;
static portMUX_TYPE s_mutex_init_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t locatorMutex() {
    if (s_mutex) return s_mutex;
    portENTER_CRITICAL(&s_mutex_init_mux);
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    portEXIT_CRITICAL(&s_mutex_init_mux);
    return s_mutex;
}

static double wrapDeltaLon(double delta) {
    while (delta > 180.0) delta -= 360.0;
    while (delta < -180.0) delta += 360.0;
    return delta;
}

static float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static LocatorNode* findOrAddNode(uint32_t node_id) {
    for (size_t i = 0; i < s_node_count; ++i) {
        if (s_nodes[i].node_id == node_id) return &s_nodes[i];
    }

    size_t slot = s_node_count;
    if (slot < PM_LOCATOR_MAX_NODES) {
        ++s_node_count;
    } else {
        slot = 0;
        for (size_t i = 1; i < PM_LOCATOR_MAX_NODES; ++i) {
            if ((int32_t)(s_nodes[i].last_seen_ms -
                          s_nodes[slot].last_seen_ms) < 0) {
                slot = i;
            }
        }
    }

    memset(&s_nodes[slot], 0, sizeof(s_nodes[slot]));
    s_nodes[slot].node_id = node_id;
    s_nodes[slot].best_rssi = -32768;
    s_nodes[slot].weakest_rssi = 32767;
    s_nodes[slot].min_hops = 255;
    return &s_nodes[slot];
}

static float pathLossDistanceM(int rssi) {
    // Coarse LongFast/SX1262 model: 22 dBm TX, 32 dB loss at 1 m,
    // path-loss exponent 2.7. It is used as an uncertainty floor, not
    // as a literal ranging measurement.
    float bounded = clampf((float)rssi, -125.0f, -35.0f);
    float exponent = (22.0f - 32.0f - bounded) / 27.0f;
    return clampf(powf(10.0f, exponent), 15.0f, 20000.0f);
}

static void updateEstimateMetadata(LocatorNode& n) {
    PMLoraLocationEstimate& e = n.estimate;
    e.node_id = n.node_id;
    e.last_seen_ms = n.last_seen_ms;
    e.total_packets = n.total_packets;
    e.last_snr = n.last_snr;
    e.best_rssi = n.best_rssi;
    e.last_rssi = n.last_rssi;
    e.direct_samples = n.direct_samples;
    e.min_hops = n.min_hops;
}

static void updateEstimate(LocatorNode& n) {
    PMLoraLocationEstimate& e = n.estimate;
    memset(&e, 0, sizeof(e));
    updateEstimateMetadata(n);

    if (!n.has_origin || n.direct_samples == 0 || n.sum_w <= 0.0) {
        e.position_valid = false;
        e.confidence = PM_LORA_CONF_NONE;
        return;
    }

    double cx = n.sum_wx / n.sum_w;
    double cy = n.sum_wy / n.sum_w;
    double weighted_var_x = n.sum_wx2 / n.sum_w - cx * cx;
    double weighted_var_y = n.sum_wy2 / n.sum_w - cy * cy;
    if (weighted_var_x < 0.0) weighted_var_x = 0.0;
    if (weighted_var_y < 0.0) weighted_var_y = 0.0;
    float weighted_rms = sqrtf((float)(weighted_var_x + weighted_var_y));

    const double count = (double)n.direct_samples;
    double mean_x = n.sum_x / count;
    double mean_y = n.sum_y / count;
    double var_x = n.sum_x2 / count - mean_x * mean_x;
    double var_y = n.sum_y2 / count - mean_y * mean_y;
    double cov_xy = n.sum_xy / count - mean_x * mean_y;
    if (var_x < 0.0) var_x = 0.0;
    if (var_y < 0.0) var_y = 0.0;

    double trace = var_x + var_y;
    double discriminant = (var_x - var_y) * (var_x - var_y) +
                          4.0 * cov_xy * cov_xy;
    double root = sqrt(discriminant < 0.0 ? 0.0 : discriminant);
    double major = (trace + root) * 0.5;
    double minor = (trace - root) * 0.5;
    float geometry = 0.0f;
    if (major > 1.0) {
        if (minor < 0.0) minor = 0.0;
        geometry = clampf(sqrtf((float)(minor / major)), 0.0f, 1.0f);
    }

    float span_x = n.max_x - n.min_x;
    float span_y = n.max_y - n.min_y;
    float route_span = sqrtf(span_x * span_x + span_y * span_y);

    float signal_floor = pathLossDistanceM(n.best_rssi) * 0.65f;
    float radius = fmaxf(25.0f, fmaxf(weighted_rms * 1.8f, signal_floor));
    // Straight-line passes have poor cross-track observability. Expand the
    // uncertainty instead of pretending the centroid is a precise fix.
    radius *= 1.0f + (1.0f - geometry) * 0.8f;
    radius = clampf(radius, 25.0f, 50000.0f);

    int rssi_span = (int)n.best_rssi - (int)n.weakest_rssi;
    pm_lora_confidence_t confidence = PM_LORA_CONF_NONE;
    if (n.direct_samples >= 3 && route_span >= 20.0f) {
        confidence = PM_LORA_CONF_LOW;
    }
    if (n.direct_samples >= 6 && route_span >= 75.0f &&
        geometry >= 0.12f && rssi_span >= 8) {
        confidence = PM_LORA_CONF_MEDIUM;
    }
    if (n.direct_samples >= 10 && route_span >= 150.0f &&
        geometry >= 0.30f && rssi_span >= 14) {
        confidence = PM_LORA_CONF_HIGH;
    }

    double lon_scale = fabs(n.lon_scale) < 1.0 ? 1.0 : n.lon_scale;
    e.latitude = (float)(n.origin_lat + cy / METERS_PER_DEGREE);
    double estimated_lon = n.origin_lon + cx / lon_scale;
    while (estimated_lon > 180.0) estimated_lon -= 360.0;
    while (estimated_lon < -180.0) estimated_lon += 360.0;
    e.longitude = (float)estimated_lon;
    e.radius_m = radius;
    e.route_span_m = route_span;
    e.geometry_score = geometry;
    e.strongest_latitude = n.strongest_lat;
    e.strongest_longitude = n.strongest_lon;
    e.confidence = confidence;
    e.position_valid = true;
}

void pm_lora_locator_reset() {
    SemaphoreHandle_t mutex = locatorMutex();
    if (!mutex) return;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    memset(s_nodes, 0, sizeof(s_nodes));
    s_node_count = 0;
    xSemaphoreGive(mutex);
}

void pm_lora_locator_observe(uint32_t node_id,
                             double observer_latitude,
                             double observer_longitude,
                             int rssi,
                             float snr,
                             uint8_t hops,
                             uint32_t now_ms) {
    if (node_id == 0) return;
    if (observer_latitude < -90.0 || observer_latitude > 90.0 ||
        observer_longitude < -180.0 || observer_longitude > 180.0) {
        return;
    }

    SemaphoreHandle_t mutex = locatorMutex();
    if (!mutex) return;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(5)) != pdTRUE) return;

    LocatorNode* node = findOrAddNode(node_id);
    LocatorNode& n = *node;
    n.last_seen_ms = now_ms;
    n.total_packets++;
    n.last_rssi = (int16_t)rssi;
    n.last_snr = snr;
    if (hops < n.min_hops) n.min_hops = hops;

    // Relayed packets identify the origin node, but their RSSI describes the
    // final relay. Keep presence metadata and exclude them from localization.
    if (hops != 0) {
        updateEstimateMetadata(n);
        xSemaphoreGive(mutex);
        return;
    }

    if (!n.has_origin) {
        n.origin_lat = observer_latitude;
        n.origin_lon = observer_longitude;
        n.lon_scale = METERS_PER_DEGREE *
                      cos(observer_latitude * M_PI / 180.0);
        n.min_x = n.max_x = 0.0f;
        n.min_y = n.max_y = 0.0f;
        n.last_x = n.last_y = 0.0f;
        n.has_origin = true;
    }

    double delta_lon = wrapDeltaLon(observer_longitude - n.origin_lon);
    float x = (float)(delta_lon * n.lon_scale);
    float y = (float)((observer_latitude - n.origin_lat) *
                      METERS_PER_DEGREE);
    float dx = x - n.last_x;
    float dy = y - n.last_y;
    float moved = sqrtf(dx * dx + dy * dy);

    bool accept = (n.direct_samples == 0) ||
                  (moved >= MIN_SAMPLE_MOVE_M) ||
                  (now_ms - n.last_sample_ms >= STATIONARY_REFRESH_MS);

    bool new_best = rssi > n.best_rssi;
    if (new_best) {
        n.best_rssi = (int16_t)rssi;
        n.strongest_lat = (float)observer_latitude;
        n.strongest_lon = (float)observer_longitude;
    }

    if (accept) {
        // Every 20 dB changes weight by 10x. This favors the closest pass
        // without allowing one noisy packet to completely dominate.
        float bounded_rssi = clampf((float)rssi, -120.0f, -40.0f);
        float weight = powf(10.0f, (bounded_rssi + 100.0f) / 20.0f);

        n.sum_w += weight;
        n.sum_wx += weight * x;
        n.sum_wy += weight * y;
        n.sum_wx2 += weight * x * x;
        n.sum_wy2 += weight * y * y;
        n.sum_wxy += weight * x * y;

        n.sum_x += x;
        n.sum_y += y;
        n.sum_x2 += x * x;
        n.sum_y2 += y * y;
        n.sum_xy += x * y;

        if (n.direct_samples == 0) {
            n.min_x = n.max_x = x;
            n.min_y = n.max_y = y;
        } else {
            if (x < n.min_x) n.min_x = x;
            if (x > n.max_x) n.max_x = x;
            if (y < n.min_y) n.min_y = y;
            if (y > n.max_y) n.max_y = y;
        }
        n.last_x = x;
        n.last_y = y;
        n.last_sample_ms = now_ms;
        if (rssi < n.weakest_rssi) n.weakest_rssi = (int16_t)rssi;
        if (n.direct_samples < UINT16_MAX) n.direct_samples++;
    }

    if (accept || new_best) updateEstimate(n);
    else updateEstimateMetadata(n);
    xSemaphoreGive(mutex);
}

size_t pm_lora_locator_snapshot(PMLoraLocationEstimate* out,
                                size_t max_estimates) {
    if (!out || max_estimates == 0) return 0;
    SemaphoreHandle_t mutex = locatorMutex();
    if (!mutex) return 0;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return 0;

    size_t count = s_node_count < max_estimates ? s_node_count : max_estimates;
    for (size_t i = 0; i < count; ++i) out[i] = s_nodes[i].estimate;
    xSemaphoreGive(mutex);

    // Small fixed arrays: insertion sort keeps the freshest node first and
    // avoids pulling in dynamic containers on Cardputer.
    for (size_t i = 1; i < count; ++i) {
        PMLoraLocationEstimate key = out[i];
        size_t j = i;
        while (j > 0 &&
               (int32_t)(key.last_seen_ms - out[j - 1].last_seen_ms) > 0) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    return count;
}

const char* pm_lora_confidence_label(pm_lora_confidence_t confidence) {
    switch (confidence) {
        case PM_LORA_CONF_LOW:    return "LOW";
        case PM_LORA_CONF_MEDIUM: return "MED";
        case PM_LORA_CONF_HIGH:   return "HIGH";
        default:                  return "COLLECT";
    }
}
