#ifndef WARDRIVE_H
#define WARDRIVE_H

#include <Arduino.h>

// Shared variables — read by launcher status bar and other apps
extern int  networks_found;
extern int  bt_found;
extern int  esp_found;       // Espressif MAC hunter count
extern bool wardrive_active;

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

#endif