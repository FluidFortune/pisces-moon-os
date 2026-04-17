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

#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "apps.h"
#include "time.h"

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  MONTH NAVIGATION
//  Trackball left/right or < > keys move between
//  months. Trackball click or header tap exits.
// ─────────────────────────────────────────────

static const char* MONTH_NAMES[] = {
    "JANUARY","FEBRUARY","MARCH","APRIL","MAY","JUNE",
    "JULY","AUGUST","SEPTEMBER","OCTOBER","NOVEMBER","DECEMBER"
};

// Days in each month — leap year handled in getDaysInMonth()
static int getDaysInMonth(int month, int year) {
    // month is 1-based
    int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)))
        return 29;
    return days[month - 1];
}

// Day of week for the 1st of a given month (0=Sun ... 6=Sat)
// Tomohiko Sakamoto's algorithm
static int firstDayOfMonth(int month, int year) {
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (month < 3) year--;
    return (year + year/4 - year/100 + year/400 + t[month-1] + 1) % 7;
}

static void drawCalendar(int month, int year, int todayDay) {
    gfx->fillScreen(0x0000);

    // Header
    gfx->fillRect(0, 0, 320, 24, 0x0821);
    gfx->drawFastHLine(0, 24, 320, 0x07E0);
    gfx->setCursor(10, 7);
    gfx->setTextColor(0x07E0);
    gfx->setTextSize(1);
    gfx->print("CALENDAR | < > MONTHS | HDR/CLK EXIT");

    // Month / Year title — centered
    char title[24];
    snprintf(title, sizeof(title), "%s %d", MONTH_NAMES[month - 1], year);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    int titleW = strlen(title) * 12;
    gfx->setCursor((320 - titleW) / 2, 33);
    gfx->print(title);
    gfx->setTextSize(1);

    // Day-of-week headers
    const char* days[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    const int colWidth = 320 / 7;
    gfx->setTextColor(0x7BEF);
    for (int i = 0; i < 7; i++) {
        gfx->setCursor(i * colWidth + (colWidth - 12) / 2, 63);
        gfx->print(days[i]);
    }
    gfx->drawFastHLine(0, 74, 320, 0x0821);

    // Grid lines
    const int startY  = 78;
    const int rowH    = 26;
    for (int i = 0; i <= 6; i++)
        gfx->drawFastHLine(0, startY + i * rowH, 320, 0x0821);
    for (int i = 1; i < 7; i++)
        gfx->drawFastVLine(i * colWidth, startY, 6 * rowH, 0x0821);

    // Dates
    int firstDow  = firstDayOfMonth(month, year);
    int daysTotal = getDaysInMonth(month, year);
    int day = 1;

    for (int row = 0; row < 6 && day <= daysTotal; row++) {
        for (int col = 0; col < 7 && day <= daysTotal; col++) {
            if (row == 0 && col < firstDow) continue;

            int cellX = col * colWidth;
            int cellY = startY + row * rowH;

            if (day == todayDay) {
                // Today — filled highlight
                gfx->fillRect(cellX + 1, cellY + 1, colWidth - 2, rowH - 2, 0x07E0);
                gfx->setTextColor(0x0000);
            } else if (col == 0 || col == 6) {
                // Weekend — dim
                gfx->setTextColor(0x4208);
            } else {
                gfx->setTextColor(0xFFFF);
            }

            // Right-align day number within cell
            int numW = (day >= 10) ? 12 : 6;
            gfx->setCursor(cellX + colWidth - numW - 3, cellY + (rowH - 8) / 2 + 1);
            gfx->print(day);
            day++;
        }
    }
}

void run_calendar() {
    // ── Get current date from NTP-synced RTC ──────────────────
    struct tm timeinfo;
    int dispMonth, dispYear, todayDay;

    if (getLocalTime(&timeinfo)) {
        dispMonth = timeinfo.tm_mon + 1;   // tm_mon is 0-based
        dispYear  = timeinfo.tm_year + 1900;
        todayDay  = timeinfo.tm_mday;
    } else {
        // No time sync — use a visible fallback so the bug is obvious
        dispMonth = 1;
        dispYear  = 2000;
        todayDay  = 0;   // 0 = no highlight
    }

    drawCalendar(dispMonth, dispYear, todayDay);

    while (true) {
        // Header tap = exit
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            if (ty < 24) return;
        }

        // Trackball: left/right = prev/next month, click = exit
        TrackballState tb = update_trackball();
        if (tb.clicked) return;
        if (tb.x == -1) {
            dispMonth--;
            if (dispMonth < 1) { dispMonth = 12; dispYear--; }
            todayDay = 0;   // Clear today highlight when navigating
            drawCalendar(dispMonth, dispYear, todayDay);
        } else if (tb.x == 1) {
            dispMonth++;
            if (dispMonth > 12) { dispMonth = 1; dispYear++; }
            todayDay = 0;
            drawCalendar(dispMonth, dispYear, todayDay);
        }

        // Keyboard: < or , = prev month, > or . = next month, Q = exit
        char c = get_keypress();
        if (c == 'q' || c == 'Q') return;
        if (c == '<' || c == ',') {
            dispMonth--;
            if (dispMonth < 1) { dispMonth = 12; dispYear--; }
            todayDay = 0;
            drawCalendar(dispMonth, dispYear, todayDay);
        } else if (c == '>' || c == '.') {
            dispMonth++;
            if (dispMonth > 12) { dispMonth = 1; dispYear++; }
            todayDay = 0;
            drawCalendar(dispMonth, dispYear, todayDay);
        }

        delay(30);
        yield();
    }
}
