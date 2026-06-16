// Pisces Moon OS
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// pm_gps was an in-progress refactor to extract GPS UART ownership
// out of wardrive_task. It was rolled back when LoRa observation
// was folded directly into the wardrive task instead of being a
// separate app — wardrive_task remains the sole owner of the GPS
// UART, which is the original design intent. This header is left
// as an empty placeholder so no stale #include resurrects a phantom
// API.

#ifndef PM_GPS_H
#define PM_GPS_H
#endif
