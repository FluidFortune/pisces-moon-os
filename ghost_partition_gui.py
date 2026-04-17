# Pisces Moon OS
# Copyright (C) 2026 Eric Becker / Fluid Fortune
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# This program is free software: you can redistribute it
# and/or modify it under the terms of the GNU Affero General
# Public License as published by the Free Software Foundation,
# either version 3 of the License, or any later version.
#
# fluidfortune.com

#!/usr/bin/env python3
"""
🌙 PISCES MOON OS — GHOST PARTITION TOOL (GUI)
===============================================
CustomTkinter-based GUI wrapper for ghost_partition_tool.py

Requirements:
    pip install customtkinter

Run:
    python3 ghost_partition_gui.py

Apple Silicon note:
    Works natively on M-series and Neo chips.
    No Rosetta required — CustomTkinter is pure Python.

"Pisces Moon. Powered by Gemini. Limited only by your imagination."
"""

import sys
import os
import platform
import threading
import subprocess
import shutil
from pathlib import Path

# ─────────────────────────────────────────────
#  DEPENDENCY CHECK — friendly error before crash
# ─────────────────────────────────────────────
try:
    import customtkinter as ctk
    from tkinter import filedialog, messagebox
except ImportError:
    print("\n[ERROR] customtkinter not installed.")
    print("  Run: pip install customtkinter\n")
    sys.exit(1)

# Import the backend logic from ghost_partition_tool.py
# If it's in the same directory, this works directly.
# If not, we fall back to inline stubs.
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from ghost_partition_tool import (
        list_removable_devices,
        calculate_partitions,
        format_macos, format_linux, format_windows,
        find_ghost_partition,
        check_dependencies,
        flip_to_stealth,
        flip_to_visible,
        get_raw_device_path,
        _read_partition_type,
        GHOST_PARTITION_IDX,
        FAT32_TYPE,
        STEALTH_TYPE,
        PLATFORM
    )
    BACKEND_AVAILABLE = True
except ImportError:
    PLATFORM = platform.system()
    BACKEND_AVAILABLE = False

# ─────────────────────────────────────────────
#  PISCES MOON COLOR PALETTE
#  Matches the T-Deck BIOS + launcher exactly.
#  RGB565 → RGB888 conversions where needed.
# ─────────────────────────────────────────────
PM_BLACK       = "#000000"   # Pure black background
PM_DARK_BG     = "#080808"   # Slightly off-black for panels
PM_HEADER_BG   = "#020A02"   # Very dark green — header bars
PM_DIVIDER     = "#0A2A0A"   # Dark green divider lines
PM_GRID        = "#040C04"   # PCB grid lines
PM_DIM_GREEN   = "#0A3010"   # Very dim green — labels
PM_MID_GREEN   = "#0A6020"   # Mid green — secondary text
PM_BRIGHT_GREEN= "#00C800"   # Bright green — OK / primary
PM_CYAN        = "#00FFFF"   # Cyan — ACTIVE / accent
PM_YELLOW      = "#FFE000"   # Yellow — WARN / triggered
PM_RED         = "#FF2000"   # Red — FAIL / danger
PM_WHITE       = "#FFFFFF"   # White — primary text
PM_DIM_TEXT    = "#607060"   # Dim text
PM_CARD_BG     = "#040F04"   # Card / panel background
PM_CARD_BORDER = "#0A3A0A"   # Card border

# Font stack — monospace terminal feel
FONT_MONO_LG  = ("Courier New", 14, "bold")
FONT_MONO_MD  = ("Courier New", 12)
FONT_MONO_SM  = ("Courier New", 10)
FONT_TITLE    = ("Courier New", 18, "bold")
FONT_SECTION  = ("Courier New", 11, "bold")

# ─────────────────────────────────────────────
#  CUSTOMTKINTER GLOBAL CONFIG
# ─────────────────────────────────────────────
ctk.set_appearance_mode("dark")
ctk.set_default_color_theme("dark-blue")

# ─────────────────────────────────────────────
#  MAIN APPLICATION WINDOW
# ─────────────────────────────────────────────
class PiscesMoonApp(ctk.CTk):
    def __init__(self):
        super().__init__()

        self.title("🌙 Pisces Moon OS — Ghost Partition Tool")
        self.geometry("900x660")
        self.minsize(820, 580)
        self.configure(fg_color=PM_BLACK)
        self.resizable(True, True)

        # State
        self.selected_device = ctk.StringVar(value="")
        self.log_lines = []
        self._log_lock = threading.Lock()

        self._build_ui()
        self._refresh_devices()

        # Startup log
        self.log(f"Pisces Moon Ghost Partition Tool", color=PM_BRIGHT_GREEN, bold=True)
        self.log(f"Platform: {PLATFORM} | Python {sys.version.split()[0]}")
        self.log(f"Backend: {'loaded' if BACKEND_AVAILABLE else 'not found — place ghost_partition_tool.py in same directory'}")
        self.log("─" * 52, color=PM_DIVIDER)
        self.log("Insert SD card and click Refresh, then select device.")

    # ─────────────────────────────────────────────
    #  UI CONSTRUCTION
    # ─────────────────────────────────────────────
    def _build_ui(self):
        # ── Header bar ──
        header = ctk.CTkFrame(self, fg_color=PM_HEADER_BG, corner_radius=0, height=48)
        header.pack(fill="x", side="top")
        header.pack_propagate(False)

        ctk.CTkLabel(
            header, text="🌙  PISCES MOON OS",
            font=FONT_TITLE, text_color=PM_BRIGHT_GREEN
        ).pack(side="left", padx=16, pady=8)

        ctk.CTkLabel(
            header, text="GHOST PARTITION TOOL  v1.0",
            font=FONT_MONO_SM, text_color=PM_DIM_GREEN
        ).pack(side="left", padx=0, pady=8)

        ctk.CTkLabel(
            header, text=f"[ {PLATFORM.upper()} ]",
            font=FONT_MONO_SM, text_color=PM_CYAN
        ).pack(side="right", padx=16, pady=8)

        # Thin green divider under header
        div = ctk.CTkFrame(self, fg_color=PM_BRIGHT_GREEN, height=1, corner_radius=0)
        div.pack(fill="x")

        # ── Main content area (left panel + right log) ──
        content = ctk.CTkFrame(self, fg_color=PM_BLACK, corner_radius=0)
        content.pack(fill="both", expand=True, padx=0, pady=0)

        # Left panel — controls
        left = ctk.CTkFrame(content, fg_color=PM_DARK_BG, corner_radius=0, width=340)
        left.pack(side="left", fill="y", padx=0, pady=0)
        left.pack_propagate(False)
        self._build_left_panel(left)

        # Vertical divider
        vdiv = ctk.CTkFrame(content, fg_color=PM_CARD_BORDER, width=1, corner_radius=0)
        vdiv.pack(side="left", fill="y")

        # Right panel — terminal log
        right = ctk.CTkFrame(content, fg_color=PM_BLACK, corner_radius=0)
        right.pack(side="left", fill="both", expand=True)
        self._build_log_panel(right)

        # ── Footer ──
        footer = ctk.CTkFrame(self, fg_color=PM_HEADER_BG, corner_radius=0, height=28)
        footer.pack(fill="x", side="bottom")
        footer.pack_propagate(False)
        ctk.CTkFrame(self, fg_color=PM_BRIGHT_GREEN, height=1, corner_radius=0).pack(
            fill="x", side="bottom"
        )

        self.status_label = ctk.CTkLabel(
            footer, text="READY",
            font=FONT_MONO_SM, text_color=PM_DIM_GREEN
        )
        self.status_label.pack(side="left", padx=12, pady=4)

        ctk.CTkLabel(
            footer,
            text='"Reticulating Splines since \'94."',
            font=FONT_MONO_SM, text_color=PM_DIM_GREEN
        ).pack(side="right", padx=12, pady=4)

    def _build_left_panel(self, parent):
        """Build the left control panel."""
        # Scrollable container
        scroll = ctk.CTkScrollableFrame(
            parent, fg_color=PM_DARK_BG,
            scrollbar_button_color=PM_CARD_BORDER,
            scrollbar_button_hover_color=PM_MID_GREEN,
            corner_radius=0
        )
        scroll.pack(fill="both", expand=True, padx=0, pady=0)

        pad = {"padx": 14, "pady": 4}

        # ── Section: Device Selection ──
        self._section_label(scroll, "// DEVICE SELECTION")

        self.device_menu = ctk.CTkComboBox(
            scroll,
            variable=self.selected_device,
            values=["No devices found"],
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG,
            border_color=PM_CARD_BORDER,
            button_color=PM_MID_GREEN,
            button_hover_color=PM_BRIGHT_GREEN,
            dropdown_fg_color=PM_CARD_BG,
            dropdown_hover_color=PM_DIM_GREEN,
            text_color=PM_BRIGHT_GREEN,
            dropdown_text_color=PM_WHITE,
            width=306,
            state="readonly"
        )
        self.device_menu.pack(**pad)

        ctk.CTkButton(
            scroll, text="⟳  REFRESH DEVICES",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_CYAN,
            width=306, height=28,
            command=self._refresh_devices
        ).pack(**pad)

        # ── Section: Format ──
        self._section_label(scroll, "// FORMAT CARD")

        self._info_box(scroll,
            "Creates two FAT32 partitions:\n"
            "  P1: PUBLIC  (medical, baseball, music)\n"
            "  P2: GHOST   (wardrive, vault, scans)\n\n"
            "Recommended: 16GB → 2×8GB\n"
            "Supported:   64GB → 2×32GB (FAT32 max)"
        )

        self.format_btn = ctk.CTkButton(
            scroll, text="⚠  FORMAT SD CARD",
            font=FONT_MONO_MD,
            fg_color="#1A0000", hover_color="#3A0000",
            border_color=PM_RED, border_width=1,
            text_color=PM_RED,
            width=306, height=36,
            command=self._on_format
        )
        self.format_btn.pack(**pad)

        # ── Section: Verify ──
        self._section_label(scroll, "// VERIFY LAYOUT")

        ctk.CTkButton(
            scroll, text="✓  VERIFY CARD LAYOUT",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_BRIGHT_GREEN,
            width=306, height=32,
            command=self._on_verify
        ).pack(**pad)

        # ── Section: Browse / Export ──
        self._section_label(scroll, "// GHOST PARTITION DATA")

        self._info_box(scroll,
            "Disaster recovery: access Ghost\n"
            "Partition data when T-Deck is\n"
            "unavailable. Card must be inserted\n"
            "and Ghost Partition mounted."
        )

        ctk.CTkButton(
            scroll, text="📂  BROWSE GHOST PARTITION",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_CYAN,
            width=306, height=32,
            command=self._on_browse
        ).pack(**pad)

        ctk.CTkButton(
            scroll, text="⬇  EXPORT TO COMPUTER",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_CYAN,
            width=306, height=32,
            command=self._on_export
        ).pack(**pad)

        # ── Section: MBR Byte-Flip ──
        self._section_label(scroll, "// STEALTH CONTROL")

        self._info_box(scroll,
            "Stealth: hides Ghost Partition from\n"
            "  Mac/Linux/Windows card readers.\n"
            "Unlock: reveals it for desktop access.\n"
            "T-Deck is unaffected by either state."
        )

        ctk.CTkButton(
            scroll, text="👁  UNLOCK GHOST PARTITION",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_BRIGHT_GREEN,
            width=306, height=32,
            command=self._on_unstealth
        ).pack(**pad)

        ctk.CTkButton(
            scroll, text="🔒  STEALTH GHOST PARTITION",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_YELLOW,
            width=306, height=32,
            command=self._on_stealth
        ).pack(**pad)

        # ── Section: Dependencies ──
        self._section_label(scroll, "// SYSTEM")

        ctk.CTkButton(
            scroll, text="⚙  CHECK DEPENDENCIES",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_DIM_TEXT,
            width=306, height=28,
            command=self._on_check_deps
        ).pack(**pad)

        ctk.CTkButton(
            scroll, text="✕  CLEAR LOG",
            font=FONT_MONO_SM,
            fg_color=PM_CARD_BG, hover_color=PM_DIM_GREEN,
            border_color=PM_CARD_BORDER, border_width=1,
            text_color=PM_DIM_TEXT,
            width=306, height=28,
            command=self._clear_log
        ).pack(**pad)

        # Spacer
        ctk.CTkFrame(scroll, fg_color="transparent", height=20).pack()

    def _build_log_panel(self, parent):
        """Build the right terminal log panel."""
        # Log header
        log_header = ctk.CTkFrame(parent, fg_color=PM_HEADER_BG, height=28, corner_radius=0)
        log_header.pack(fill="x")
        log_header.pack_propagate(False)
        ctk.CTkLabel(
            log_header, text="// TERMINAL OUTPUT",
            font=FONT_SECTION, text_color=PM_MID_GREEN
        ).pack(side="left", padx=12, pady=4)

        # The log textbox — dark terminal style
        self.log_text = ctk.CTkTextbox(
            parent,
            fg_color=PM_BLACK,
            text_color=PM_BRIGHT_GREEN,
            font=FONT_MONO_SM,
            corner_radius=0,
            border_width=0,
            wrap="word",
            state="disabled",
            scrollbar_button_color=PM_CARD_BORDER,
            scrollbar_button_hover_color=PM_MID_GREEN
        )
        self.log_text.pack(fill="both", expand=True, padx=0, pady=0)

        # Configure text tags for colors
        self.log_text._textbox.tag_configure("green",  foreground=PM_BRIGHT_GREEN)
        self.log_text._textbox.tag_configure("cyan",   foreground=PM_CYAN)
        self.log_text._textbox.tag_configure("yellow", foreground=PM_YELLOW)
        self.log_text._textbox.tag_configure("red",    foreground=PM_RED)
        self.log_text._textbox.tag_configure("dim",    foreground=PM_DIM_TEXT)
        self.log_text._textbox.tag_configure("white",  foreground=PM_WHITE)
        self.log_text._textbox.tag_configure("bold",   font=FONT_MONO_MD)

    def _section_label(self, parent, text):
        """Draw a section header label matching BIOS style."""
        frame = ctk.CTkFrame(parent, fg_color=PM_HEADER_BG, height=22, corner_radius=0)
        frame.pack(fill="x", padx=0, pady=(10, 2))
        frame.pack_propagate(False)
        ctk.CTkLabel(
            frame, text=text,
            font=FONT_SECTION, text_color=PM_MID_GREEN
        ).pack(side="left", padx=10, pady=2)

    def _info_box(self, parent, text):
        """Draw a dim info box."""
        ctk.CTkLabel(
            parent, text=text,
            font=FONT_MONO_SM, text_color=PM_DIM_TEXT,
            justify="left", anchor="w",
            fg_color=PM_CARD_BG,
            corner_radius=4,
            wraplength=286
        ).pack(padx=14, pady=(2, 4), fill="x")

    # ─────────────────────────────────────────────
    #  LOGGING
    # ─────────────────────────────────────────────
    def log(self, message, color=None, bold=False):
        """Thread-safe log to terminal panel."""
        def _write():
            self.log_text.configure(state="normal")
            tag = None
            if color == PM_BRIGHT_GREEN or color is None:
                tag = "green"
            elif color == PM_CYAN:
                tag = "cyan"
            elif color == PM_YELLOW:
                tag = "yellow"
            elif color == PM_RED:
                tag = "red"
            elif color == PM_DIM_TEXT or color == PM_DIM_GREEN:
                tag = "dim"
            elif color == PM_WHITE:
                tag = "white"

            line = f"{message}\n"
            self.log_text._textbox.insert("end", line, (tag,) if tag else ())
            self.log_text._textbox.see("end")
            self.log_text.configure(state="disabled")

        self.after(0, _write)

    def log_ok(self, msg):
        self.log(f"[  OK  ] {msg}", color=PM_BRIGHT_GREEN)

    def log_info(self, msg):
        self.log(f"[ INFO ] {msg}", color=PM_CYAN)

    def log_warn(self, msg):
        self.log(f"[ WARN ] {msg}", color=PM_YELLOW)

    def log_error(self, msg):
        self.log(f"[ FAIL ] {msg}", color=PM_RED)

    def log_step(self, msg):
        self.log(f"[ .... ] {msg}", color=PM_DIM_TEXT)

    def _clear_log(self):
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")
        self.log("Log cleared.", color=PM_DIM_TEXT)

    def _set_status(self, text, color=None):
        self.after(0, lambda: self.status_label.configure(
            text=text,
            text_color=color or PM_DIM_GREEN
        ))

    # ─────────────────────────────────────────────
    #  DEVICE MANAGEMENT
    # ─────────────────────────────────────────────
    def _refresh_devices(self):
        self.log_step("Scanning for removable devices...")
        self._set_status("SCANNING...", PM_YELLOW)

        def _scan():
            if not BACKEND_AVAILABLE:
                self.after(0, lambda: self.device_menu.configure(
                    values=["ghost_partition_tool.py not found"]
                ))
                self.log_warn("Backend not available. Place ghost_partition_tool.py in same folder.")
                self._set_status("BACKEND MISSING", PM_RED)
                return

            devices = list_removable_devices()
            if devices:
                labels = [f"{path}  ({size})  {name}" for path, size, name in devices]
                paths  = [path for path, size, name in devices]
                self.after(0, lambda: self.device_menu.configure(values=labels))
                self.after(0, lambda: self.selected_device.set(labels[0]))
                self._device_paths = paths
                self._device_labels = labels
                self.log_ok(f"Found {len(devices)} removable device(s).")
                for path, size, name in devices:
                    self.log(f"         {path}  {size}  {name}", color=PM_CYAN)
            else:
                self.after(0, lambda: self.device_menu.configure(
                    values=["No removable devices found"]
                ))
                self.log_warn("No removable devices detected.")
                self.log("         Insert SD card via USB reader and refresh.", color=PM_DIM_TEXT)

            self._set_status("READY", PM_DIM_GREEN)

        threading.Thread(target=_scan, daemon=True).start()

    def _get_selected_device_path(self):
        """Resolve the selected combobox entry back to a device path."""
        if not hasattr(self, '_device_labels') or not hasattr(self, '_device_paths'):
            return None
        sel = self.selected_device.get()
        if sel in self._device_labels:
            idx = self._device_labels.index(sel)
            return self._device_paths[idx]
        return None

    # ─────────────────────────────────────────────
    #  FORMAT
    # ─────────────────────────────────────────────
    def _on_format(self):
        device = self._get_selected_device_path()
        if not device:
            self.log_error("No device selected. Refresh and select a device first.")
            return

        # Confirmation dialog — two step
        confirm1 = messagebox.askyesno(
            "⚠ FORMAT SD CARD",
            f"This will ERASE ALL DATA on:\n\n{device}\n\n"
            f"Two FAT32 partitions will be created:\n"
            f"  • PMOON-PUB   (Public / Decoy)\n"
            f"  • PMOON-GHOST (Ghost / Tactical)\n\n"
            f"Are you sure you want to continue?",
            icon="warning"
        )
        if not confirm1:
            self.log_warn("Format cancelled.")
            return

        confirm2 = messagebox.askyesno(
            "⚠ FINAL CONFIRMATION",
            f"LAST CHANCE:\n\n"
            f"ALL DATA on {device} will be permanently erased.\n\n"
            f"Type carefully — there is no undo.\n\n"
            f"Proceed with format?",
            icon="warning"
        )
        if not confirm2:
            self.log_warn("Format cancelled at final confirmation.")
            return

        self.log("─" * 52, color=PM_DIVIDER)
        self.log(f"FORMATTING {device}", color=PM_RED, bold=True)
        self.format_btn.configure(state="disabled")
        self._set_status("FORMATTING...", PM_RED)

        def _run():
            # Redirect print output to our log
            import io
            original_stdout = sys.stdout

            class LogCapture(io.StringIO):
                def write(inner_self, s):
                    if s.strip():
                        if "[ERROR]" in s or "FAIL" in s:
                            self.log_error(s.strip().lstrip("[ERRORINFOWARN] "))
                        elif "[WARN]" in s:
                            self.log_warn(s.strip().lstrip("[ERRORINFOWARN] "))
                        elif "[INFO]" in s or "OK" in s or "complete" in s.lower():
                            self.log_ok(s.strip().lstrip("[ERRORINFOWARN] "))
                        else:
                            self.log_step(s.strip())

            sys.stdout = LogCapture()
            try:
                if not BACKEND_AVAILABLE:
                    raise RuntimeError("Backend not available")

                if PLATFORM == "Darwin":
                    format_macos(device, skip_confirm=True)
                elif PLATFORM == "Linux":
                    format_linux(device, skip_confirm=True)
                elif PLATFORM == "Windows":
                    format_windows(device, skip_confirm=True)
                else:
                    raise RuntimeError(f"Unsupported platform: {PLATFORM}")

                self.log_ok("Format sequence complete.")
                self._set_status("FORMAT COMPLETE", PM_BRIGHT_GREEN)

            except Exception as e:
                self.log_error(f"Format failed: {e}")
                self._set_status("FORMAT FAILED", PM_RED)
            finally:
                sys.stdout = original_stdout
                self.after(0, lambda: self.format_btn.configure(state="normal"))
                self._refresh_devices()

        threading.Thread(target=_run, daemon=True).start()

    # ─────────────────────────────────────────────
    #  VERIFY
    # ─────────────────────────────────────────────
    def _on_verify(self):
        device = self._get_selected_device_path()
        if not device:
            self.log_error("No device selected.")
            return

        self.log("─" * 52, color=PM_DIVIDER)
        self.log_step(f"Verifying layout on {device}...")
        self._set_status("VERIFYING...", PM_YELLOW)

        def _run():
            try:
                if PLATFORM == "Darwin":
                    result = subprocess.run(
                        ["diskutil", "list", device],
                        capture_output=True, text=True
                    )
                    output = result.stdout
                    for line in output.splitlines():
                        self.log(f"  {line}", color=PM_DIM_TEXT)

                    if "PMOON-PUB" in output and "PMOON-GHOST" in output:
                        self.log_ok("Both Pisces Moon partitions detected.")
                        self.log_ok("Card is correctly formatted for Ghost Partition.")
                        self._set_status("VERIFIED OK", PM_BRIGHT_GREEN)
                    elif "PMOON-PUB" in output:
                        self.log_warn("Only PUBLIC partition found.")
                        self.log_warn("Ghost Partition not yet created — use FORMAT.")
                        self._set_status("PARTIAL LAYOUT", PM_YELLOW)
                    else:
                        self.log_warn("No Pisces Moon partitions detected.")
                        self.log("         Use FORMAT to prepare this card.", color=PM_DIM_TEXT)
                        self._set_status("NOT FORMATTED", PM_YELLOW)

                elif PLATFORM == "Linux":
                    result = subprocess.run(
                        ["lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT", device],
                        capture_output=True, text=True
                    )
                    for line in result.stdout.splitlines():
                        self.log(f"  {line}", color=PM_DIM_TEXT)

                    if "PMOON-GHOST" in result.stdout:
                        self.log_ok("Ghost Partition detected.")
                        self._set_status("VERIFIED OK", PM_BRIGHT_GREEN)
                    else:
                        self.log_warn("No Ghost Partition detected.")
                        self._set_status("NOT FORMATTED", PM_YELLOW)

                elif PLATFORM == "Windows":
                    self.log_info("Run 'diskpart → list volume' to verify manually.")
                    self._set_status("MANUAL CHECK NEEDED", PM_YELLOW)

            except Exception as e:
                self.log_error(f"Verify failed: {e}")
                self._set_status("VERIFY FAILED", PM_RED)

        threading.Thread(target=_run, daemon=True).start()

    # ─────────────────────────────────────────────
    #  BROWSE
    # ─────────────────────────────────────────────
    def _on_browse(self):
        self.log("─" * 52, color=PM_DIVIDER)
        self.log_step("Locating Ghost Partition...")
        self._set_status("SCANNING...", PM_YELLOW)

        def _run():
            ghost_path = find_ghost_partition() if BACKEND_AVAILABLE else None

            if not ghost_path:
                self.log_warn("Ghost Partition not auto-detected.")
                self.log("         Insert card and ensure PMOON-GHOST is mounted.", color=PM_DIM_TEXT)

                # Ask user to browse manually
                def _ask():
                    path = filedialog.askdirectory(
                        title="Select Ghost Partition mount point"
                    )
                    if path:
                        self._browse_path(path)
                    else:
                        self._set_status("BROWSE CANCELLED", PM_DIM_GREEN)

                self.after(0, _ask)
                return

            self._browse_path(ghost_path)

        threading.Thread(target=_run, daemon=True).start()

    def _browse_path(self, ghost_path):
        self.log_ok(f"Ghost Partition: {ghost_path}")
        self._set_status("BROWSING...", PM_CYAN)

        total_files = 0
        total_bytes = 0

        try:
            for root, dirs, files in os.walk(ghost_path):
                rel = os.path.relpath(root, ghost_path)
                if rel == ".":
                    rel = "/"
                level = rel.count(os.sep)
                indent = "  " * level
                self.log(f"{indent}📁 {os.path.basename(root)}/", color=PM_CYAN)

                subindent = "  " * (level + 1)
                for f in files:
                    fp = os.path.join(root, f)
                    size = os.path.getsize(fp)
                    total_bytes += size
                    total_files += 1
                    size_str = (f"{size/1024:.1f}KB"
                                if size < 1024*1024
                                else f"{size/1024/1024:.1f}MB")
                    self.log(f"{subindent}  {f}  ({size_str})", color=PM_DIM_TEXT)

            self.log(f"\n  Total: {total_files} files, "
                     f"{total_bytes/1024:.1f}KB", color=PM_WHITE)
            self._set_status(f"{total_files} FILES FOUND", PM_BRIGHT_GREEN)

        except Exception as e:
            self.log_error(f"Browse error: {e}")
            self._set_status("BROWSE ERROR", PM_RED)

    # ─────────────────────────────────────────────
    #  EXPORT
    # ─────────────────────────────────────────────
    def _on_export(self):
        self.log("─" * 52, color=PM_DIVIDER)

        # Ask destination first
        default_dest = str(Path.home() / "PiscesMoonExport")
        dest = filedialog.askdirectory(
            title="Choose export destination folder",
            initialdir=str(Path.home())
        )
        if not dest:
            self.log_warn("Export cancelled.")
            return

        self.log_step("Locating Ghost Partition...")
        self._set_status("EXPORTING...", PM_CYAN)

        def _run():
            ghost_path = find_ghost_partition() if BACKEND_AVAILABLE else None

            if not ghost_path:
                self.log_warn("Ghost Partition not auto-detected.")
                def _ask():
                    path = filedialog.askdirectory(
                        title="Select Ghost Partition mount point"
                    )
                    if path:
                        self._do_export(path, dest)
                    else:
                        self._set_status("EXPORT CANCELLED", PM_DIM_GREEN)
                self.after(0, _ask)
                return

            self._do_export(ghost_path, dest)

        threading.Thread(target=_run, daemon=True).start()

    def _do_export(self, ghost_path, dest):
        self.log_ok(f"Source: {ghost_path}")
        self.log_ok(f"Dest:   {dest}")

        copied = 0
        errors = 0
        dest_path = Path(dest)

        try:
            for root, dirs, files in os.walk(ghost_path):
                rel_root = os.path.relpath(root, ghost_path)
                dest_root = dest_path / rel_root
                dest_root.mkdir(parents=True, exist_ok=True)

                for f in files:
                    src = os.path.join(root, f)
                    dst = dest_root / f
                    try:
                        shutil.copy2(src, dst)
                        copied += 1
                        rel = os.path.join(rel_root, f)
                        self.log(f"  ✓ {rel}", color=PM_BRIGHT_GREEN)
                    except Exception as e:
                        errors += 1
                        self.log_error(f"  Failed: {f} — {e}")

            self.log("─" * 52, color=PM_DIVIDER)
            self.log_ok(f"Export complete — {copied} files copied.")
            if errors:
                self.log_warn(f"{errors} file(s) failed to copy.")
            self.log_info("Wardrive CSVs → open in Excel or import to WiGLE")
            self.log_info("Beacon/scan JSON → open in any JSON viewer or Python")
            self._set_status(f"EXPORTED {copied} FILES", PM_BRIGHT_GREEN)

        except Exception as e:
            self.log_error(f"Export failed: {e}")
            self._set_status("EXPORT FAILED", PM_RED)

    # ─────────────────────────────────────────────
    #  STEALTH / UNSTEALTH
    # ─────────────────────────────────────────────
    def _on_stealth(self):
        """Apply MBR byte-flip to hide Ghost Partition from desktop OS."""
        device = self._get_selected_device_path()
        if not device:
            self.log_error("No device selected. Refresh and select a device first.")
            return

        self.log("─" * 52, color=PM_DIVIDER)
        self.log_step(f"Applying stealth to Ghost Partition on {device}...")
        self._set_status("STEALTHING...", PM_YELLOW)

        def _run():
            try:
                if not BACKEND_AVAILABLE:
                    raise RuntimeError("Backend not available")
                import subprocess
                # Unmount ghost partition first on macOS
                if PLATFORM == "Darwin":
                    subprocess.run(["diskutil", "unmount", f"{device}s2"], capture_output=True)
                raw = get_raw_device_path(device)
                current = _read_partition_type(raw, GHOST_PARTITION_IDX)
                if current is not None:
                    self.log_info(f"Current type: 0x{current:02X}")
                if flip_to_stealth(raw):
                    self.log_ok("Ghost Partition stealthed — invisible to consumer OS.")
                    self.log_info("T-Deck will still mount it correctly.")
                    self._set_status("STEALTHED", PM_YELLOW)
                else:
                    self.log_error("Stealth failed — check permissions.")
                    self._set_status("STEALTH FAILED", PM_RED)
            except Exception as e:
                self.log_error(f"Stealth error: {e}")
                self._set_status("ERROR", PM_RED)

        import threading
        threading.Thread(target=_run, daemon=True).start()

    def _on_unstealth(self):
        """Remove MBR byte-flip — restore Ghost Partition visibility on desktop."""
        device = self._get_selected_device_path()
        if not device:
            self.log_error("No device selected. Refresh and select a device first.")
            return

        self.log("─" * 52, color=PM_DIVIDER)
        self.log_step(f"Unlocking Ghost Partition on {device}...")
        self._set_status("UNLOCKING...", PM_BRIGHT_GREEN)

        def _run():
            try:
                if not BACKEND_AVAILABLE:
                    raise RuntimeError("Backend not available")
                import subprocess, time
                raw = get_raw_device_path(device)
                current = _read_partition_type(raw, GHOST_PARTITION_IDX)
                if current is not None:
                    self.log_info(f"Current type: 0x{current:02X}")
                if flip_to_visible(raw):
                    self.log_ok("Ghost Partition unlocked — visible on desktop.")
                    if PLATFORM == "Darwin":
                        time.sleep(1)
                        subprocess.run(["diskutil", "mount", f"{device}s2"], capture_output=True)
                        self.log_info("Auto-mount attempted. Check Finder for PMOON-GHOST.")
                    self.log_warn("Remember to Stealth when done to re-hide the partition.")
                    self._set_status("UNLOCKED", PM_BRIGHT_GREEN)
                else:
                    self.log_error("Unlock failed — check permissions.")
                    self._set_status("UNLOCK FAILED", PM_RED)
            except Exception as e:
                self.log_error(f"Unlock error: {e}")
                self._set_status("ERROR", PM_RED)

        import threading
        threading.Thread(target=_run, daemon=True).start()

    # ─────────────────────────────────────────────
    #  DEPENDENCIES
    # ─────────────────────────────────────────────
    def _on_check_deps(self):
        self.log("─" * 52, color=PM_DIVIDER)
        self.log_step("Checking system dependencies...")

        def _run():
            if PLATFORM == "Darwin":
                self.log_ok("macOS: diskutil — built-in, no install needed.")
                self.log_ok("macOS: No additional dependencies required.")
                self._set_status("DEPS OK", PM_BRIGHT_GREEN)

            elif PLATFORM == "Linux":
                tools = {"parted": False, "mkfs.fat": False, "lsblk": False}
                for tool in tools:
                    found = shutil.which(tool) is not None
                    tools[tool] = found
                    if found:
                        self.log_ok(f"{tool} — found at {shutil.which(tool)}")
                    else:
                        self.log_error(f"{tool} — NOT FOUND")

                missing = [t for t, found in tools.items() if not found]
                if missing:
                    self.log_warn(f"Install missing tools:")
                    self.log(f"  sudo apt install parted dosfstools util-linux",
                             color=PM_YELLOW)
                    self._set_status("DEPS MISSING", PM_RED)
                else:
                    self.log_ok("All Linux dependencies satisfied.")
                    self._set_status("DEPS OK", PM_BRIGHT_GREEN)

            elif PLATFORM == "Windows":
                self.log_ok("Windows: diskpart — built-in.")
                try:
                    import ctypes
                    if ctypes.windll.shell32.IsUserAnAdmin():
                        self.log_ok("Running as Administrator — format operations available.")
                        self._set_status("DEPS OK (ADMIN)", PM_BRIGHT_GREEN)
                    else:
                        self.log_warn("Not running as Administrator.")
                        self.log_warn("Format operations require elevated privileges.")
                        self._set_status("NEED ADMIN", PM_YELLOW)
                except Exception:
                    self.log_info("Could not check admin status.")
                    self._set_status("DEPS CHECK DONE", PM_DIM_GREEN)

        threading.Thread(target=_run, daemon=True).start()


# ─────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────
if __name__ == "__main__":
    app = PiscesMoonApp()
    app.mainloop()
