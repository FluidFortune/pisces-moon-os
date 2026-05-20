// Retired Arduino_GFX subclass driver. The active T-LoRa Pager display
// driver is the standalone inline PMDispTLoRaPager in pm_disp_tlorapager.h.
#if 0
// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifdef DEVICE_TLORAPAGER

#include "pm_disp_tlorapager.h"
#include <Arduino.h>

// ============================================================
//  ST7796 register constants
// ============================================================
#define ST7796_MADCTL  0x36
#define ST7796_CASET   0x2A
#define ST7796_RASET   0x2B
#define ST7796_RAMWR   0x2C

// ============================================================
//  LilyGo's official rotation table for the T-LoRa-Pager.
//  Each row: { MADCTL byte, visible_w, visible_h, x_offset, y_offset }
//
//  Source: LilyGoLib/src/LilyGo_LoRa_Pager.cpp
//      static const DispRotationConfig_t rotation_config[4] = {
//          {0xE8, DISP_HEIGHT, DISP_WIDTH, 0, 49},
//          {0x48, DISP_WIDTH, DISP_HEIGHT, 49, 0},
//          {0x28, DISP_HEIGHT, DISP_WIDTH, 0, 49},
//          {0x88, DISP_WIDTH, DISP_HEIGHT, 49, 0},
//      };
//  DISP_WIDTH = 480, DISP_HEIGHT = 222
// ============================================================
const PMArduinoST7796_TLoraPager::RotationCfg
PMArduinoST7796_TLoraPager::ROTATION_TABLE[4] = {
    { 0xE8, 222, 480,  0, 49 },  // Rotation 0: portrait
    { 0x48, 480, 222, 49,  0 },  // Rotation 1: landscape   ← default
    { 0x28, 222, 480,  0, 49 },  // Rotation 2: portrait flipped
    { 0x88, 480, 222, 49,  0 },  // Rotation 3: landscape flipped
};

// ============================================================
//  Constructor — pass native panel WIDTH=222 HEIGHT=480 to the
//  parent. setRotation() updates _width/_height as needed.
//  Pass zero col/row offsets — we apply our own.
// ============================================================
PMArduinoST7796_TLoraPager::PMArduinoST7796_TLoraPager(
    Arduino_DataBus *bus, int8_t rst, uint8_t r, bool ips)
    : Arduino_ST7796(bus, rst, r, ips, 222, 480, 0, 0, 0, 0),
      _pm_x_offset(0), _pm_y_offset(49)
{
}

// ============================================================
//  setRotation — override with LilyGo's exact values.
//  We do NOT call Arduino_ST7796::setRotation (would write
//  wrong MADCTL). Replicate the bookkeeping it does + write
//  LilyGo's MADCTL byte directly.
// ============================================================
void PMArduinoST7796_TLoraPager::setRotation(uint8_t r) {
    _rotation = r & 0x03;
    const RotationCfg &cfg = ROTATION_TABLE[_rotation];

    // Set visible logical dimensions and clipping bounds.
    _width  = cfg.w;
    _height = cfg.h;
    _max_x  = _width  - 1;
    _max_y  = _height - 1;

    // Store our offsets — writeAddrWindow applies these.
    _pm_x_offset = cfg.x_off;
    _pm_y_offset = cfg.y_off;

    // Zero out parent offsets so they don't double-apply.
    _xStart = 0;
    _yStart = 0;

    // Invalidate the address-window cache so writeAddrWindow
    // re-sends CASET/RASET on the next draw call.
    _currentX = 0xFFFF;
    _currentY = 0xFFFF;
    _currentW = 0xFFFF;
    _currentH = 0xFFFF;

    // Write LilyGo's MADCTL byte directly.
    _bus->beginWrite();
    _bus->writeC8D8(ST7796_MADCTL, cfg.madctl);
    _bus->endWrite();

    Serial.printf("[PMDisp] setRotation(%u): MADCTL=0x%02X w=%u h=%u xoff=%u yoff=%u\n",
                  _rotation, cfg.madctl, cfg.w, cfg.h, cfg.x_off, cfg.y_off);
}

// ============================================================
//  writeAddrWindow — apply LilyGo's offsets to both start and
//  end addresses.
// ============================================================
void PMArduinoST7796_TLoraPager::writeAddrWindow(
    int16_t x, int16_t y, uint16_t w, uint16_t h)
{
    if ((x != _currentX) || (w != _currentW)) {
        _currentX = x;
        _currentW = w;
        uint16_t xs = x + _pm_x_offset;
        uint16_t xe = xs + w - 1;
        _bus->writeC8D16D16(ST7796_CASET, xs, xe);
    }
    if ((y != _currentY) || (h != _currentH)) {
        _currentY = y;
        _currentH = h;
        uint16_t ys = y + _pm_y_offset;
        uint16_t ye = ys + h - 1;
        _bus->writeC8D16D16(ST7796_RASET, ys, ye);
    }
    _bus->writeCommand(ST7796_RAMWR);
}

#endif // DEVICE_TLORAPAGER
#endif
