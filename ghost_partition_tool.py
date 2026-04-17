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
╔══════════════════════════════════════════════════════════════╗
║          PISCES MOON OS — GHOST PARTITION TOOL               ║
║          SD Card Formatter & Partition Manager               ║
╠══════════════════════════════════════════════════════════════╣
║  Cross-platform utility for:                                 ║
║    • Formatting MicroSD cards with the dual-partition        ║
║      layout required by Pisces Moon OS Ghost Partition       ║
║    • Browsing and exporting Ghost Partition data             ║
║      (disaster recovery when T-Deck is unavailable)         ║
║    • Verifying card health and partition layout              ║
║                                                              ║
║  Platform support:                                           ║
║    macOS  — diskutil (built-in, no dependencies)            ║
║    Linux  — parted + mkfs.fat (apt install parted dosfstools)║
║    Windows — diskpart + format (run as Administrator)        ║
║                                                              ║
║  PARTITION LAYOUT PRODUCED:                                  ║
║    Partition 1 (FAT32, ≤32GB) — Public / Decoy              ║
║      /data/medical/   Medical reference database             ║
║      /data/baseball/  Baseball statistics database           ║
║      /data/trails/    Hiking trail database                  ║
║      /music/          Audio files                            ║
║                                                              ║
║    Partition 2 (FAT32, ≤32GB) — Ghost / Tactical            ║
║      /wardrive/       Wardrive CSV logs                      ║
║      /data/gemini/    Gemini conversation history            ║
║      /vault/          Gemini vault saves                     ║
║      /scans/          Network scan results                   ║
║      /cyber_logs/     Pkt sniffer / beacon / scan logs       ║
║                                                              ║
║  RECOMMENDED CARD SIZES:                                     ║
║    Ideal:     16GB card → two 8GB FAT32 partitions           ║
║    Good:      32GB card → two 16GB FAT32 partitions          ║
║    Supported: 64GB card → two 32GB FAT32 partitions          ║
║    Max per partition: 32GB (FAT32 specification limit)       ║
║                                                              ║
║  SAFETY:                                                     ║
║    This tool WILL ERASE the target card completely.          ║
║    Double-check the device path before confirming.           ║
║    The tool requires confirmation at each destructive step.  ║
║                                                              ║
║  "Pisces Moon. Powered by Gemini.                            ║
║   Limited only by your imagination."                         ║
╚══════════════════════════════════════════════════════════════╝

Usage:
    python3 ghost_partition_tool.py              — interactive menu
    python3 ghost_partition_tool.py --format     — format a card
    python3 ghost_partition_tool.py --browse     — browse Ghost Partition
    python3 ghost_partition_tool.py --verify     — verify card layout
    python3 ghost_partition_tool.py --export DIR — export Ghost data to DIR
    python3 ghost_partition_tool.py --help       — show this help
"""

import sys
import os
import platform
import subprocess
import shutil
import argparse
import getpass
from pathlib import Path

# ─────────────────────────────────────────────
#  PLATFORM DETECTION
# ─────────────────────────────────────────────
PLATFORM = platform.system()  # 'Darwin', 'Linux', 'Windows'

CYAN    = "\033[96m"
GREEN   = "\033[92m"
YELLOW  = "\033[93m"
RED     = "\033[91m"
DIM     = "\033[2m"
BOLD    = "\033[1m"
RESET   = "\033[0m"

# Disable colors on Windows unless Windows Terminal
if PLATFORM == "Windows" and "WT_SESSION" not in os.environ:
    CYAN = GREEN = YELLOW = RED = DIM = BOLD = RESET = ""

def banner():
    print(f"""
{CYAN}{BOLD}╔══════════════════════════════════════════════════╗
║       🌙 PISCES MOON OS — GHOST PARTITION TOOL  ║
║          SD Card Formatter & Partition Manager   ║
╚══════════════════════════════════════════════════╝{RESET}
{DIM}Platform: {PLATFORM} | Python: {sys.version.split()[0]}{RESET}
""")

def info(msg):  print(f"{GREEN}[INFO]{RESET} {msg}")
def warn(msg):  print(f"{YELLOW}[WARN]{RESET} {msg}")
def error(msg): print(f"{RED}[ERROR]{RESET} {msg}")
def step(msg):  print(f"{CYAN}[....]{RESET} {msg}")

# ─────────────────────────────────────────────
#  DEPENDENCY CHECK
# ─────────────────────────────────────────────
def check_dependencies():
    """Check that required system tools are available."""
    missing = []

    if PLATFORM == "Darwin":
        # macOS — diskutil is always present
        pass

    elif PLATFORM == "Linux":
        for tool in ["parted", "mkfs.fat", "lsblk"]:
            if shutil.which(tool) is None:
                missing.append(tool)
        if missing:
            warn(f"Missing tools: {', '.join(missing)}")
            print(f"  Install with: sudo apt install parted dosfstools util-linux")

    elif PLATFORM == "Windows":
        # diskpart is always present on Windows
        # Check if running as admin
        try:
            import ctypes
            if not ctypes.windll.shell32.IsUserAnAdmin():
                warn("Not running as Administrator.")
                print("  Format operations require elevated privileges.")
                print("  Right-click the script and choose 'Run as administrator'.")
        except Exception:
            pass

    return len(missing) == 0

# ─────────────────────────────────────────────
#  CARD DETECTION
# ─────────────────────────────────────────────
def list_removable_devices():
    """Returns a list of (device_path, size_str, name) tuples for removable media."""
    devices = []

    if PLATFORM == "Darwin":
        result = subprocess.run(
            ["diskutil", "list", "-plist"],
            capture_output=True, text=True
        )
        # Parse diskutil list output (plain text version is easier)
        result = subprocess.run(["diskutil", "list"], capture_output=True, text=True)
        current_disk = None
        for line in result.stdout.splitlines():
            if line.startswith("/dev/disk") and "(external" in line:
                current_disk = line.split()[0]
            elif current_disk and "*" in line and "GB" in line:
                parts = line.split()
                for i, p in enumerate(parts):
                    if "GB" in p or "MB" in p:
                        size = parts[i-1] + " " + p
                        devices.append((current_disk, size, "Removable Disk"))
                        break
                current_disk = None

    elif PLATFORM == "Linux":
        result = subprocess.run(
            ["lsblk", "-d", "-o", "NAME,SIZE,RM,TYPE,VENDOR", "--json"],
            capture_output=True, text=True
        )
        import json
        try:
            data = json.loads(result.stdout)
            for dev in data.get("blockdevices", []):
                if dev.get("rm") == "1" and dev.get("type") == "disk":
                    name = dev.get("vendor", "").strip() or "Removable Disk"
                    devices.append((f"/dev/{dev['name']}", dev['size'], name))
        except (json.JSONDecodeError, KeyError):
            # Fallback plain text parse
            result = subprocess.run(["lsblk", "-d", "-o", "NAME,SIZE,RM"], capture_output=True, text=True)
            for line in result.stdout.splitlines()[1:]:
                parts = line.split()
                if len(parts) >= 3 and parts[2] == "1":
                    devices.append((f"/dev/{parts[0]}", parts[1], "Removable Disk"))

    elif PLATFORM == "Windows":
        # Use wmic to find removable disks
        result = subprocess.run(
            ["wmic", "diskdrive", "where", "MediaType='Removable Media'",
             "get", "DeviceID,Size,Model"],
            capture_output=True, text=True, shell=True
        )
        for line in result.stdout.splitlines()[1:]:
            parts = line.split()
            if len(parts) >= 2:
                try:
                    size_gb = int(parts[-1]) // (1024**3)
                    device_id = parts[0]
                    model = " ".join(parts[1:-1])
                    devices.append((device_id, f"{size_gb}GB", model))
                except (ValueError, IndexError):
                    pass

    return devices

def select_device():
    """Interactive device selection. Returns device path or None."""
    devices = list_removable_devices()

    if not devices:
        warn("No removable devices detected.")
        print("  Insert your MicroSD card (via USB reader) and try again.")
        manual = input("  Or enter device path manually (e.g. /dev/sdb): ").strip()
        if manual:
            return manual
        return None

    print(f"\n{CYAN}Detected removable devices:{RESET}")
    for i, (path, size, name) in enumerate(devices):
        print(f"  [{i+1}] {path}  {size}  {name}")
    print(f"  [0] Enter path manually")

    try:
        choice = int(input("\nSelect device: ").strip())
        if choice == 0:
            return input("Enter device path: ").strip()
        if 1 <= choice <= len(devices):
            return devices[choice - 1][0]
    except (ValueError, IndexError):
        pass

    error("Invalid selection.")
    return None


# ─────────────────────────────────────────────
#  MBR BYTE-FLIP — GHOST PARTITION STEALTH
#
#  Every MBR partition entry has a 1-byte type field.
#  FAT32 = 0x0B. We change Partition 2 to 0xDA
#  (Non-FS Data) so consumer OS refuses to mount it.
#  The T-Deck mounts by index, not type byte — works fine.
#  The desktop tool flips it back to 0x0B for access,
#  then re-stealth when done.
# ─────────────────────────────────────────────
MBR_TABLE_OFFSET    = 446   # MBR partition table starts at byte 446
PARTITION_ENTRY_SIZE= 16    # Each entry is 16 bytes
TYPE_BYTE_OFFSET    = 4     # Type byte is 4th byte in each entry
FAT32_TYPE          = 0x0B  # Standard FAT32
STEALTH_TYPE        = 0xDA  # Non-FS Data — unrecognized by consumer OS
GHOST_PARTITION_IDX = 2     # Partition 2 is the Ghost Partition (1-based)

def _mbr_type_offset(partition_index):
    """Returns absolute byte offset of the type field for given partition (1-based)."""
    return MBR_TABLE_OFFSET + ((partition_index - 1) * PARTITION_ENTRY_SIZE) + TYPE_BYTE_OFFSET

def _read_partition_type(device, partition_index):
    """Read the current type byte for a partition."""
    offset = _mbr_type_offset(partition_index)
    try:
        with open(device, 'rb') as f:
            f.seek(offset)
            return ord(f.read(1))
    except PermissionError:
        return None

def flip_to_stealth(device, partition_index=GHOST_PARTITION_IDX):
    """
    Set Ghost Partition type byte to 0xDA.
    After this, macOS/Linux/Windows will not auto-mount it.
    The T-Deck is unaffected — SdFat mounts by partition index.
    Requires raw disk write access (admin on macOS/Linux, Administrator on Windows).
    """
    offset = _mbr_type_offset(partition_index)
    try:
        with open(device, 'r+b') as f:
            f.seek(offset)
            current = ord(f.read(1))
            if current == STEALTH_TYPE:
                info(f"Partition {partition_index} already stealthed (0x{STEALTH_TYPE:02X}).")
                return True
            f.seek(offset)
            f.write(bytes([STEALTH_TYPE]))
        info(f"Stealth applied: Partition {partition_index} type 0x{current:02X} → 0x{STEALTH_TYPE:02X}")
        info("Ghost Partition is now invisible to consumer operating systems.")
        return True
    except PermissionError:
        error("Permission denied. Run with sudo (macOS/Linux) or as Administrator (Windows).")
        return False
    except Exception as e:
        error(f"Byte-flip failed: {e}")
        return False

def flip_to_visible(device, partition_index=GHOST_PARTITION_IDX):
    """
    Restore Ghost Partition type byte to 0x0B (FAT32).
    After this, macOS/Linux will mount it normally.
    Call flip_to_stealth() again when done to re-hide.
    """
    offset = _mbr_type_offset(partition_index)
    try:
        with open(device, 'r+b') as f:
            f.seek(offset)
            current = ord(f.read(1))
            if current == FAT32_TYPE:
                info(f"Partition {partition_index} already visible (0x{FAT32_TYPE:02X}).")
                return True
            f.seek(offset)
            f.write(bytes([FAT32_TYPE]))
        info(f"Partition {partition_index} type 0x{current:02X} → 0x{FAT32_TYPE:02X} (FAT32)")
        info("Ghost Partition is now visible. Mount it, export your data, then re-stealth.")
        return True
    except PermissionError:
        error("Permission denied. Run with sudo (macOS/Linux) or as Administrator (Windows).")
        return False
    except Exception as e:
        error(f"Byte-flip failed: {e}")
        return False

def get_raw_device_path(device):
    """
    On macOS, diskutil gives /dev/diskN but raw access needs /dev/rdiskN.
    rdisk bypasses the buffer cache for direct MBR access.
    """
    if PLATFORM == "Darwin" and not device.startswith("/dev/r"):
        return device.replace("/dev/disk", "/dev/rdisk")
    return device

def cmd_stealth():
    """Apply stealth byte-flip to Ghost Partition on a card."""
    banner()
    print(f"{CYAN}=== STEALTH GHOST PARTITION ==={RESET}")
    print()
    print("This changes Partition 2's type byte to 0xDA.")
    print("macOS, Linux, and Windows will stop auto-mounting it.")
    print("The T-Deck is unaffected and will still mount it normally.")
    print()

    device = select_device()
    if not device:
        return

    raw = get_raw_device_path(device)

    # Unmount ghost partition first if mounted
    if PLATFORM == "Darwin":
        subprocess.run(["diskutil", "unmount", f"{device}s2"], capture_output=True)

    current = _read_partition_type(raw, GHOST_PARTITION_IDX)
    if current is not None:
        info(f"Current Partition 2 type: 0x{current:02X}")

    flip_to_stealth(raw)

def cmd_unstealth():
    """Remove stealth byte-flip — restore Ghost Partition visibility on desktop."""
    banner()
    print(f"{CYAN}=== UNLOCK GHOST PARTITION ==={RESET}")
    print()
    print("This restores Partition 2's type byte to 0x0B (FAT32).")
    print("macOS/Linux will mount it. Browse and export your data.")
    print("Run Stealth again when done to re-hide the partition.")
    print()

    device = select_device()
    if not device:
        return

    raw = get_raw_device_path(device)

    current = _read_partition_type(raw, GHOST_PARTITION_IDX)
    if current is not None:
        info(f"Current Partition 2 type: 0x{current:02X}")

    if flip_to_visible(raw):
        if PLATFORM == "Darwin":
            import time
            time.sleep(1)
            subprocess.run(["diskutil", "mount", f"{device}s2"], capture_output=True)
            info("Attempted auto-mount of Ghost Partition.")
            info("Check Finder for PMOON-GHOST volume.")

# ─────────────────────────────────────────────
#  PARTITION SIZE CALCULATOR
# ─────────────────────────────────────────────
def calculate_partitions(total_gb):
    """
    Given total card size in GB, calculate two FAT32 partition sizes.
    FAT32 max volume = 32GB. Both partitions must be <= 32GB.
    Returns (p1_gb, p2_gb) or raises ValueError if card is too small.
    """
    if total_gb < 2:
        raise ValueError(f"Card too small ({total_gb}GB). Minimum 2GB required.")

    # Each partition gets half the card, capped at 32GB
    half = total_gb // 2
    p1 = min(half, 32)
    p2 = min(total_gb - p1, 32)

    if p2 < 1:
        raise ValueError(f"Card ({total_gb}GB) too small for dual partitions.")

    return p1, p2

# ─────────────────────────────────────────────
#  FORMAT — macOS
# ─────────────────────────────────────────────
def format_macos(device, skip_confirm=False):
    """Format card with two FAT32 partitions on macOS using diskutil.

    skip_confirm=True suppresses all input() calls — used by the GUI,
    which handles confirmation via its own dialog before calling this.
    """
    info(f"Target device: {device}")

    # Get disk size
    result = subprocess.run(
        ["diskutil", "info", "-plist", device],
        capture_output=True, text=True
    )
    # Parse size from diskutil info
    result_text = subprocess.run(
        ["diskutil", "info", device], capture_output=True, text=True
    ).stdout
    total_gb = 0
    for line in result_text.splitlines():
        if "Disk Size" in line and "GB" in line:
            try:
                total_gb = int(float(line.split("(")[0].split()[-2]))
                break
            except (ValueError, IndexError):
                pass
        elif "Disk Size" in line and "MB" in line:
            try:
                total_gb = max(1, int(float(line.split("(")[0].split()[-2])) // 1024)
                break
            except (ValueError, IndexError):
                pass

    if total_gb == 0:
        if skip_confirm:
            error("Could not detect card size and running in GUI mode — aborting.")
            return False
        total_gb = int(input("Could not detect card size. Enter size in GB: ").strip())

    p1_gb, p2_gb = calculate_partitions(total_gb)

    print(f"\n{YELLOW}About to format {device} ({total_gb}GB){RESET}")
    print(f"  Partition 1 (PUBLIC):  {p1_gb}GB FAT32  — label: PMOON-PUB")
    print(f"  Partition 2 (GHOST):   {p2_gb}GB FAT32  — label: PMOON-GHOST")
    print(f"\n{RED}WARNING: ALL DATA ON {device} WILL BE ERASED.{RESET}")

    if not skip_confirm:
        confirm = input("Type 'FORMAT' to confirm: ").strip()
        if confirm != "FORMAT":
            warn("Cancelled.")
            return False

    step("Unmounting disk...")
    subprocess.run(["diskutil", "unmountDisk", device], check=True)

    step(f"Partitioning {device} with MBR + 2x FAT32...")
    result = subprocess.run([
        "diskutil", "partitionDisk", device, "MBR",
        "FAT32", "PMOON-PUB",   f"{p1_gb}G",
        "FAT32", "PMOON-GHOST", f"{p2_gb}G"
    ], capture_output=True, text=True)

    if result.returncode != 0:
        error("diskutil partitionDisk failed:")
        print(result.stderr)
        return False

    info("Partitioning complete.")
    create_directory_structure_macos(device)

    # Apply stealth byte-flip after format
    step("Applying Ghost Partition stealth byte-flip...")
    import time
    time.sleep(2)  # Let partitions settle
    raw = get_raw_device_path(device)
    if flip_to_stealth(raw):
        info("Ghost Partition is now invisible to consumer OS.")
        info("The T-Deck will still mount it correctly.")
    else:
        warn("Stealth byte-flip failed — Ghost Partition visible on desktop.")
        warn("Run 'Stealth Ghost Partition' from the menu to apply manually.")
    return True

def create_directory_structure_macos(device):
    """Create Pisces Moon directory structure on both partitions."""
    import time
    time.sleep(2)  # Let macOS mount the new partitions

    pub_mount   = f"/Volumes/PMOON-PUB"
    ghost_mount = f"/Volumes/PMOON-GHOST"

    if os.path.exists(pub_mount):
        step("Creating Public Partition directory structure...")
        for d in ["/data/medical", "/data/baseball", "/data/trails",
                  "/music", "/logs", "/recordings"]:
            os.makedirs(pub_mount + d, exist_ok=True)
        # Write a readme so the partition isn't completely empty
        with open(pub_mount + "/README.txt", "w") as f:
            f.write("Pisces Moon OS — Public Partition\n")
            f.write("Medical, baseball, and trail reference data lives here.\n")
            f.write("This partition is visible on all operating systems.\n")
        info(f"Public partition ready at {pub_mount}")
    else:
        warn(f"Public partition not auto-mounted at {pub_mount}")

    if os.path.exists(ghost_mount):
        step("Creating Ghost Partition directory structure...")
        for d in ["/wardrive", "/data/gemini", "/vault", "/scans",
                  "/cyber_logs", "/notes"]:
            os.makedirs(ghost_mount + d, exist_ok=True)
        info(f"Ghost partition ready at {ghost_mount}")
    else:
        warn(f"Ghost partition not auto-mounted at {ghost_mount}")
        info("You can create the directory structure manually after mounting.")

# ─────────────────────────────────────────────
#  FORMAT — Linux
# ─────────────────────────────────────────────
def format_linux(device, skip_confirm=False):
    """Format card with two FAT32 partitions on Linux using parted + mkfs.fat.

    skip_confirm=True suppresses all input() calls — used by the GUI,
    which handles confirmation via its own dialog before calling this.
    """
    info(f"Target device: {device}")

    # Get disk size via lsblk
    result = subprocess.run(
        ["lsblk", "-b", "-d", "-o", "SIZE", "--noheadings", device],
        capture_output=True, text=True
    )
    try:
        total_bytes = int(result.stdout.strip())
        total_gb = total_bytes // (1024**3)
    except ValueError:
        if skip_confirm:
            error("Could not detect card size and running in GUI mode — aborting.")
            return False
        total_gb = int(input("Could not detect size. Enter card size in GB: ").strip())

    p1_gb, p2_gb = calculate_partitions(total_gb)
    p1_end = p1_gb * 1024  # in MiB

    print(f"\n{YELLOW}About to format {device} ({total_gb}GB){RESET}")
    print(f"  Partition 1 (PUBLIC):  {p1_gb}GB FAT32")
    print(f"  Partition 2 (GHOST):   {p2_gb}GB FAT32")
    print(f"\n{RED}WARNING: ALL DATA ON {device} WILL BE ERASED.{RESET}")
    print(f"{RED}Ensure you have unmounted all partitions first.{RESET}")

    if not skip_confirm:
        confirm = input("Type 'FORMAT' to confirm: ").strip()
        if confirm != "FORMAT":
            warn("Cancelled.")
            return False

    # Unmount any mounted partitions
    step("Unmounting partitions...")
    for i in [1, 2]:
        subprocess.run(["umount", f"{device}{i}"], capture_output=True)
        subprocess.run(["umount", f"{device}p{i}"], capture_output=True)

    step("Creating MBR partition table...")
    subprocess.run(["parted", "-s", device, "mklabel", "msdos"], check=True)

    step(f"Creating partition 1 (0 — {p1_end}MiB)...")
    subprocess.run([
        "parted", "-s", device, "mkpart", "primary", "fat32",
        "1MiB", f"{p1_end}MiB"
    ], check=True)

    step(f"Creating partition 2 ({p1_end}MiB — end)...")
    subprocess.run([
        "parted", "-s", device, "mkpart", "primary", "fat32",
        f"{p1_end}MiB", "100%"
    ], check=True)

    # Determine partition device names (sdb1/sdb2 or sdb1/sdbp2)
    p1 = f"{device}1" if os.path.exists(f"{device}1") else f"{device}p1"
    p2 = f"{device}2" if os.path.exists(f"{device}2") else f"{device}p2"

    import time
    time.sleep(1)
    # Re-read partition table
    subprocess.run(["partprobe", device], capture_output=True)
    time.sleep(1)

    step(f"Formatting partition 1 as FAT32 (PMOON-PUB)...")
    subprocess.run(["mkfs.fat", "-F", "32", "-n", "PMOON-PUB", p1], check=True)

    step(f"Formatting partition 2 as FAT32 (PMOON-GHOST)...")
    subprocess.run(["mkfs.fat", "-F", "32", "-n", "PMOON-GHOST", p2], check=True)

    info("Format complete. Mount partitions manually to create directory structure:")
    print(f"  sudo mount {p1} /mnt/pmoon-pub && sudo mount {p2} /mnt/pmoon-ghost")

    return True

# ─────────────────────────────────────────────
#  FORMAT — Windows
# ─────────────────────────────────────────────
def format_windows(device, skip_confirm=False, disk_num_hint=None, total_gb_hint=0):
    """Format card using diskpart on Windows. Requires Administrator.

    skip_confirm=True suppresses all input() calls — used by the GUI,
    which handles confirmation via its own dialog before calling this.
    disk_num_hint: disk number string (e.g. "1") — GUI passes this from device path.
    total_gb_hint: card size in GB — GUI passes this from device detection.
    """
    warn("Windows format requires Administrator privileges.")
    warn("diskpart will be used — double-check your disk number.")

    import re as _re

    # Resolve disk number — use hint from GUI or extract from device path or prompt
    if disk_num_hint:
        disk_num = str(disk_num_hint)
    else:
        m = _re.search(r'(\d+)$', str(device))
        if m:
            disk_num = m.group(1)
        elif skip_confirm:
            error("Could not determine disk number from device path — aborting.")
            return False
        else:
            step("Listing available disks via diskpart...")
            result = subprocess.run(
                ["diskpart"],
                input="list disk\nexit\n",
                capture_output=True, text=True, shell=True
            )
            print(result.stdout)
            disk_num = input("Enter the disk NUMBER for your SD card (e.g. 1): ").strip()

    # Resolve card size — use hint from GUI or detect via wmic or prompt
    total_gb = total_gb_hint
    if total_gb == 0:
        try:
            wmic_result = subprocess.run(
                ["wmic", "diskdrive", "where", f"Index={disk_num}", "get", "Size"],
                capture_output=True, text=True, shell=True
            )
            for line in wmic_result.stdout.splitlines():
                line = line.strip()
                if line.isdigit():
                    total_gb = int(line) // (1024 ** 3)
                    break
        except Exception:
            pass

    if total_gb == 0:
        if skip_confirm:
            error("Could not detect card size and running in GUI mode — aborting.")
            return False
        try:
            total_gb = int(input("Enter card size in GB: ").strip())
        except ValueError:
            error("Invalid size.")
            return False

    p1_gb, p2_gb = calculate_partitions(total_gb)

    print(f"\n{YELLOW}About to format Disk {disk_num} ({total_gb}GB){RESET}")
    print(f"  Partition 1 (PUBLIC):  {p1_gb}GB FAT32")
    print(f"  Partition 2 (GHOST):   {p2_gb}GB FAT32")
    print(f"\n{RED}WARNING: ALL DATA ON DISK {disk_num} WILL BE ERASED.{RESET}")

    if not skip_confirm:
        confirm = input("Type 'FORMAT' to confirm: ").strip()
        if confirm != "FORMAT":
            warn("Cancelled.")
            return False

    diskpart_script = f"""select disk {disk_num}
clean
create partition primary size={p1_gb * 1024}
format fs=fat32 label="PMOON-PUB" quick
assign
create partition primary size={p2_gb * 1024}
format fs=fat32 label="PMOON-GHOST" quick
assign
exit
"""
    script_path = os.path.join(os.environ.get("TEMP", "."), "pmoon_format.txt")
    with open(script_path, "w") as f:
        f.write(diskpart_script)

    step("Running diskpart...")
    result = subprocess.run(
        ["diskpart", "/s", script_path],
        capture_output=True, text=True, shell=True
    )
    os.unlink(script_path)

    if result.returncode != 0:
        error("diskpart failed:")
        print(result.stderr)
        return False

    info("Format complete.")
    info("Note: Windows may only show the first partition (PMOON-PUB) in Explorer.")
    info("This is expected — use Pisces Moon Linux or this tool to access PMOON-GHOST.")
    return True

# ─────────────────────────────────────────────
#  FORMAT — MAIN DISPATCHER
# ─────────────────────────────────────────────
def cmd_format():
    """Main format command — dispatches to platform-specific handler."""
    banner()
    print(f"{CYAN}=== FORMAT SD CARD FOR GHOST PARTITION ==={RESET}\n")

    check_dependencies()

    print("RECOMMENDED CARD SIZES:")
    print("  16GB → two 8GB partitions  (ideal)")
    print("  32GB → two 16GB partitions (good)")
    print("  64GB → two 32GB partitions (supported, max per FAT32 spec)\n")
    print("Cards larger than 64GB are supported but partitions are capped at 32GB.")
    print("Remaining space on larger cards is unused by Pisces Moon OS.\n")

    device = select_device()
    if not device:
        return

    if PLATFORM == "Darwin":
        format_macos(device)
    elif PLATFORM == "Linux":
        format_linux(device)
    elif PLATFORM == "Windows":
        format_windows(device)
    else:
        error(f"Unsupported platform: {PLATFORM}")

# ─────────────────────────────────────────────
#  VERIFY — Check card layout
# ─────────────────────────────────────────────
def cmd_verify():
    """Verify that an SD card has the correct Pisces Moon partition layout."""
    banner()
    print(f"{CYAN}=== VERIFY CARD LAYOUT ==={RESET}\n")

    device = select_device()
    if not device:
        return

    print()
    if PLATFORM == "Darwin":
        result = subprocess.run(
            ["diskutil", "list", device], capture_output=True, text=True
        )
        print(result.stdout)
        if "PMOON-PUB" in result.stdout and "PMOON-GHOST" in result.stdout:
            info("Card layout verified — both Pisces Moon partitions found.")
        elif "PMOON-PUB" in result.stdout:
            warn("Only PUBLIC partition found — Ghost Partition not yet created.")
        else:
            warn("No Pisces Moon partitions detected on this card.")
            info("Use --format to prepare this card for Pisces Moon OS.")

    elif PLATFORM == "Linux":
        result = subprocess.run(
            ["lsblk", "-o", "NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT", device],
            capture_output=True, text=True
        )
        print(result.stdout)
        if "PMOON-GHOST" in result.stdout:
            info("Card layout verified — Ghost Partition detected.")
        elif "PMOON-PUB" in result.stdout:
            warn("Only PUBLIC partition found — Ghost Partition not yet created.")
        else:
            warn("No Pisces Moon partitions detected.")

    elif PLATFORM == "Windows":
        result = subprocess.run(
            ["diskpart"],
            input=f"list volume\nexit\n",
            capture_output=True, text=True, shell=True
        )
        print(result.stdout)
        info("Look for volumes labeled PMOON-PUB and PMOON-GHOST in the list above.")

# ─────────────────────────────────────────────
#  BROWSE — Browse Ghost Partition contents
# ─────────────────────────────────────────────
def find_ghost_partition():
    """Try to find the mounted Ghost Partition path."""
    candidates = []

    if PLATFORM == "Darwin":
        candidates = ["/Volumes/PMOON-GHOST"]
    elif PLATFORM == "Linux":
        candidates = [
            "/media/PMOON-GHOST",
            "/mnt/pmoon-ghost",
            "/run/media/" + os.environ.get("USER", "") + "/PMOON-GHOST"
        ]
    elif PLATFORM == "Windows":
        # Check all drive letters for PMOON-GHOST label
        import string
        for letter in string.ascii_uppercase:
            drive = f"{letter}:\\"
            if os.path.exists(drive):
                try:
                    result = subprocess.run(
                        ["vol", drive], capture_output=True, text=True, shell=True
                    )
                    if "PMOON-GHOST" in result.stdout:
                        candidates.insert(0, drive)
                except Exception:
                    pass

    for c in candidates:
        if os.path.exists(c):
            return c
    return None

def cmd_browse():
    """Browse Ghost Partition contents — disaster recovery mode."""
    banner()
    print(f"{CYAN}=== BROWSE GHOST PARTITION ==={RESET}\n")

    ghost_path = find_ghost_partition()

    if not ghost_path:
        warn("Ghost Partition not auto-detected.")
        ghost_path = input("Enter Ghost Partition mount path manually: ").strip()
        if not ghost_path or not os.path.exists(ghost_path):
            error("Path not found.")
            return

    info(f"Ghost Partition found at: {ghost_path}")
    print()

    # Walk and display contents
    total_files = 0
    total_bytes = 0
    for root, dirs, files in os.walk(ghost_path):
        rel = os.path.relpath(root, ghost_path)
        if rel == ".":
            rel = "/"
        level = rel.count(os.sep)
        indent = "  " * level
        print(f"{CYAN}{indent}{os.path.basename(root)}/{RESET}")
        subindent = "  " * (level + 1)
        for f in files:
            fp = os.path.join(root, f)
            size = os.path.getsize(fp)
            total_bytes += size
            total_files += 1
            size_str = f"{size/1024:.1f}KB" if size < 1024*1024 else f"{size/1024/1024:.1f}MB"
            print(f"{subindent}{f}  {DIM}({size_str}){RESET}")

    print()
    info(f"Total: {total_files} files, {total_bytes/1024:.1f}KB")

# ─────────────────────────────────────────────
#  EXPORT — Copy Ghost Partition data to desktop
# ─────────────────────────────────────────────
def cmd_export(dest_dir=None):
    """Export Ghost Partition contents to a local directory."""
    banner()
    print(f"{CYAN}=== EXPORT GHOST PARTITION DATA ==={RESET}\n")

    ghost_path = find_ghost_partition()
    if not ghost_path:
        warn("Ghost Partition not auto-detected.")
        ghost_path = input("Enter Ghost Partition mount path: ").strip()
        if not ghost_path or not os.path.exists(ghost_path):
            error("Path not found.")
            return

    if not dest_dir:
        default = os.path.join(os.path.expanduser("~"), "PiscesMoonExport")
        dest_dir = input(f"Export destination [{default}]: ").strip() or default

    dest = Path(dest_dir)
    dest.mkdir(parents=True, exist_ok=True)

    info(f"Exporting from {ghost_path} → {dest}")

    copied = 0
    for root, dirs, files in os.walk(ghost_path):
        rel_root = os.path.relpath(root, ghost_path)
        dest_root = dest / rel_root
        dest_root.mkdir(parents=True, exist_ok=True)
        for f in files:
            src = os.path.join(root, f)
            dst = dest_root / f
            shutil.copy2(src, dst)
            copied += 1
            print(f"  {GREEN}✓{RESET} {os.path.join(rel_root, f)}")

    print()
    info(f"Export complete — {copied} files copied to {dest}")
    info("Wardrive CSVs can be opened in Excel or imported to WiGLE.")
    info("Beacon/scan JSON files can be opened in any JSON viewer.")

# ─────────────────────────────────────────────
#  INTERACTIVE MENU
# ─────────────────────────────────────────────
def interactive_menu():
    banner()
    while True:
        print(f"{CYAN}Main Menu:{RESET}")
        print("  [1] Format SD card for Ghost Partition")
        print("  [2] Verify card layout")
        print("  [3] Browse Ghost Partition contents")
        print("  [4] Export Ghost Partition data to computer")
        print("  [5] Check dependencies")
        print(f"  {CYAN}[6] Unlock Ghost Partition (show on desktop){RESET}")
        print(f"  {CYAN}[7] Stealth Ghost Partition (hide from desktop){RESET}")
        print("  [Q] Quit")
        print()

        choice = input("Choice: ").strip().upper()
        print()

        if choice == "1":
            cmd_format()
        elif choice == "2":
            cmd_verify()
        elif choice == "3":
            cmd_browse()
        elif choice == "4":
            cmd_export()
        elif choice == "5":
            ok = check_dependencies()
            if ok:
                info("All dependencies satisfied.")
            print()
        elif choice == "6":
            cmd_unstealth()
        elif choice == "7":
            cmd_stealth()
        elif choice == "Q":
            print("Reticulating splines... done.")
            break
        else:
            warn("Invalid choice.")
        print()

# ─────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Pisces Moon OS — Ghost Partition Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument("--format",  action="store_true", help="Format SD card")
    parser.add_argument("--verify",  action="store_true", help="Verify card layout")
    parser.add_argument("--browse",  action="store_true", help="Browse Ghost Partition")
    parser.add_argument("--export",  metavar="DIR",       help="Export Ghost data to DIR")
    parser.add_argument("--stealth", action="store_true", help="Apply stealth byte-flip to Ghost Partition")
    parser.add_argument("--unstealth", action="store_true", help="Restore Ghost Partition visibility on desktop")

    args = parser.parse_args()

    if args.format:
        cmd_format()
    elif args.verify:
        cmd_verify()
    elif args.browse:
        cmd_browse()
    elif args.export:
        cmd_export(args.export)
    elif args.stealth:
        cmd_stealth()
    elif args.unstealth:
        cmd_unstealth()
    else:
        interactive_menu()
