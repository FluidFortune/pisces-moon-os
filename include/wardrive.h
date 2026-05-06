#ifndef WARDRIVE_H
#define WARDRIVE_H

#include <Arduino.h>

// Shared variables — read by launcher status bar and other apps
extern int  networks_found;         // count from last WiFi scan
extern volatile int  bt_found;      // count from current BLE window
extern int  esp_found;              // Espressif MAC hunter count
extern bool wardrive_active;

// Session totals — accumulate across all scans, never reset until reboot.
// These are the right values for Bridge mode to display — they stay
// meaningful whether wardrive is currently running or idle.
extern int      networks_total;     // cumulative networks seen this boot
extern int      ble_total;          // cumulative BLE devices seen this boot
extern uint32_t last_scan_ms;       // millis() of most recent WiFi scan

// Traffic flags — set by apps that need exclusive radio or SD access.
// Wardrive task checks these before touching the radio or SD card.
extern volatile bool wifi_in_use; // Set by any app making WiFi/HTTP calls
extern volatile bool sd_in_use;   // Set by WiFi File Manager — pauses SD writes

// Core functions
void init_wardrive_core();
void run_wardrive();
void wardrive_task(void* pvParameters);

// Gamepad BLE handoff — call before/after gamepad pairing
// to give the gamepad client exclusive NimBLE stack access.
void wardrive_ble_stop();
void wardrive_ble_resume();

// Returns the current session log filename (e.g. "/wardrive_0003.csv").
// Empty string until the wardrive task has started and created the file.
const char* wardrive_get_log_filename();

// ─────────────────────────────────────────────
//  v1.1 — Bridge streaming hook
//
//  When set true, the wardrive task emits JSON events on Serial
//  for each detected network/BLE device. Bridge enables this
//  during a connected session so the host receives live data
//  without having to poll wardrive_status.
//
//  Event format:
//    {"event":"wifi_seen","mac":"AA:BB:CC:DD:EE:FF","ssid":"...",
//     "rssi":-52,"ch":6,"enc":"WPA","lat":34.067,"lng":-118.204}
//    {"event":"ble_seen","mac":"...","name":"...","rssi":-68}
//
//  The flag is volatile because it's read on Core 0 (wardrive task)
//  and written on Core 1 (bridge_app). No mutex needed — single
//  bool, atomic read/write on ESP32-S3.
// ─────────────────────────────────────────────
extern volatile bool wardrive_bridge_streaming;

#endif
