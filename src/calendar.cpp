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

#include <Arduino.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "theme.h"
#include "keyboard.h"
#include "trackball.h"
#include "time.h"

extern Arduino_GFX *gfx;

static bool calSyncNTP() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (time(nullptr) > 100000UL) return true;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    unsigned long t0 = millis();
    while (time(nullptr) < 100000UL && millis() - t0 < 5000) {
        delay(200); yield();
    }
    return time(nullptr) > 100000UL;
}

static int daysInMonth(int month, int year) {
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 1 && ((year%4==0 && year%100!=0) || year%400==0)) return 29;
    return days[month];
}

// Day of week for 1st of month (0=Sun) via Zeller
static int firstDayOfMonth(int month, int year) {
    int m = month + 1;
    int y = year;
    if (m < 3) { m += 12; y--; }
    int k = y % 100, j = y / 100;
    int h = (1 + (13*(m+1)/5) + k + k/4 + j/4 + 5*j) % 7;
    return (h + 6) % 7;
}

static const char* MONTH_NAMES[] = {
    "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
    "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
};

static void calDraw(int month, int year, int td, int tm_, int ty_) {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    gfx->setTextSize(1); gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
    gfx->print("CALENDAR | < > MONTH | Q/HDR=EXIT");

    // Month + year centred
    char title[32];
    snprintf(title, sizeof(title), "%s %d", MONTH_NAMES[month], year);
    gfx->setTextSize(1); gfx->setTextColor(C_WHITE);
    gfx->setCursor((320 - (int)strlen(title)*6) / 2, 30);
    gfx->print(title);

    // Day headers
    const char* days[] = {"SU","MO","TU","WE","TH","FR","SA"};
    int colW = 45;
    gfx->setTextColor(C_GREY);
    for (int i = 0; i < 7; i++) {
        gfx->setCursor(i * colW + 16, 44);
        gfx->print(days[i]);
    }
    gfx->drawFastHLine(0, 54, 320, C_DARK);

    int startFd = firstDayOfMonth(month, year);
    int numDays = daysInMonth(month, year);
    int rowH = 27, startY = 58;
    int col = startFd, row = 0;

    for (int d = 1; d <= numDays; d++) {
        int x = col * colW + 14;
        int y = startY + row * rowH;
        bool isToday = (d == td && month == tm_ && year == ty_);
        if (isToday) {
            gfx->fillRoundRect(col*colW+2, y-1, colW-4, rowH-2, 4, C_GREEN);
            gfx->setTextColor(C_BLACK);
        } else {
            gfx->setTextColor(col == 0 ? 0xF800 : C_WHITE);
        }
        gfx->setTextSize(1);
        gfx->setCursor(x, y + 7);
        if (d < 10) { gfx->print(" "); gfx->print(d); }
        else gfx->print(d);
        col++;
        if (col == 7) { col = 0; row++; }
    }

    gfx->setTextColor(C_GREY); gfx->setTextSize(1);
    gfx->setCursor(10, 226);
    gfx->print("< or , = prev    T = today    > or . = next");
}

void run_calendar() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->setCursor(10, 7); gfx->print("CALENDAR");
    gfx->setTextColor(C_GREY); gfx->setCursor(10, 40);
    gfx->print("Syncing time...");

    calSyncNTP();

    int todayDay = 1, todayMonth = 0, todayYear = 2026;
    if (time(nullptr) > 100000UL) {
        struct tm t; getLocalTime(&t);
        todayDay   = t.tm_mday;
        todayMonth = t.tm_mon;
        todayYear  = t.tm_year + 1900;
    }

    int viewMonth = todayMonth, viewYear = todayYear;
    calDraw(viewMonth, viewYear, todayDay, todayMonth, todayYear);

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            break;
        }
        char k = get_keypress();
        TrackballState tb = update_trackball();
        if (k == 'q' || k == 'Q') break;
        if (k == 't' || k == 'T') {
            viewMonth = todayMonth; viewYear = todayYear;
            calDraw(viewMonth, viewYear, todayDay, todayMonth, todayYear);
            continue;
        }
        bool prev = (k == ',' || k == '<' || tb.x == -1);
        bool next = (k == '.' || k == '>' || tb.x ==  1);
        if (prev) {
            if (--viewMonth < 0) { viewMonth = 11; viewYear--; }
            calDraw(viewMonth, viewYear, todayDay, todayMonth, todayYear);
        }
        if (next) {
            if (++viewMonth > 11) { viewMonth = 0; viewYear++; }
            calDraw(viewMonth, viewYear, todayDay, todayMonth, todayYear);
        }
        delay(50); yield();
    }
}
