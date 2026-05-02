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
╔══════════════════════════════════════════════════════════════════╗
║       PISCES MOON OS — WARDRIVE CSV SPLITTER                     ║
║       Splits wardrive data into Smelter-ready chunks             ║
╠══════════════════════════════════════════════════════════════════╣
║                                                                  ║
║  Spadra Smelter handles ~300K rows comfortably.                  ║
║  This tool breaks your monolithic wardrive.csv into              ║
║  chunks that Smelter can actually load without hanging.          ║
║                                                                  ║
║  Split modes:                                                    ║
║    --rows N      Split every N rows (default: 50000)             ║
║    --date        One file per unique drive date                  ║
║    --session     One file per wardrive session (by time gap)     ║
║    --geo         Split by geographic bounding box                ║
║    --filter      Filter/deduplicate without splitting            ║
║                                                                  ║
║  All modes:                                                      ║
║    - Preserve the WiGLE CSV header in every output file          ║
║    - Deduplicate by MAC+location by default (--no-dedup to skip) ║
║    - Strip BLE entries with --wifi-only                          ║
║    - Strip WiFi entries with --ble-only                          ║
║    - Filter weak signals with --min-rssi N (e.g. --min-rssi -80) ║
║    - Print stats for each output file                            ║
║    - Never modifies the source file                              ║
║                                                                  ║
║  Output:                                                         ║
║    ./smelter_output/wardrive_chunk_001.csv                       ║
║    ./smelter_output/wardrive_chunk_002.csv  ...etc               ║
║                                                                  ║
║  "The network is a resource. The intelligence is yours."         ║
╚══════════════════════════════════════════════════════════════════╝

Usage:
    python3 wardrive_splitter.py wardrive.csv
    python3 wardrive_splitter.py wardrive.csv --rows 50000
    python3 wardrive_splitter.py wardrive.csv --date
    python3 wardrive_splitter.py wardrive.csv --session --session-gap 60
    python3 wardrive_splitter.py wardrive.csv --geo --geo-size 0.1
    python3 wardrive_splitter.py wardrive.csv --filter --wifi-only --min-rssi -80
    python3 wardrive_splitter.py wardrive.csv --date --wifi-only --min-rssi -75
"""

import sys
import os
import csv
import argparse
import math
from pathlib import Path
from datetime import datetime, timedelta
from collections import defaultdict

# ─────────────────────────────────────────────
#  TERMINAL COLORS
# ─────────────────────────────────────────────
CYAN   = "\033[96m"
GREEN  = "\033[92m"
YELLOW = "\033[93m"
RED    = "\033[91m"
DIM    = "\033[2m"
BOLD   = "\033[1m"
RESET  = "\033[0m"

if sys.platform == "win32" and "WT_SESSION" not in os.environ:
    CYAN = GREEN = YELLOW = RED = DIM = BOLD = RESET = ""

def banner():
    print(f"""
{CYAN}{BOLD}╔══════════════════════════════════════════════════╗
║    🌙 PISCES MOON — WARDRIVE CSV SPLITTER       ║
║       Smelter-ready chunk generator             ║
╚══════════════════════════════════════════════════╝{RESET}
{DIM}Spadra Smelter handles ~300K rows. Feed it sensibly.{RESET}
""")

def info(msg):  print(f"{GREEN}[INFO]{RESET} {msg}")
def warn(msg):  print(f"{YELLOW}[WARN]{RESET} {msg}")
def error(msg): print(f"{RED}[FAIL]{RESET} {msg}")
def step(msg):  print(f"{CYAN}[....]{RESET} {msg}")
def ok(msg):    print(f"{GREEN}[ OK ]{RESET} {msg}")

# ─────────────────────────────────────────────
#  WIGLE CSV HEADER
#  Pisces Moon writes this exact header.
#  Smelter requires these columns in this order.
# ─────────────────────────────────────────────
WIGLE_HEADER = [
    "MAC", "SSID", "AuthMode", "FirstSeen", "Channel",
    "RSSI", "CurrentLatitude", "CurrentLongitude",
    "AltitudeMeters", "AccuracyMeters", "Type"
]

# ─────────────────────────────────────────────
#  CSV LOADER
#  Reads the source file, validates columns,
#  returns list of row dicts.
# ─────────────────────────────────────────────
def load_csv(path):
    step(f"Loading {path} ...")
    rows = []
    skipped = 0

    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        # Skip WiGLE app header lines (start with #) if present
        first = f.readline().strip()
        if first.startswith('#'):
            # WiGLE app format has two header lines — skip both
            f.readline()
        elif first.startswith('MAC'):
            # Already at our header line — rewind
            f.seek(0)
        else:
            # Unknown format — rewind and hope for the best
            f.seek(0)

        reader = csv.DictReader(f)

        # Check columns
        if reader.fieldnames:
            missing = [c for c in WIGLE_HEADER if c not in reader.fieldnames]
            if missing:
                error(f"Missing required columns: {missing}")
                error("Expected WiGLE-format CSV from Pisces Moon wardrive.")
                sys.exit(1)

        for row in reader:
            try:
                # Basic validation — skip rows with no MAC or garbage coords
                mac = row.get('MAC', '').strip()
                lat = float(row.get('CurrentLatitude', 0) or 0)
                lng = float(row.get('CurrentLongitude', 0) or 0)

                if not mac:
                    skipped += 1; continue

                # Skip pre-GPS-fix coordinates (0,0 or near-null island)
                if abs(lat) < 0.01 and abs(lng) < 0.01:
                    skipped += 1; continue

                rows.append(row)
            except (ValueError, KeyError):
                skipped += 1
                continue

    info(f"Loaded {len(rows):,} valid rows ({skipped:,} skipped)")
    return rows

# ─────────────────────────────────────────────
#  FILTERS
# ─────────────────────────────────────────────
def apply_filters(rows, wifi_only=False, ble_only=False,
                  min_rssi=None, no_dedup=False):
    original = len(rows)

    # Type filter
    if wifi_only:
        rows = [r for r in rows if r.get('Type','').upper() == 'WIFI']
        info(f"  WiFi-only filter: {len(rows):,} rows remain")
    elif ble_only:
        rows = [r for r in rows if r.get('Type','').upper() == 'BT-LE']
        info(f"  BLE-only filter: {len(rows):,} rows remain")

    # Signal strength filter
    if min_rssi is not None:
        before = len(rows)
        filtered = []
        for r in rows:
            try:
                rssi = int(r.get('RSSI', -999) or -999)
                if rssi >= min_rssi:
                    filtered.append(r)
            except (ValueError, TypeError):
                filtered.append(r)  # Keep rows with unparseable RSSI
        rows = filtered
        info(f"  RSSI >= {min_rssi} filter: {len(rows):,} rows remain "
             f"({before - len(rows):,} weak signals dropped)")

    # Deduplication — keep first occurrence of each MAC+lat+lng combo
    # This collapses repeated scans of the same AP from the same location
    # without losing APs seen at multiple locations (which Smelter uses
    # for persistence scoring)
    if not no_dedup:
        before = len(rows)
        seen = set()
        deduped = []
        for r in rows:
            try:
                lat = round(float(r.get('CurrentLatitude', 0) or 0), 4)
                lng = round(round(float(r.get('CurrentLongitude', 0) or 0), 4), 4)
                key = (r.get('MAC','').upper(), lat, lng)
                if key not in seen:
                    seen.add(key)
                    deduped.append(r)
            except (ValueError, TypeError):
                deduped.append(r)
        rows = deduped
        info(f"  Dedup (MAC+location): {len(rows):,} rows remain "
             f"({before - len(rows):,} duplicates removed)")

    return rows

# ─────────────────────────────────────────────
#  OUTPUT WRITER
# ─────────────────────────────────────────────
def write_chunk(rows, out_path, label=""):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=WIGLE_HEADER, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(rows)

    size_kb = os.path.getsize(out_path) / 1024
    wifi_count = sum(1 for r in rows if r.get('Type','').upper() == 'WIFI')
    ble_count  = sum(1 for r in rows if r.get('Type','').upper() == 'BT-LE')
    unique_mac = len(set(r.get('MAC','') for r in rows))

    tag = f" [{label}]" if label else ""
    ok(f"{os.path.basename(out_path)}{tag}")
    print(f"       {len(rows):>8,} rows  |  "
          f"{wifi_count:,} WiFi  {ble_count:,} BLE  |  "
          f"{unique_mac:,} unique MACs  |  "
          f"{size_kb:.1f} KB")

    return len(rows)

# ─────────────────────────────────────────────
#  SPLIT MODES
# ─────────────────────────────────────────────

def split_by_rows(rows, out_dir, chunk_size, base_name):
    """Split into fixed-size chunks by row count."""
    step(f"Splitting into chunks of {chunk_size:,} rows...")
    chunks = [rows[i:i+chunk_size] for i in range(0, len(rows), chunk_size)]
    info(f"  {len(chunks)} chunks to write")
    print()

    total_written = 0
    for i, chunk in enumerate(chunks, 1):
        fname = os.path.join(out_dir, f"{base_name}_{i:03d}.csv")
        total_written += write_chunk(chunk, fname)

    return len(chunks), total_written


def split_by_date(rows, out_dir, base_name):
    """One output file per unique date in FirstSeen column."""
    step("Splitting by drive date (FirstSeen column)...")

    date_buckets = defaultdict(list)
    unparseable = []

    for row in rows:
        seen = row.get('FirstSeen', '').strip()
        date_str = None
        # Handle both "YYYY-MM-DD HH:MM:SS" and "YYYY-MM-DD HH:MM:SS.sss"
        for fmt in ('%Y-%m-%d %H:%M:%S', '%Y-%m-%dT%H:%M:%S',
                    '%Y-%m-%d %H:%M:%S.%f', '%m/%d/%Y %H:%M:%S'):
            try:
                dt = datetime.strptime(seen[:19], fmt[:len(fmt)])
                date_str = dt.strftime('%Y-%m-%d')
                break
            except ValueError:
                continue
        if date_str:
            date_buckets[date_str].append(row)
        else:
            unparseable.append(row)

    dates = sorted(date_buckets.keys())
    info(f"  {len(dates)} unique dates found")
    if unparseable:
        warn(f"  {len(unparseable):,} rows had unparseable dates — written to _unknown_date.csv")
    print()

    total_written = 0
    for date in dates:
        fname = os.path.join(out_dir, f"{base_name}_{date}.csv")
        total_written += write_chunk(date_buckets[date], fname, label=date)

    if unparseable:
        fname = os.path.join(out_dir, f"{base_name}_unknown_date.csv")
        write_chunk(unparseable, fname, label="unknown date")

    return len(dates) + (1 if unparseable else 0), total_written


def split_by_session(rows, out_dir, base_name, gap_minutes=60):
    """
    Split by driving session — a new session starts when there's a gap
    of more than gap_minutes between consecutive timestamps.
    Good for finding natural "I went out wardiving today" boundaries.
    """
    step(f"Splitting by session (gap threshold: {gap_minutes} min)...")

    # Sort by FirstSeen
    def parse_ts(row):
        seen = row.get('FirstSeen', '').strip()
        for fmt in ('%Y-%m-%d %H:%M:%S', '%Y-%m-%dT%H:%M:%S', '%m/%d/%Y %H:%M:%S'):
            try:
                return datetime.strptime(seen[:19], fmt[:len(fmt)])
            except ValueError:
                continue
        return datetime.min

    rows_sorted = sorted(rows, key=parse_ts)
    gap = timedelta(minutes=gap_minutes)

    sessions = []
    current_session = []
    last_ts = None

    for row in rows_sorted:
        ts = parse_ts(row)
        if last_ts is None or (ts != datetime.min and ts - last_ts > gap):
            if current_session:
                sessions.append(current_session)
            current_session = [row]
        else:
            current_session.append(row)
        if ts != datetime.min:
            last_ts = ts

    if current_session:
        sessions.append(current_session)

    info(f"  {len(sessions)} sessions detected")
    print()

    total_written = 0
    for i, session in enumerate(sessions, 1):
        # Use the date of the first row as the label
        first_ts = parse_ts(session[0])
        label = first_ts.strftime('%Y-%m-%d') if first_ts != datetime.min else f"session_{i:03d}"
        fname = os.path.join(out_dir, f"{base_name}_session_{i:03d}.csv")
        total_written += write_chunk(session, fname, label=label)

    return len(sessions), total_written


def split_by_geo(rows, out_dir, base_name, cell_size=0.1):
    """
    Split by geographic grid cell.
    cell_size is in decimal degrees (~11km at equator for 0.1).
    Each output file covers one grid square — useful for
    neighborhood-level Smelter analysis.
    """
    step(f"Splitting by geography (grid cell: {cell_size}° ≈ "
         f"{cell_size * 111:.0f}km)...")

    geo_buckets = defaultdict(list)
    no_gps = []

    for row in rows:
        try:
            lat = float(row.get('CurrentLatitude', 0) or 0)
            lng = float(row.get('CurrentLongitude', 0) or 0)
            if abs(lat) < 0.01 and abs(lng) < 0.01:
                no_gps.append(row); continue
            # Snap to grid
            cell_lat = math.floor(lat / cell_size) * cell_size
            cell_lng = math.floor(lng / cell_size) * cell_size
            cell_key = (round(cell_lat, 6), round(cell_lng, 6))
            geo_buckets[cell_key].append(row)
        except (ValueError, TypeError):
            no_gps.append(row)

    cells = sorted(geo_buckets.keys())
    info(f"  {len(cells)} geographic cells populated")
    if no_gps:
        warn(f"  {len(no_gps):,} rows had no GPS — written to _no_gps.csv")
    print()

    total_written = 0
    for i, cell in enumerate(cells, 1):
        lat, lng = cell
        label = f"{lat:+.4f},{lng:+.4f}"
        fname = os.path.join(out_dir, f"{base_name}_geo_{i:03d}.csv")
        total_written += write_chunk(geo_buckets[cell], fname, label=label)

    if no_gps:
        fname = os.path.join(out_dir, f"{base_name}_no_gps.csv")
        write_chunk(no_gps, fname, label="no GPS")

    return len(cells) + (1 if no_gps else 0), total_written


def filter_only(rows, out_dir, base_name):
    """Write a single cleaned/filtered output file without splitting."""
    step("Writing filtered output (no split)...")
    print()
    fname = os.path.join(out_dir, f"{base_name}_filtered.csv")
    count = write_chunk(rows, fname, label="filtered")
    return 1, count

# ─────────────────────────────────────────────
#  STATS SUMMARY
# ─────────────────────────────────────────────
def print_summary(source_path, out_dir, num_files, total_rows,
                  source_rows, elapsed_s):
    print()
    print(f"{CYAN}{'─' * 52}{RESET}")
    print(f"{BOLD}  SUMMARY{RESET}")
    print(f"{'─' * 52}")
    source_mb = os.path.getsize(source_path) / (1024 * 1024)
    print(f"  Source:       {os.path.basename(source_path)} "
          f"({source_mb:.1f} MB, {source_rows:,} rows after filter)")
    print(f"  Output files: {num_files}")
    print(f"  Total rows:   {total_rows:,}")
    print(f"  Output dir:   {out_dir}/")
    print(f"  Time:         {elapsed_s:.1f}s")
    print(f"{'─' * 52}")
    print(f"  {GREEN}Drop any output file straight into Spadra Smelter.{RESET}")
    print(f"  {DIM}Source file untouched at: {source_path}{RESET}")
    print()

# ─────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────
def main():
    banner()

    parser = argparse.ArgumentParser(
        description="Pisces Moon wardrive CSV splitter for Spadra Smelter",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )

    parser.add_argument("input", help="Source wardrive CSV file (wardrive.csv or wardrive_NNNN.csv)")
    parser.add_argument("-o", "--output-dir", default="smelter_output",
                        help="Output directory (default: ./smelter_output)")
    parser.add_argument("--name", default=None,
                        help="Base name for output files (default: derived from input filename)")

    # Split modes — mutually exclusive
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--rows", type=int, default=None, metavar="N",
                             help="Split every N rows (default mode if nothing else specified: 50000)")
    mode_group.add_argument("--date", action="store_true",
                             help="One file per unique drive date")
    mode_group.add_argument("--session", action="store_true",
                             help="One file per wardrive session (split on time gap)")
    mode_group.add_argument("--geo", action="store_true",
                             help="Split by geographic grid cell")
    mode_group.add_argument("--filter", action="store_true",
                             help="Filter/deduplicate only — no split")

    # Session options
    parser.add_argument("--session-gap", type=int, default=60, metavar="MINUTES",
                        help="Minutes of silence that marks a new session (default: 60)")

    # Geo options
    parser.add_argument("--geo-size", type=float, default=0.1, metavar="DEGREES",
                        help="Grid cell size in decimal degrees (default: 0.1 ≈ 11km)")

    # Filter options
    parser.add_argument("--wifi-only", action="store_true",
                        help="Exclude BLE entries from output")
    parser.add_argument("--ble-only", action="store_true",
                        help="Exclude WiFi entries from output")
    parser.add_argument("--min-rssi", type=int, default=None, metavar="RSSI",
                        help="Drop entries weaker than RSSI (e.g. --min-rssi -80)")
    parser.add_argument("--no-dedup", action="store_true",
                        help="Skip deduplication (keep all rows including exact duplicates)")

    args = parser.parse_args()

    # Validate input
    if not os.path.exists(args.input):
        error(f"File not found: {args.input}")
        sys.exit(1)

    if args.wifi_only and args.ble_only:
        error("Cannot use --wifi-only and --ble-only together.")
        sys.exit(1)

    # Derive base name from input filename
    base_name = args.name or Path(args.input).stem

    import time
    t0 = time.time()

    # Load
    rows = load_csv(args.input)
    source_rows_raw = len(rows)
    print()

    # Apply filters
    info("Applying filters...")
    rows = apply_filters(
        rows,
        wifi_only=args.wifi_only,
        ble_only=args.ble_only,
        min_rssi=args.min_rssi,
        no_dedup=args.no_dedup
    )
    print()

    if not rows:
        warn("No rows remain after filtering. Nothing to write.")
        sys.exit(0)

    # Default mode if nothing specified
    if not any([args.rows, args.date, args.session, args.geo, args.filter]):
        args.rows = 50000

    # Split
    out_dir = args.output_dir

    if args.date:
        num_files, total_written = split_by_date(rows, out_dir, base_name)
    elif args.session:
        num_files, total_written = split_by_session(
            rows, out_dir, base_name, gap_minutes=args.session_gap)
    elif args.geo:
        num_files, total_written = split_by_geo(
            rows, out_dir, base_name, cell_size=args.geo_size)
    elif args.filter:
        num_files, total_written = filter_only(rows, out_dir, base_name)
    else:
        # --rows mode (default)
        chunk_size = args.rows or 50000
        num_files, total_written = split_by_rows(
            rows, out_dir, chunk_size, base_name)

    elapsed = time.time() - t0
    print_summary(args.input, out_dir, num_files, total_written,
                  source_rows_raw, elapsed)


if __name__ == "__main__":
    main()
