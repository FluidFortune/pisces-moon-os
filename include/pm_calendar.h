// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_calendar.h — Touch-kiosk calendar for C28P + Maxine
//
//  Month-grid calendar with NTP-synced dates. Persists per-day
//  notes in NoSQL category "calendar". One note per day max
//  (multi-line); future-day notes act as reminders.
//
//  Date keys are "YYYY-MM-DD" stored as the NoSQL entry title.
//  Content is the free-form note body.
//
//  Touch flow:
//    Month view → tap a day → opens day detail
//    Day detail → ADD/EDIT NOTE → opens pm_text_input
//    Day detail → DELETE → confirms then removes the entry
//    Month view → tap < or > → previous/next month
//    Month view → top strip → exit
//
//  NTP-aware: relies on time(nullptr) being valid. If not synced
//  yet, the calendar still works but defaults to the device's
//  current epoch (1970-01-01) and shows a "NO NTP" badge.
//
//  T-Deck Plus, T-LoRa Pager, and Cardputer ADV continue to use
//  the existing calendar.cpp keyboard-driven UI. This file is
//  gated to DEVICE_C28P / DEVICE_MAXINE only.
// ─────────────────────────────────────────────

#pragma once

void pm_run_calendar();   // touch-kiosk entry point
