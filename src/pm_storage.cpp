// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_storage.cpp — Unified SD storage HAL implementation
//
//  Dual-backend layer. C28P uses Arduino's fs::FS via SD_MMC over
//  4-bit SDIO. Every other device uses SdFat over SPI. This file
//  is the ONE place where that fork happens — everything else in
//  the OS calls pm_storage::open() and friends without caring.
//
//  Two impls live inside the same struct Impl, gated by #ifdef.
//  Each File holds a pointer to an Impl rather than embedding the
//  backend type directly, so the public class doesn't leak any
//  knowledge of fs::File vs FsFile sizes (which differ enough that
//  embedding either would have made the class device-specific).
// ─────────────────────────────────────────────

#include "pm_storage.h"
#include <stdarg.h>
#include <string.h>

extern volatile bool g_sd_ready;

#ifdef DEVICE_C28P
  #include <FS.h>
  #include <SD_MMC.h>
#else
  #include "SdFat.h"
  extern SdFat sd;
#endif

namespace pm_storage {

// ─────────────────────────────────────────────
//  Impl — backend-specific state
//
//  Hidden from the header so callers don't pull in FS.h / SdFat.h
//  transitively. Allocated on the heap (one per open File) and
//  freed in File::~File().
//
//  is_dir            true if this Impl wraps a directory handle
//                    (returned by openDir() or by openNextEntry()
//                    when the entry was itself a directory).
//
//  dir_prefix        SdFat path prefix for openNextEntry(). SdFat's
//                    FsFile::getName() returns just the leaf name;
//                    fs::File::name() returns the full path. To make
//                    name() behave consistently across backends we
//                    cache the parent directory and prepend it. On
//                    the SD_MMC backend this field is unused.
//
//  name_cache        Cached return value for name(). Lifetime: until
//                    the next openNextEntry() call on the same File,
//                    matching the documented contract in the header.
// ─────────────────────────────────────────────
struct File::Impl {
#ifdef DEVICE_C28P
    fs::File f;
#else
    FsFile   f;
#endif
    bool   is_dir;
    String dir_prefix;
    String name_cache;
};

// ─────────────────────────────────────────────
//  File — rule of five (move-only)
// ─────────────────────────────────────────────
File::File() : _impl(nullptr) {}

File::File(Impl* impl) : _impl(impl) {}

File::~File() {
    if (_impl) {
        if (_impl->f) _impl->f.close();
        delete _impl;
    }
}

File::File(File&& other) noexcept : _impl(other._impl) {
    other._impl = nullptr;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (_impl) {
            if (_impl->f) _impl->f.close();
            delete _impl;
        }
        _impl = other._impl;
        other._impl = nullptr;
    }
    return *this;
}

File::operator bool() const {
    if (!_impl) return false;
    // fs::File and FsFile both have an operator bool() returning
    // open-ness, which is what we want here.
    return static_cast<bool>(_impl->f);
}

// ─────────────────────────────────────────────
//  Stream I/O
// ─────────────────────────────────────────────
size_t File::write(const uint8_t* data, size_t len) {
    if (!_impl || !_impl->f) return 0;
    return _impl->f.write(data, len);
}

size_t File::write(uint8_t b) {
    return write(&b, 1);
}

size_t File::read(uint8_t* buf, size_t len) {
    if (!_impl || !_impl->f) return 0;
    int n = _impl->f.read(buf, len);
    return (n < 0) ? 0 : (size_t)n;
}

int File::read() {
    if (!_impl || !_impl->f) return -1;
    uint8_t b;
    int n = _impl->f.read(&b, 1);
    return (n == 1) ? (int)b : -1;
}

size_t File::print(const char* s) {
    if (!s) return 0;
    return write(reinterpret_cast<const uint8_t*>(s), strlen(s));
}

size_t File::println(const char* s) {
    size_t n = print(s);
    n += write((const uint8_t*)"\n", 1);
    return n;
}

size_t File::println() {
    return write((const uint8_t*)"\n", 1);
}

size_t File::printf(const char* fmt, ...) {
    if (!_impl || !_impl->f) return 0;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t to_write = (n >= (int)sizeof(buf)) ? sizeof(buf) - 1 : (size_t)n;
    return _impl->f.write(reinterpret_cast<const uint8_t*>(buf), to_write);
}

bool File::seek(size_t pos) {
    if (!_impl || !_impl->f) return false;
#ifdef DEVICE_C28P
    return _impl->f.seek(pos);
#else
    return _impl->f.seek((uint32_t)pos);
#endif
}

size_t File::position() const {
    if (!_impl || !_impl->f) return 0;
    return _impl->f.position();
}

size_t File::size() const {
    if (!_impl || !_impl->f) return 0;
    return _impl->f.size();
}

void File::flush() {
    if (!_impl || !_impl->f) return;
    _impl->f.flush();
}

void File::close() {
    if (!_impl) return;
    if (_impl->f) _impl->f.close();
}

// ─────────────────────────────────────────────
//  Directory iteration
// ─────────────────────────────────────────────
File File::openNextEntry() {
    if (!_impl || !_impl->is_dir || !_impl->f) return File();

    // Construct Impl first, then open the entry directly INTO impl->f.
    // This avoids needing a temporary FsFile / fs::File and an assignment
    // operator — SdFat 2.2.3's FsFile has both copy constructor (private)
    // and copy assignment (deleted) blocked at the FsBaseFile level, and
    // does not define a move operator. The only safe pattern is in-place
    // open.
    Impl* impl = new Impl();
    impl->is_dir = false;

#ifdef DEVICE_C28P
    impl->f = _impl->f.openNextFile();
    if (!impl->f) {
        delete impl;
        return File();
    }
    impl->is_dir = impl->f.isDirectory();
    impl->name_cache = String(impl->f.name());
#else
    if (!impl->f.openNext(&_impl->f, O_READ)) {
        delete impl;
        return File();
    }
    impl->is_dir = impl->f.isDir();
    char leaf[80];
    impl->f.getName(leaf, sizeof(leaf));
    // Build full path to match SD_MMC's behavior.
    if (_impl->dir_prefix.endsWith("/")) {
        impl->name_cache = _impl->dir_prefix + String(leaf);
    } else if (_impl->dir_prefix.length() > 0) {
        impl->name_cache = _impl->dir_prefix + "/" + String(leaf);
    } else {
        impl->name_cache = String("/") + String(leaf);
    }
    impl->dir_prefix = impl->name_cache;
#endif

    return File(impl);
}

bool File::isDirectory() const {
    if (!_impl) return false;
    return _impl->is_dir;
}

const char* File::name() const {
    if (!_impl) return "";
    return _impl->name_cache.c_str();
}

// ─────────────────────────────────────────────
//  Top-level operations
// ─────────────────────────────────────────────
bool ready() {
    return g_sd_ready;
}

bool exists(const char* path) {
#ifdef DEVICE_C28P
    return SD_MMC.exists(path);
#else
    return sd.exists(path);
#endif
}

bool mkdir(const char* path) {
#ifdef DEVICE_C28P
    return SD_MMC.mkdir(path);
#else
    return sd.mkdir(path);
#endif
}

bool remove(const char* path) {
#ifdef DEVICE_C28P
    return SD_MMC.remove(path);
#else
    return sd.remove(path);
#endif
}

bool rmdir(const char* path) {
#ifdef DEVICE_C28P
    return SD_MMC.rmdir(path);
#else
    return sd.rmdir(path);
#endif
}

File open(const char* path, Mode mode) {
    File::Impl* impl = new File::Impl();
    impl->is_dir = false;
    impl->name_cache = String(path);

#ifdef DEVICE_C28P
    const char* m = "r";
    switch (mode) {
        case Mode::Read:   m = FILE_READ;   break;
        case Mode::Write:  m = FILE_WRITE;  break;
        case Mode::Append: m = FILE_APPEND; break;
    }
    impl->f = SD_MMC.open(path, m);
    if (impl->f && impl->f.isDirectory()) {
        // Mode-mismatched: caller said open() but the path is a dir.
        // Close it and return an invalid handle so isOpen() is false.
        impl->f.close();
    }
#else
    oflag_t flags = O_READ;
    switch (mode) {
        case Mode::Read:   flags = O_READ; break;
        case Mode::Write:  flags = (O_WRITE | O_CREAT | O_TRUNC); break;
        case Mode::Append: flags = (O_WRITE | O_CREAT | O_APPEND); break;
    }
    if (!impl->f.open(path, flags)) {
        // FsFile failure leaves the handle in a closed state; that's
        // what operator bool() picks up.
    }
#endif

    if (!impl->f) {
        delete impl;
        return File();
    }
    return File(impl);
}

File openDir(const char* path) {
    File::Impl* impl = new File::Impl();
    impl->is_dir = true;
    impl->name_cache = String(path);
    impl->dir_prefix = String(path);

#ifdef DEVICE_C28P
    impl->f = SD_MMC.open(path);
    if (!impl->f || !impl->f.isDirectory()) {
        if (impl->f) impl->f.close();
        delete impl;
        return File();
    }
#else
    if (!impl->f.open(path, O_READ)) {
        delete impl;
        return File();
    }
    if (!impl->f.isDir()) {
        impl->f.close();
        delete impl;
        return File();
    }
#endif
    return File(impl);
}

}  // namespace pm_storage
