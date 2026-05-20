// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)

#include <Arduino.h>
#include <WiFi.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "touch.h"
#include "theme.h"
#include "keyboard.h"
#include "pm_input.h"
#include "trackball.h"
#include "time.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 135;
#else
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
#endif

static bool calSyncNTP() {
    if (WiFi.status() != WL_CONNECTED) return false;
    if (time(nullptr) > 100000UL) return true;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    unsigned long t0 = millis();
    while (time(nullptr) < 100000UL && millis() - t0 < 5000) { delay(200); yield(); }
    return time(nullptr) > 100000UL;
}

static int daysInMonth(int month, int year) {
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 1 && ((year%4==0 && year%100!=0) || year%400==0)) return 29;
    return days[month];
}

static int firstDayOfMonth(int month, int year) {
    int m = month + 1, y = year;
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
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->drawFastHLine(0, 24, DISP_W, C_GREEN);
    gfx->setTextSize(1); gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
#ifdef DEVICE_TLORAPAGER
    gfx->print("CALENDAR | < > MONTH | Q EXIT");
#else
    gfx->print("CALENDAR | < > MONTH | Q/HDR=EXIT");
#endif

    char title[32];
    snprintf(title, sizeof(title), "%s %d", MONTH_NAMES[month], year);
    gfx->setTextSize(1); gfx->setTextColor(C_WHITE);
    gfx->setCursor((DISP_W - (int)strlen(title) * 6) / 2, 30);
    gfx->print(title);

    const char* days[] = {"SU","MO","TU","WE","TH","FR","SA"};
    // Grid uses full available width
    const int gridLeft  = 4;
    const int gridRight = DISP_W - 4;
    const int colW = (gridRight - gridLeft) / 7;

    gfx->setTextColor(C_GREY);
    for (int i = 0; i < 7; i++) {
        // Center "SU"/"MO" (2 chars * 6px = 12px) in colW
        gfx->setCursor(gridLeft + i * colW + (colW - 12) / 2, 44);
        gfx->print(days[i]);
    }
    gfx->drawFastHLine(0, 54, DISP_W, C_DARK);

    int startFd = firstDayOfMonth(month, year);
    int numDays = daysInMonth(month, year);
    int rowH = 27, startY = 58;
    int col = startFd, row = 0;

    for (int d = 1; d <= numDays; d++) {
        int cellX = gridLeft + col * colW;
        int cellY = startY + row * rowH;
        bool isToday = (d == td && month == tm_ && year == ty_);
        if (isToday) {
            gfx->fillRoundRect(cellX + 2, cellY - 1, colW - 4, rowH - 2, 4, C_GREEN);
            gfx->setTextColor(C_BLACK);
        } else {
            gfx->setTextColor(col == 0 ? 0xF800 : C_WHITE);
        }
        gfx->setTextSize(1);
        // Center 2-digit day number in cell
        gfx->setCursor(cellX + (colW - 12) / 2, cellY + 7);
        if (d < 10) { gfx->print(" "); gfx->print(d); } else gfx->print(d);
        col++;
        if (col == 7) { col = 0; row++; }
    }

    gfx->setTextColor(C_GREY); gfx->setTextSize(1);
    gfx->setCursor(10, DISP_H - 14);
    gfx->print("< or , = prev    T = today    > or . = next");
}

void run_calendar() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
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
        if (get_touch(&tx, &ty) && ty < 40) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            break;
        }
        char k = get_keypress();
        TrackballState tb = update_trackball();
        if (pm_is_exit_key(k)) break;
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
