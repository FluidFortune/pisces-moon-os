# ELF API v1.1 — SPI Bus Treaty Compliance

## What Changed

ELF modules now have a safe, treaty-compliant way to access the SD card.

### Before (v1.0)

ELF modules called `ctx->sd->open()` directly. This bypassed the firmware's
`spi_mutex`, which means an ELF doing SD I/O could collide with:
- The Ghost Engine wardrive task on Core 0
- Active LoRa transmission
- Any other firmware-side SPI operation

Symptoms: random reboots, corrupted data, or worse, silent SPI bus
arbitration failures that surface hours later.

### After (v1.1)

ElfContext now exposes nine SPI-safe SD helper functions:

```cpp
int  (*sd_open_read)(const char* path);
int  (*sd_open_write)(const char* path, bool append);
int  (*sd_read)(int handle, void* buf, int len);
int  (*sd_write)(int handle, const void* buf, int len);
void (*sd_close)(int handle);
bool (*sd_exists)(const char* path);
bool (*sd_mkdir)(const char* path);
bool (*sd_remove)(const char* path);
int  (*sd_size)(const char* path);
```

Each helper internally takes `spi_mutex` before any bus access, blocks if
a firmware-side operation holds the lock, and releases on completion.
**Treaty violations are now impossible by construction.**

ElfContext also now exposes `spi_mutex_ptr` directly for ELF authors who
need finer-grained control (e.g. holding the mutex across multiple
operations). Use this carefully — long mutex holds starve the wardrive
task.

## Migration

### For ELF authors

**Old code:**
```cpp
extern "C" int elf_main(void* ctx_ptr) {
    ElfContext* ctx = (ElfContext*)ctx_ptr;

    // UNSAFE — bypasses spi_mutex
    FsFile f = ctx->sd->open("/mydata.txt", O_READ);
    if (f) {
        char buf[64];
        f.read(buf, 64);
        f.close();
    }
}
```

**New code:**
```cpp
extern "C" int elf_main(void* ctx_ptr) {
    ElfContext* ctx = (ElfContext*)ctx_ptr;

    // Check API version supports the helpers
    if (ctx->api_minor < 1) {
        // Fall back to legacy approach with manual mutex
        if (ctx->spi_mutex_ptr &&
            xSemaphoreTake(*ctx->spi_mutex_ptr, pdMS_TO_TICKS(1000)) == pdTRUE) {
            FsFile f = ctx->sd->open("/mydata.txt", O_READ);
            if (f) { /* read */ f.close(); }
            xSemaphoreGive(*ctx->spi_mutex_ptr);
        }
        return 0;
    }

    // v1.1+ — use safe helpers
    int h = ctx->sd_open_read("/mydata.txt");
    if (h >= 0) {
        char buf[64];
        ctx->sd_read(h, buf, 64);
        ctx->sd_close(h);
    }
    return 0;
}
```

### For existing v1.0 ELF modules

They continue to work — `ctx->sd` is still populated. But they're treaty
non-compliant. They should migrate to the helpers when convenient.

The `retro_elf_pack.cpp` launcher itself runs in firmware context with
direct mutex access, so it doesn't need migration. The actual emulator
ELFs (NES, GB, Atari) will need to be rebuilt against the new ABI when
they're updated.

## Manifest Update

ELF modules using v1.1 features should declare it in their JSON manifest:

```json
{
  "name":     "My App",
  "elf":      "my_app.elf",
  "psram_kb": 256,
  "api":      [1, 1]
}
```

The loader checks `api[0]` (major) for compatibility but not `api[1]`
(minor) — minor version mismatches are non-fatal. v1.0 ELFs work on v1.1
firmware (the new fields are populated and the old `ctx->sd` still works).
v1.1 ELFs will check `ctx->api_minor >= 1` themselves before using helpers.

## Safety Properties

The helpers guarantee:

1. **No bus collision** — every SD operation holds the mutex
2. **No handle leak** — `elf_free_psram()` releases all handles on ELF exit
3. **Bounded mutex hold time** — each helper releases between operations,
   never holds across user code
4. **Graceful degradation** — if mutex acquisition times out, helper
   returns failure code; ELF can retry

## What Still Requires Care

These are NOT covered by the helpers and need manual treaty compliance:

- **LoRa transmit** — ELF modules should not transmit on LoRa directly.
  Use `ctx->wifi_in_use` and `ctx->lora_active` flags to detect when the
  firmware is using the radio. If you must transmit, take `spi_mutex`
  manually around the entire transaction.

- **Direct SPI device access** — if your ELF has its own SPI peripheral,
  take `*ctx->spi_mutex_ptr` before any access.

- **WiFi scanning** — same pattern as the firmware-side wardrive: never
  hold the mutex during the scan itself, only during result writes.

## Files Changed

- `include/elf_loader.h` — ElfContext extended with v1.1 fields
- `src/elf_loader.cpp` — Helper implementations + handle table
- API version bumped from `1.0` to `1.1`

## Testing

After flashing, verify:

1. **Existing ELF modules still load** — retro_elf_pack should still find
   and launch any installed `.elf` files
2. **Random reboots during wardrive + ELF launch** should disappear
3. **Long wardrive sessions** with concurrent ELF use should remain stable
4. **`ctx->api_minor` reads as 1** in any new ELF you write

If you wrote any ELF modules using `ctx->sd->open()` directly, they will
work but should be migrated to use the helpers when you next rebuild.
