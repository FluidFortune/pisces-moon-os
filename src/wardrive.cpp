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

/**
 * PISCES MOON OS — WARDRIVE v2.6
 * Logs WiFi APs + BLE devices to per-session numbered CSV files.
 *
 * v2.6 changes:
 * - Session files: each run creates wardrive_0001.csv, wardrive_0002.csv, etc.
 *   The next available number is found at task startup by scanning the SD card.
 *   The monolithic wardrive.csv is no longer written.
 * - Active filename exposed via wardrive_get_log_filename() for UI display.
 * - Ghost Partition aware: files written to Ghost Partition when in Tactical Mode,
 *   public SD root otherwise. (Same behavior as before — Ghost Partition path
 *   is handled by the sd object already pointing to the correct partition.)
 *
 * All other architecture preserved:
 * - wifi_in_use traffic flag
 * - wardrive_active extern
 * - vTaskDelay yields on Core 0
 * - Auto-baud GPS hunter
 * - spi_mutex on every SD access
 */

#include "wardrive.h"
#include <Arduino.h>
#include <FS.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include "SdFat.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include <NimBLEDevice.h>
#include <NimBLEScan.h>

extern TinyGPSPlus gps;
extern SdFat sd;
extern Arduino_GFX *gfx;
extern volatile bool wifi_in_use;
extern SemaphoreHandle_t spi_mutex;

int  networks_found  = 0;
int  bt_found        = 0;
int  esp_found       = 0;
bool wardrive_active = false;
HardwareSerial SerialGPS(1);

// ─────────────────────────────────────────────
//  SESSION LOG FILENAME
//  Stored here so the UI can read it via
//  wardrive_get_log_filename().
// ─────────────────────────────────────────────
static char _current_log_file[32] = "";

const char* wardrive_get_log_filename() {
    return _current_log_file;
}

// ─────────────────────────────────────────────
//  NEXT SESSION NUMBER
//  Scans SD card for wardrive_NNNN.csv files
//  and returns the next available number (1-9999).
//  Runs once at task startup — spi_mutex must be
//  held by the caller or called before scan loop.
// ─────────────────────────────────────────────
static int _find_next_session_number() {
    // Walk /wardrive_0001.csv .. /wardrive_9999.csv
    // Return the first number that doesn't exist.
    for (int n = 1; n <= 9999; n++) {
        char path[32];
        snprintf(path, sizeof(path), "/wardrive_%04d.csv", n);
        if (!sd.exists(path)) {
            return n;
        }
    }
    // Fallback — wrap to 1 (shouldn't happen in practice)
    return 1;
}

// ─────────────────────────────────────────────
//  GPS TIMESTAMP HELPER
// ─────────────────────────────────────────────
static String gps_timestamp() {
    if (!gps.date.isValid() || !gps.time.isValid()) {
        return "1970-01-01 00:00:00";
    }
    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(),
             gps.date.month(),
             gps.date.day(),
             gps.time.hour(),
             gps.time.minute(),
             gps.time.second());
    return String(buf);
}

// ─────────────────────────────────────────────
//  ESPRESSIF MAC HUNTER
// ─────────────────────────────────────────────
static bool isEspressifMAC(const char* mac) {
    // Known Espressif OUI prefixes (first 3 octets)
    static const char* espOUIs[] = {
        "24:0A:C4", "24:6F:28", "30:AE:A4", "3C:71:BF",
        "3C:61:05", "48:3F:DA", "4C:11:AE", "54:43:B2",
        "58:BF:25", "60:01:94", "68:C6:3A", "70:03:9F",
        "7C:9E:BD", "84:0D:8E", "84:CC:A8", "8C:AA:B5",
        "90:97:D5", "94:B5:55", "98:CD:AC", "A0:20:A6",
        "A4:CF:12", "A4:E5:7C", "AC:0B:FB", "B4:E6:2D",
        "BC:DD:C2", "C4:4F:33", "C8:2B:96", "CC:50:E3",
        "D8:BC:38", "DC:4F:22", "E0:98:06", "E8:68:E7",
        "EC:94:CB", "F0:08:D1", "FC:F5:C4",
        nullptr
    };
    for (int i = 0; espOUIs[i]; i++) {
        if (strncasecmp(mac, espOUIs[i], 8) == 0) return true;
    }
    return false;
}

// ─────────────────────────────────────────────
//  BLE QUEUE
// ─────────────────────────────────────────────
#define BLE_QUEUE_SIZE 32
struct BLEResult {
    char mac[18];
    char name[32];
    int  rssi;
};
static BLEResult     bleQueue[BLE_QUEUE_SIZE];
static volatile int  bleHead = 0;
static volatile int  bleTail = 0;
static portMUX_TYPE  bleMux  = portMUX_INITIALIZER_UNLOCKED;

static void enqueueBLE(const char* mac, const char* name, int rssi) {
    portENTER_CRITICAL(&bleMux);
    int next = (bleHead + 1) % BLE_QUEUE_SIZE;
    if (next != bleTail) {
        strncpy(bleQueue[bleHead].mac,  mac,  17); bleQueue[bleHead].mac[17]  = 0;
        strncpy(bleQueue[bleHead].name, name, 31); bleQueue[bleHead].name[31] = 0;
        bleQueue[bleHead].rssi = rssi;
        bleHead = next;
    }
    portEXIT_CRITICAL(&bleMux);
}

class WardriveCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        const char* mac  = dev->getAddress().toString().c_str();
        const char* name = dev->haveName() ? dev->getName().c_str() : "Unknown";
        enqueueBLE(mac, name, dev->getRSSI());
        bt_found++;
    }
};

static NimBLEScan*         wdScan      = nullptr;
static WardriveCallbacks*  wdCallbacks = nullptr;
static bool                nimbleInit  = false;

static void initWardriveBLE() {
    if (nimbleInit) return;
    NimBLEDevice::init("");
    wdScan      = NimBLEDevice::getScan();
    wdCallbacks = new WardriveCallbacks();
    wdScan->setAdvertisedDeviceCallbacks(wdCallbacks, false);
    wdScan->setActiveScan(true);
    wdScan->setInterval(160);
    wdScan->setWindow(80);
    nimbleInit = true;
}

static void flushBLEQueue(const char* log_file) {
    if (!gps.location.isValid()) {
        portENTER_CRITICAL(&bleMux);
        bleTail = bleHead;
        portEXIT_CRITICAL(&bleMux);
        return;
    }
    while (true) {
        portENTER_CRITICAL(&bleMux);
        if (bleTail == bleHead) { portEXIT_CRITICAL(&bleMux); break; }
        BLEResult r = bleQueue[bleTail];
        bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
        portEXIT_CRITICAL(&bleMux);

        if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            FsFile file = sd.open(log_file, O_WRITE | O_APPEND);
            if (file) {
                file.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,%s\n",
                    r.mac, r.name, "BT-LE", gps_timestamp().c_str(),
                    0, r.rssi,
                    gps.location.lat(), gps.location.lng(),
                    gps.altitude.feet(), 10, "BT-LE");
                file.close();
            }
            xSemaphoreGive(spi_mutex);
        }
    }
}

// ─────────────────────────────────────────────
//  CORE 0 BACKGROUND TASK (Ghost Engine)
// ─────────────────────────────────────────────
void wardrive_task(void *pvParameters) {
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Wait for Core 1 BIOS + WiFi auto-connect to finish
    vTaskDelay(pdMS_TO_TICKS(15000));

    uint32_t      current_baud    = 38400;
    SerialGPS.setRxBufferSize(512);
    SerialGPS.begin(current_baud, SERIAL_8N1, 44, 43);

    unsigned long last_baud_switch = millis();
    unsigned long last_switch_time = 0;
    unsigned long last_ble_flush   = 0;
    uint32_t      last_chars_delta = 0;
    bool          scanningWiFi     = true;

    // ── SESSION FILE SELECTION ────────────────────────────────────
    // Find the next available wardrive_NNNN.csv and create it with
    // the CSV header. This happens once per boot — each power cycle
    // starts a new session file.
    int session_num = 1;
    if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        session_num = _find_next_session_number();
        snprintf(_current_log_file, sizeof(_current_log_file),
                 "/wardrive_%04d.csv", session_num);

        FsFile file = sd.open(_current_log_file, O_WRITE | O_CREAT);
        if (file) {
            file.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,"
                         "CurrentLatitude,CurrentLongitude,"
                         "AltitudeMeters,AccuracyMeters,Type");
            file.close();
        }
        xSemaphoreGive(spi_mutex);
    } else {
        // Mutex timeout fallback — use a timestamp-based name
        snprintf(_current_log_file, sizeof(_current_log_file),
                 "/wardrive_%04d.csv", (int)(millis() / 1000) % 9999);
    }

    Serial.printf("[WARDRIVE] Session file: %s\n", _current_log_file);
    // ─────────────────────────────────────────────────────────────

    initWardriveBLE();

    WiFi.mode(WIFI_STA);
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(false);
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    for (;;) {
        // GPS feed
        while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());

        // GPS auto-baud
        if (millis() - last_baud_switch > 5000) {
            uint32_t current_chars = gps.charsProcessed();
            uint32_t delta = current_chars - last_chars_delta;
            last_chars_delta = current_chars;

            if (delta < 10 || gps.passedChecksum() == 0) {
                current_baud = (current_baud == 38400) ? 9600 : 38400;
                SerialGPS.updateBaudRate(current_baud);
                Serial.printf("[GPS] Silent (%lu chars/5s). Trying %lu baud.\n",
                              (unsigned long)delta, (unsigned long)current_baud);
            } else {
                Serial.printf("[GPS] OK @ %lu baud | sats:%d | fix:%s\n",
                              (unsigned long)current_baud,
                              gps.satellites.value(),
                              gps.location.isValid() ? "YES" : "NO");
            }
            last_baud_switch = millis();
        }

        if (!wardrive_active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // TIME-SLICED RADIO (WiFi vs BLE)
        if (millis() - last_switch_time > 4000 && !wifi_in_use) {
            last_switch_time = millis();
            scanningWiFi = !scanningWiFi;

            if (scanningWiFi) {
                wdScan->clearResults();
                WiFi.mode(WIFI_STA);
                vTaskDelay(pdMS_TO_TICKS(50));

                while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());

                int n = WiFi.scanNetworks(false, true);

                while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());

                if (n > 0) {
                    networks_found = n;
                    if (gps.location.isValid()) {
                        if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            FsFile file = sd.open(_current_log_file, O_WRITE | O_APPEND);
                            if (file) {
                                for (int i = 0; i < n; ++i) {
                                    String wMac = WiFi.BSSIDstr(i);
                                    bool   wEsp = isEspressifMAC(wMac.c_str());
                                    if (wEsp) esp_found++;
                                    String ssid = wEsp
                                        ? "[ESP32] " + WiFi.SSID(i)
                                        : WiFi.SSID(i);
                                    file.printf("%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,%s\n",
                                        wMac.c_str(),
                                        ssid.c_str(),
                                        WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "WPA",
                                        gps_timestamp().c_str(),
                                        WiFi.channel(i),
                                        WiFi.RSSI(i),
                                        gps.location.lat(),
                                        gps.location.lng(),
                                        gps.altitude.meters(),
                                        5,
                                        "WIFI");
                                }
                                file.close();
                            }
                            xSemaphoreGive(spi_mutex);
                        }
                    }
                }
                WiFi.scanDelete();

            } else {
                // BLE window
                wdScan->start(2, false);
            }
        }

        // Flush BLE queue periodically
        if (millis() - last_ble_flush > 5000) {
            flushBLEQueue(_current_log_file);
            last_ble_flush = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void init_wardrive_core() {
    xTaskCreatePinnedToCore(wardrive_task, "WarDriveCore", 8192, NULL, 1, NULL, 0);
}

// ─────────────────────────────────────────────
//  GAMEPAD BLE HANDOFF HELPERS
// ─────────────────────────────────────────────
void wardrive_ble_stop() {
    wardrive_active = false;
    if (wdScan) {
        wdScan->stop();
        wdScan->setAdvertisedDeviceCallbacks(nullptr, false);
    }
}

void wardrive_ble_resume() {
    if (wdScan && wdCallbacks) {
        wdScan->setAdvertisedDeviceCallbacks(wdCallbacks, false);
    }
    wardrive_active = true;
}

// ─────────────────────────────────────────────
//  WARDRIVE UI APP
// ─────────────────────────────────────────────
void run_wardrive() {
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 320, 24, 0x18C3);
    gfx->setCursor(10, 7);
    gfx->setTextColor(0x07E0);
    gfx->setTextSize(1);
    gfx->print("WARDRIVE LIVE | TAP HEADER TO EXIT");

    // Static labels
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(2);
    gfx->setCursor(10, 35);  gfx->print("WIFI:");
    gfx->setCursor(10, 60);  gfx->print("BLE: ");
    gfx->setCursor(10, 85);  gfx->print("SATS:");
    gfx->setCursor(10, 110); gfx->print("ALT: ");
    gfx->setCursor(10, 135); gfx->print("LAT: ");
    gfx->setCursor(10, 160); gfx->print("LNG: ");

    // Show current session filename in dim text at top of content area
    gfx->setTextSize(1);
    gfx->setTextColor(0x4208);
    gfx->setCursor(10, 27);
    gfx->print(_current_log_file[0] ? _current_log_file : "waiting...");

    while (true) {
        gfx->fillRect(90, 35, 230, 150, 0x0000);

        gfx->setTextSize(2);

        gfx->setTextColor(networks_found > 0 ? 0x07E0 : 0xFD20);
        gfx->setCursor(90, 35); gfx->println(networks_found);

        gfx->setTextColor(0x07FF);
        gfx->setCursor(90, 60); gfx->println(bt_found);

        gfx->setTextColor(0xFFFF);
        gfx->setCursor(90, 85); gfx->println(gps.satellites.value());

        gfx->setTextColor(0xC618);
        gfx->setCursor(90, 110);
        gfx->printf("%.0f FT", gps.location.isValid() ? gps.altitude.feet() : 0.0);

        gfx->setTextColor(0xFFFF);
        gfx->setCursor(90, 135);
        gfx->printf("%.5f", gps.location.isValid() ? gps.location.lat() : 0.0);
        gfx->setCursor(90, 160);
        gfx->printf("%.5f", gps.location.isValid() ? gps.location.lng() : 0.0);

        if (esp_found > 0) {
            gfx->setTextSize(1);
            gfx->setTextColor(0xF800);
            gfx->setCursor(190, 175);
            gfx->printf("ESP32s: %d", esp_found);
        }

        uint16_t btnColor = wardrive_active ? 0x07E0 : 0xF800;
        uint16_t txtColor = wardrive_active ? 0x0000 : 0xFFFF;
        gfx->fillRect(10, 200, 300, 30, btnColor);
        gfx->setTextColor(txtColor);
        gfx->setTextSize(1);
        gfx->setCursor(20, 212);
        gfx->print(wardrive_active
            ? "LOGGING WIFI+BLE (TAP TO PAUSE)"
            : "PAUSED (TAP TO START)");

        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                break;
            } else if (ty > 190) {
                wardrive_active = !wardrive_active;
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
            }
        }
        delay(500);
        yield();
    }
}
