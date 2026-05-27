// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#include "cardputer_i2c_module.h"

#ifdef DEVICE_CARDPUTER_ADV

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_wifi_types.h>
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>

#include "pm_input.h"
#include "pm_lora_pins.h"
#include "pi4ioe_cap.h"

#ifndef PIN_P4_I2C_SDA
#define PIN_P4_I2C_SDA 2
#endif
#ifndef PIN_P4_I2C_SCL
#define PIN_P4_I2C_SCL 1
#endif
#ifndef PM_P4_TETHER_UART
#define PM_P4_TETHER_UART 1
#endif
#ifndef PIN_P4_UART_RX
#define PIN_P4_UART_RX 1
#endif
#ifndef PIN_P4_UART_TX
#define PIN_P4_UART_TX 2
#endif
#ifndef PM_P4_UART_BAUD
#define PM_P4_UART_BAUD 921600
#endif

#define PM_CARDPUTER_I2C_ADDR          0x32
#define PM_CARDPUTER_I2C_VERSION       1
#define PM_CARDPUTER_I2C_NAME_LEN      24
#define PM_CARDPUTER_I2C_LORA_MAX      180
#define PM_CARDPUTER_I2C_WIFI_FRAME_MAX 96
#define PM_CARDPUTER_KEY_Q_DEPTH       16
#define PM_CARDPUTER_LORA_Q_DEPTH      4
#define PM_CARDPUTER_WIFI_Q_DEPTH      12
#define PM_CARDPUTER_BLE_Q_DEPTH       24
#define PM_CARDPUTER_BLE_NAME_MAX      32
#define PM_CARDPUTER_BLE_MFG_MAX       12

#define PM_CARDPUTER_CAP_KEYBOARD      (1u << 0)
#define PM_CARDPUTER_CAP_GPS           (1u << 1)
#define PM_CARDPUTER_CAP_LORA          (1u << 2)
#define PM_CARDPUTER_CAP_WIFI          (1u << 3)
#define PM_CARDPUTER_CAP_BLE           (1u << 4)
#define PM_CARDPUTER_CAP_WIFI_PROMISC  (1u << 5)
#define PM_CARDPUTER_CAP_WIFI_SCAN     (1u << 6)

enum {
    PM_CARDPUTER_REG_WHOAMI         = 0x00,
    PM_CARDPUTER_REG_STATUS         = 0x01,
    PM_CARDPUTER_REG_GPS            = 0x02,
    PM_CARDPUTER_REG_KEY_POP        = 0x03,
    PM_CARDPUTER_REG_LORA_RX_POP    = 0x04,
    PM_CARDPUTER_REG_LORA_TX        = 0x05,
    PM_CARDPUTER_REG_LORA_STATUS    = 0x06,
    PM_CARDPUTER_REG_WIFI_CTRL      = 0x10,
    PM_CARDPUTER_REG_WIFI_FRAME_POP = 0x11,
    PM_CARDPUTER_REG_WIFI_STATUS    = 0x12,
    PM_CARDPUTER_REG_PING           = 0x7f,
};

enum {
    PM_CARDPUTER_WIFI_OP_PROMISC_STOP  = 0,
    PM_CARDPUTER_WIFI_OP_PROMISC_START = 1,
    PM_CARDPUTER_WIFI_OP_SET_CHANNEL   = 2,
};

struct __attribute__((packed)) PmCardputerWhoami {
    uint8_t  magic0;
    uint8_t  magic1;
    uint8_t  version;
    uint8_t  device_kind;
    uint32_t caps;
    char     name[PM_CARDPUTER_I2C_NAME_LEN];
};

struct __attribute__((packed)) PmCardputerStatus {
    uint8_t  magic0;
    uint8_t  magic1;
    uint8_t  version;
    uint8_t  flags;
    uint32_t uptime_ms;
    uint32_t caps;
    uint16_t queued_keys;
    uint16_t queued_lora;
    int32_t  heap_free;
};

struct __attribute__((packed)) PmCardputerGps {
    uint8_t  valid;
    uint8_t  sats;
    uint8_t  fix_quality;
    uint8_t  reserved;
    int32_t  lat_e7;
    int32_t  lon_e7;
    int32_t  alt_cm;
    uint32_t age_ms;
};

struct __attribute__((packed)) PmCardputerKey {
    uint8_t  available;
    uint8_t  kind;
    uint8_t  down;
    uint8_t  modifiers;
    uint32_t code;
    uint32_t timestamp_ms;
};

struct __attribute__((packed)) PmCardputerLoraRx {
    uint8_t  available;
    uint8_t  len;
    int8_t   rssi;
    int8_t   snr_x4;
    uint32_t freq_khz;
    uint8_t  data[PM_CARDPUTER_I2C_LORA_MAX];
};

struct __attribute__((packed)) PmCardputerWifiCtrl {
    uint8_t reg;
    uint8_t op;
    uint8_t channel;
    uint8_t filter;
};

struct __attribute__((packed)) PmCardputerWifiFrame {
    uint8_t  available;
    uint8_t  frame_type;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  mac[6];
    uint16_t len;
    uint8_t  data[PM_CARDPUTER_I2C_WIFI_FRAME_MAX];
};

struct PmCardputerBleSeen {
    uint8_t available;
    char    mac[18];
    char    name[PM_CARDPUTER_BLE_NAME_MAX];
    int8_t  rssi;
    uint8_t mfg[PM_CARDPUTER_BLE_MFG_MAX];
    uint8_t mfg_len;
};

struct __attribute__((packed)) PmCardputerLoraStatus {
    uint8_t  ready;
    uint8_t  queued_rx;
    int16_t  last_state;
    uint32_t freq_khz;
};

struct __attribute__((packed)) PmCardputerWifiStatus {
    uint8_t  active;
    uint8_t  channel;
    uint8_t  queued_frames;
    uint8_t  filter;
    uint32_t captured;
    uint32_t dropped;
};

extern TinyGPSPlus gps;
extern HardwareSerial SerialGPS;
extern SemaphoreHandle_t spi_mutex;
extern SPIClass cardputerSdSPI;

static TwoWire s_p4_i2c = TwoWire(1);
static HardwareSerial s_p4_uart(2);
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started = false;
static bool s_gps_started = false;
static bool s_p4_uart_started = false;
static SemaphoreHandle_t s_p4_uart_mutex = nullptr;
static volatile uint8_t s_last_reg = PM_CARDPUTER_REG_WHOAMI;

static PmCardputerGps s_gps_snapshot = {};
static PmCardputerKey s_key_q[PM_CARDPUTER_KEY_Q_DEPTH];
static uint8_t s_key_head = 0;
static uint8_t s_key_tail = 0;
static uint8_t s_key_count = 0;

static uint8_t s_lora_tx[PM_CARDPUTER_I2C_LORA_MAX];
static uint8_t s_lora_tx_len = 0;
static volatile bool s_lora_tx_pending = false;
static PmCardputerLoraRx s_lora_q[PM_CARDPUTER_LORA_Q_DEPTH];
static uint8_t s_lora_head = 0;
static uint8_t s_lora_tail = 0;
static uint8_t s_lora_count = 0;
static volatile bool s_lora_rx_flag = false;
static bool s_lora_init_attempted = false;
static bool s_lora_ready = false;
static int16_t s_lora_last_state = 0;
static uint32_t s_lora_freq_khz = 906875;
static SPISettings s_lora_spi_settings(2000000, MSBFIRST, SPI_MODE0);
static Module* s_lora_module = nullptr;
static SX1262* s_lora_radio = nullptr;

static PmCardputerWifiFrame s_wifi_q[PM_CARDPUTER_WIFI_Q_DEPTH];
static uint8_t s_wifi_head = 0;
static uint8_t s_wifi_tail = 0;
static uint8_t s_wifi_count = 0;
static volatile bool s_wifi_promisc_active = false;
static uint8_t s_wifi_channel = 1;
static uint8_t s_wifi_filter = 0;
static uint32_t s_wifi_captured = 0;
static uint32_t s_wifi_dropped = 0;
static PmCardputerWifiCtrl s_wifi_pending_ctrl = {};
static volatile bool s_wifi_ctrl_pending = false;

static PmCardputerBleSeen s_ble_q[PM_CARDPUTER_BLE_Q_DEPTH];
static uint8_t s_ble_head = 0;
static uint8_t s_ble_tail = 0;
static uint8_t s_ble_count = 0;
static bool s_ble_scan_requested = false;
static bool s_ble_active_scan = false;
static bool s_ble_initialized = false;
static bool s_ble_owns_init = false;
static uint16_t s_ble_interval = 240;
static uint16_t s_ble_window = 60;
static uint32_t s_ble_last_scan_ms = 0;
static uint32_t s_ble_seen_total = 0;
static uint32_t s_ble_dropped_total = 0;
static NimBLEScan* s_ble_scan = nullptr;
class P4BridgeBleCallbacks;
static P4BridgeBleCallbacks* s_ble_callbacks = nullptr;

static uint32_t module_caps() {
    uint32_t caps = PM_CARDPUTER_CAP_KEYBOARD |
                    PM_CARDPUTER_CAP_GPS |
                    PM_CARDPUTER_CAP_WIFI |
                    PM_CARDPUTER_CAP_BLE |
                    PM_CARDPUTER_CAP_WIFI_PROMISC |
                    PM_CARDPUTER_CAP_WIFI_SCAN;
#if defined(PIN_LORA_CS) && defined(PIN_LORA_IRQ)
    caps |= PM_CARDPUTER_CAP_LORA;
#endif
    return caps;
}

static uint32_t map_key_to_p4(uint8_t raw) {
    switch (raw) {
        case PM_KEY_ENTER: return 0x0a;
        case PM_KEY_ESC: return 0x1b;
        case PM_KEY_BACKSPACE: return 0x08;
        case PM_KEY_TAB: return 0x09;
        case PM_KEY_UP: return 0x101;
        case PM_KEY_DOWN: return 0x102;
        case PM_KEY_LEFT: return 0x103;
        case PM_KEY_RIGHT: return 0x104;
        case PM_KEY_HOME: return 0x105;
        case PM_KEY_END: return 0x106;
        case PM_KEY_PGUP: return 0x107;
        case PM_KEY_PGDN: return 0x108;
        case PM_KEY_DEL: return 0x7f;
        default: return raw;
    }
}

static const char* uart_cmd_value(const char* line, const char* key) {
    if (!line || !key) return nullptr;
    size_t key_len = strlen(key);
    const char* p = line;
    while ((p = strstr(p, key)) != nullptr) {
        bool left_ok = (p == line || p[-1] == ' ');
        bool right_ok = (p[key_len] == '=');
        if (left_ok && right_ok) return p + key_len + 1;
        p += key_len;
    }
    return nullptr;
}

static int uart_cmd_int(const char* line, const char* key, int fallback) {
    const char* v = uart_cmd_value(line, key);
    if (!v) return fallback;
    char* end = nullptr;
    long n = strtol(v, &end, 0);
    return (end && end != v) ? (int)n : fallback;
}

static int uart_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t uart_hex_decode(const char* hex, uint8_t* out, size_t out_len) {
    if (!hex || !out || out_len == 0) return 0;
    size_t n = 0;
    while (hex[0] && hex[1] && n < out_len) {
        if (hex[0] == ' ' || hex[0] == '\r' || hex[0] == '\n') break;
        int hi = uart_hex_nibble(hex[0]);
        int lo = uart_hex_nibble(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static void uart_write_hex(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; data && i < len; i++) {
        char pair[2] = { hex[(data[i] >> 4) & 0x0f], hex[data[i] & 0x0f] };
        s_p4_uart.write((const uint8_t*)pair, sizeof(pair));
    }
}

static bool p4_uart_lock() {
    if (!s_p4_uart_mutex) return true;
    return xSemaphoreTake(s_p4_uart_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}

static void p4_uart_unlock() {
    if (s_p4_uart_mutex) xSemaphoreGive(s_p4_uart_mutex);
}

static void p4_uart_emit_hello() {
    if (!s_p4_uart_started) return;
    if (!p4_uart_lock()) return;
    s_p4_uart.printf("PMU1 HELLO caps=0x%08lX name=Cardputer_ADV\n",
                     (unsigned long)module_caps());
    p4_uart_unlock();
}

static void p4_uart_emit_key(const PmCardputerKey& ev) {
    if (!s_p4_uart_started) return;
    if (!p4_uart_lock()) return;
    s_p4_uart.printf("PMU1 KEY code=%lu down=%u mod=%u\n",
                     (unsigned long)ev.code, ev.down, ev.modifiers);
    p4_uart_unlock();
}

static void p4_uart_emit_gps() {
    if (!s_p4_uart_started) return;
    PmCardputerGps g = {};
    portENTER_CRITICAL(&s_mux);
    g = s_gps_snapshot;
    portEXIT_CRITICAL(&s_mux);
    if (!p4_uart_lock()) return;
    s_p4_uart.printf("PMU1 GPS valid=%u sats=%u fix=%u lat_e7=%ld lon_e7=%ld alt_cm=%ld age=%lu\n",
                     g.valid, g.sats, g.fix_quality,
                     (long)g.lat_e7, (long)g.lon_e7, (long)g.alt_cm,
                     (unsigned long)g.age_ms);
    p4_uart_unlock();
}

static void p4_uart_emit_lora(const PmCardputerLoraRx& ev) {
    if (!s_p4_uart_started || !ev.available) return;
    if (!p4_uart_lock()) return;
    s_p4_uart.printf("PMU1 LORA rssi=%d snr_x4=%d freq=%lu len=%u data=",
                     ev.rssi, ev.snr_x4, (unsigned long)ev.freq_khz, ev.len);
    uart_write_hex(ev.data, ev.len);
    s_p4_uart.print('\n');
    p4_uart_unlock();
}

static void p4_uart_emit_wifi(const PmCardputerWifiFrame& ev) {
    if (!s_p4_uart_started || !ev.available) return;
    uint16_t n = ev.len;
    if (n > PM_CARDPUTER_I2C_WIFI_FRAME_MAX) n = PM_CARDPUTER_I2C_WIFI_FRAME_MAX;
    if (!p4_uart_lock()) return;
    s_p4_uart.printf("PMU1 WF type=%u ch=%u rssi=%d mac=%02X%02X%02X%02X%02X%02X len=%u data=",
                     ev.frame_type, ev.channel, ev.rssi,
                     ev.mac[0], ev.mac[1], ev.mac[2], ev.mac[3], ev.mac[4], ev.mac[5],
                     n);
    uart_write_hex(ev.data, n);
    s_p4_uart.print('\n');
    p4_uart_unlock();
}

static void p4_uart_emit_ble(const PmCardputerBleSeen& ev) {
    if (!s_p4_uart_started || !ev.available || !ev.mac[0]) return;
    if (!p4_uart_lock()) return;
    s_p4_uart.printf("PMU1 BLE mac=%s rssi=%d type=public name_hex=",
                     ev.mac, ev.rssi);
    uart_write_hex((const uint8_t*)ev.name, strnlen(ev.name, sizeof(ev.name)));
    s_p4_uart.print(" mfg=");
    uart_write_hex(ev.mfg, ev.mfg_len);
    s_p4_uart.print('\n');
    p4_uart_unlock();
}

void cardputer_i2c_module_offer_key(char c) {
    PmCardputerKey ev = {};
    ev.available = 1;
    ev.kind = 0;
    ev.down = 1;
    ev.modifiers = 0;
    ev.code = map_key_to_p4((uint8_t)c);
    ev.timestamp_ms = millis();

    portENTER_CRITICAL(&s_mux);
    if (s_key_count < PM_CARDPUTER_KEY_Q_DEPTH) {
        s_key_q[s_key_head] = ev;
        s_key_head = (uint8_t)((s_key_head + 1) % PM_CARDPUTER_KEY_Q_DEPTH);
        s_key_count++;
    }
    portEXIT_CRITICAL(&s_mux);
    p4_uart_emit_key(ev);
}

static bool pop_key(PmCardputerKey* out) {
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_key_count > 0) {
        *out = s_key_q[s_key_tail];
        s_key_tail = (uint8_t)((s_key_tail + 1) % PM_CARDPUTER_KEY_Q_DEPTH);
        s_key_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static void push_lora_rx(const PmCardputerLoraRx* ev) {
    portENTER_CRITICAL(&s_mux);
    if (s_lora_count < PM_CARDPUTER_LORA_Q_DEPTH) {
        s_lora_q[s_lora_head] = *ev;
        s_lora_head = (uint8_t)((s_lora_head + 1) % PM_CARDPUTER_LORA_Q_DEPTH);
        s_lora_count++;
    }
    portEXIT_CRITICAL(&s_mux);
}

static bool pop_lora_rx(PmCardputerLoraRx* out) {
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_lora_count > 0) {
        *out = s_lora_q[s_lora_tail];
        s_lora_tail = (uint8_t)((s_lora_tail + 1) % PM_CARDPUTER_LORA_Q_DEPTH);
        s_lora_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static bool pop_wifi_frame(PmCardputerWifiFrame* out) {
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_wifi_count > 0) {
        *out = s_wifi_q[s_wifi_tail];
        s_wifi_tail = (uint8_t)((s_wifi_tail + 1) % PM_CARDPUTER_WIFI_Q_DEPTH);
        s_wifi_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static void push_ble_seen(const PmCardputerBleSeen* ev) {
    if (!ev || !ev->available || !ev->mac[0]) return;
    portENTER_CRITICAL(&s_mux);
    if (s_ble_count < PM_CARDPUTER_BLE_Q_DEPTH) {
        s_ble_q[s_ble_head] = *ev;
        s_ble_head = (uint8_t)((s_ble_head + 1) % PM_CARDPUTER_BLE_Q_DEPTH);
        s_ble_count++;
        s_ble_seen_total++;
    } else {
        s_ble_dropped_total++;
    }
    portEXIT_CRITICAL(&s_mux);
}

static bool pop_ble_seen(PmCardputerBleSeen* out) {
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_ble_count > 0) {
        *out = s_ble_q[s_ble_tail];
        s_ble_tail = (uint8_t)((s_ble_tail + 1) % PM_CARDPUTER_BLE_Q_DEPTH);
        s_ble_count--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

static void IRAM_ATTR push_wifi_frame_isr(const PmCardputerWifiFrame* ev) {
    portENTER_CRITICAL_ISR(&s_mux);
    if (s_wifi_count < PM_CARDPUTER_WIFI_Q_DEPTH) {
        s_wifi_q[s_wifi_head] = *ev;
        s_wifi_head = (uint8_t)((s_wifi_head + 1) % PM_CARDPUTER_WIFI_Q_DEPTH);
        s_wifi_count++;
        s_wifi_captured++;
    } else {
        s_wifi_dropped++;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}

static void update_gps_snapshot() {
    PmCardputerGps next = {};
    next.valid = gps.location.isValid() ? 1 : 0;
    next.sats = gps.satellites.isValid() ? (uint8_t)((gps.satellites.value() > 255) ? 255 : gps.satellites.value()) : 0;
    next.fix_quality = next.valid ? 1 : 0;
    next.lat_e7 = next.valid ? (int32_t)(gps.location.lat() * 10000000.0) : 0;
    next.lon_e7 = next.valid ? (int32_t)(gps.location.lng() * 10000000.0) : 0;
    next.alt_cm = gps.altitude.isValid() ? (int32_t)(gps.altitude.meters() * 100.0) : 0;
    next.age_ms = gps.location.isValid() ? (uint32_t)gps.location.age() : UINT32_MAX;

    portENTER_CRITICAL(&s_mux);
    s_gps_snapshot = next;
    portEXIT_CRITICAL(&s_mux);
}

static void make_whoami(PmCardputerWhoami* out) {
    memset(out, 0, sizeof(*out));
    out->magic0 = 'P';
    out->magic1 = 'M';
    out->version = PM_CARDPUTER_I2C_VERSION;
    out->device_kind = 1;
    out->caps = module_caps();
    strncpy(out->name, "Cardputer ADV", sizeof(out->name) - 1);
}

static void make_status(PmCardputerStatus* out) {
    memset(out, 0, sizeof(*out));
    out->magic0 = 'P';
    out->magic1 = 'M';
    out->version = PM_CARDPUTER_I2C_VERSION;
    out->flags = (s_wifi_promisc_active ? 0x01 : 0x00) | (s_lora_ready ? 0x02 : 0x00);
    out->uptime_ms = millis();
    out->caps = module_caps();
    out->heap_free = (int32_t)ESP.getFreeHeap();
    portENTER_CRITICAL(&s_mux);
    out->queued_keys = s_key_count;
    out->queued_lora = s_lora_count;
    portEXIT_CRITICAL(&s_mux);
}

static void IRAM_ATTR wifi_promisc_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_wifi_promisc_active || !buf) return;
    const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* p = pkt->payload;
    uint16_t sig_len = pkt->rx_ctrl.sig_len;
    if (!p || sig_len < 10) return;

    PmCardputerWifiFrame ev = {};
    ev.available = 1;
    ev.frame_type = (type == WIFI_PKT_MGMT) ? 0 : (type == WIFI_PKT_CTRL ? 1 : (type == WIFI_PKT_DATA ? 2 : 3));
    ev.channel = pkt->rx_ctrl.channel;
    ev.rssi = (int8_t)pkt->rx_ctrl.rssi;
    if (sig_len >= 16) {
        memcpy(ev.mac, &p[10], 6);
    } else {
        memcpy(ev.mac, p, sig_len < 6 ? sig_len : 6);
    }
    ev.len = sig_len;
    uint16_t copy_len = sig_len;
    if (copy_len > PM_CARDPUTER_I2C_WIFI_FRAME_MAX) copy_len = PM_CARDPUTER_I2C_WIFI_FRAME_MAX;
    memcpy(ev.data, p, copy_len);
    push_wifi_frame_isr(&ev);
}

static uint32_t wifi_filter_mask(uint8_t filter) {
    switch (filter) {
        case 1: return WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
        case 2: return WIFI_PROMIS_FILTER_MASK_CTRL;
        case 3: return WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_CTRL | WIFI_PROMIS_FILTER_MASK_DATA;
        default: return WIFI_PROMIS_FILTER_MASK_MGMT;
    }
}

static bool wifi_promisc_start(uint8_t channel, uint8_t filter) {
    if (channel < 1 || channel > 13) channel = 1;
    s_wifi_channel = channel;
    s_wifi_filter = filter;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_promiscuous(false);

    wifi_promiscuous_filter_t f = {};
    f.filter_mask = wifi_filter_mask(filter);
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(&wifi_promisc_cb);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        Serial.printf("[P4-BRIDGE] WiFi promiscuous start failed: %d\n", (int)err);
        return false;
    }

    portENTER_CRITICAL(&s_mux);
    s_wifi_head = s_wifi_tail = s_wifi_count = 0;
    s_wifi_captured = 0;
    s_wifi_dropped = 0;
    portEXIT_CRITICAL(&s_mux);
    s_wifi_promisc_active = true;
    Serial.printf("[P4-BRIDGE] WiFi promiscuous active ch=%u filter=%u\n", channel, filter);
    return true;
}

static void wifi_promisc_stop() {
    if (!s_wifi_promisc_active) return;
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    s_wifi_promisc_active = false;
    Serial.printf("[P4-BRIDGE] WiFi promiscuous stopped captured=%lu dropped=%lu\n",
                  (unsigned long)s_wifi_captured, (unsigned long)s_wifi_dropped);
}

static void wifi_set_channel(uint8_t channel) {
    if (channel < 1 || channel > 13) return;
    s_wifi_channel = channel;
    if (s_wifi_promisc_active) esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

static void ble_extract_adv_field(NimBLEAdvertisedDevice* dev, uint8_t field_type,
                                  uint8_t* out, size_t* out_len, size_t out_max) {
    if (out_len) *out_len = 0;
    if (!dev || !out || !out_len || out_max == 0) return;

    const uint8_t* payload = dev->getPayload();
    const size_t payload_len = dev->getPayloadLength();
    size_t pos = 0;
    while (payload && pos + 1 < payload_len) {
        uint8_t field_len = payload[pos];
        if (field_len == 0) break;
        if (pos + field_len >= payload_len) break;

        uint8_t type = payload[pos + 1];
        if (type == field_type) {
            size_t data_len = field_len - 1;
            if (data_len > out_max) data_len = out_max;
            memcpy(out, payload + pos + 2, data_len);
            *out_len = data_len;
            return;
        }
        pos += field_len + 1;
    }
}

static void ble_extract_name(NimBLEAdvertisedDevice* dev, char* out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = 0;
    uint8_t tmp[PM_CARDPUTER_BLE_NAME_MAX] = {};
    size_t n = 0;
    ble_extract_adv_field(dev, 0x09, tmp, &n, sizeof(tmp) - 1);
    if (n == 0) ble_extract_adv_field(dev, 0x08, tmp, &n, sizeof(tmp) - 1);
    if (n >= out_len) n = out_len - 1;
    for (size_t i = 0; i < n; i++) {
        char c = (char)tmp[i];
        out[i] = (c >= 32 && c <= 126 && c != '\r' && c != '\n') ? c : '.';
    }
    out[n] = 0;
}

static void ble_format_mac(const NimBLEAddress& addr, char out[18]) {
    const uint8_t* native = addr.getNative();
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             native[5], native[4], native[3], native[2], native[1], native[0]);
}

class P4BridgeBleCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!s_ble_scan_requested || !dev) return;

        PmCardputerBleSeen ev = {};
        ev.available = 1;
        ev.rssi = (int8_t)dev->getRSSI();
        ble_format_mac(dev->getAddress(), ev.mac);
        ble_extract_name(dev, ev.name, sizeof(ev.name));

        size_t mfg_len = 0;
        ble_extract_adv_field(dev, 0xff, ev.mfg, &mfg_len, sizeof(ev.mfg));
        ev.mfg_len = (uint8_t)mfg_len;
        push_ble_seen(&ev);
    }
};

static bool ble_bridge_init() {
    if (s_ble_initialized && s_ble_scan) return true;

    uint32_t free_heap = ESP.getFreeHeap();
    if (free_heap < 55000) {
        Serial.printf("[P4-BRIDGE] BLE scan skipped: low heap %lu\n",
                      (unsigned long)free_heap);
        return false;
    }

    bool already_init = NimBLEDevice::getInitialized();
    s_ble_owns_init = !already_init;
    if (!already_init) {
        NimBLEDevice::setScanDuplicateCacheSize(32);
        NimBLEDevice::init("");
    }

    s_ble_scan = NimBLEDevice::getScan();
    if (!s_ble_callbacks) s_ble_callbacks = new P4BridgeBleCallbacks();
    if (!s_ble_scan || !s_ble_callbacks) {
        Serial.println("[P4-BRIDGE] BLE scan init failed");
        return false;
    }

    s_ble_scan->setAdvertisedDeviceCallbacks(s_ble_callbacks, false);
    s_ble_scan->setActiveScan(s_ble_active_scan);
    s_ble_scan->setMaxResults(0);
    s_ble_scan->setInterval(s_ble_interval);
    s_ble_scan->setWindow(s_ble_window);
    s_ble_initialized = true;
    Serial.printf("[P4-BRIDGE] BLE scanner ready active=%u heap=%lu\n",
                  s_ble_active_scan ? 1 : 0,
                  (unsigned long)ESP.getFreeHeap());
    return true;
}

static void ble_bridge_start(bool active, uint16_t interval, uint16_t window) {
    if (s_wifi_promisc_active) wifi_promisc_stop();
    s_ble_active_scan = active;
    if (interval >= 20 && interval <= 1000) s_ble_interval = interval;
    if (window >= 10 && window <= s_ble_interval) s_ble_window = window;

    portENTER_CRITICAL(&s_mux);
    s_ble_head = s_ble_tail = s_ble_count = 0;
    portEXIT_CRITICAL(&s_mux);
    s_ble_scan_requested = true;
    s_ble_last_scan_ms = 0;
    Serial.printf("[P4-BRIDGE] BLE scan requested active=%u interval=%u window=%u\n",
                  s_ble_active_scan ? 1 : 0, s_ble_interval, s_ble_window);
}

static void ble_bridge_stop() {
    s_ble_scan_requested = false;
    if (s_ble_scan) {
        s_ble_scan->stop();
        s_ble_scan->clearResults();
    }
    if (s_ble_initialized && s_ble_owns_init && NimBLEDevice::getInitialized()) {
        NimBLEDevice::deinit(true);
        s_ble_scan = nullptr;
        s_ble_initialized = false;
        s_ble_owns_init = false;
    }
    Serial.printf("[P4-BRIDGE] BLE scan stopped seen=%lu dropped=%lu heap=%lu\n",
                  (unsigned long)s_ble_seen_total,
                  (unsigned long)s_ble_dropped_total,
                  (unsigned long)ESP.getFreeHeap());
}

static void ble_bridge_poll() {
    if (!s_ble_scan_requested) return;
    uint32_t now = millis();
    if (s_ble_last_scan_ms != 0 && now - s_ble_last_scan_ms < 1500) return;
    s_ble_last_scan_ms = now;

    if (!ble_bridge_init()) return;
    if (!s_ble_scan) return;
    s_ble_scan->setActiveScan(s_ble_active_scan);
    s_ble_scan->setInterval(s_ble_interval);
    s_ble_scan->setWindow(s_ble_window);
    s_ble_scan->start(1, false);
    s_ble_scan->clearResults();

    uint32_t heap_after = ESP.getFreeHeap();
    if (heap_after < 12000) {
        Serial.printf("[P4-BRIDGE] BLE heap floor hit after scan (%lu); stopping\n",
                      (unsigned long)heap_after);
        ble_bridge_stop();
    }
}

static void IRAM_ATTR lora_bridge_rx_isr() {
    s_lora_rx_flag = true;
}

static bool lora_bridge_begin() {
#if defined(PIN_LORA_CS) && defined(PIN_LORA_IRQ)
    if (s_lora_ready) return true;
    if (s_lora_init_attempted && s_lora_last_state != 0) return false;
    s_lora_init_attempted = true;

    pi4ioe_cap_init();
    pinMode(PM_LORA_CS, OUTPUT);
    digitalWrite(PM_LORA_CS, HIGH);
#ifdef PIN_SD_CS
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    cardputerSdSPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
#else
    cardputerSdSPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
#endif

    s_lora_module = new Module(PM_LORA_CS, PM_LORA_IRQ, PM_LORA_RST, PM_LORA_BUSY,
                               cardputerSdSPI, s_lora_spi_settings);
    s_lora_radio = new SX1262(s_lora_module);
    if (!s_lora_module || !s_lora_radio) {
        s_lora_last_state = -1000;
        return false;
    }

    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("[P4-BRIDGE] LoRa bridge init: SPI mutex timeout");
        s_lora_last_state = -1001;
        return false;
    }

    int state = s_lora_radio->begin(906.875f, 250.0f, 11, 5, 0x2B, 22, 16);
    if (state == RADIOLIB_ERR_NONE) {
        s_lora_radio->setDio2AsRfSwitch(true);
        s_lora_radio->setCurrentLimit(140.0f);
        s_lora_radio->setRxBoostedGainMode(true);
        s_lora_radio->setPacketReceivedAction(lora_bridge_rx_isr);
        state = s_lora_radio->startReceive();
    }
    xSemaphoreGiveRecursive(spi_mutex);

    s_lora_last_state = (int16_t)state;
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[P4-BRIDGE] LoRa bridge init failed: %d\n", state);
        delete s_lora_radio; s_lora_radio = nullptr;
        delete s_lora_module; s_lora_module = nullptr;
        return false;
    }

    s_lora_ready = true;
    Serial.println("[P4-BRIDGE] LoRa bridge ready: 906.875MHz BW250 SF11");
    return true;
#else
    s_lora_last_state = -1002;
    return false;
#endif
}

static void lora_bridge_poll_rx() {
    if (!s_lora_ready || !s_lora_radio || !s_lora_rx_flag) return;
    s_lora_rx_flag = false;
    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(30)) != pdTRUE) {
        s_lora_rx_flag = true;
        return;
    }

    PmCardputerLoraRx ev = {};
    size_t rx_len = s_lora_radio->getPacketLength();
    size_t read_len = rx_len;
    if (read_len > PM_CARDPUTER_I2C_LORA_MAX) read_len = PM_CARDPUTER_I2C_LORA_MAX;
    int state = s_lora_radio->readData(ev.data, read_len);
    float rssi = 0.0f;
    float snr = 0.0f;
    if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_CRC_MISMATCH) {
        rssi = s_lora_radio->getRSSI();
        snr = s_lora_radio->getSNR();
    }
    s_lora_radio->startReceive();
    xSemaphoreGiveRecursive(spi_mutex);

    s_lora_last_state = (int16_t)state;
    if (state == RADIOLIB_ERR_NONE && rx_len <= PM_CARDPUTER_I2C_LORA_MAX) {
        ev.available = 1;
        ev.len = (uint8_t)read_len;
        ev.rssi = (int8_t)rssi;
        ev.snr_x4 = (int8_t)(snr * 4.0f);
        ev.freq_khz = s_lora_freq_khz;
        push_lora_rx(&ev);
    }
}

static void lora_bridge_send_pending() {
    if (!s_lora_tx_pending) return;
    uint8_t buf[PM_CARDPUTER_I2C_LORA_MAX];
    uint8_t len = 0;
    portENTER_CRITICAL(&s_mux);
    len = s_lora_tx_len;
    if (len > 0) memcpy(buf, s_lora_tx, len);
    s_lora_tx_pending = false;
    portEXIT_CRITICAL(&s_mux);
    if (len == 0) return;
    if (!lora_bridge_begin()) return;

    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("[P4-BRIDGE] LoRa TX: SPI mutex timeout");
        return;
    }
    s_lora_rx_flag = false;
    int state = s_lora_radio->transmit(buf, len);
    s_lora_radio->startReceive();
    xSemaphoreGiveRecursive(spi_mutex);
    s_lora_last_state = (int16_t)state;
    Serial.printf("[P4-BRIDGE] LoRa TX %u bytes state=%d\n", len, state);
}

static void make_lora_status(PmCardputerLoraStatus* out) {
    memset(out, 0, sizeof(*out));
    out->ready = s_lora_ready ? 1 : 0;
    out->last_state = s_lora_last_state;
    out->freq_khz = s_lora_freq_khz;
    portENTER_CRITICAL(&s_mux);
    out->queued_rx = s_lora_count;
    portEXIT_CRITICAL(&s_mux);
}

static void make_wifi_status(PmCardputerWifiStatus* out) {
    memset(out, 0, sizeof(*out));
    out->active = s_wifi_promisc_active ? 1 : 0;
    out->channel = s_wifi_channel;
    out->filter = s_wifi_filter;
    portENTER_CRITICAL(&s_mux);
    out->queued_frames = s_wifi_count;
    out->captured = s_wifi_captured;
    out->dropped = s_wifi_dropped;
    portEXIT_CRITICAL(&s_mux);
}

static void on_receive(int len) {
    if (len <= 0) return;
    uint8_t reg = s_p4_i2c.read();
    len--;

    if (reg == PM_CARDPUTER_REG_LORA_TX) {
        uint8_t n = 0;
        if (len > 0 && s_p4_i2c.available()) {
            n = s_p4_i2c.read();
            len--;
        }
        if (n > PM_CARDPUTER_I2C_LORA_MAX) n = PM_CARDPUTER_I2C_LORA_MAX;
        uint8_t got = 0;
        portENTER_CRITICAL(&s_mux);
        while (len > 0 && s_p4_i2c.available() && got < n) {
            s_lora_tx[got++] = s_p4_i2c.read();
            len--;
        }
        s_lora_tx_len = got;
        s_lora_tx_pending = got > 0;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    if (reg == PM_CARDPUTER_REG_WIFI_CTRL) {
        PmCardputerWifiCtrl ctrl = {};
        ctrl.reg = reg;
        if (len > 0 && s_p4_i2c.available()) { ctrl.op = s_p4_i2c.read(); len--; }
        if (len > 0 && s_p4_i2c.available()) { ctrl.channel = s_p4_i2c.read(); len--; }
        if (len > 0 && s_p4_i2c.available()) { ctrl.filter = s_p4_i2c.read(); len--; }
        portENTER_CRITICAL(&s_mux);
        s_wifi_pending_ctrl = ctrl;
        s_wifi_ctrl_pending = true;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    s_last_reg = reg;
}

static void on_request() {
    uint8_t reg = s_last_reg;
    if (reg == PM_CARDPUTER_REG_WHOAMI) {
        PmCardputerWhoami who;
        make_whoami(&who);
        s_p4_i2c.write((const uint8_t*)&who, sizeof(who));
        return;
    }
    if (reg == PM_CARDPUTER_REG_STATUS) {
        PmCardputerStatus st;
        make_status(&st);
        s_p4_i2c.write((const uint8_t*)&st, sizeof(st));
        return;
    }
    if (reg == PM_CARDPUTER_REG_GPS) {
        PmCardputerGps g;
        portENTER_CRITICAL(&s_mux);
        g = s_gps_snapshot;
        portEXIT_CRITICAL(&s_mux);
        s_p4_i2c.write((const uint8_t*)&g, sizeof(g));
        return;
    }
    if (reg == PM_CARDPUTER_REG_KEY_POP) {
        PmCardputerKey key;
        pop_key(&key);
        s_p4_i2c.write((const uint8_t*)&key, sizeof(key));
        return;
    }
    if (reg == PM_CARDPUTER_REG_LORA_RX_POP) {
        PmCardputerLoraRx rx;
        pop_lora_rx(&rx);
        s_p4_i2c.write((const uint8_t*)&rx, sizeof(rx));
        return;
    }
    if (reg == PM_CARDPUTER_REG_LORA_STATUS) {
        PmCardputerLoraStatus st;
        make_lora_status(&st);
        s_p4_i2c.write((const uint8_t*)&st, sizeof(st));
        return;
    }
    if (reg == PM_CARDPUTER_REG_WIFI_FRAME_POP) {
        PmCardputerWifiFrame frame;
        pop_wifi_frame(&frame);
        s_p4_i2c.write((const uint8_t*)&frame, sizeof(frame));
        return;
    }
    if (reg == PM_CARDPUTER_REG_WIFI_STATUS) {
        PmCardputerWifiStatus st;
        make_wifi_status(&st);
        s_p4_i2c.write((const uint8_t*)&st, sizeof(st));
        return;
    }
    if (reg == PM_CARDPUTER_REG_PING) {
        uint8_t pong = 0xa5;
        s_p4_i2c.write(&pong, 1);
        return;
    }
    uint8_t zero = 0;
    s_p4_i2c.write(&zero, 1);
}

static void process_wifi_ctrl() {
    if (!s_wifi_ctrl_pending) return;
    PmCardputerWifiCtrl ctrl = {};
    portENTER_CRITICAL(&s_mux);
    ctrl = s_wifi_pending_ctrl;
    s_wifi_ctrl_pending = false;
    portEXIT_CRITICAL(&s_mux);

    if (ctrl.op == PM_CARDPUTER_WIFI_OP_PROMISC_START) {
        wifi_promisc_start(ctrl.channel, ctrl.filter);
    } else if (ctrl.op == PM_CARDPUTER_WIFI_OP_PROMISC_STOP) {
        wifi_promisc_stop();
    } else if (ctrl.op == PM_CARDPUTER_WIFI_OP_SET_CHANNEL) {
        wifi_set_channel(ctrl.channel);
    }
}

static void process_uart_command(const char* line) {
    if (!line || strncmp(line, "PMU1 ", 5) != 0) return;
    if (strncmp(line + 5, "PING", 4) == 0) {
        p4_uart_emit_hello();
        return;
    }
    if (strncmp(line + 5, "CMD ", 4) != 0) return;

    const char* cmd = line + 9;
    if (strncmp(cmd, "wifi_promisc_start", 18) == 0) {
        uint8_t ch = (uint8_t)uart_cmd_int(line, "ch", 1);
        uint8_t filter = (uint8_t)uart_cmd_int(line, "filter", 0xff);
        wifi_promisc_start(ch, filter);
        return;
    }
    if (strncmp(cmd, "wifi_promisc_stop", 17) == 0) {
        wifi_promisc_stop();
        return;
    }
    if (strncmp(cmd, "wifi_set_channel", 16) == 0) {
        uint8_t ch = (uint8_t)uart_cmd_int(line, "ch", 1);
        wifi_set_channel(ch);
        return;
    }
    if (strncmp(cmd, "ble_scan_start", 14) == 0) {
        bool active = uart_cmd_int(line, "active", 0) != 0;
        uint16_t interval = (uint16_t)uart_cmd_int(line, "interval", s_ble_interval);
        uint16_t window = (uint16_t)uart_cmd_int(line, "window", s_ble_window);
        ble_bridge_start(active, interval, window);
        return;
    }
    if (strncmp(cmd, "ble_scan_stop", 13) == 0) {
        ble_bridge_stop();
        return;
    }
    if (strncmp(cmd, "lora_tx", 7) == 0) {
        const char* hex = uart_cmd_value(line, "data");
        uint8_t buf[PM_CARDPUTER_I2C_LORA_MAX];
        size_t len = uart_hex_decode(hex, buf, sizeof(buf));
        if (len == 0) return;
        portENTER_CRITICAL(&s_mux);
        memcpy(s_lora_tx, buf, len);
        s_lora_tx_len = (uint8_t)len;
        s_lora_tx_pending = true;
        portEXIT_CRITICAL(&s_mux);
        return;
    }
}

static void poll_p4_uart() {
    if (!s_p4_uart_started) return;
    static char line[192];
    static size_t pos = 0;
    while (s_p4_uart.available() > 0) {
        char c = (char)s_p4_uart.read();
        if (c == '\n') {
            line[pos] = 0;
            if (pos > 0 && line[pos - 1] == '\r') line[pos - 1] = 0;
            process_uart_command(line);
            pos = 0;
        } else if (pos + 1 < sizeof(line)) {
            line[pos++] = c;
        } else {
            pos = 0;
        }
    }
}

static void drain_uart_event_queues() {
    if (!s_p4_uart_started) return;

    for (int i = 0; i < 4; i++) {
        PmCardputerWifiFrame f = {};
        if (!pop_wifi_frame(&f) || !f.available) break;
        p4_uart_emit_wifi(f);
    }

    for (int i = 0; i < 2; i++) {
        PmCardputerLoraRx rx = {};
        if (!pop_lora_rx(&rx) || !rx.available) break;
        p4_uart_emit_lora(rx);
    }

    for (int i = 0; i < 8; i++) {
        PmCardputerBleSeen ble = {};
        if (!pop_ble_seen(&ble) || !ble.available) break;
        p4_uart_emit_ble(ble);
    }
}

static void module_task(void*) {
#if defined(PIN_GPS_RX) && defined(PIN_GPS_TX)
    static const uint32_t gps_bauds[] = {115200, 38400, 9600};
    uint8_t gps_baud_idx = 0;
    uint32_t current_baud = gps_bauds[gps_baud_idx];
    uint32_t last_baud_check = millis();
    uint32_t last_chars = 0;
    SerialGPS.setRxBufferSize(512);
    SerialGPS.begin(current_baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    s_gps_started = true;
    Serial.printf("[P4-BRIDGE] GPS cap UART started RX=%d TX=%d baud=%lu\n",
                  PIN_GPS_RX, PIN_GPS_TX, (unsigned long)current_baud);
#endif

    uint32_t last_gps_update = 0;
    uint32_t last_uart_gps_emit = 0;
    uint32_t last_uart_hello = 0;
    while (true) {
        poll_p4_uart();
        process_wifi_ctrl();

        if (s_gps_started) {
            int budget = 192;
            while (budget-- > 0 && SerialGPS.available() > 0) {
                gps.encode((char)SerialGPS.read());
            }
            uint32_t now = millis();
            if (now - last_gps_update > 500) {
                update_gps_snapshot();
                last_gps_update = now;
            }
            if (now - last_uart_gps_emit > 1000) {
                p4_uart_emit_gps();
                last_uart_gps_emit = now;
            }
#if defined(PIN_GPS_RX) && defined(PIN_GPS_TX)
            if (now - last_baud_check > 5000) {
                uint32_t chars = gps.charsProcessed();
                uint32_t delta = chars - last_chars;
                last_chars = chars;
                if (delta < 10 || gps.passedChecksum() == 0) {
                    gps_baud_idx = (uint8_t)((gps_baud_idx + 1) % (sizeof(gps_bauds) / sizeof(gps_bauds[0])));
                    current_baud = gps_bauds[gps_baud_idx];
                    SerialGPS.updateBaudRate(current_baud);
                    Serial.printf("[P4-BRIDGE] GPS cap trying %lu baud (%lu chars/5s)\n",
                                  (unsigned long)current_baud, (unsigned long)delta);
                }
                last_baud_check = now;
            }
#endif
        }

        if (s_p4_uart_started && millis() - last_uart_hello > 2000) {
            p4_uart_emit_hello();
            last_uart_hello = millis();
        }

        if (!s_lora_ready && millis() > 9000) {
            lora_bridge_begin();
        }
        lora_bridge_send_pending();
        lora_bridge_poll_rx();
        ble_bridge_poll();
        drain_uart_event_queues();

        delay(10);
    }
}

void cardputer_i2c_module_begin() {
    if (s_started) return;
    s_started = true;

    PmCardputerGps empty_gps = {};
    empty_gps.age_ms = UINT32_MAX;
    s_gps_snapshot = empty_gps;

#if PM_P4_TETHER_UART
    if (!s_p4_uart_mutex) s_p4_uart_mutex = xSemaphoreCreateMutex();
    s_p4_uart.setRxBufferSize(2048);
    s_p4_uart.begin(PM_P4_UART_BAUD, SERIAL_8N1, PIN_P4_UART_RX, PIN_P4_UART_TX);
    s_p4_uart_started = true;
    Serial.printf("[P4-UART] Cardputer module ready RX=G%d TX=G%d baud=%lu caps=0x%08lX\n",
                  PIN_P4_UART_RX,
                  PIN_P4_UART_TX,
                  (unsigned long)PM_P4_UART_BAUD,
                  (unsigned long)module_caps());
    p4_uart_emit_hello();
#else
    s_p4_i2c.onReceive(on_receive);
    s_p4_i2c.onRequest(on_request);
    bool ok = s_p4_i2c.begin((uint8_t)PM_CARDPUTER_I2C_ADDR,
                             PIN_P4_I2C_SDA,
                             PIN_P4_I2C_SCL,
                             400000);
    Serial.printf("[P4-I2C] Cardputer module %s addr=0x%02X SDA=G%d SCL=G%d caps=0x%08lX\n",
                  ok ? "ready" : "FAILED",
                  PM_CARDPUTER_I2C_ADDR,
                  PIN_P4_I2C_SDA,
                  PIN_P4_I2C_SCL,
                  (unsigned long)module_caps());
#endif

    xTaskCreatePinnedToCore(module_task, "P4TETHER", 6144, nullptr, 1, nullptr, 0);
}

#endif // DEVICE_CARDPUTER_ADV
