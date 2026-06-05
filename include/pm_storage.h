// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_storage.h — Unified SD storage HAL
//
//  WHY THIS EXISTS:
//
//  The five Pisces Moon device targets split into two camps for SD
//  storage:
//    SPI bus (T-Deck Plus, T-LoRa Pager, Cardputer ADV, Maxine)
//      → SdFat library, FsFile handles, O_READ / O_WRITE flag style
//    SDIO 4-bit (C28P)
//      → Arduino fs::FS (SD_MMC), fs::File handles, FILE_READ / FILE_WRITE
//
//  v1.2.x handled this with a per-file macro adapter at the top of
//  every SD-touching translation unit (nosql_store, ereader,
//  c28p_media, c28p_wardrive_engine CSV writer). Five duplicates of
//  the same #ifdef block + a typedef that aliased the file type, all
//  drifting independently as features were added.
//
//  v1.3 collapses that into one HAL. Every caller now uses
//  pm_storage::open(), pm_storage::exists(), pm_storage::mkdir(),
//  and the pm_storage::File handle. The dual-backend gymnastics
//  live in exactly one .cpp file and never bleed out.
//
//  DESIGN CHOICE — wrapper vs fs::FS inheritance:
//
//  An alternative design considered was `SdFatFs : public fs::FS`
//  with a matching `SdFatFileImpl : public FileImpl`. That would let
//  every caller speak the Arduino fs::FS API directly. We chose the
//  wrapper API instead because:
//    1. fs::FS / FSImpl / FileImpl are Arduino-ESP32 internals. The
//       virtual method set has changed between core versions and is
//       not part of the public Arduino API contract. A wrapper here
//       insulates us from upstream churn.
//    2. The Pisces Moon SD surface is small — about a dozen unique
//       call sites total. Writing a leaner API matched exactly to
//       what we need is less code than the FSImpl adapter would be.
//    3. Files that explicitly need fs::FS& (the ESP32-audioI2S
//       Audio::connecttoFS call in c28p_audio_app.cpp) are already
//       device-gated. They don't suffer from the dual-backend
//       boilerplate this HAL exists to eliminate, so they continue
//       to talk to SD_MMC directly on C28P.
//
//  THREAD SAFETY:
//
//  Operations are NOT thread-safe; concurrent calls from wardrive
//  task and UI thread to the same File handle will corrupt state.
//  The SPI bus is protected by spi_mutex on the SdFat backends but
//  pm_storage doesn't acquire it for the caller. Existing call
//  sites pattern-match the v1.2.x behavior: high-level apps hold
//  spi_mutex around their I/O sequences and call pm_storage inside.
// ─────────────────────────────────────────────

#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace pm_storage {

// Open-mode enum. Maps to FILE_READ / FILE_WRITE / FILE_APPEND on
// SD_MMC, and to O_READ / O_WRITE|O_CREAT|O_TRUNC / O_WRITE|O_CREAT|
// O_APPEND on SdFat.
enum class Mode : uint8_t {
    Read,
    Write,    // create-or-truncate
    Append,   // create-if-missing, position at end
};

// Opaque file handle. Movable but not copyable so the underlying
// backend handle has a single owner. Destruction closes the file
// (matches the v1.2.x SdFat/SD_MMC behavior, important for crash
// safety in long-running tasks that scope-exit on error).
class File {
public:
    File();
    ~File();

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;

    // Truthy if the file is open. Use `if (file) { ... }`.
    explicit operator bool() const;
    bool isOpen() const { return static_cast<bool>(*this); }

    // ── Stream I/O ──
    size_t write(const uint8_t* data, size_t len);
    size_t write(uint8_t b);
    size_t read(uint8_t* buf, size_t len);
    int    read();        // returns next byte, or -1 on EOF / not-open

    size_t print(const char* s);
    size_t println(const char* s);
    size_t println();
    // Note: printf is implemented inline so we don't depend on a
    // backend-specific vsnprintf path. Buffer is stack-allocated
    // at 256 chars; longer formats are silently truncated.
    size_t printf(const char* fmt, ...);

    bool   seek(size_t pos);
    size_t position() const;
    size_t size() const;

    void   flush();
    void   close();

    // ── Directory iteration ──
    //
    // Open as a directory via pm_storage::openDir(path), then call
    // openNextEntry() repeatedly until the returned File is !isOpen().
    //
    // The returned entry is itself a File. If it represents a sub-
    // directory, isDirectory() returns true and you can either skip
    // it (Pisces Moon's NoSQL store is one level deep — no recursion
    // needed) or call openNextEntry() on it to descend.
    //
    // name() returns the full path of the entry on both backends
    // (SD_MMC.openNextFile() returns full paths natively; the SdFat
    // backend prepends the parent directory's path before returning
    // so they look identical).

    File   openNextEntry();
    bool   isDirectory() const;
    const char* name() const;   // pointer valid until next call on this File

private:
    struct Impl;
    Impl* _impl;

    explicit File(Impl* impl);
    friend File open(const char*, Mode);
    friend File openDir(const char*);
};

// ── Top-level operations ──

// Is the underlying SD card mounted and ready? Wraps g_sd_ready
// for callers that don't want to know which global flag to read.
bool ready();

bool exists(const char* path);
bool mkdir(const char* path);
bool remove(const char* path);
bool rmdir(const char* path);

File open(const char* path, Mode mode);
File openDir(const char* path);

}  // namespace pm_storage
