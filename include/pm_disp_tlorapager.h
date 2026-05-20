// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef PM_DISP_TLORAPAGER_H
#define PM_DISP_TLORAPAGER_H

#ifdef DEVICE_TLORAPAGER

// ============================================================
//  pm_disp_tlorapager.h
//  Pisces Moon OS — T-LoraPager display driver (standalone)
//
//  Zero dependencies on Arduino_GFX, Arduino_DataBus,
//  Adafruit_GFX, or any third-party display library.
//
//  Owns SPI directly. Implements its own ST7796 init sequence,
//  drawing primitives, line/circle/triangle algorithms, and
//  text rendering with embedded 5x7 font.
//
//  Address space: native panel coordinates only. (0,0) =
//  top-left as user holds device in landscape. width()=480,
//  height()=222.
//
//  LilyGoLib's T-LoRa-Pager rotation table uses controller-native
//  width/height names. For Pisces Moon's user-facing 480x222
//  landscape, the tuple that is 90 degrees counter-clockwise from
//  the previously flashed 0x48 state is:
//    MADCTL=0xE8, width=480, height=222, x=0, y=49
//  The 0x48/0x88 pair are the two 180-degree-opposed mappings that
//  made the UI read as portrait relative to the keyboard.
//
//  v1.2.1 — SPI Bus Treaty support: setSharedMutex() allows the
//  driver to share a global SemaphoreHandle_t with wardrive/SD/
//  LoRa/NFC, so all four SPI peripherals coordinate through one
//  lock. Pass nullptr or never call to keep using internal mutex.
// ============================================================

#include <Arduino.h>
#include <Print.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ── Embedded 5x7 ASCII font (ASCII 0x20–0x7E) ──────────────
static const uint8_t PM_FONT_5X7[][5] PROGMEM = {
    {0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x00,0x00,0x5F,0x00,0x00}, // !
    {0x00,0x07,0x00,0x07,0x00}, // "
    {0x14,0x7F,0x14,0x7F,0x14}, // #
    {0x24,0x2A,0x7F,0x2A,0x12}, // $
    {0x23,0x13,0x08,0x64,0x62}, // %
    {0x36,0x49,0x55,0x22,0x50}, // &
    {0x00,0x05,0x03,0x00,0x00}, // '
    {0x00,0x1C,0x22,0x41,0x00}, // (
    {0x00,0x41,0x22,0x1C,0x00}, // )
    {0x14,0x08,0x3E,0x08,0x14}, // *
    {0x08,0x08,0x3E,0x08,0x08}, // +
    {0x00,0x50,0x30,0x00,0x00}, // ,
    {0x08,0x08,0x08,0x08,0x08}, // -
    {0x00,0x60,0x60,0x00,0x00}, // .
    {0x20,0x10,0x08,0x04,0x02}, // /
    {0x3E,0x51,0x49,0x45,0x3E}, // 0
    {0x00,0x42,0x7F,0x40,0x00}, // 1
    {0x42,0x61,0x51,0x49,0x46}, // 2
    {0x21,0x41,0x45,0x4B,0x31}, // 3
    {0x18,0x14,0x12,0x7F,0x10}, // 4
    {0x27,0x45,0x45,0x45,0x39}, // 5
    {0x3C,0x4A,0x49,0x49,0x30}, // 6
    {0x01,0x71,0x09,0x05,0x03}, // 7
    {0x36,0x49,0x49,0x49,0x36}, // 8
    {0x06,0x49,0x49,0x29,0x1E}, // 9
    {0x00,0x36,0x36,0x00,0x00}, // :
    {0x00,0x56,0x36,0x00,0x00}, // ;
    {0x08,0x14,0x22,0x41,0x00}, // <
    {0x14,0x14,0x14,0x14,0x14}, // =
    {0x00,0x41,0x22,0x14,0x08}, // >
    {0x02,0x01,0x51,0x09,0x06}, // ?
    {0x32,0x49,0x79,0x41,0x3E}, // @
    {0x7E,0x11,0x11,0x11,0x7E}, // A
    {0x7F,0x49,0x49,0x49,0x36}, // B
    {0x3E,0x41,0x41,0x41,0x22}, // C
    {0x7F,0x41,0x41,0x22,0x1C}, // D
    {0x7F,0x49,0x49,0x49,0x41}, // E
    {0x7F,0x09,0x09,0x09,0x01}, // F
    {0x3E,0x41,0x49,0x49,0x7A}, // G
    {0x7F,0x08,0x08,0x08,0x7F}, // H
    {0x00,0x41,0x7F,0x41,0x00}, // I
    {0x20,0x40,0x41,0x3F,0x01}, // J
    {0x7F,0x08,0x14,0x22,0x41}, // K
    {0x7F,0x40,0x40,0x40,0x40}, // L
    {0x7F,0x02,0x0C,0x02,0x7F}, // M
    {0x7F,0x04,0x08,0x10,0x7F}, // N
    {0x3E,0x41,0x41,0x41,0x3E}, // O
    {0x7F,0x09,0x09,0x09,0x06}, // P
    {0x3E,0x41,0x51,0x21,0x5E}, // Q
    {0x7F,0x09,0x19,0x29,0x46}, // R
    {0x46,0x49,0x49,0x49,0x31}, // S
    {0x01,0x01,0x7F,0x01,0x01}, // T
    {0x3F,0x40,0x40,0x40,0x3F}, // U
    {0x1F,0x20,0x40,0x20,0x1F}, // V
    {0x3F,0x40,0x38,0x40,0x3F}, // W
    {0x63,0x14,0x08,0x14,0x63}, // X
    {0x07,0x08,0x70,0x08,0x07}, // Y
    {0x61,0x51,0x49,0x45,0x43}, // Z
    {0x00,0x7F,0x41,0x41,0x00}, // [
    {0x02,0x04,0x08,0x10,0x20}, // backslash
    {0x00,0x41,0x41,0x7F,0x00}, // ]
    {0x04,0x02,0x01,0x02,0x04}, // ^
    {0x40,0x40,0x40,0x40,0x40}, // _
    {0x00,0x01,0x02,0x04,0x00}, // `
    {0x20,0x54,0x54,0x54,0x78}, // a
    {0x7F,0x48,0x44,0x44,0x38}, // b
    {0x38,0x44,0x44,0x44,0x20}, // c
    {0x38,0x44,0x44,0x48,0x7F}, // d
    {0x38,0x54,0x54,0x54,0x18}, // e
    {0x08,0x7E,0x09,0x01,0x02}, // f
    {0x0C,0x52,0x52,0x52,0x3E}, // g
    {0x7F,0x08,0x04,0x04,0x78}, // h
    {0x00,0x44,0x7D,0x40,0x00}, // i
    {0x20,0x40,0x44,0x3D,0x00}, // j
    {0x7F,0x10,0x28,0x44,0x00}, // k
    {0x00,0x41,0x7F,0x40,0x00}, // l
    {0x7C,0x04,0x18,0x04,0x78}, // m
    {0x7C,0x08,0x04,0x04,0x78}, // n
    {0x38,0x44,0x44,0x44,0x38}, // o
    {0x7C,0x14,0x14,0x14,0x08}, // p
    {0x08,0x14,0x14,0x18,0x7C}, // q
    {0x7C,0x08,0x04,0x04,0x08}, // r
    {0x48,0x54,0x54,0x54,0x20}, // s
    {0x04,0x3F,0x44,0x40,0x20}, // t
    {0x3C,0x40,0x40,0x20,0x7C}, // u
    {0x1C,0x20,0x40,0x20,0x1C}, // v
    {0x3C,0x40,0x30,0x40,0x3C}, // w
    {0x44,0x28,0x10,0x28,0x44}, // x
    {0x0C,0x50,0x50,0x50,0x3C}, // y
    {0x44,0x64,0x54,0x4C,0x44}, // z
    {0x00,0x08,0x36,0x41,0x00}, // {
    {0x00,0x00,0x7F,0x00,0x00}, // |
    {0x00,0x41,0x36,0x08,0x00}, // }
    {0x08,0x08,0x2A,0x1C,0x08}, // ~
};

static constexpr uint8_t  PM_TLORA_MADCTL_LANDSCAPE = 0xE8;
static constexpr uint16_t PM_TLORA_X_OFFSET          = 0;
static constexpr uint16_t PM_TLORA_Y_OFFSET          = 49;

class PMDispTLoRaPager : public Print {
public:
    PMDispTLoRaPager(int8_t sck, int8_t miso, int8_t mosi,
                     int8_t cs, int8_t dc, int8_t bl,
                     uint32_t freq_hz = 40000000UL,
                     SPIClass &spi = SPI)
        : _spi(&spi), _sck(sck), _miso(miso), _mosi(mosi),
          _cs(cs), _dc(dc), _bl(bl), _freq(freq_hz),
          _lock(nullptr), _owns_lock(true),
          _width(480), _height(222),
          _x_offset(PM_TLORA_X_OFFSET), _y_offset(PM_TLORA_Y_OFFSET),
          _cursor_x(0), _cursor_y(0),
          _text_color(0xFFFF), _text_bg(0x0000),
          _text_bg_set(false), _text_size(1)
    {}

    bool begin() {
        // Only create internal mutex if we haven't been given a shared one.
        if (!_lock) {
            _lock = xSemaphoreCreateMutex();
            _owns_lock = true;
        }
        if (!_lock) return false;

        pinMode(_cs, OUTPUT); digitalWrite(_cs, HIGH);
        pinMode(_dc, OUTPUT); digitalWrite(_dc, HIGH);
        if (_bl >= 0) { pinMode(_bl, OUTPUT); digitalWrite(_bl, LOW); }

        _spi->begin(_sck, _miso, _mosi);

        // LilyGo's 19-command ST7796 init sequence (verbatim).
        struct InitCmd { uint8_t cmd; uint8_t data[15]; uint8_t len; };
        static const InitCmd init_list[19] = {
            {0x01, {0x00},                                             0x80},
            {0x11, {0x00},                                             0x80},
            {0xF0, {0xC3},                                             0x01},
            {0xF0, {0xC3},                                             0x01},
            {0xF0, {0x96},                                             0x01},
            {0x36, {PM_TLORA_MADCTL_LANDSCAPE},                        0x01},
            {0x3A, {0x55},                                             0x01},
            {0xB4, {0x01},                                             0x01},
            {0xB6, {0x80, 0x02, 0x3B},                                 0x03},
            {0xE8, {0x40,0x8A,0x00,0x00,0x29,0x19,0xA5,0x33},          0x08},
            {0xC1, {0x06},                                             0x01},
            {0xC2, {0xA7},                                             0x01},
            {0xC5, {0x18},                                             0x81},
            {0xE0, {0xF0,0x09,0x0b,0x06,0x04,0x15,0x2F,0x54,0x42,0x3C,0x17,0x14,0x18,0x1B}, 0x0F},
            {0xE1, {0xE0,0x09,0x0b,0x06,0x04,0x03,0x2B,0x43,0x42,0x3B,0x16,0x14,0x17,0x1B}, 0x8F},
            {0xF0, {0x3c},                                             0x01},
            {0xF0, {0x69},                                             0x81},
            {0x21, {0x00},                                             0x01},
            {0x29, {0x00},                                             0x01},
        };
        for (uint32_t i = 0; i < 19; i++) {
            const InitCmd &c = init_list[i];
            writeParams(c.cmd, c.data, c.len & 0x1F);
            if (c.len & 0x80) delay(120);
        }
        Serial.printf("[PMDisp] standalone driver init complete: MADCTL=0x%02X offset=(%u,%u)\n",
                      PM_TLORA_MADCTL_LANDSCAPE,
                      PM_TLORA_X_OFFSET,
                      PM_TLORA_Y_OFFSET);
        return true;
    }

    // ── SPI Bus Treaty ───────────────────────────────────
    // Inject a shared mutex AFTER begin() but before any drawing.
    // The driver will use the shared mutex instead of its private
    // one for all SPI transactions, so it coordinates with the SD,
    // LoRa, NFC, and wardrive code paths that also take this mutex.
    //
    // Pass nullptr to revert to private locking (no-op if already
    // using shared). Safe to call once at boot.
    void setSharedMutex(SemaphoreHandle_t shared) {
        if (!shared) return;
        // If we created our own internal mutex, free it before adopting
        // the shared one — otherwise we leak a SemaphoreHandle.
        if (_owns_lock && _lock) {
            vSemaphoreDelete(_lock);
        }
        _lock = shared;
        _owns_lock = false;
        Serial.println("[PMDisp] joined SPI Bus Treaty (shared mutex adopted)");
    }

    int16_t width()  const { return _width;  }
    int16_t height() const { return _height; }

    void setBacklight(bool on) {
        if (_bl >= 0) digitalWrite(_bl, on ? HIGH : LOW);
    }

    bool lockSPI(TickType_t ticks = portMAX_DELAY) {
        // The mutex variant depends on whether we own _lock (private,
        // non-recursive) or adopted it as the shared SPI Bus Treaty
        // mutex (which is recursive — see main.cpp where spi_mutex is
        // created with xSemaphoreCreateRecursiveMutex()). Calling
        // xSemaphoreTake() on a recursive mutex is undefined behavior
        // in FreeRTOS and was the root cause of the v1.2.0 Pager SD
        // mount failure: the display driver's beginTxn/endTxn calls
        // silently failed to acquire the mutex, leaving the bus in an
        // inconsistent state when the late-SD-mount task then tried
        // to claim it.
        if (!_lock) return false;
        if (_owns_lock) {
            return xSemaphoreTake(_lock, ticks) == pdTRUE;
        } else {
            return xSemaphoreTakeRecursive(_lock, ticks) == pdTRUE;
        }
    }
    void unlockSPI() {
        if (!_lock) return;
        if (_owns_lock) {
            xSemaphoreGive(_lock);
        } else {
            xSemaphoreGiveRecursive(_lock);
        }
    }

    // ── Drawing primitives ────────────────────────────────

    void fillScreen(uint16_t color) {
        fillRect(0, 0, _width, _height, color);
    }

    void drawPixel(int16_t x, int16_t y, uint16_t color) {
        if (x < 0 || y < 0 || x >= _width || y >= _height) return;
        beginTxn();
        setAddrWindowInTxn(x, y, x, y);
        digitalWrite(_dc, HIGH);
        uint8_t buf[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
        _spi->writeBytes(buf, 2);
        endTxn();
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        if (w <= 0 || h <= 0) return;
        if (x >= _width || y >= _height) return;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > _width)  w = _width  - x;
        if (y + h > _height) h = _height - y;
        if (w <= 0 || h <= 0) return;

        beginTxn();
        setAddrWindowInTxn(x, y, x + w - 1, y + h - 1);
        pushColorRepeatInTxn(color, (uint32_t)w * h);
        endTxn();
    }

    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
        if (w <= 0 || y < 0 || y >= _height) return;
        if (x < 0) { w += x; x = 0; }
        if (x + w > _width) w = _width - x;
        if (w <= 0) return;
        beginTxn();
        setAddrWindowInTxn(x, y, x + w - 1, y);
        pushColorRepeatInTxn(color, w);
        endTxn();
    }

    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
        if (h <= 0 || x < 0 || x >= _width) return;
        if (y < 0) { h += y; y = 0; }
        if (y + h > _height) h = _height - y;
        if (h <= 0) return;
        beginTxn();
        setAddrWindowInTxn(x, y, x, y + h - 1);
        pushColorRepeatInTxn(color, h);
        endTxn();
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        drawFastHLine(x,         y,         w, color);
        drawFastHLine(x,         y + h - 1, w, color);
        drawFastVLine(x,         y,         h, color);
        drawFastVLine(x + w - 1, y,         h, color);
    }

    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
        bool steep = abs(y1 - y0) > abs(x1 - x0);
        if (steep) { int16_t t; t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
        if (x0 > x1) { int16_t t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        int16_t dx = x1 - x0;
        int16_t dy = abs(y1 - y0);
        int16_t err = dx / 2;
        int16_t ystep = (y0 < y1) ? 1 : -1;
        for (; x0 <= x1; x0++) {
            if (steep) drawPixel(y0, x0, color);
            else       drawPixel(x0, y0, color);
            err -= dy;
            if (err < 0) { y0 += ystep; err += dx; }
        }
    }

    void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
        int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
        drawPixel(x0, y0 + r, color); drawPixel(x0, y0 - r, color);
        drawPixel(x0 + r, y0, color); drawPixel(x0 - r, y0, color);
        while (x < y) {
            if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
            x++; ddF_x += 2; f += ddF_x;
            drawPixel(x0 + x, y0 + y, color); drawPixel(x0 - x, y0 + y, color);
            drawPixel(x0 + x, y0 - y, color); drawPixel(x0 - x, y0 - y, color);
            drawPixel(x0 + y, y0 + x, color); drawPixel(x0 - y, y0 + x, color);
            drawPixel(x0 + y, y0 - x, color); drawPixel(x0 - y, y0 - x, color);
        }
    }

    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
        drawFastVLine(x0, y0 - r, 2 * r + 1, color);
        fillCircleHelper(x0, y0, r, 3, 0, color);
    }

    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        drawFastHLine(x + r,       y,           w - 2 * r, color);
        drawFastHLine(x + r,       y + h - 1,   w - 2 * r, color);
        drawFastVLine(x,           y + r,       h - 2 * r, color);
        drawFastVLine(x + w - 1,   y + r,       h - 2 * r, color);
        drawCircleHelper(x + r,         y + r,         r, 1, color);
        drawCircleHelper(x + w - r - 1, y + r,         r, 2, color);
        drawCircleHelper(x + w - r - 1, y + h - r - 1, r, 4, color);
        drawCircleHelper(x + r,         y + h - r - 1, r, 8, color);
    }

    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        fillRect(x + r, y, w - 2 * r, h, color);
        fillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
        fillCircleHelper(x + r,         y + r, r, 2, h - 2 * r - 1, color);
    }

    void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color) {
        drawLine(x0, y0, x1, y1, color);
        drawLine(x1, y1, x2, y2, color);
        drawLine(x2, y2, x0, y0, color);
    }

    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2, uint16_t color) {
        int16_t a, b, y, last;
        if (y0 > y1) { int16_t t; t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
        if (y1 > y2) { int16_t t; t = y2; y2 = y1; y1 = t; t = x2; x2 = x1; x1 = t; }
        if (y0 > y1) { int16_t t; t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
        if (y0 == y2) {
            a = b = x0;
            if (x1 < a) a = x1; else if (x1 > b) b = x1;
            if (x2 < a) a = x2; else if (x2 > b) b = x2;
            drawFastHLine(a, y0, b - a + 1, color);
            return;
        }
        int16_t dx01 = x1 - x0, dy01 = y1 - y0;
        int16_t dx02 = x2 - x0, dy02 = y2 - y0;
        int16_t dx12 = x2 - x1, dy12 = y2 - y1;
        int32_t sa = 0, sb = 0;
        last = (y1 == y2) ? y1 : y1 - 1;
        for (y = y0; y <= last; y++) {
            a = x0 + sa / dy01;
            b = x0 + sb / dy02;
            sa += dx01; sb += dx02;
            if (a > b) { int16_t t = a; a = b; b = t; }
            drawFastHLine(a, y, b - a + 1, color);
        }
        sa = (int32_t)dx12 * (y - y1);
        sb = (int32_t)dx02 * (y - y0);
        for (; y <= y2; y++) {
            a = x1 + sa / dy12;
            b = x0 + sb / dy02;
            sa += dx12; sb += dx02;
            if (a > b) { int16_t t = a; a = b; b = t; }
            drawFastHLine(a, y, b - a + 1, color);
        }
    }

    // ── Text ──────────────────────────────────────────────

    void setCursor(int16_t x, int16_t y) { _cursor_x = x; _cursor_y = y; }
    void setTextColor(uint16_t c)        { _text_color = c; _text_bg_set = false; }
    void setTextColor(uint16_t c, uint16_t bg) { _text_color = c; _text_bg = bg; _text_bg_set = true; }
    void setTextSize(uint8_t s)          { _text_size = (s == 0) ? 1 : s; }
    int16_t getCursorX() const { return _cursor_x; }
    int16_t getCursorY() const { return _cursor_y; }

    virtual size_t write(uint8_t c) override {
        if (c == '\n') {
            _cursor_x = 0;
            _cursor_y += 8 * _text_size;
        } else if (c == '\r') {
            _cursor_x = 0;
        } else {
            drawChar(_cursor_x, _cursor_y, c, _text_color,
                     _text_bg_set ? _text_bg : _text_color,
                     _text_size, !_text_bg_set);
            _cursor_x += 6 * _text_size;
            if (_cursor_x + 6 * _text_size > _width) {
                _cursor_x = 0;
                _cursor_y += 8 * _text_size;
            }
        }
        return 1;
    }

    // ── Bitmap blitting ─────────────────────────────────
    //
    // draw16bitRGBBitmap: pixels are RGB565 in NATIVE host byte
    // order (little-endian on ESP32). We byte-swap as we push
    // because the ST7796 wants high byte first.
    //
    // draw16bitBeRGBBitmap: pixels are already big-endian
    // (high byte first) — push straight through.
    //
    void draw16bitRGBBitmap(int16_t x, int16_t y,
                            uint16_t *bitmap,
                            int16_t w, int16_t h)
    {
        // Clip
        if (w <= 0 || h <= 0) return;
        if (x >= _width || y >= _height) return;
        int16_t srcx = 0, srcy = 0;
        int16_t draw_w = w, draw_h = h;
        if (x < 0) { srcx = -x; draw_w += x; x = 0; }
        if (y < 0) { srcy = -y; draw_h += y; y = 0; }
        if (x + draw_w > _width)  draw_w = _width  - x;
        if (y + draw_h > _height) draw_h = _height - y;
        if (draw_w <= 0 || draw_h <= 0) return;

        beginTxn();
        setAddrWindowInTxn(x, y, x + draw_w - 1, y + draw_h - 1);

        // For each row in the clipped region, byte-swap into
        // a line buffer and push.
        uint16_t line[draw_w];
        for (int16_t row = 0; row < draw_h; row++) {
            uint16_t *src = bitmap + (srcy + row) * w + srcx;
            for (int16_t col = 0; col < draw_w; col++) {
                uint16_t p = src[col];
                line[col] = (p >> 8) | (p << 8);
            }
            _spi->writeBytes((const uint8_t *)line, (size_t)draw_w * 2);
        }
        endTxn();
    }

    void draw16bitBeRGBBitmap(int16_t x, int16_t y,
                              uint16_t *bitmap,
                              int16_t w, int16_t h)
    {
        // Same clipping as native-order version.
        if (w <= 0 || h <= 0) return;
        if (x >= _width || y >= _height) return;
        int16_t srcx = 0, srcy = 0;
        int16_t draw_w = w, draw_h = h;
        if (x < 0) { srcx = -x; draw_w += x; x = 0; }
        if (y < 0) { srcy = -y; draw_h += y; y = 0; }
        if (x + draw_w > _width)  draw_w = _width  - x;
        if (y + draw_h > _height) draw_h = _height - y;
        if (draw_w <= 0 || draw_h <= 0) return;

        beginTxn();
        setAddrWindowInTxn(x, y, x + draw_w - 1, y + draw_h - 1);

        // Pixels already big-endian — push straight from source.
        // If no clipping happened (full-width region), push the
        // whole bitmap at once.
        if (draw_w == w && srcx == 0) {
            _spi->writeBytes((const uint8_t *)(bitmap + srcy * w),
                             (size_t)w * draw_h * 2);
        } else {
            // Row-by-row for the clipped case.
            for (int16_t row = 0; row < draw_h; row++) {
                uint16_t *src = bitmap + (srcy + row) * w + srcx;
                _spi->writeBytes((const uint8_t *)src, (size_t)draw_w * 2);
            }
        }
        endTxn();
    }

    void drawChar(int16_t x, int16_t y, uint8_t c,
                  uint16_t color, uint16_t bg,
                  uint8_t size, bool transparent_bg)
    {
        if (c < 0x20 || c > 0x7E) return;
        const uint8_t *glyph = PM_FONT_5X7[c - 0x20];
        for (int8_t col = 0; col < 6; col++) {
            uint8_t line = (col < 5) ? pgm_read_byte(&glyph[col]) : 0;
            for (int8_t row = 0; row < 8; row++) {
                bool pixel_on = (row < 7) && (line & (1 << row));
                if (pixel_on) {
                    if (size == 1) drawPixel(x + col, y + row, color);
                    else fillRect(x + col * size, y + row * size, size, size, color);
                } else if (!transparent_bg) {
                    if (size == 1) drawPixel(x + col, y + row, bg);
                    else fillRect(x + col * size, y + row * size, size, size, bg);
                }
            }
        }
    }

private:
    inline void beginTxn() {
        // Use recursive variant when _lock is the shared Treaty mutex.
        // See lockSPI() comment for the bug this prevents.
        if (_owns_lock) {
            xSemaphoreTake(_lock, portMAX_DELAY);
        } else {
            xSemaphoreTakeRecursive(_lock, portMAX_DELAY);
        }
        digitalWrite(_cs, LOW);
        _spi->beginTransaction(SPISettings(_freq, MSBFIRST, SPI_MODE0));
    }
    inline void endTxn() {
        _spi->endTransaction();
        digitalWrite(_cs, HIGH);
        if (_owns_lock) {
            xSemaphoreGive(_lock);
        } else {
            xSemaphoreGiveRecursive(_lock);
        }
    }

    void writeParams(uint8_t cmd, const uint8_t *data, size_t length) {
        beginTxn();
        digitalWrite(_dc, LOW);
        _spi->write(cmd);
        digitalWrite(_dc, HIGH);
        for (size_t i = 0; i < length; i++) _spi->write(data[i]);
        endTxn();
    }

    void setAddrWindowInTxn(int16_t x, int16_t y, int16_t xe, int16_t ye) {
        uint16_t xs  = x  + _x_offset;
        uint16_t xe_ = xe + _x_offset;
        uint16_t ys  = y  + _y_offset;
        uint16_t ye_ = ye + _y_offset;

        // CASET
        digitalWrite(_dc, LOW);  _spi->write(0x2A);  digitalWrite(_dc, HIGH);
        uint8_t ca[4] = { (uint8_t)(xs >> 8), (uint8_t)xs,
                          (uint8_t)(xe_ >> 8), (uint8_t)xe_ };
        _spi->writeBytes(ca, 4);

        // RASET
        digitalWrite(_dc, LOW);  _spi->write(0x2B);  digitalWrite(_dc, HIGH);
        uint8_t ra[4] = { (uint8_t)(ys >> 8), (uint8_t)ys,
                          (uint8_t)(ye_ >> 8), (uint8_t)ye_ };
        _spi->writeBytes(ra, 4);

        // RAMWR
        digitalWrite(_dc, LOW);  _spi->write(0x2C);  digitalWrite(_dc, HIGH);
    }

    void pushColorRepeatInTxn(uint16_t color, uint32_t count) {
        uint8_t buf[128];
        uint8_t hi = color >> 8;
        uint8_t lo = color & 0xFF;
        for (int i = 0; i < 128; i += 2) { buf[i] = hi; buf[i+1] = lo; }
        while (count >= 64) {
            _spi->writeBytes(buf, 128);
            count -= 64;
        }
        if (count > 0) {
            _spi->writeBytes(buf, count * 2);
        }
    }

    void drawCircleHelper(int16_t x0, int16_t y0, int16_t r,
                          uint8_t corners, uint16_t color)
    {
        int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
        while (x < y) {
            if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
            x++; ddF_x += 2; f += ddF_x;
            if (corners & 0x4) { drawPixel(x0 + x, y0 + y, color); drawPixel(x0 + y, y0 + x, color); }
            if (corners & 0x2) { drawPixel(x0 + x, y0 - y, color); drawPixel(x0 + y, y0 - x, color); }
            if (corners & 0x8) { drawPixel(x0 - y, y0 + x, color); drawPixel(x0 - x, y0 + y, color); }
            if (corners & 0x1) { drawPixel(x0 - y, y0 - x, color); drawPixel(x0 - x, y0 - y, color); }
        }
    }

    void fillCircleHelper(int16_t x0, int16_t y0, int16_t r,
                          uint8_t corners, int16_t delta, uint16_t color)
    {
        int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
        while (x < y) {
            if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
            x++; ddF_x += 2; f += ddF_x;
            if (corners & 0x1) {
                drawFastVLine(x0 + x, y0 - y, 2 * y + 1 + delta, color);
                drawFastVLine(x0 + y, y0 - x, 2 * x + 1 + delta, color);
            }
            if (corners & 0x2) {
                drawFastVLine(x0 - x, y0 - y, 2 * y + 1 + delta, color);
                drawFastVLine(x0 - y, y0 - x, 2 * x + 1 + delta, color);
            }
        }
    }

    SPIClass *_spi;
    int8_t _sck, _miso, _mosi, _cs, _dc, _bl;
    uint32_t _freq;
    SemaphoreHandle_t _lock;
    bool              _owns_lock;

    int16_t  _width, _height;
    uint16_t _x_offset, _y_offset;

    int16_t  _cursor_x, _cursor_y;
    uint16_t _text_color, _text_bg;
    bool     _text_bg_set;
    uint8_t  _text_size;
};

#endif // DEVICE_TLORAPAGER

#endif // PM_DISP_TLORAPAGER_H