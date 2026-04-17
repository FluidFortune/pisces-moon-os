// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This program is free software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or any later version.
//
// fluidfortune.com

/**
 * PISCES MOON OS — WIFI FILE MANAGER v2.0
 * ==========================================
 * HTTP file server — browse, upload, download, delete SD card files
 * from any browser on the same WiFi network. No software required
 * on the host computer.
 *
 * v2.0 additions:
 *   /backup.zip  — streams the entire SD card as a ZIP archive.
 *                  Uses data-descriptor mode (no seeking required),
 *                  CRC32 computed on-the-fly during streaming.
 *                  Filename: sdcard_backup_NNNN.zip where NNNN is
 *                  the current wardrive session number.
 *
 *   /select      — multi-file download page. Checkboxes on every file,
 *                  Select All toggle, Download Selected opens each
 *                  checked file as a new tab download. Pure HTML/JS,
 *                  no server-side changes needed for selection logic.
 *
 * All v1.0 capabilities preserved:
 *   Full directory tree browsing, file download, upload, delete, mkdir.
 *
 * SPI Bus Treaty:
 *   All SD operations use spi_mutex. HTTP server runs on Core 1.
 *   Wardrive continues on Core 0 unaffected.
 *   wifi_in_use = true for the session duration.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "SdFat.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "theme.h"
#include "wifi_filemgr.h"
#include "wardrive.h"

extern SdFat             sd;
extern Arduino_GFX      *gfx;
extern volatile bool     wifi_in_use;
extern SemaphoreHandle_t spi_mutex;

static WebServer _server(80);
static bool      _serverRunning = false;

// ─────────────────────────────────────────────
//  HELPERS
// ─────────────────────────────────────────────
static String _humanSize(uint32_t bytes) {
    if (bytes < 1024)    return String(bytes) + " B";
    if (bytes < 1048576) return String(bytes / 1024) + " KB";
    return String(bytes / 1048576) + " MB";
}

static String _htmlEsc(const String& s) {
    String o = s;
    o.replace("&","&amp;"); o.replace("<","&lt;");
    o.replace(">","&gt;");  o.replace("\"","&quot;");
    return o;
}

static String _contentType(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".txt"))  return "text/plain";
    if (path.endsWith(".csv"))  return "text/csv";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".jpg"))  return "image/jpeg";
    if (path.endsWith(".wav"))  return "audio/wav";
    return "application/octet-stream";
}

static String _parentOf(const String& path) {
    if (path == "/") return "/";
    int i = path.lastIndexOf('/');
    return (i == 0) ? "/" : path.substring(0, i);
}

// ─────────────────────────────────────────────
//  CRC32
//  Standard reflected CRC32 (ZIP compatible).
//  Table-free nibble-at-a-time — small and fast
//  enough for streaming 1KB chunks over SPI.
// ─────────────────────────────────────────────
static uint32_t _crc32_update(uint32_t crc, const uint8_t* buf, size_t len) {
    // Byte-at-a-time CRC32 using the standard ZIP polynomial 0xEDB88320
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

// ─────────────────────────────────────────────
//  ZIP STREAMING
//
//  ZIP local file header layout (30 bytes + name):
//    0x04034b50  signature
//    version needed: 20 (2.0)
//    flags: 0x0008  (data descriptor follows — no CRC/sizes in local header)
//    compression: 0 (stored)
//    mod time / date: 0
//    crc32: 0        (filled in data descriptor)
//    compressed size: 0   (filled in data descriptor)
//    uncompressed size: 0 (filled in data descriptor)
//    filename length
//    extra field length: 0
//    filename (variable)
//
//  Data descriptor (16 bytes, after file data):
//    0x08074b50  signature
//    crc32
//    compressed size   (uint32)
//    uncompressed size (uint32)
//
//  Central directory entry (46 bytes + name):
//    0x02014b50  signature
//    version made: 20
//    version needed: 20
//    flags: 0x0008
//    compression: 0
//    mod time/date: 0
//    crc32
//    compressed size
//    uncompressed size
//    filename length
//    extra length: 0
//    comment length: 0
//    disk number: 0
//    internal attr: 0
//    external attr: 0
//    local header offset
//    filename
//
//  End of central directory (22 bytes):
//    0x06054b50  signature
//    disk number: 0
//    central dir disk: 0
//    entries this disk
//    total entries
//    central dir size
//    central dir offset
//    comment length: 0
// ─────────────────────────────────────────────

// Write a uint16 little-endian to a byte buffer at offset, return new offset
static int _put16(uint8_t* buf, int off, uint16_t v) {
    buf[off]   = v & 0xFF;
    buf[off+1] = (v >> 8) & 0xFF;
    return off + 2;
}

// Write a uint32 little-endian to a byte buffer at offset, return new offset
static int _put32(uint8_t* buf, int off, uint32_t v) {
    buf[off]   = v & 0xFF;
    buf[off+1] = (v >> 8) & 0xFF;
    buf[off+2] = (v >> 16) & 0xFF;
    buf[off+3] = (v >> 24) & 0xFF;
    return off + 4;
}

struct ZipCentralEntry {
    String   name;          // path within ZIP (no leading /)
    uint32_t offset;        // byte offset of local header in stream
    uint32_t crc32;
    uint32_t size;
};

// Maximum files we track for the central directory.
// Each entry uses ~80 bytes of heap (String + 3 uint32s).
// 512 entries = ~40KB — well within PSRAM budget.
#define ZIP_MAX_FILES 512

static ZipCentralEntry* _zipEntries   = nullptr;
static int              _zipFileCount = 0;

// Write local file header for a single entry (data descriptor mode)
static uint32_t _zip_write_local_header(WiFiClient& c, const String& name) {
    uint8_t hdr[30];
    int o = 0;
    o = _put32(hdr, o, 0x04034b50); // sig
    o = _put16(hdr, o, 20);          // version needed
    o = _put16(hdr, o, 0x0008);      // flags: data descriptor
    o = _put16(hdr, o, 0);           // compression: stored
    o = _put16(hdr, o, 0);           // mod time
    o = _put16(hdr, o, 0);           // mod date
    o = _put32(hdr, o, 0);           // crc32 (0 — in descriptor)
    o = _put32(hdr, o, 0);           // compressed size (0)
    o = _put32(hdr, o, 0);           // uncompressed size (0)
    o = _put16(hdr, o, (uint16_t)name.length()); // filename len
    o = _put16(hdr, o, 0);           // extra field len
    c.write(hdr, 30);
    c.write((const uint8_t*)name.c_str(), name.length());
    return 30 + name.length();
}

// Write data descriptor after file data
static void _zip_write_data_descriptor(WiFiClient& c,
                                        uint32_t crc,
                                        uint32_t size) {
    uint8_t desc[16];
    int o = 0;
    o = _put32(desc, o, 0x08074b50); // sig
    o = _put32(desc, o, crc);
    o = _put32(desc, o, size);       // compressed = uncompressed (stored)
    o = _put32(desc, o, size);
    c.write(desc, 16);
}

// Write central directory entry
static void _zip_write_central_entry(WiFiClient& c,
                                      const ZipCentralEntry& e) {
    uint8_t rec[46];
    int o = 0;
    o = _put32(rec, o, 0x02014b50); // sig
    o = _put16(rec, o, 20);          // version made
    o = _put16(rec, o, 20);          // version needed
    o = _put16(rec, o, 0x0008);      // flags
    o = _put16(rec, o, 0);           // compression: stored
    o = _put16(rec, o, 0);           // mod time
    o = _put16(rec, o, 0);           // mod date
    o = _put32(rec, o, e.crc32);
    o = _put32(rec, o, e.size);      // compressed
    o = _put32(rec, o, e.size);      // uncompressed
    o = _put16(rec, o, (uint16_t)e.name.length());
    o = _put16(rec, o, 0);           // extra
    o = _put16(rec, o, 0);           // comment
    o = _put16(rec, o, 0);           // disk number
    o = _put16(rec, o, 0);           // internal attr
    o = _put32(rec, o, 0);           // external attr
    o = _put32(rec, o, e.offset);    // local header offset
    c.write(rec, 46);
    c.write((const uint8_t*)e.name.c_str(), e.name.length());
}

// Write end-of-central-directory record
static void _zip_write_eocd(WiFiClient& c,
                             uint16_t count,
                             uint32_t cdSize,
                             uint32_t cdOffset) {
    uint8_t eocd[22];
    int o = 0;
    o = _put32(eocd, o, 0x06054b50); // sig
    o = _put16(eocd, o, 0);           // disk number
    o = _put16(eocd, o, 0);           // central dir disk
    o = _put16(eocd, o, count);       // entries this disk
    o = _put16(eocd, o, count);       // total entries
    o = _put32(eocd, o, cdSize);
    o = _put32(eocd, o, cdOffset);
    o = _put16(eocd, o, 0);           // comment length
    c.write(eocd, 22);
}

// ─────────────────────────────────────────────
//  RECURSIVE ZIP WALKER
//  Walks the SD card recursively, streaming each
//  file as a stored (uncompressed) ZIP entry.
//  Populates _zipEntries[] for the central directory.
//  Returns total bytes written to stream.
// ─────────────────────────────────────────────
static uint32_t _zip_walk(WiFiClient& client,
                           const String& sdPath,
                           const String& zipPrefix,
                           uint32_t& streamOffset) {
    if (_zipFileCount >= ZIP_MAX_FILES) return 0;

    FsFile dir = sd.open(sdPath.c_str());
    if (!dir || !dir.isDir()) { dir.close(); return 0; }

    uint32_t written = 0;
    FsFile entry;

    // Dirs first pass, then files — keeps archive organised
    for (int pass = 0; pass < 2; pass++) {
        dir.rewindDirectory();
        while (entry.openNext(&dir, O_RDONLY)) {
            char nm[64]; entry.getName(nm, sizeof(nm));
            bool isDir = entry.isDir();

            if ((pass == 0) != isDir) { entry.close(); continue; }

            String entryName = String(nm);
            String fullSdPath = (sdPath == "/") ? "/" + entryName
                                                 : sdPath + "/" + entryName;
            String zipName    = zipPrefix.length()
                                ? zipPrefix + "/" + entryName
                                : entryName;

            if (isDir) {
                entry.close();
                // Recurse — release and re-acquire mutex around the recursive call
                xSemaphoreGive(spi_mutex);
                _zip_walk(client, fullSdPath, zipName, streamOffset);
                xSemaphoreTake(spi_mutex, portMAX_DELAY);
            } else {
                if (_zipFileCount >= ZIP_MAX_FILES) { entry.close(); break; }

                uint32_t fileSize = entry.fileSize();

                // Record this entry for the central directory
                _zipEntries[_zipFileCount].name   = zipName;
                _zipEntries[_zipFileCount].offset = streamOffset;
                _zipEntries[_zipFileCount].size   = fileSize;
                _zipEntries[_zipFileCount].crc32  = 0; // filled below

                // Write local header
                uint32_t hdrSize = _zip_write_local_header(client, zipName);
                streamOffset += hdrSize;

                // Stream file data + compute CRC32
                uint32_t crc    = 0;
                uint32_t remain = fileSize;
                uint8_t  buf[512];

                while (remain > 0) {
                    size_t toRead = min((uint32_t)sizeof(buf), remain);
                    size_t got    = entry.read(buf, toRead);
                    if (got == 0) break;
                    crc = _crc32_update(crc, buf, got);
                    client.write(buf, got);
                    streamOffset += got;
                    remain       -= got;
                    yield();
                }

                // Write data descriptor
                _zip_write_data_descriptor(client, crc, fileSize);
                streamOffset += 16;

                _zipEntries[_zipFileCount].crc32 = crc;
                _zipFileCount++;

                entry.close();
            }
        }
    }

    dir.close();
    return written;
}

// ─────────────────────────────────────────────
//  CYBERPUNK CSS — inline, single string
//  Declared here so all handlers below can use it.
// ─────────────────────────────────────────────
static const char _CSS[] PROGMEM =
    "body{font-family:monospace;background:#080808;color:#00ff88;margin:0}"
    "h1{background:#001a0d;padding:10px 18px;margin:0;font-size:15px;"
        "border-bottom:1px solid #00ff88;letter-spacing:2px}"
    ".ip{float:right;color:#005522;font-size:12px}"
    ".bc{padding:6px 18px;font-size:11px;background:#040c07;"
        "border-bottom:1px solid #002211}"
    ".bc a{color:#00aa55;text-decoration:none}"
    ".bc a:hover{color:#00ff88}"
    "table{width:100%;border-collapse:collapse}"
    "th{background:#001a0d;padding:6px 12px;text-align:left;"
        "font-size:11px;color:#005522;border-bottom:1px solid #002211}"
    "td{padding:5px 12px;border-bottom:1px solid #0a140c;font-size:12px}"
    "tr:hover td{background:#0a1a0d}"
    ".dn{color:#00ccff} .fn{color:#00ff88} .sz{color:#004422;text-align:right}"
    ".ac a{color:#ff6600;text-decoration:none;margin-left:10px;font-size:11px}"
    ".ac a:hover{color:#ff9900}"
    ".up{padding:16px 18px;background:#040c07;border-top:1px solid #002211}"
    ".up h3{margin:0 0 8px;font-size:12px;color:#00aa55}"
    "input[type=file]{color:#00ff88;font-family:monospace;font-size:12px}"
    "input[type=text]{background:#001a0d;color:#00ff88;border:1px solid #003322;"
        "padding:4px 8px;font-family:monospace;font-size:12px}"
    "button{background:#002211;color:#00ff88;border:1px solid #00ff88;"
        "padding:5px 14px;font-family:monospace;cursor:pointer;margin-left:6px}"
    "button:hover{background:#00ff88;color:#000}"
    ".note{font-size:10px;color:#003322;margin-top:6px}";

// ─────────────────────────────────────────────
//  BACKUP ZIP HANDLER
//  GET /backup.zip
//  Streams entire SD card as a ZIP archive.
// ─────────────────────────────────────────────
static void _handleBackupZip() {
    // Allocate central directory table in PSRAM
    _zipEntries = (ZipCentralEntry*)ps_malloc(ZIP_MAX_FILES * sizeof(ZipCentralEntry));
    if (!_zipEntries) {
        _server.send(503, "text/plain", "Out of memory");
        return;
    }
    // Placement-new each String member
    for (int i = 0; i < ZIP_MAX_FILES; i++) {
        new (&_zipEntries[i]) ZipCentralEntry();
    }
    _zipFileCount = 0;

    // Build filename: sdcard_backup_NNNN.zip
    const char* sessionFile = wardrive_get_log_filename();
    int sessionNum = 0;
    if (sessionFile && sessionFile[0]) {
        // Extract number from "/wardrive_0003.csv" → 3
        const char* p = strrchr(sessionFile, '_');
        if (p) sessionNum = atoi(p + 1);
    }
    char zipName[40];
    if (sessionNum > 0)
        snprintf(zipName, sizeof(zipName), "sdcard_backup_%04d.zip", sessionNum);
    else
        snprintf(zipName, sizeof(zipName), "sdcard_backup.zip");

    // Send headers — chunked transfer (we don't know final size)
    WiFiClient client = _server.client();
    String hdr = "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/zip\r\n"
                 "Content-Disposition: attachment; filename=\"" +
                 String(zipName) + "\"\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "Connection: close\r\n\r\n";
    client.print(hdr);

    // Walk and stream — hold mutex across each file, release between dirs
    uint32_t streamOffset = 0;
    if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        _zip_walk(client, "/", "", streamOffset);
        xSemaphoreGive(spi_mutex);
    }

    // Write central directory
    uint32_t cdOffset = streamOffset;
    uint32_t cdSize   = 0;

    for (int i = 0; i < _zipFileCount; i++) {
        uint32_t before = streamOffset;
        _zip_write_central_entry(client, _zipEntries[i]);
        uint32_t entrySize = 46 + _zipEntries[i].name.length();
        cdSize       += entrySize;
        streamOffset += entrySize;
    }

    // Write EOCD
    _zip_write_eocd(client, (uint16_t)_zipFileCount, cdSize, cdOffset);

    // Terminate chunked transfer
    client.print("0\r\n\r\n");
    client.flush();

    // Free central directory table
    for (int i = 0; i < ZIP_MAX_FILES; i++) {
        _zipEntries[i].~ZipCentralEntry();
    }
    free(_zipEntries);
    _zipEntries = nullptr;

    Serial.printf("[FILEMGR] Backup ZIP sent: %d files, %lu bytes\n",
                  _zipFileCount, (unsigned long)streamOffset);
}

// ─────────────────────────────────────────────
//  MULTI-FILE SELECT PAGE
//  GET /select  — lists all files with checkboxes.
//  "Download Selected" opens each in a new tab.
//  Pure HTML/JS — no additional server endpoints.
// ─────────────────────────────────────────────

// Recursive helper: appends file rows to html string
static void _selectWalkDir(const String& sdPath,
                            const String& indent,
                            String& html) {
    FsFile dir = sd.open(sdPath.c_str());
    if (!dir || !dir.isDir()) { dir.close(); return; }

    FsFile entry;
    // Dirs first
    for (int pass = 0; pass < 2; pass++) {
        dir.rewindDirectory();
        while (entry.openNext(&dir, O_RDONLY)) {
            char nm[64]; entry.getName(nm, sizeof(nm));
            bool isDir = entry.isDir();
            if ((pass == 0) != isDir) { entry.close(); continue; }

            String n        = String(nm);
            String fullPath = (sdPath == "/") ? "/" + n : sdPath + "/" + n;

            if (isDir) {
                html += "<tr><td colspan='3' style='color:#00ccff;padding-left:"
                     + indent + "px'>&#128193; " + _htmlEsc(n) + "/</td></tr>";
                entry.close();

                // Recurse with increased indent
                int nextIndent = indent.toInt() + 16;
                xSemaphoreGive(spi_mutex);
                _selectWalkDir(fullPath, String(nextIndent), html);
                xSemaphoreTake(spi_mutex, portMAX_DELAY);
            } else {
                uint32_t sz = entry.fileSize();
                String ep   = _htmlEsc(fullPath);
                html += "<tr>"
                        "<td style='padding-left:" + indent + "px'>"
                        "<input type='checkbox' class='fchk' value='/dl?path="
                        + ep + "' name='f'> "
                        "<span style='color:#00ff88'>" + _htmlEsc(n) + "</span></td>"
                        "<td class='sz'>" + _humanSize(sz) + "</td>"
                        "<td class='ac'><a href='/dl?path=" + ep + "'>&#11015; GET</a></td>"
                        "</tr>";
                entry.close();
            }
        }
    }
    dir.close();
}

static void _handleSelectPage() {
    String html;
    html.reserve(8192);

    html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>Pisces Moon — Select Files</title><style>";
    html += String(FPSTR(_CSS));
    // Extra styles for checkbox page
    html += ".cb-bar{padding:10px 18px;background:#001a0d;"
            "border-bottom:1px solid #003322;display:flex;gap:10px;align-items:center}"
            "input[type=checkbox]{accent-color:#00ff88;width:14px;height:14px;cursor:pointer}"
            "</style></head><body>";

    html += "<h1>&#9651; PISCES MOON / SELECT FILES"
            "<span class='ip'>&#9675; " + WiFi.localIP().toString() + "</span></h1>";

    // Toolbar
    html += "<div class='cb-bar'>"
            "<button onclick='selAll(true)'>&#9745; SELECT ALL</button>"
            "<button onclick='selAll(false)'>&#9744; NONE</button>"
            "<button onclick='dlSelected()' style='color:#00ff88;border-color:#00ff88'>"
            "&#11015; DOWNLOAD SELECTED</button>"
            "<span id='cnt' style='color:#004422;font-size:11px'>0 selected</span>"
            "</div>";

    // Shortcut bar — wardrive files
    html += "<div class='bc'>"
            "<a href='/?path=/'>&#8962; Browser</a> &nbsp;|&nbsp; "
            "<a href='/backup.zip' style='color:#00ff88'>&#128230; FULL BACKUP (ZIP)</a>"
            "</div>";

    // File table with checkboxes
    html += "<table><tr>"
            "<th><input type='checkbox' id='all' onchange='selAll(this.checked)'> NAME</th>"
            "<th>SIZE</th><th>DL</th></tr>";

    if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        _selectWalkDir("/", "18", html);
        xSemaphoreGive(spi_mutex);
    }
    html += "</table>";

    // JavaScript — select all + open each checked file in new tab
    html += "<script>"
            "function selAll(v){"
            "  document.querySelectorAll('.fchk').forEach(c=>c.checked=v);"
            "  document.getElementById('all').checked=v;"
            "  updateCount();"
            "}"
            "function updateCount(){"
            "  var n=document.querySelectorAll('.fchk:checked').length;"
            "  document.getElementById('cnt').textContent=n+' selected';"
            "}"
            "document.addEventListener('change',function(e){"
            "  if(e.target.classList.contains('fchk')) updateCount();"
            "});"
            "function dlSelected(){"
            "  var chk=document.querySelectorAll('.fchk:checked');"
            "  if(chk.length===0){alert('No files selected.');return;}"
            "  if(chk.length>20&&!confirm('Download '+chk.length+' files? Each opens in a new tab.'))return;"
            "  chk.forEach(function(c,i){"
            "    setTimeout(function(){window.open(c.value,'_blank');},"
            "    i*300);"  // stagger 300ms apart to avoid browser throttling
            "  });"
            "}"
            "</script>";

    html += "</body></html>";
    _server.send(200, "text/html", html);
}

// ─────────────────────────────────────────────
//  DIRECTORY BROWSER
// ─────────────────────────────────────────────
static void _handleRoot() {
    String path = _server.hasArg("path") ? _server.arg("path") : "/";
    if (!path.startsWith("/")) path = "/" + path;

    String html;
    html.reserve(4096);
    html  = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Pisces Moon FS</title><style>";
    html += String(FPSTR(_CSS));
    html += "</style></head><body>";
    html += "<h1>&#9651; PISCES MOON / FILE MANAGER"
            "<span class='ip'>&#9675; " + WiFi.localIP().toString() + "</span></h1>";

    // Breadcrumb + backup/select links
    html += "<div class='bc'><a href='/?path=/'>&#8962; /</a>";
    if (path != "/") {
        String tmp = "", seg = path.substring(1);
        int p;
        while ((p = seg.indexOf('/')) != -1) {
            tmp += "/" + seg.substring(0,p);
            html += " / <a href='/?path=" + tmp + "'>" + seg.substring(0,p) + "</a>";
            seg = seg.substring(p+1);
        }
        if (seg.length()) html += " / <b>" + _htmlEsc(seg) + "</b>";
    }
    html += " &nbsp;|&nbsp; "
            "<a href='/select' style='color:#00ccff'>&#9745; SELECT FILES</a>"
            " &nbsp;|&nbsp; "
            "<a href='/backup.zip' style='color:#00ff88'>&#128230; FULL BACKUP ZIP</a>"
            "</div>";

    // File table
    html += "<table><tr><th>NAME</th><th>SIZE</th><th>ACTIONS</th></tr>";

    if (path != "/") {
        String par = _parentOf(path);
        html += "<tr><td class='dn'><a href='/?path=" + par +
                "' style='color:#00ccff'>&#8593; ..</a></td>"
                "<td class='sz'>—</td><td></td></tr>";
    }

    if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        FsFile dir = sd.open(path.c_str());
        if (dir && dir.isDir()) {
            for (int pass = 0; pass < 2; pass++) {
                dir.rewindDirectory();
                FsFile entry;
                while (entry.openNext(&dir, O_RDONLY)) {
                    char nm[64]; entry.getName(nm, sizeof(nm));
                    bool isDir = entry.isDir();
                    if ((pass==0) != isDir) { entry.close(); continue; }

                    String n  = String(nm);
                    String ep = (path == "/") ? "/" + n : path + "/" + n;

                    if (isDir) {
                        html += "<tr><td class='dn'>&#128193; "
                                "<a href='/?path=" + ep + "' style='color:#00ccff'>"
                                + _htmlEsc(n) + "/</a></td>"
                                "<td class='sz'>—</td>"
                                "<td class='ac'><a href='/del?path=" + ep +
                                "' onclick=\"return confirm('Delete "+_htmlEsc(n)+"?')\">&#128465; DEL</a></td></tr>";
                    } else {
                        uint32_t sz = entry.fileSize();
                        html += "<tr><td class='fn'>&#128196; "
                                "<a href='/dl?path=" + ep + "' style='color:#00ff88'>"
                                + _htmlEsc(n) + "</a></td>"
                                "<td class='sz'>" + _humanSize(sz) + "</td>"
                                "<td class='ac'>"
                                "<a href='/dl?path=" + ep + "'>&#11015; GET</a>"
                                "<a href='/del?path=" + ep +
                                "' onclick=\"return confirm('Delete "+_htmlEsc(n)+"?')\">&#128465; DEL</a>"
                                "</td></tr>";
                    }
                    entry.close();
                }
            }
            dir.close();
        }
        xSemaphoreGive(spi_mutex);
    }
    html += "</table>";

    // Upload form
    html += "<div class='up'><h3>&#11014; UPLOAD TO: " + _htmlEsc(path) + "</h3>"
            "<form method='POST' action='/ul' enctype='multipart/form-data'>"
            "<input type='hidden' name='path' value='" + _htmlEsc(path) + "'>"
            "<input type='file' name='file' multiple>"
            "<button type='submit'>SEND</button>"
            "</form>"
            "<p class='note'>Drag .elf files here to deploy to /apps/ without removing the SD card.</p>"
            "</div>";

    // Mkdir form
    html += "<div class='up' style='padding-top:8px'>"
            "<form method='POST' action='/mkdir'>"
            "<input type='hidden' name='path' value='" + _htmlEsc(path) + "'>"
            "<input type='text' name='d' placeholder='new folder name' size='20'>"
            "<button type='submit'>&#128193; MKDIR</button>"
            "</form></div>";

    html += "</body></html>";
    _server.send(200, "text/html", html);
}

// ─────────────────────────────────────────────
//  DOWNLOAD (single file)
// ─────────────────────────────────────────────
static void _handleDownload() {
    if (!_server.hasArg("path")) { _server.send(400,"text/plain","Missing path"); return; }
    String path = _server.arg("path");

    if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        FsFile file = sd.open(path.c_str(), O_READ);
        if (!file || file.isDir()) {
            xSemaphoreGive(spi_mutex);
            _server.send(404,"text/plain","Not found"); return;
        }
        String fname = path.substring(path.lastIndexOf('/')+1);
        _server.sendHeader("Content-Disposition","attachment; filename=\""+fname+"\"");
        _server.sendHeader("Content-Length", String(file.fileSize()));
        _server.setContentLength(file.fileSize());
        _server.send(200, _contentType(fname), "");
        WiFiClient client = _server.client();
        uint8_t buf[1024]; size_t n;
        while ((n = file.read(buf, sizeof(buf))) > 0) {
            client.write(buf, n); yield();
        }
        file.close();
        xSemaphoreGive(spi_mutex);
    } else {
        _server.send(503,"text/plain","SD busy");
    }
}

// ─────────────────────────────────────────────
//  UPLOAD
// ─────────────────────────────────────────────
static FsFile _upFile;
static bool   _upOk  = false;
static String _upDir = "/";

static void _handleUploadData() {
    HTTPUpload& up = _server.upload();
    if (up.status == UPLOAD_FILE_START) {
        _upOk  = false;
        _upDir = _server.hasArg("path") ? _server.arg("path") : "/";
        String dest = (_upDir == "/") ? "/" + String(up.filename.c_str())
                                      : _upDir + "/" + String(up.filename.c_str());
        Serial.printf("[FILEMGR] Upload: %s\n", dest.c_str());
        if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (!sd.exists(_upDir.c_str())) sd.mkdir(_upDir.c_str());
            _upFile = sd.open(dest.c_str(), O_WRITE | O_CREAT | O_TRUNC);
            xSemaphoreGive(spi_mutex);
            _upOk = (bool)_upFile;
        }
    } else if (up.status == UPLOAD_FILE_WRITE && _upOk) {
        if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            _upFile.write(up.buf, up.currentSize);
            xSemaphoreGive(spi_mutex);
        }
    } else if (up.status == UPLOAD_FILE_END && _upOk) {
        if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            _upFile.close();
            xSemaphoreGive(spi_mutex);
        }
        Serial.printf("[FILEMGR] Upload done: %lu B\n", (unsigned long)up.totalSize);
    }
}

static void _handleUploadDone() {
    if (_upOk) {
        _server.sendHeader("Location", "/?path=" + _upDir);
        _server.send(303);
    } else {
        _server.send(500,"text/plain","Upload failed");
    }
}

// ─────────────────────────────────────────────
//  DELETE
// ─────────────────────────────────────────────
static void _handleDelete() {
    if (!_server.hasArg("path")) { _server.send(400,"text/plain","Missing path"); return; }
    String path = _server.arg("path");
    String par  = _parentOf(path);
    if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (sd.exists(path.c_str())) {
            if (!sd.remove(path.c_str())) sd.rmdir(path.c_str());
        }
        xSemaphoreGive(spi_mutex);
    }
    _server.sendHeader("Location","/?path="+par);
    _server.send(303);
}

// ─────────────────────────────────────────────
//  MKDIR
// ─────────────────────────────────────────────
static void _handleMkdir() {
    String base = _server.hasArg("path") ? _server.arg("path") : "/";
    String name = _server.hasArg("d")    ? _server.arg("d")    : "";
    if (name.length() > 0) {
        String np = (base == "/") ? "/" + name : base + "/" + name;
        if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            sd.mkdir(np.c_str());
            xSemaphoreGive(spi_mutex);
        }
    }
    _server.sendHeader("Location","/?path="+base);
    _server.send(303);
}

// ─────────────────────────────────────────────
//  SCREEN
// ─────────────────────────────────────────────
static void _drawScreen(const String& ip) {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->print("WIFI FILE MGR | TAP HEADER TO STOP");

    gfx->setTextSize(2); gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 34); gfx->print("SERVER ACTIVE");

    gfx->setTextSize(1); gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 62); gfx->print("Type this EXACTLY in browser:");

    // Full URL — explicit http:// so user doesn't let browser auto-HTTPS
    gfx->setTextSize(1); gfx->setTextColor(0x07FF);
    gfx->setCursor(10, 76);
    gfx->print("http://"); gfx->print(ip); gfx->print("/");

    gfx->setTextColor(0xFFE0); gfx->setCursor(10, 94);
    gfx->print("Test: http://"); gfx->print(ip); gfx->print("/ping");

    gfx->setTextColor(C_GREY); gfx->setCursor(10, 110);
    gfx->print("Must type http:// — not just the IP.");
    gfx->setCursor(10, 122); gfx->print("If ping fails: router AP isolation ON.");
    gfx->setCursor(10, 134); gfx->print("(Disable 'Client Isolation' in router)");

    gfx->fillRect(0, 210, 320, 30, 0x2000);
    gfx->drawFastHLine(0, 210, 320, 0xF800);
    gfx->setTextColor(0xF800); gfx->setCursor(60, 220);
    gfx->print("TAP HEADER TO STOP SERVER");
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_wifi_filemgr() {
    if (WiFi.status() != WL_CONNECTED) {
        gfx->fillScreen(C_BLACK);
        gfx->fillRect(0, 0, 320, 24, C_DARK);
        gfx->drawFastHLine(0, 24, 320, 0xF800);
        gfx->setCursor(10, 7); gfx->setTextColor(0xF800); gfx->setTextSize(1);
        gfx->print("NOT CONNECTED TO WIFI");
        gfx->setCursor(10, 50); gfx->setTextColor(C_GREY);
        gfx->print("Connect via COMMS > WIFI JOIN first.");
        delay(2500);
        return;
    }

    wifi_in_use = true;
    String ip = WiFi.localIP().toString();

    // Fully reset the server before registering handlers.
    // WebServer is a static object — if we've run before, stop() left the
    // TCP socket in a half-closed state and old handlers are still registered.
    // close() forces the underlying lwIP socket fully shut before begin().
    _server.stop();
    _server.close();
    delay(100);   // Let lwIP release the socket and port 80

    _server.on("/",           HTTP_GET,  _handleRoot);
    _server.on("/dl",         HTTP_GET,  _handleDownload);
    _server.on("/del",        HTTP_GET,  _handleDelete);
    _server.on("/mkdir",      HTTP_POST, _handleMkdir);
    _server.on("/ul",         HTTP_POST, _handleUploadDone, _handleUploadData);
    _server.on("/select",     HTTP_GET,  _handleSelectPage);
    _server.on("/backup.zip", HTTP_GET,  _handleBackupZip);
    // /ping — lightweight test endpoint, no SD access required.
    // If this works but / doesn't, the SD mutex is the problem.
    // If even this fails, check AP isolation on the router.
    _server.on("/ping",       HTTP_GET,  [](){
        _server.send(200, "text/plain", "PISCES MOON OK");
    });

    _server.begin();
    _serverRunning = true;
    Serial.printf("[FILEMGR] http://%s/\n", ip.c_str());

    _drawScreen(ip);

    while (_serverRunning) {
        _server.handleClient();
        yield();
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            _serverRunning = false;
        }
        delay(2);
    }

    _server.stop();
    wifi_in_use = false;
    Serial.println("[FILEMGR] Stopped.");
}
