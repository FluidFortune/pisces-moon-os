// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  weather.cpp — Open-Meteo weather app, all devices
//
//  ENTRY POINT: void run_weather();
//
//  Shared core (parsing, API call, weather code → text mapping)
//  is identical on all devices. The render + input loop is per-device
//  because screen geometry and input modalities differ.
//
//  PORTRAIT DEVICES (C28P, Heltec V4, C5):
//    240×320, touch-only. Top exit bar at y<14. Card layout vertical.
//
//  T-DECK PLUS:
//    320×240 landscape. Keyboard + trackball + (no touch). 
//    Two-column: left current weather, right 3-day forecast.
//    Keyboard `q`/ESC quit, `r` refresh.
//
//  CARDPUTER ADV:
//    240×135 landscape. Keyboard only. Compact stack:
//    one line big temp, one line conditions, three small forecast rows.
//    Keyboard `q`/ESC quit, `r` refresh.
//
//  T-LORA PAGER:
//    480×222 landscape. NES buttons + touch.
//    Wide layout: current weather panel left, forecast strip right.
//    B button or top-touch quit, A button or refresh-touch refresh.
//
//  API: Open-Meteo (https://api.open-meteo.com/v1/forecast)
//    Free, no key, no rate limits for personal use.
// ─────────────────────────────────────────────

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "nosql_store.h"
#include "game_input.h"

extern Arduino_GFX *gfx;

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
extern bool c28p_touch_read(int16_t *x, int16_t *y);
#endif
#if defined(DEVICE_C5)
extern bool c5_touch_read(int16_t *x, int16_t *y);
#endif

// T-LoRa Pager has no touch hardware — NES buttons (dpad + A/B) only.

// ─── Default location: Pasadena, CA ───
static constexpr float DEFAULT_LAT = 34.1478f;
static constexpr float DEFAULT_LON = -118.1445f;
static constexpr const char* DEFAULT_LOC_NAME = "PASADENA, CA";

// ─── State ───
struct WeatherData {
    bool        valid = false;
    char        location[32];
    float       lat = DEFAULT_LAT;
    float       lon = DEFAULT_LON;
    float       current_temp_f = 0.0f;
    int         current_code = 0;
    float       wind_mph = 0.0f;
    int         day_codes[3] = {0,0,0};
    float       day_highs[3] = {0,0,0};
    float       day_lows[3]  = {0,0,0};
    char        day_names[3][12];
    uint32_t    fetched_at = 0;
};

static WeatherData wx;

// ─── Weather code → text + color ───
struct WCodeInfo { int code; const char* text; uint16_t color; };

static const WCodeInfo W_CODES[] = {
    {  0, "CLEAR",          0xFFE0 },
    {  1, "MOSTLY CLEAR",   0xFFE0 },
    {  2, "PARTLY CLOUDY",  0xC618 },
    {  3, "OVERCAST",       0x8410 },
    { 45, "FOG",            0xC618 },
    { 48, "RIME FOG",       0xC618 },
    { 51, "LT DRIZZLE",     0x07FF },
    { 53, "DRIZZLE",        0x07FF },
    { 55, "HVY DRIZZLE",    0x07FF },
    { 61, "LIGHT RAIN",     0x041F },
    { 63, "RAIN",           0x041F },
    { 65, "HEAVY RAIN",     0x041F },
    { 71, "LIGHT SNOW",     0xFFFF },
    { 73, "SNOW",           0xFFFF },
    { 75, "HEAVY SNOW",     0xFFFF },
    { 80, "SHOWERS",        0x041F },
    { 81, "SHOWERS",        0x041F },
    { 82, "HVY SHOWERS",    0x041F },
    { 95, "THUNDERSTORM",   0xF800 },
    { 96, "THUNDER+HAIL",   0xF800 },
    { 99, "THUNDER+HAIL",   0xF800 },
};
static constexpr int W_CODES_N = sizeof(W_CODES) / sizeof(WCodeInfo);

static const char* w_code_text(int c) {
    for (int i = 0; i < W_CODES_N; i++)
        if (W_CODES[i].code == c) return W_CODES[i].text;
    return "UNKNOWN";
}
static uint16_t w_code_color(int c) {
    for (int i = 0; i < W_CODES_N; i++)
        if (W_CODES[i].code == c) return W_CODES[i].color;
    return 0xFFFF;
}

static void compute_day_names() {
    snprintf(wx.day_names[0], sizeof(wx.day_names[0]), "TODAY");
    snprintf(wx.day_names[1], sizeof(wx.day_names[1]), "TMRW");
    snprintf(wx.day_names[2], sizeof(wx.day_names[2]), "+2");
}

// ─── Open-Meteo fetch (identical across devices) ───
static bool fetch_weather() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WX] WiFi not connected");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,wind_speed_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&temperature_unit=fahrenheit&wind_speed_unit=mph"
        "&forecast_days=3&timezone=auto",
        wx.lat, wx.lon);

    Serial.printf("[WX] GET %s\n", url);
    http.begin(client, url);
    http.setTimeout(8000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[WX] HTTP %d\n", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    StaticJsonDocument<2048> doc;
    if (deserializeJson(doc, body)) {
        Serial.println("[WX] JSON parse failed");
        return false;
    }

    JsonObject current = doc["current"];
    if (!current.isNull()) {
        wx.current_temp_f = current["temperature_2m"] | 0.0f;
        wx.current_code = current["weather_code"] | 0;
        wx.wind_mph = current["wind_speed_10m"] | 0.0f;
    }

    JsonObject daily = doc["daily"];
    if (!daily.isNull()) {
        JsonArray codes = daily["weather_code"];
        JsonArray highs = daily["temperature_2m_max"];
        JsonArray lows  = daily["temperature_2m_min"];
        for (int i = 0; i < 3; i++) {
            wx.day_codes[i] = (codes && i < (int)codes.size()) ? codes[i].as<int>() : 0;
            wx.day_highs[i] = (highs && i < (int)highs.size()) ? highs[i].as<float>() : 0.0f;
            wx.day_lows[i]  = (lows  && i < (int)lows.size())  ? lows[i].as<float>()  : 0.0f;
        }
    }

    compute_day_names();
    wx.valid = true;
    wx.fetched_at = millis();
    return true;
}

static void load_location() {
    snprintf(wx.location, sizeof(wx.location), "%s", DEFAULT_LOC_NAME);
    wx.lat = DEFAULT_LAT;
    wx.lon = DEFAULT_LON;

    nosql_init("settings");
    int total = nosql_get_count("settings");
    String t, c;
    for (int i = 0; i < total; i++) {
        if (!nosql_get_entry("settings", i, t, c)) continue;
        if (t == "weather_location") {
            int p1 = c.indexOf('|');
            int p2 = c.indexOf('|', p1 + 1);
            if (p1 > 0 && p2 > p1) {
                String name = c.substring(0, p1);
                snprintf(wx.location, sizeof(wx.location), "%s", name.c_str());
                wx.lat = c.substring(p1 + 1, p2).toFloat();
                wx.lon = c.substring(p2 + 1).toFloat();
            }
            break;
        }
    }
}

// ─────────────────────────────────────────────
//  PER-DEVICE RENDER + LOOP
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4) || defined(DEVICE_C5)
// ─── Portrait 240×320 (C28P, Heltec V4, C5) ───
// All three boards share the same 240×320 portrait geometry and the
// same touch-only input model. Only the touch driver differs:
// C28P/Heltec use the FT6336G capacitive controller (c28p_touch_read),
// the C5 uses the XPT2046 resistive controller (c5_touch_read).

static void wx_p_chrome() {
    gfx->fillRect(0, 14, 240, 320 - 14, 0x0000);
    gfx->fillRect(0, 14, 240, 22, 0x031F);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 18);
    gfx->print("WEATHER");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 22);
    gfx->print("< EXIT");
}

static void wx_p_loading() {
    gfx->fillRect(0, 40, 240, 280, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(40, 130);
    gfx->print("Fetching...");
}

static void wx_p_error(const char* msg) {
    gfx->fillRect(0, 40, 240, 280, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xF800);
    gfx->setCursor(20, 110);
    gfx->print("ERROR");
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(20, 140);
    gfx->print(msg);
    gfx->setTextColor(0x8410);
    gfx->setCursor(20, 160);
    gfx->print("Check WiFi connection.");
}

static void wx_p_data() {
    gfx->fillRect(0, 40, 240, 280, 0x0000);

    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 44);
    gfx->print(wx.location);

    gfx->setTextSize(4);
    gfx->setTextColor(0xFFE0);
    char tempbuf[8];
    snprintf(tempbuf, sizeof(tempbuf), "%d", (int)round(wx.current_temp_f));
    gfx->setCursor(20, 60);
    gfx->print(tempbuf);
    gfx->setTextSize(2);
    gfx->setCursor(20 + strlen(tempbuf) * 24 + 4, 64);
    gfx->print("F");

    gfx->setTextSize(1);
    gfx->setTextColor(w_code_color(wx.current_code));
    gfx->setCursor(20, 100);
    gfx->print(w_code_text(wx.current_code));
    gfx->setTextColor(0xC618);
    gfx->setCursor(20, 114);
    gfx->printf("WIND: %.0f MPH", wx.wind_mph);

    gfx->drawFastHLine(0, 138, 240, 0x4208);
    gfx->setTextColor(0x8410);
    gfx->setCursor(8, 142);
    gfx->print("3-DAY FORECAST");

    for (int i = 0; i < 3; i++) {
        int y = 156 + i * 36;
        gfx->fillRect(8, y, 224, 30, 0x18C3);
        gfx->drawRect(8, y, 224, 30, 0x4208);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(16, y + 4);
        gfx->print(wx.day_names[i]);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(16, y + 16);
        gfx->printf("%d / %d F",
                    (int)round(wx.day_highs[i]),
                    (int)round(wx.day_lows[i]));
        gfx->setTextColor(w_code_color(wx.day_codes[i]));
        gfx->setCursor(110, y + 10);
        gfx->print(w_code_text(wx.day_codes[i]));
    }

    int btn_y = 270;
    gfx->fillRect(20, btn_y, 200, 36, 0x0260);
    gfx->drawRect(20, btn_y, 200, 36, 0x07E0);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(64, btn_y + 10);
    gfx->print("REFRESH");
}

static void wx_portrait_loop() {
    wx_p_chrome();
    wx_p_loading();
    if (!fetch_weather()) wx_p_error("Fetch failed");
    else wx_p_data();

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
#if defined(DEVICE_C5)
        bool touched = c5_touch_read(&tx, &ty);
#else
        bool touched = c28p_touch_read(&tx, &ty);
#endif

        if (touched && !was_touched) {
            if (ty < 14) pressed = 1;
            else if (ty >= 270 && ty < 306 && tx >= 20 && tx < 220) pressed = 0;
        } else if (!touched && was_touched) {
            if (pressed == 1) return;
            else if (pressed == 0) {
                wx_p_loading();
                if (!fetch_weather()) wx_p_error("Fetch failed");
                else wx_p_data();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}
#endif // portrait devices

#if defined(DEVICE_TDECK_PLUS)
// ─── T-Deck Plus 320×240 landscape ───

static void wx_tdeck_chrome() {
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 320, 22, 0x031F);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 4);
    gfx->print("WEATHER");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(200, 8);
    gfx->print("Q=QUIT  R=REFRESH");
}

static void wx_tdeck_loading() {
    gfx->fillRect(0, 26, 320, 240 - 26, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(80, 110);
    gfx->print("Fetching...");
}

static void wx_tdeck_error(const char* msg) {
    gfx->fillRect(0, 26, 320, 240 - 26, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xF800);
    gfx->setCursor(20, 90);
    gfx->print("ERROR");
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(20, 120);
    gfx->print(msg);
}

static void wx_tdeck_data() {
    gfx->fillRect(0, 26, 320, 240 - 26, 0x0000);

    // Left half — current
    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 30);
    gfx->print(wx.location);

    gfx->setTextSize(5);
    gfx->setTextColor(0xFFE0);
    char tempbuf[8];
    snprintf(tempbuf, sizeof(tempbuf), "%d", (int)round(wx.current_temp_f));
    gfx->setCursor(20, 60);
    gfx->print(tempbuf);
    gfx->setTextSize(2);
    gfx->setCursor(20 + strlen(tempbuf) * 30 + 4, 76);
    gfx->print("F");

    gfx->setTextSize(1);
    gfx->setTextColor(w_code_color(wx.current_code));
    gfx->setCursor(20, 130);
    gfx->print(w_code_text(wx.current_code));
    gfx->setTextColor(0xC618);
    gfx->setCursor(20, 146);
    gfx->printf("WIND: %.0f MPH", wx.wind_mph);

    // Right half — 3-day forecast
    gfx->drawFastVLine(170, 30, 200, 0x4208);
    gfx->setTextColor(0x8410);
    gfx->setCursor(180, 30);
    gfx->print("3-DAY FORECAST");
    for (int i = 0; i < 3; i++) {
        int y = 48 + i * 56;
        gfx->fillRect(178, y, 138, 50, 0x18C3);
        gfx->drawRect(178, y, 138, 50, 0x4208);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(184, y + 4);
        gfx->print(wx.day_names[i]);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(184, y + 18);
        gfx->printf("%d/%d F",
                    (int)round(wx.day_highs[i]),
                    (int)round(wx.day_lows[i]));
        gfx->setTextColor(w_code_color(wx.day_codes[i]));
        gfx->setCursor(184, y + 32);
        gfx->print(w_code_text(wx.day_codes[i]));
    }
}

static void wx_tdeck_loop() {
    wx_tdeck_chrome();
    wx_tdeck_loading();
    if (!fetch_weather()) wx_tdeck_error("Fetch failed");
    else wx_tdeck_data();

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return;
        if (input.key == 'r' || input.key == 'R') {
            wx_tdeck_loading();
            if (!fetch_weather()) wx_tdeck_error("Fetch failed");
            else wx_tdeck_data();
        }
        delay(30); yield();
    }
}
#endif // DEVICE_TDECK_PLUS

#if defined(DEVICE_CARDPUTER_ADV)
// ─── Cardputer 240×135 landscape ───

static void wx_cp_chrome() {
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 240, 14, 0x031F);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(4, 4);
    gfx->print("WX");
    gfx->setTextColor(0x8410);
    gfx->setCursor(120, 4);
    gfx->print("Q=quit R=refresh");
}

static void wx_cp_loading() {
    gfx->fillRect(0, 16, 240, 135 - 16, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(80, 60);
    gfx->print("Fetching...");
}

static void wx_cp_error(const char* msg) {
    gfx->fillRect(0, 16, 240, 135 - 16, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xF800);
    gfx->setCursor(4, 30);
    gfx->print("ERROR: ");
    gfx->setTextColor(0xFFFF);
    gfx->print(msg);
}

static void wx_cp_data() {
    gfx->fillRect(0, 16, 240, 135 - 16, 0x0000);

    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(4, 18);
    gfx->print(wx.location);

    // Big temp + conditions on one row
    gfx->setTextSize(3);
    gfx->setTextColor(0xFFE0);
    char tempbuf[8];
    snprintf(tempbuf, sizeof(tempbuf), "%dF", (int)round(wx.current_temp_f));
    gfx->setCursor(4, 30);
    gfx->print(tempbuf);

    gfx->setTextSize(1);
    gfx->setTextColor(w_code_color(wx.current_code));
    gfx->setCursor(100, 32);
    gfx->print(w_code_text(wx.current_code));
    gfx->setTextColor(0xC618);
    gfx->setCursor(100, 46);
    gfx->printf("Wind %.0f mph", wx.wind_mph);

    // 3-day compact rows
    gfx->drawFastHLine(0, 62, 240, 0x4208);
    for (int i = 0; i < 3; i++) {
        int y = 68 + i * 18;
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(4, y);
        gfx->print(wx.day_names[i]);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(50, y);
        gfx->printf("%d/%d F", (int)round(wx.day_highs[i]), (int)round(wx.day_lows[i]));
        gfx->setTextColor(w_code_color(wx.day_codes[i]));
        gfx->setCursor(120, y);
        gfx->print(w_code_text(wx.day_codes[i]));
    }
}

static void wx_cp_loop() {
    wx_cp_chrome();
    wx_cp_loading();
    if (!fetch_weather()) wx_cp_error("Fetch failed");
    else wx_cp_data();

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return;
        if (input.key == 'r' || input.key == 'R') {
            wx_cp_loading();
            if (!fetch_weather()) wx_cp_error("Fetch failed");
            else wx_cp_data();
        }
        delay(30); yield();
    }
}
#endif // DEVICE_CARDPUTER_ADV

#if defined(DEVICE_TLORAPAGER)
// ─── T-LoRa Pager 480×222 landscape ───

static void wx_tlp_chrome() {
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 480, 22, 0x031F);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 4);
    gfx->print("WEATHER");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(340, 8);
    gfx->print("B=QUIT  A=REFRESH");
}

static void wx_tlp_loading() {
    gfx->fillRect(0, 26, 480, 222 - 26, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(160, 100);
    gfx->print("Fetching...");
}

static void wx_tlp_error(const char* msg) {
    gfx->fillRect(0, 26, 480, 222 - 26, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xF800);
    gfx->setCursor(20, 80);
    gfx->print("ERROR");
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(20, 110);
    gfx->print(msg);
}

static void wx_tlp_data() {
    gfx->fillRect(0, 26, 480, 222 - 26, 0x0000);

    // Left third — current
    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 30);
    gfx->print(wx.location);

    gfx->setTextSize(6);
    gfx->setTextColor(0xFFE0);
    char tempbuf[8];
    snprintf(tempbuf, sizeof(tempbuf), "%d", (int)round(wx.current_temp_f));
    gfx->setCursor(16, 56);
    gfx->print(tempbuf);
    gfx->setTextSize(2);
    gfx->setCursor(16 + strlen(tempbuf) * 36 + 4, 80);
    gfx->print("F");

    gfx->setTextSize(2);
    gfx->setTextColor(w_code_color(wx.current_code));
    gfx->setCursor(16, 130);
    gfx->print(w_code_text(wx.current_code));
    gfx->setTextSize(1);
    gfx->setTextColor(0xC618);
    gfx->setCursor(16, 158);
    gfx->printf("WIND: %.0f MPH", wx.wind_mph);

    // Right two-thirds — 3-day stacked horizontally
    gfx->drawFastVLine(220, 30, 180, 0x4208);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(232, 30);
    gfx->print("3-DAY FORECAST");
    for (int i = 0; i < 3; i++) {
        int x = 232 + i * 82;
        int y = 50;
        gfx->fillRect(x, y, 76, 140, 0x18C3);
        gfx->drawRect(x, y, 76, 140, 0x4208);
        gfx->setTextSize(2);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(x + 8, y + 8);
        gfx->print(wx.day_names[i]);
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(x + 8, y + 40);
        gfx->printf("Hi %d F", (int)round(wx.day_highs[i]));
        gfx->setCursor(x + 8, y + 56);
        gfx->printf("Lo %d F", (int)round(wx.day_lows[i]));
        gfx->setTextColor(w_code_color(wx.day_codes[i]));
        gfx->setCursor(x + 8, y + 90);
        gfx->print(w_code_text(wx.day_codes[i]));
    }
}

static void wx_tlp_loop() {
    wx_tlp_chrome();
    wx_tlp_loading();
    if (!fetch_weather()) wx_tlp_error("Fetch failed");
    else wx_tlp_data();

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit || input.b) return;
        if (input.a) {
            wx_tlp_loading();
            if (!fetch_weather()) wx_tlp_error("Fetch failed");
            else wx_tlp_data();
        }
        delay(30); yield();
    }
}
#endif // DEVICE_TLORAPAGER

// ─────────────────────────────────────────────
//  PUBLIC ENTRY — dispatches by device
// ─────────────────────────────────────────────
void run_weather() {
    load_location();

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4) || defined(DEVICE_C5)
    wx_portrait_loop();
#elif defined(DEVICE_TDECK_PLUS)
    wx_tdeck_loop();
#elif defined(DEVICE_CARDPUTER_ADV)
    wx_cp_loop();
#elif defined(DEVICE_TLORAPAGER)
    wx_tlp_loop();
#else
    // Unknown device — show fallback
    gfx->fillScreen(0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xF800);
    gfx->setCursor(10, 10);
    gfx->print("Weather not configured for this device");
    delay(2000);
#endif
}

// Keep C28P-specific entry name as alias for backward compatibility
// with c28p_boot.cpp dispatch table.
#ifdef DEVICE_C28P
void c28p_run_weather() { run_weather(); }
#endif