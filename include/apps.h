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

#ifndef APPS_H
#define APPS_H

#include <Arduino.h>

// OS Core
void run_launcher();
void show_splash_screen();

// App Suite — COMMS
void run_wifi_connect();
void run_gps();

// App Suite — TOOLS
void run_notepad();
void run_calculator();
void run_clock();
void run_calendar();
void run_etch();
void run_filesystem();
void run_about();
void run_system();

// App Suite — GAMES
void run_snake();
void run_pacman();
void run_galaga();
void run_chess();

// App Suite — INTEL
void run_terminal();
void run_baseball();

// App Suite — GAMES (ports)
void run_doom();
void run_simcity();

// App Suite — INTEL reference
void run_trails();

// App Suite — CYBER (security education tools)
void run_wardrive();
void run_pkt_sniffer();
void run_beacon_spotter();
void run_net_scanner();
void run_hash_tool();

// Audio
void run_audio_player();
void run_audio_recorder();

// New Apps v0.9.5
void run_voice_terminal();   // Speech-to-Text + Gemini + Text-to-Speech
void run_lora_voice();       // LoRa push-to-talk walkie-talkie (Codec2)
void run_ssh_client();       // SSH terminal client for homelab access
void run_micropython();      // Interactive MicroPython REPL

// ============================================================
//  ELF ENGINE — v0.9.6 "ELF ON A SHELF"
// ============================================================

// Retro ELF Pack — ROM browser + NES/GB/Atari emulator launcher
// SD card: /roms/nes/ /roms/gb/ /roms/atari/  +  /apps/*.elf
// Requires: 8BitDo Zero 2 in BLE mode (SELECT+RIGHT 3s = purple LED)
void run_retro_pack();       // GAMES category — "RETRO" launcher tile

// ELF App Browser — generic SD app loader
// Lists all .elf files in /apps/ and launches selected module
void run_elf_browser();      // SYSTEM category — "ELF APPS" launcher tile

// Gamepad Pairing Screen — BLE Zero 2 setup and status
// Shows connection status, button test, pairing instructions
void run_gamepad_setup();    // SYSTEM category — "GAMEPAD" launcher tile

// ============================================================
//  Utils
// ============================================================
String get_text_input(int x, int y);

// WiFi File Manager — HTTP server for SD card access over WiFi
// Browse, upload, download, delete without removing the MicroSD card
// Primary use: deploy .elf files to /apps/ from Mac/PC browser
void run_wifi_filemgr();

// ============================================================
//  CYBER EXPANSION — v0.9.7
// ============================================================

// BLE GATT Explorer — connect to BLE device, enumerate services/characteristics
// Logs full GATT tree to /cyber_logs/gatt_NNNN.json
// USE ONLY ON DEVICES YOU OWN OR HAVE AUTHORIZATION TO TEST
void run_ble_gatt_explorer();

// WPA Handshake Capture — passive EAPOL 4-way handshake capture
// Saves .hccapx files to /cyber_logs/ for Hashcat offline analysis (-m 2500)
// PASSIVE ONLY — no deauth injection, no transmission
// USE ONLY ON NETWORKS YOU OWN OR HAVE AUTHORIZATION TO TEST
void run_wpa_handshake();

// RF Spectrum Visualizer — SX1262 RSSI sweep across configurable frequency range
// Scrolling waterfall + peak-hold bar chart, 150-960 MHz
// SPI Bus Treaty: sets lora_voice_active during session
void run_rf_spectrum();

// Probe Request Intelligence — passive 802.11 probe request analysis
// Reveals what SSIDs nearby devices are actively seeking (network history)
// Device fingerprinting by MAC OUI, sortable by device or by SSID
// Saves to /cyber_logs/probe_NNNN.json on exit
void run_probe_intel();

// Offline Packet Analysis — rules-based post-session analysis engine
// Reads beacon_*.json and pkt_*.csv from /cyber_logs/
// Detects: deauth floods, evil twins, encryption downgrades, probe patterns,
// hidden AP anomalies, channel anomalies. No WiFi required.
void run_offline_pkt_analysis();

// ============================================================
//  DUCKY SUITE — v0.9.7
// ============================================================

// BLE Ducky — wireless HID keyboard injection over Bluetooth LE
// Works in standard build. Advertises as "PM-Keyboard".
// Payloads: /payloads/*.txt (DuckyScript format)
// USE ONLY ON SYSTEMS YOU OWN OR HAVE AUTHORIZATION TO TEST
void run_ble_ducky();

// USB Ducky — wired USB HID keyboard injection
// Standard build: shows HID flash instructions
// HID build (pio run -e esp32s3_hid): full wired injection
// USE ONLY ON SYSTEMS YOU OWN OR HAVE AUTHORIZATION TO TEST
void run_usb_ducky();

// WiFi Ducky — network payload delivery + reverse C2
// HTTP POST/GET, SSH exec, reverse command channel
// Requires WiFi. Targets: /payloads/wifi_targets.json
// USE ONLY ON SYSTEMS YOU OWN OR HAVE AUTHORIZATION TO TEST
void run_wifi_ducky();

// Bridge App — USB Serial JSON bridge for web emulator
// Connect any ESP32 running Pisces Moon to piscesdemo.fluidfortune.com
// Chrome/Edge only (Web Serial API). Launch from SYSTEM menu.
void run_bridge();

// Bridge App — USB Serial JSON bridge for web emulator
// Connect any ESP32 running Pisces Moon to piscesdemo.fluidfortune.com
// Chrome/Edge only (Web Serial API). Launch from SYSTEM menu.
void run_bridge();

#endif // APPS_H