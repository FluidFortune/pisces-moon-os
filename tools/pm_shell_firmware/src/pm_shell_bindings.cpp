// pm_shell — Pisces Moon Sovereign Shell Firmware
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_shell_bindings.cpp — The pm.* namespace
//
//  Registers all binding functions as fields of a single global table
//  named "pm". Sub-tables (pm.gpio, pm.i2c, pm.storage, pm.wifi, pm.audio)
//  are created as well.
//
//  Style:
//    - Each binding is a static int pm_xxx_yyy(lua_State* L) returning
//      the number of values pushed onto the stack.
//    - luaL_check* functions handle argument validation and throw
//      Lua errors on type mismatch — no manual type guards needed.
//    - Bindings that touch hardware acquire/release within a single
//      call. None hold mutexes across lua_error (deliberate — see
//      README's "Risks" section).
//
//  Surface (~17 functions):
//    pm.version()                  → string
//    pm.uptime()                   → integer  (seconds since boot)
//    pm.millis()                   → integer  (ms since boot)
//    pm.heap()                     → table    {internal, psram, total}
//    pm.delay(ms)                  → nil      (yields via vTaskDelay)
//    pm.reboot()                   → never returns
//    pm.gpio.mode(pin, mode_str)
//    pm.gpio.read(pin)             → 0 | 1
//    pm.gpio.write(pin, level)
//    pm.i2c.scan()                 → table of integers (addresses)
//    pm.i2c.read(addr, reg)        → integer | nil
//    pm.i2c.write(addr, reg, val)  → boolean
//    pm.storage.exists(path)       → boolean
//    pm.storage.read(path)         → string | nil
//    pm.storage.write(path, text)  → boolean
//    pm.storage.list(dir)          → table of strings
//    pm.storage.mkdir(path)        → boolean
//    pm.storage.remove(path)       → boolean
//    pm.wifi.scan()                → table of network tables
//    pm.wifi.connect(ssid, pw)     → boolean
//    pm.wifi.disconnect()
//    pm.wifi.status()              → table {connected, ip, rssi}
//    pm.audio.tone(pin, freq, ms)
//    pm.dofile(path)               → boolean
// ─────────────────────────────────────────────

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// ─── SD backend selection ────────────────────────────────
// C28P routes its SD card through the SDIO 4-bit peripheral and uses
// Arduino's SD_MMC library. All other devices use the standard SPI
// path through Arduino's <SD.h>. Both backends expose fs::File and
// fs::FS, so the binding code below is identical — only init differs.
#ifdef PM_SD_BACKEND_SDMMC
  #include <FS.h>
  #include <SD_MMC.h>
  static fs::FS& sd_fs() { return SD_MMC; }
#else
  #include <FS.h>
  #include <SD.h>
  static SPIClass s_sdSPI(HSPI);
  static fs::FS& sd_fs() { return SD; }
#endif

static bool s_sd_mounted = false;
static bool s_i2c_initialized = false;

// ─── One-shot SD mount ───────────────────────────────────
//
// Lazy: we don't mount at boot. The first pm.storage.* call triggers
// the mount. If the user has no card inserted, mount fails and the
// call returns nil/false; subsequent calls retry until the user
// inserts one. This lets the shell come up clean on a card-less device.
static bool ensure_sd_mounted() {
    if (s_sd_mounted) return true;
#ifdef PM_SD_BACKEND_SDMMC
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD,
                   PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
    if (SD_MMC.begin("/sd_mmc", false, false, 20000)) {
        s_sd_mounted = true;
    }
#else
    s_sdSPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
    if (SD.begin(PIN_SD_CS, s_sdSPI, 4000000)) {
        s_sd_mounted = true;
    }
#endif
    return s_sd_mounted;
}

// ─── One-shot I2C init ───────────────────────────────────
static void ensure_i2c_initialized() {
    if (s_i2c_initialized) return;
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);   // 100 kHz, safe default
    s_i2c_initialized = true;
}

// ═════════════════════════════════════════════════════════
//  Core diagnostics
// ═════════════════════════════════════════════════════════

#ifndef PM_SHELL_VERSION
#define PM_SHELL_VERSION "0.0.0-dev"
#endif

static int pm_version(lua_State* L) {
    lua_pushstring(L, PM_SHELL_VERSION);
    return 1;
}

static int pm_uptime(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(millis() / 1000));
    return 1;
}

static int pm_millis(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)millis());
    return 1;
}

static int pm_heap(lua_State* L) {
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total    = ESP.getFreeHeap();
    size_t psram    = (total > internal) ? (total - internal) : 0;

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)internal); lua_setfield(L, -2, "internal");
    lua_pushinteger(L, (lua_Integer)psram);    lua_setfield(L, -2, "psram");
    lua_pushinteger(L, (lua_Integer)total);    lua_setfield(L, -2, "total");
    return 1;
}

static int pm_delay(lua_State* L) {
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000;   // cap at 60s so a typo doesn't hang the shell
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

static int pm_reboot(lua_State* L) {
    (void)L;
    Serial.println("[SHELL] Rebooting in 100ms...");
    Serial.flush();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;   // unreachable
}

// ═════════════════════════════════════════════════════════
//  pm.gpio
// ═════════════════════════════════════════════════════════

static int pm_gpio_mode(lua_State* L) {
    int pin = (int)luaL_checkinteger(L, 1);
    const char* mode = luaL_checkstring(L, 2);
    if      (strcmp(mode, "input")        == 0) pinMode(pin, INPUT);
    else if (strcmp(mode, "output")       == 0) pinMode(pin, OUTPUT);
    else if (strcmp(mode, "input_pullup") == 0) pinMode(pin, INPUT_PULLUP);
    else if (strcmp(mode, "input_pulldown") == 0) pinMode(pin, INPUT_PULLDOWN);
    else return luaL_error(L, "pm.gpio.mode: unknown mode '%s' "
                              "(use input/output/input_pullup/input_pulldown)", mode);
    return 0;
}

static int pm_gpio_read(lua_State* L) {
    int pin = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, digitalRead(pin));
    return 1;
}

static int pm_gpio_write(lua_State* L) {
    int pin = (int)luaL_checkinteger(L, 1);
    int level = (int)luaL_checkinteger(L, 2);
    digitalWrite(pin, level ? HIGH : LOW);
    return 0;
}

// ═════════════════════════════════════════════════════════
//  pm.i2c
// ═════════════════════════════════════════════════════════

static int pm_i2c_scan(lua_State* L) {
    ensure_i2c_initialized();
    lua_newtable(L);
    int found = 0;
    // Scan 0x08..0x77 (reserved addresses excluded)
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        Wire.beginTransmission(addr);
        uint8_t err = Wire.endTransmission();
        if (err == 0) {
            found++;
            lua_pushinteger(L, addr);
            lua_rawseti(L, -2, found);
        }
    }
    return 1;
}

static int pm_i2c_read(lua_State* L) {
    ensure_i2c_initialized();
    int addr = (int)luaL_checkinteger(L, 1);
    int reg  = (int)luaL_checkinteger(L, 2);
    Wire.beginTransmission((uint8_t)addr);
    Wire.write((uint8_t)reg);
    if (Wire.endTransmission(false) != 0) {
        lua_pushnil(L);
        return 1;
    }
    int n = Wire.requestFrom((uint8_t)addr, (uint8_t)1);
    if (n != 1 || !Wire.available()) {
        lua_pushnil(L);
        return 1;
    }
    int val = Wire.read();
    lua_pushinteger(L, val);
    return 1;
}

static int pm_i2c_write(lua_State* L) {
    ensure_i2c_initialized();
    int addr = (int)luaL_checkinteger(L, 1);
    int reg  = (int)luaL_checkinteger(L, 2);
    int val  = (int)luaL_checkinteger(L, 3);
    Wire.beginTransmission((uint8_t)addr);
    Wire.write((uint8_t)reg);
    Wire.write((uint8_t)val);
    uint8_t err = Wire.endTransmission();
    lua_pushboolean(L, err == 0);
    return 1;
}

// ═════════════════════════════════════════════════════════
//  pm.storage
// ═════════════════════════════════════════════════════════

static int pm_storage_exists(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!ensure_sd_mounted()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, sd_fs().exists(path) ? 1 : 0);
    return 1;
}

static int pm_storage_read(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!ensure_sd_mounted()) { lua_pushnil(L); return 1; }
    fs::File f = sd_fs().open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        lua_pushnil(L);
        return 1;
    }
    // Read into a luaL_Buffer in chunks
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    uint8_t chunk[256];
    while (f.available()) {
        int n = f.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        luaL_addlstring(&b, (const char*)chunk, n);
    }
    f.close();
    luaL_pushresult(&b);
    return 1;
}

static int pm_storage_write(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    size_t len;
    const char* text = luaL_checklstring(L, 2, &len);
    if (!ensure_sd_mounted()) { lua_pushboolean(L, 0); return 1; }
    fs::File f = sd_fs().open(path, FILE_WRITE);
    if (!f) { lua_pushboolean(L, 0); return 1; }
    size_t written = f.write(reinterpret_cast<const uint8_t*>(text), len);
    f.close();
    lua_pushboolean(L, written == len ? 1 : 0);
    return 1;
}

static int pm_storage_list(lua_State* L) {
    const char* dirpath = luaL_checkstring(L, 1);
    lua_newtable(L);
    if (!ensure_sd_mounted()) return 1;
    fs::File dir = sd_fs().open(dirpath);
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return 1;
    }
    int idx = 0;
    fs::File entry = dir.openNextFile();
    while (entry) {
        // Both SD and SD_MMC return entry.name() as a leaf or as a
        // full path depending on library version. Strip any directory
        // prefix to be consistent across backends.
        const char* n = entry.name();
        const char* slash = strrchr(n, '/');
        if (slash) n = slash + 1;
        idx++;
        lua_pushstring(L, n);
        lua_rawseti(L, -2, idx);
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return 1;
}

static int pm_storage_mkdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!ensure_sd_mounted()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, sd_fs().mkdir(path) ? 1 : 0);
    return 1;
}

static int pm_storage_remove(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!ensure_sd_mounted()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, sd_fs().remove(path) ? 1 : 0);
    return 1;
}

// ═════════════════════════════════════════════════════════
//  pm.wifi
// ═════════════════════════════════════════════════════════

// Returns a string describing the encryption type for human-friendly
// scan output. Used by pm.wifi.scan().
static const char* enc_to_string(int enc) {
    switch (enc) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:              return "WEP";
        case WIFI_AUTH_WPA_PSK:          return "WPA";
        case WIFI_AUTH_WPA2_PSK:         return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:     return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE:  return "WPA2-EAP";
        case WIFI_AUTH_WPA3_PSK:         return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:    return "WPA2/WPA3";
        default:                         return "unknown";
    }
}

static int pm_wifi_scan(lua_State* L) {
    // Ensure STA mode before scanning. If user previously disconnected,
    // mode may have dropped — bring it back up.
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true);   // synchronous, include hidden
    lua_newtable(L);
    if (n <= 0) return 1;

    for (int i = 0; i < n; i++) {
        lua_newtable(L);
        lua_pushstring(L,  WiFi.SSID(i).c_str());          lua_setfield(L, -2, "ssid");
        lua_pushstring(L,  WiFi.BSSIDstr(i).c_str());      lua_setfield(L, -2, "bssid");
        lua_pushinteger(L, WiFi.RSSI(i));                  lua_setfield(L, -2, "rssi");
        lua_pushinteger(L, WiFi.channel(i));               lua_setfield(L, -2, "channel");
        lua_pushstring(L,  enc_to_string(WiFi.encryptionType(i)));
        lua_setfield(L, -2, "encryption");
        lua_rawseti(L, -2, i + 1);
    }
    WiFi.scanDelete();
    return 1;
}

static int pm_wifi_connect(lua_State* L) {
    const char* ssid     = luaL_checkstring(L, 1);
    const char* password = luaL_optstring(L, 2, "");   // open networks ok
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    // Wait up to ~6 seconds for connection
    uint32_t deadline = millis() + 6000;
    while (WiFi.status() != WL_CONNECTED &&
           (int32_t)(deadline - millis()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    lua_pushboolean(L, WiFi.status() == WL_CONNECTED ? 1 : 0);
    return 1;
}

static int pm_wifi_disconnect(lua_State* L) {
    (void)L;
    WiFi.disconnect(true, false);
    return 0;
}

static int pm_wifi_status(lua_State* L) {
    lua_newtable(L);
    bool connected = (WiFi.status() == WL_CONNECTED);
    lua_pushboolean(L, connected ? 1 : 0); lua_setfield(L, -2, "connected");
    if (connected) {
        lua_pushstring(L,  WiFi.localIP().toString().c_str()); lua_setfield(L, -2, "ip");
        lua_pushinteger(L, WiFi.RSSI());                       lua_setfield(L, -2, "rssi");
        lua_pushstring(L,  WiFi.SSID().c_str());               lua_setfield(L, -2, "ssid");
    } else {
        lua_pushstring(L, ""); lua_setfield(L, -2, "ip");
        lua_pushinteger(L, 0); lua_setfield(L, -2, "rssi");
        lua_pushstring(L, ""); lua_setfield(L, -2, "ssid");
    }
    return 1;
}

// ═════════════════════════════════════════════════════════
//  pm.audio.tone — device-agnostic via LEDC
// ═════════════════════════════════════════════════════════
//
// Uses ESP32-S3's LEDC (PWM) peripheral to generate a square wave on
// any pin. No codec assumptions — works whether the device has an
// ES8311, a MAX98357A, a piezo buzzer, or nothing at all (in which
// case the pin just toggles uselessly).
//
// Arduino-ESP32 3.x changed the LEDC API. We use the unified call:
//   ledcAttach(pin, freq, resolution) → bool
//   ledcWrite(pin, duty) → bool
// This works in core 3.x; on 2.x we fall back to the channel-based API.

#if ESP_ARDUINO_VERSION_MAJOR >= 3
static int pm_audio_tone(lua_State* L) {
    int pin  = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    int ms   = (int)luaL_checkinteger(L, 3);
    if (freq < 20 || freq > 20000) {
        return luaL_error(L, "pm.audio.tone: freq must be 20..20000 Hz");
    }
    if (ms < 1 || ms > 10000) {
        return luaL_error(L, "pm.audio.tone: ms must be 1..10000");
    }
    if (!ledcAttach(pin, freq, 8)) {
        return luaL_error(L, "pm.audio.tone: ledcAttach failed on pin %d", pin);
    }
    ledcWrite(pin, 128);   // 50% duty
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledcWrite(pin, 0);
    ledcDetach(pin);
    return 0;
}
#else
// Arduino-ESP32 2.x — channel-based LEDC API.
// We use channel 0 throughout. Sufficient for one-shot tones.
static int pm_audio_tone(lua_State* L) {
    int pin  = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    int ms   = (int)luaL_checkinteger(L, 3);
    if (freq < 20 || freq > 20000) {
        return luaL_error(L, "pm.audio.tone: freq must be 20..20000 Hz");
    }
    if (ms < 1 || ms > 10000) {
        return luaL_error(L, "pm.audio.tone: ms must be 1..10000");
    }
    ledcSetup(0, freq, 8);
    ledcAttachPin(pin, 0);
    ledcWrite(0, 128);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledcWrite(0, 0);
    ledcDetachPin(pin);
    return 0;
}
#endif

// ═════════════════════════════════════════════════════════
//  pm.dofile
// ═════════════════════════════════════════════════════════
//
// Loads a Lua script from the SD card and runs it through the same
// lua_State as the REPL. Replaces stdlib dofile() which doesn't know
// about SD_MMC paths or non-default SPI buses.

static int pm_dofile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!ensure_sd_mounted()) {
        return luaL_error(L, "pm.dofile: SD card not mounted (insert and retry)");
    }
    fs::File f = sd_fs().open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        return luaL_error(L, "pm.dofile: cannot open '%s'", path);
    }

    // Read the whole file into a buffer. Scripts are expected to be
    // small; we cap at 64 KB to keep the heap impact bounded.
    size_t size = f.size();
    if (size == 0) {
        f.close();
        lua_pushboolean(L, 1);
        return 1;
    }
    if (size > 65536) {
        f.close();
        return luaL_error(L, "pm.dofile: '%s' too large (%u bytes, cap is 64 KB)",
                          path, (unsigned)size);
    }
    char* buf = (char*)malloc(size);
    if (!buf) {
        f.close();
        return luaL_error(L, "pm.dofile: out of memory loading '%s'", path);
    }
    size_t got = f.read(reinterpret_cast<uint8_t*>(buf), size);
    f.close();
    if (got != size) {
        free(buf);
        return luaL_error(L, "pm.dofile: short read on '%s' (%u/%u)",
                          path, (unsigned)got, (unsigned)size);
    }

    // Use the basename as the chunk name for error messages
    const char* chunkname = strrchr(path, '/');
    chunkname = chunkname ? chunkname + 1 : path;
    int status = luaL_loadbuffer(L, buf, size, chunkname);
    free(buf);
    if (status != LUA_OK) {
        // The error message is now on top of the stack.
        return lua_error(L);
    }
    // Run with whatever the script returns left on the stack.
    int top_before = lua_gettop(L) - 1;
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        return lua_error(L);
    }
    int n_returned = lua_gettop(L) - top_before;
    // If script returned nothing, push a success boolean for convenience
    if (n_returned == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    return n_returned;
}

// ═════════════════════════════════════════════════════════
//  Registration
// ═════════════════════════════════════════════════════════

// Helper: build a sub-table named `name` inside the table at the top
// of the stack, register `funcs` into it, then pop the sub-table.
static void register_subtable(lua_State* L, const char* name, const luaL_Reg* funcs) {
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    lua_setfield(L, -2, name);
}

// Top-level pm.* functions (not under a sub-table)
static const luaL_Reg pm_top_funcs[] = {
    {"version",  pm_version},
    {"uptime",   pm_uptime},
    {"millis",   pm_millis},
    {"heap",     pm_heap},
    {"delay",    pm_delay},
    {"reboot",   pm_reboot},
    {"dofile",   pm_dofile},
    {NULL, NULL}
};

static const luaL_Reg pm_gpio_funcs[] = {
    {"mode",  pm_gpio_mode},
    {"read",  pm_gpio_read},
    {"write", pm_gpio_write},
    {NULL, NULL}
};

static const luaL_Reg pm_i2c_funcs[] = {
    {"scan",  pm_i2c_scan},
    {"read",  pm_i2c_read},
    {"write", pm_i2c_write},
    {NULL, NULL}
};

static const luaL_Reg pm_storage_funcs[] = {
    {"exists", pm_storage_exists},
    {"read",   pm_storage_read},
    {"write",  pm_storage_write},
    {"list",   pm_storage_list},
    {"mkdir",  pm_storage_mkdir},
    {"remove", pm_storage_remove},
    {NULL, NULL}
};

static const luaL_Reg pm_wifi_funcs[] = {
    {"scan",       pm_wifi_scan},
    {"connect",    pm_wifi_connect},
    {"disconnect", pm_wifi_disconnect},
    {"status",     pm_wifi_status},
    {NULL, NULL}
};

static const luaL_Reg pm_audio_funcs[] = {
    {"tone", pm_audio_tone},
    {NULL, NULL}
};

void pm_shell_register_bindings(lua_State* L) {
    lua_newtable(L);                              // the pm table
    luaL_setfuncs(L, pm_top_funcs, 0);            // top-level fns

    register_subtable(L, "gpio",    pm_gpio_funcs);
    register_subtable(L, "i2c",     pm_i2c_funcs);
    register_subtable(L, "storage", pm_storage_funcs);
    register_subtable(L, "wifi",    pm_wifi_funcs);
    register_subtable(L, "audio",   pm_audio_funcs);

    lua_setglobal(L, "pm");
}
