// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_weather.cpp — Weather app for C28P
//
//  PURPOSE:
//
//  Fetches current weather + 3-day forecast from Open-Meteo (free,
//  no API key required) and displays them on the C28P's 240×320
//  portrait touchscreen. Location is configurable; defaults to the
//  user's last-saved location (NoSQL "settings/location") or falls
//  back to Pasadena, CA (Eric's general area) if no location saved.
//
//  Open-Meteo endpoints:
//    https://api.open-meteo.com/v1/forecast?latitude=X&longitude=Y
//        &current=temperature_2m,weather_code,wind_speed_10m
//        &daily=weather_code,temperature_2m_max,temperature_2m_min
//        &temperature_unit=fahrenheit
//        &wind_speed_unit=mph
//        &timezone=auto
//
//  TOUCH UI LAYOUT:
//
//   y=  0..14   Exit bar (owned by dpad layer; tap exits)
//   y= 16..32   Location name (large)
//   y= 36..86   Current conditions panel (temp + icon + description)
//   y= 92..192  3-day forecast (3 rows × 32px each)
//   y=196..220  Wind + humidity row
//   y=224..260  REFRESH button
//   y=264..300  CHANGE LOCATION button
//
//  No D-pad chrome — this is a touch-only app and the dpad layer
//  is hidden when c28p_run_weather() is running. Top exit-bar
//  region still routes to exit.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "c28p_dpad.h"
#include "nosql_store.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t *x, int16_t *y);

// ─── Default location (Pasadena, CA fallback) ───
static constexpr float DEFAULT_LAT = 34.1478f;
static constexpr float DEFAULT_LON = -118.1445f;
static constexpr const char* DEFAULT_LOC_NAME = "PASADENA, CA";

// ─── Weather state ───
struct WeatherData {
    bool        valid = false;
    char        location[32];
    float       current_temp_f;
    int         current_code;
    float       wind_mph;
    int         day_codes[3];
    float       day_highs[3];
    float       day_lows[3];
    char        day_names[3][12];
    uint32_t    fetched_at;
};

static WeatherData wx;

// ─── Weather code → text + color ───
struct WCodeInfo {
    int code;
    const char* text;
    uint16_t color;
};

static const WCodeInfo W_CODES[] = {
    {  0, "CLEAR",            0xFFE0 },  // yellow
    {  1, "MOSTLY CLEAR",     0xFFE0 },
    {  2, "PARTLY CLOUDY",    0xC618 },  // light grey
    {  3, "OVERCAST",         0x8410 },  // grey
    { 45, "FOG",              0xC618 },
    { 48, "RIME FOG",         0xC618 },
    { 51, "LIGHT DRIZZLE",    0x07FF },  // cyan
    { 53, "DRIZZLE",          0x07FF },
    { 55, "HEAVY DRIZZLE",    0x07FF },
    { 61, "LIGHT RAIN",       0x041F },  // blue
    { 63, "RAIN",             0x041F },
    { 65, "HEAVY RAIN",       0x041F },
    { 71, "LIGHT SNOW",       0xFFFF },  // white
    { 73, "SNOW",             0xFFFF },
    { 75, "HEAVY SNOW",       0xFFFF },
    { 80, "RAIN SHOWERS",     0x041F },
    { 81, "RAIN SHOWERS",     0x041F },
    { 82, "HEAVY SHOWERS",    0x041F },
    { 95, "THUNDERSTORM",     0xF800 },  // red
    { 96, "THUNDER + HAIL",   0xF800 },
    { 99, "THUNDER + HAIL",   0xF800 },
};
static constexpr int W_CODES_N = sizeof(W_CODES) / sizeof(WCodeInfo);

static const char* w_code_text(int code) {
    for (int i = 0; i < W_CODES_N; i++) {
        if (W_CODES[i].code == code) return W_CODES[i].text;
    }
    return "UNKNOWN";
}

static uint16_t w_code_color(int code) {
    for (int i = 0; i < W_CODES_N; i++) {
        if (W_CODES[i].code == code) return W_CODES[i].color;
    }
    return 0xFFFF;
}

// ─── Day name from offset ───
static const char* W_DAYS[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

static void compute_day_names() {
    // Without a real-time clock from NTP, just label as TODAY/TOMORROW/+2
    snprintf(wx.day_names[0], sizeof(wx.day_names[0]), "TODAY");
    snprintf(wx.day_names[1], sizeof(wx.day_names[1]), "TOMORROW");
    snprintf(wx.day_names[2], sizeof(wx.day_names[2]), "+2 DAYS");
}

// ─── Fetch from Open-Meteo ───
static bool fetch_weather(float lat, float lon) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[C28P-WX] WiFi not connected");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();   // Open-Meteo cert chain; we don't pin
    HTTPClient http;

    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,weather_code,wind_speed_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&temperature_unit=fahrenheit&wind_speed_unit=mph"
        "&forecast_days=3&timezone=auto",
        lat, lon);

    Serial.printf("[C28P-WX] GET %s\n", url);

    http.begin(client, url);
    http.setTimeout(8000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[C28P-WX] HTTP %d\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    // Parse JSON — Open-Meteo response is ~1-2KB, well within ArduinoJson budget
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[C28P-WX] JSON parse failed: %s\n", err.c_str());
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

// ─── Drawing helpers ───
static void wx_draw_chrome() {
    // Skip exit bar (y < 14) — dpad layer owns that
    gfx->fillRect(0, 14, 240, 320 - 14, 0x0000);

    // Title
    gfx->fillRect(0, 14, 240, 22, 0x031F);   // deep blue header
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 18);
    gfx->print("WEATHER");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 22);
    gfx->print("< EXIT");
}

static void wx_draw_loading() {
    gfx->fillRect(0, 40, 240, 200, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(40, 130);
    gfx->print("Fetching...");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(40, 160);
    gfx->print("Open-Meteo API");
}

static void wx_draw_error(const char* msg) {
    gfx->fillRect(0, 40, 240, 200, 0x0000);
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

static void wx_draw_data() {
    gfx->fillRect(0, 40, 240, 320 - 40, 0x0000);

    // Location
    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 44);
    gfx->print(wx.location);

    // Current conditions — big temp
    gfx->setTextSize(4);
    gfx->setTextColor(0xFFE0);
    char tempbuf[8];
    snprintf(tempbuf, sizeof(tempbuf), "%d", (int)round(wx.current_temp_f));
    gfx->setCursor(20, 60);
    gfx->print(tempbuf);
    gfx->setTextSize(2);
    gfx->setCursor(20 + strlen(tempbuf) * 24 + 4, 64);
    gfx->print("F");

    // Conditions text
    gfx->setTextSize(1);
    gfx->setTextColor(w_code_color(wx.current_code));
    gfx->setCursor(20, 100);
    gfx->print(w_code_text(wx.current_code));

    // Wind
    gfx->setTextColor(0xC618);
    gfx->setCursor(20, 114);
    gfx->printf("WIND: %.0f MPH", wx.wind_mph);

    // 3-day forecast
    gfx->drawFastHLine(0, 138, 240, 0x4208);
    gfx->setTextColor(0x8410);
    gfx->setCursor(8, 142);
    gfx->print("3-DAY FORECAST");

    for (int i = 0; i < 3; i++) {
        int y = 156 + i * 36;
        gfx->fillRect(8, y, 224, 30, 0x18C3);
        gfx->drawRect(8, y, 224, 30, 0x4208);

        // Day name
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(16, y + 4);
        gfx->print(wx.day_names[i]);

        // High/low
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(16, y + 16);
        gfx->printf("%d / %d F",
                    (int)round(wx.day_highs[i]),
                    (int)round(wx.day_lows[i]));

        // Code text
        gfx->setTextColor(w_code_color(wx.day_codes[i]));
        gfx->setCursor(110, y + 10);
        gfx->print(w_code_text(wx.day_codes[i]));
    }

    // REFRESH button
    int btn_y = 270;
    gfx->fillRect(20, btn_y, 200, 36, 0x0260);
    gfx->drawRect(20, btn_y, 200, 36, 0x07E0);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(64, btn_y + 10);
    gfx->print("REFRESH");
}

// ─── Public entry ───
void c28p_run_weather() {
    // Initialize NoSQL category if not present
    nosql_init("settings");

    // Load saved location or use default
    snprintf(wx.location, sizeof(wx.location), "%s", DEFAULT_LOC_NAME);
    float lat = DEFAULT_LAT;
    float lon = DEFAULT_LON;
    {
        String title, content;
        int total = nosql_get_count("settings");
        for (int i = 0; i < total; i++) {
            if (!nosql_get_entry("settings", i, title, content)) continue;
            if (title == "weather_location") {
                // content format: "name|lat|lon"
                int p1 = content.indexOf('|');
                int p2 = content.indexOf('|', p1 + 1);
                if (p1 > 0 && p2 > p1) {
                    String name = content.substring(0, p1);
                    String slat = content.substring(p1 + 1, p2);
                    String slon = content.substring(p2 + 1);
                    snprintf(wx.location, sizeof(wx.location), "%s", name.c_str());
                    lat = slat.toFloat();
                    lon = slon.toFloat();
                }
                break;
            }
        }
    }

    wx_draw_chrome();
    wx_draw_loading();

    bool ok = fetch_weather(lat, lon);
    if (!ok) {
        wx_draw_error("Fetch failed");
    } else {
        wx_draw_data();
    }

    // Touch loop
    bool was_touched = false;
    int pressed = -1;   // -1=none, 0=refresh, 1=exit
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            if (ty < 14) {
                pressed = 1;
            } else if (ty >= 270 && ty < 306 && tx >= 20 && tx < 220) {
                pressed = 0;
            }
        } else if (!touched && was_touched) {
            if (pressed == 1) {
                return;
            } else if (pressed == 0) {
                wx_draw_loading();
                bool ok2 = fetch_weather(lat, lon);
                if (!ok2) wx_draw_error("Fetch failed");
                else wx_draw_data();
            }
            pressed = -1;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

#endif // DEVICE_C28P