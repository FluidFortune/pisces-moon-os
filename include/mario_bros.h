// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  mario_bros.h — Pisces Moon original arcade-style platform game
//
//  Single-screen platform-flipping arcade game in the spirit of
//  Nintendo's 1983 Mario Bros. arcade cabinet (NOT the 1985 NES
//  side-scroller, which is a different game). This implementation
//  is entirely original code — our own engine, our own collision
//  model, our own level data and tuning constants. Pisces Moon's
//  pixel-art aesthetic, our protagonist "Captain Pisces" (the
//  jumping astronaut who replaced our retired side-scroller hero
//  of the same name).
//
//  Genre overview (the genre is the genre, not the IP):
//    - Fixed single-screen play field. No camera, no scrolling.
//    - Five horizontal platforms separated by floor gaps.
//    - Two pipes top-left / top-right spawn enemies that fall
//      onto the highest platform and walk down level by level.
//    - Player punches the platform from below to flip an enemy
//      on the surface above; flipped enemies can be kicked off
//      the screen for points. Touching an upright enemy is fatal.
//    - POW block in the center: 3 uses, cracks visually with each
//      hit, stuns every grounded enemy on screen.
//    - Phase progression: enemy mix, spawn rate, and speed escalate
//      every cleared phase. Periodic ice phases turn the floor
//      slippery (reduced friction, slide on stop).
//    - Wraparound pipes on either side of every floor — walk off
//      the left edge, reappear on the right.
//
//  Enemy types (escalating phase order):
//    - Shellcreeper (Goomba-shaped turtle) — single hit to flip.
//    - Sidestepper (crab) — two hits required; first hit angers
//      it (color shifts, speed up), second flips it.
//    - Fighter Fly — moves in hops rather than walks; can only
//      be flipped when its hop is on the ground.
//    - Each phase, surviving enemies turn red and accelerate.
//
//  DEVICES
//
//  All five: T-Deck Plus, T-LoRa Pager, Cardputer ADV, C28P, Maxine.
//  Per-device viewport scaling — the play field stretches to fill
//  the available width while maintaining the 5-platform layout
//  proportionally. Touch kiosks (C28P, Maxine) reuse the existing
//  virtual D-pad chrome painted by c28p_dpad / maxine_dpad.
//
//  INPUT MODEL
//
//    Keyboard devices  (T-Deck, Pager, Cardputer)
//      A / Left arrow     move left
//      D / Right arrow    move right
//      Space / W / B      jump
//      Q                  quit
//
//    Touch kiosks  (C28P, Maxine)
//      Virtual D-pad left/right    move
//      Virtual D-pad A button      jump
//      Top-strip / EXIT tile       quit
//
//  PERSISTENCE
//
//    High score saved to /mario_bros_hs.txt via SdFat (T-Deck, Pager,
//    Cardputer, Maxine) or SD_MMC (C28P) under the same SPI Bus
//    Treaty as Tetris / Pac-Man / Galaga: high-score I/O is wrapped
//    in spi_mutex when the bus is shared with the Ghost Engine.
// ─────────────────────────────────────────────

#ifndef MARIO_BROS_H
#define MARIO_BROS_H

void run_mario_bros();

#endif
