// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <Arduino.h>

#ifdef DEVICE_CARDPUTER_ADV

bool readPagerBattery(int &percent, uint16_t &mv) {
    percent = 0;
    mv = 0;
    return false;
}

#endif // DEVICE_CARDPUTER_ADV
