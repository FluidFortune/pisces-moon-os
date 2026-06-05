// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_text_input.h — Modal on-screen touch keyboard
//
//  pm_text_input() shows a full-screen modal with a prompt label,
//  a text edit area, and a QWERTY soft keyboard. It blocks until
//  the user taps ENTER (confirm) or the top EXIT bar (cancel).
//
//  Used by pm_notes, pm_contacts, and pm_calendar on the C28P and
//  Maxine touch kiosks where there is no physical keyboard. The
//  function is a no-op (returns false) on keyboard-equipped devices
//  — those use keyboard.cpp directly.
//
//  KEYBOARD LAYERS:
//    lower   — q w e r t y u i o p
//              a s d f g h j k l ⌫
//              ⇧ z x c v b n m .
//              123 , SPACE . ↵
//    UPPER   — same layout, capitalized; shift returns to lower
//    sym     — digits, math/punctuation; ABC returns to lower
//
//  CONFIRM / CANCEL:
//    ENTER on the keyboard      → confirm (returns true)
//    EXIT bar at top of screen  → cancel  (returns false)
// ─────────────────────────────────────────────

#pragma once

#include <stddef.h>

// Show the modal keyboard. Blocks until ENTER or CANCEL.
//
// label:    short prompt (e.g. "NOTE:", "PHONE:", "EMAIL:")
// out:      caller-supplied buffer; populated on confirm
// outlen:   size of out buffer; maximum text length is outlen - 1
// initial:  pre-fill text (may be nullptr or "")
//
// Returns true on confirm (ENTER), false on cancel (EXIT bar).
// On cancel, *out is left unchanged.
bool pm_text_input(const char* label, char* out, size_t outlen, const char* initial);
