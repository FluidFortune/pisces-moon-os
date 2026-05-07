/*
 * Pisces Moon OS — pm_promiscuous.h
 * True 802.11 promiscuous mode capture for ESP32 / ESP32-S3.
 *
 * Copyright (C) 2026 Eric Becker / Fluid Fortune
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ── SAFETY FIX vs original ──────────────────────────────────────────────────
 * The original design called the user callback directly from the WiFi ISR
 * context. This is unsafe — Serial.println(), mutexes, and blocking I/O
 * crash when called from WiFi task context.
 *
 * This version uses a FreeRTOS queue between the WiFi task and app task:
 *
 *   WiFi task (Core 0):
 *     _pm_promiscuous_cb() → parse frame → xQueueSendFromISR() → queue
 *
 *   App task (Core 1, called from bridge_app main loop):
 *     pm_promiscuous_drain() → xQueueReceive() → user callback → Serial TX
 *
 * Serial.println() and spi_mutex are safe in the user callback because
 * it runs on the app task after pm_promiscuous_drain() pulls from the queue.
 *
 * ── MODE SELECTION ──────────────────────────────────────────────────────────
 * Promiscuous mode is only available in Bridge mode (activated by host).
 * Standalone wardrive uses scan mode (WiFi.scanNetworks).
 * These two modes are mutually exclusive — wardrive.cpp checks
 * wardrive_promiscuous_active before touching the WiFi radio.
 *
 * ── BANDWIDTH ───────────────────────────────────────────────────────────────
 * Busy environment: ~100 frames/sec × 200 bytes JSON = ~20 KB/s.
 * Bridge serial runs at 921600 baud (~95 KB/s) — comfortable headroom.
 * Use pm_promiscuous_set_subtype_mask() to filter if needed.
 */

#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ── Configuration ──────────────────────────────────────────────────────────
#define PM_CHANNEL_HOP_MS    250    // ms between channel hops
#define PM_CHANNEL_MIN       1
#define PM_CHANNEL_MAX       13     // 11=US/FCC  13=EU  14=JP
#define PM_PKT_QUEUE_SIZE    64     // FreeRTOS queue depth

// ── Frame subtype identifiers ───────────────────────────────────────────────
typedef enum {
    PM_SUB_BEACON,
    PM_SUB_PROBE_REQ,
    PM_SUB_PROBE_RESP,
    PM_SUB_AUTH,
    PM_SUB_DEAUTH,
    PM_SUB_ASSOC_REQ,
    PM_SUB_ASSOC_RESP,
    PM_SUB_DISASSOC,
    PM_SUB_ACTION,
    PM_SUB_DATA,
    PM_SUB_OTHER,
} pm_subtype_t;

// ── Parsed packet info ──────────────────────────────────────────────────────
// Fixed-size flat struct — safe to copy into FreeRTOS queue from ISR.
typedef struct {
    pm_subtype_t subtype;
    char         subtype_str[16];
    char         src_mac_str[18];
    char         dst_mac_str[18];
    char         bssid_str[18];
    char         ssid[33];
    uint8_t      channel;
    int8_t       rssi;
    uint16_t     seq_num;
    uint32_t     millis_at_capture;  // filled on drain, not in ISR
} pm_pkt_info_t;

typedef void (*pm_pkt_callback_t)(const pm_pkt_info_t*);

// ── Internal state ──────────────────────────────────────────────────────────
static pm_pkt_callback_t _pm_user_cb         = nullptr;
static QueueHandle_t     _pm_queue           = nullptr;
static volatile bool     _pm_active          = false;
static volatile uint8_t  _pm_current_channel = 1;
static volatile uint32_t _pm_last_hop        = 0;
static volatile uint32_t _pm_pkt_count       = 0;
static volatile uint32_t _pm_pkt_dropped     = 0;
static uint32_t          _pm_subtype_mask    = 0xFFFF;

// ── Public visualizer counters (read by Bridge visualizer) ──────────────────
// Defined once in wardrive.cpp — extern here so any includer can read them
extern volatile uint32_t pm_frames_per_sec;
extern volatile uint32_t pm_beacon_count;
extern volatile uint32_t pm_probe_req_count;
extern volatile uint32_t pm_deauth_count;
extern volatile uint32_t pm_other_count;

// ── Helpers ─────────────────────────────────────────────────────────────────
static inline void _pm_mac_to_str(const uint8_t* mac, char* out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void _pm_subtype_to_str(uint8_t sub, uint8_t type, char* out) {
    if (type == 0) {
        switch (sub) {
            case 0x0: strncpy(out, "assoc-req",   15); return;
            case 0x1: strncpy(out, "assoc-resp",  15); return;
            case 0x2: strncpy(out, "reassoc-req", 15); return;
            case 0x3: strncpy(out, "reassoc-rsp", 15); return;
            case 0x4: strncpy(out, "probe-req",   15); return;
            case 0x5: strncpy(out, "probe-resp",  15); return;
            case 0x8: strncpy(out, "beacon",      15); return;
            case 0xA: strncpy(out, "disassoc",    15); return;
            case 0xB: strncpy(out, "auth",        15); return;
            case 0xC: strncpy(out, "deauth",      15); return;
            case 0xD: strncpy(out, "action",      15); return;
            default:  strncpy(out, "mgmt",        15); return;
        }
    } else if (type == 1) { strncpy(out, "ctrl",  15); }
    else if   (type == 2) { strncpy(out, "data",  15); }
    else                   { strncpy(out, "other", 15); }
}

static void _pm_extract_ssid(const uint8_t* payload, size_t len, char* out) {
    size_t i = 0;
    while (i + 2 <= len) {
        uint8_t tag = payload[i];
        uint8_t tl  = payload[i + 1];
        if (i + 2 + tl > len) break;
        if (tag == 0) {
            int n = tl < 32 ? tl : 32;
            memcpy(out, &payload[i + 2], n);
            out[n] = 0;
            return;
        }
        i += 2 + tl;
    }
    out[0] = 0;
}

// ── WiFi ISR callback — WiFi task context, NOT app task ─────────────────────
// NO Serial, NO mutex, NO blocking calls. Only queue writes.
static void IRAM_ATTR _pm_promiscuous_cb(void* buf,
                                          wifi_promiscuous_pkt_type_t type) {
    if (!_pm_active || !_pm_queue) return;
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t* raw = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* p = raw->payload;
    int len = raw->rx_ctrl.sig_len;
    if (len < 24) return;

    uint8_t fctl0        = p[0];
    uint8_t fctl_type    = (fctl0 >> 2) & 0x3;
    uint8_t fctl_subtype = (fctl0 >> 4) & 0xF;
    if (!(_pm_subtype_mask & (1u << fctl_subtype))) return;

    pm_pkt_info_t info;

    _pm_subtype_to_str(fctl_subtype, fctl_type, info.subtype_str);
    info.subtype_str[15]   = 0;
    info.channel           = raw->rx_ctrl.channel;
    info.rssi              = (int8_t)raw->rx_ctrl.rssi;
    info.millis_at_capture = 0;  // millis() unsafe from ISR — stamped on drain

    switch (fctl_subtype) {
        case 0x4: info.subtype = PM_SUB_PROBE_REQ;  break;
        case 0x5: info.subtype = PM_SUB_PROBE_RESP; break;
        case 0x8: info.subtype = PM_SUB_BEACON;     break;
        case 0xA: info.subtype = PM_SUB_DISASSOC;   break;
        case 0xB: info.subtype = PM_SUB_AUTH;       break;
        case 0xC: info.subtype = PM_SUB_DEAUTH;     break;
        case 0xD: info.subtype = PM_SUB_ACTION;     break;
        default:  info.subtype = PM_SUB_OTHER;      break;
    }

    _pm_mac_to_str(&p[4],  info.dst_mac_str);
    _pm_mac_to_str(&p[10], info.src_mac_str);
    _pm_mac_to_str(&p[16], info.bssid_str);
    info.seq_num = ((uint16_t)p[22] | ((uint16_t)p[23] << 8)) >> 4;

    int ssid_off = -1;
    if (fctl_subtype == 0x4) ssid_off = 24;
    else if (fctl_subtype == 0x8 || fctl_subtype == 0x5) ssid_off = 36;
    if (ssid_off > 0 && ssid_off < len) {
        _pm_extract_ssid(&p[ssid_off], len - ssid_off, info.ssid);
    } else {
        info.ssid[0] = 0;
    }

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(_pm_queue, &info, &woken) != pdTRUE) {
        _pm_pkt_dropped++;
    } else {
        _pm_pkt_count++;
    }
    if (woken) portYIELD_FROM_ISR();
}

// ── Public API ──────────────────────────────────────────────────────────────

/**
 * Begin promiscuous capture.
 * Only call from Bridge mode — mutually exclusive with wardrive scan mode.
 */
inline bool pm_promiscuous_begin(pm_pkt_callback_t cb) {
    if (_pm_active) return false;

    _pm_queue = xQueueCreate(PM_PKT_QUEUE_SIZE, sizeof(pm_pkt_info_t));
    if (!_pm_queue) {
        Serial.println("[pm_promiscuous] queue alloc failed");
        return false;
    }
    _pm_user_cb = cb;

    esp_wifi_set_promiscuous(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();

    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&_pm_promiscuous_cb);

    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        Serial.printf("[pm_promiscuous] enable failed: %d\n", err);
        vQueueDelete(_pm_queue);
        _pm_queue = nullptr;
        return false;
    }

    _pm_active = true;
    _pm_current_channel = PM_CHANNEL_MIN;
    esp_wifi_set_channel(_pm_current_channel, WIFI_SECOND_CHAN_NONE);
    _pm_last_hop     = millis();
    _pm_pkt_count    = 0;
    _pm_pkt_dropped  = 0;
    pm_frames_per_sec  = 0;
    pm_beacon_count    = 0;
    pm_probe_req_count = 0;
    pm_deauth_count    = 0;
    pm_other_count     = 0;

    Serial.println("[pm_promiscuous] capture started");
    return true;
}

/**
 * Stop promiscuous capture. Safe to call when not active.
 */
inline void pm_promiscuous_end() {
    if (!_pm_active) return;
    esp_wifi_set_promiscuous(false);
    _pm_active = false;
    _pm_user_cb = nullptr;
    if (_pm_queue) { vQueueDelete(_pm_queue); _pm_queue = nullptr; }
    Serial.printf("[pm_promiscuous] stopped — %lu captured, %lu dropped\n",
                  _pm_pkt_count, _pm_pkt_dropped);
}

/**
 * Drain queue to user callback. Call from app task (bridge_app main loop).
 * Safe for Serial TX, mutex, etc.
 */
inline void pm_promiscuous_drain(int max_per_call = 16) {
    if (!_pm_active || !_pm_queue || !_pm_user_cb) return;
    pm_pkt_info_t info;
    int n = 0;
    while (n < max_per_call && xQueueReceive(_pm_queue, &info, 0) == pdTRUE) {
        info.millis_at_capture = millis();
        switch (info.subtype) {
            case PM_SUB_BEACON:    pm_beacon_count++;    break;
            case PM_SUB_PROBE_REQ: pm_probe_req_count++; break;
            case PM_SUB_DEAUTH:
            case PM_SUB_DISASSOC:  pm_deauth_count++;    break;
            default:               pm_other_count++;     break;
        }
        _pm_user_cb(&info);
        n++;
    }
}

/**
 * Advance channel hop + update fps counter.
 * Call from bridge_app main loop alongside pm_promiscuous_drain().
 */
inline void pm_promiscuous_tick() {
    if (!_pm_active) return;
    uint32_t now = millis();
    if (now - _pm_last_hop >= PM_CHANNEL_HOP_MS) {
        if (++_pm_current_channel > PM_CHANNEL_MAX)
            _pm_current_channel = PM_CHANNEL_MIN;
        esp_wifi_set_channel(_pm_current_channel, WIFI_SECOND_CHAN_NONE);
        _pm_last_hop = now;
    }
    static uint32_t fps_count_last = 0;
    static uint32_t fps_ms_last    = 0;
    if (now - fps_ms_last >= 1000) {
        pm_frames_per_sec = _pm_pkt_count - fps_count_last;
        fps_count_last    = _pm_pkt_count;
        fps_ms_last       = now;
    }
}

inline void pm_promiscuous_lock_channel(uint8_t ch) {
    if (ch < PM_CHANNEL_MIN || ch > PM_CHANNEL_MAX) return;
    _pm_current_channel = ch;
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    _pm_last_hop = UINT32_MAX;
}

inline void pm_promiscuous_unlock_channel() { _pm_last_hop = millis(); }

inline void pm_promiscuous_set_subtype_mask(uint32_t mask) {
    _pm_subtype_mask = mask;
}

inline bool pm_promiscuous_is_active() { return _pm_active; }

// ── Stats struct — convenient for the Bridge visualizer ────────────────────
typedef struct {
    uint32_t captured;
    uint32_t dropped;
    uint32_t frames_per_sec;
    uint32_t beacon_count;
    uint32_t probe_req_count;
    uint32_t deauth_count;
    uint32_t other_count;
    uint8_t  current_channel;
} pm_stats_t;

inline void pm_promiscuous_get_stats(pm_stats_t* s) {
    if (!s) return;
    s->captured       = _pm_pkt_count;
    s->dropped        = _pm_pkt_dropped;
    s->frames_per_sec = pm_frames_per_sec;
    s->beacon_count   = pm_beacon_count;
    s->probe_req_count= pm_probe_req_count;
    s->deauth_count   = pm_deauth_count;
    s->other_count    = pm_other_count;
    s->current_channel= _pm_current_channel;
}

// Convenience accessor for current channel
inline uint8_t pm_promiscuous_channel() { return _pm_current_channel; }

inline void pm_promiscuous_stats(uint32_t* captured, uint32_t* dropped,
                                  uint8_t* ch, uint32_t* fps) {
    if (captured) *captured = _pm_pkt_count;
    if (dropped)  *dropped  = _pm_pkt_dropped;
    if (ch)       *ch       = _pm_current_channel;
    if (fps)      *fps      = pm_frames_per_sec;
}
