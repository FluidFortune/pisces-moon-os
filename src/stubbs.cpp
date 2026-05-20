// Retired placeholder: real implementations now live in
// voice_terminal.cpp, lora_voice.cpp, and retro_elf_pack.cpp.
#if 0
// ============================================================
//  stubs.cpp — Pisces Moon OS v0.9.6
//
//  Provides linker-satisfying definitions for three apps whose
//  full implementations are pending:
//
//    run_voice_terminal()  — STT + Gemini + TTS (needs cloud API work)
//    run_lora_voice()      — LoRa Codec2 PTT   (needs ESP32_Codec2 lib)
//    run_retro_pack()      — ROM browser + ELF  (needs emulator ELFs on SD)
//
//  Also defines the SPI Bus Treaty flag:
//    volatile bool lora_voice_active
//  which elf_loader.cpp declares extern and links against.
//
//  Each stub shows a styled placeholder screen and exits cleanly.
//  Replace individual functions here with full .cpp files when ready;
//  remove the stub entry from this file at that point.
//
// ============================================================

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"

extern Arduino_GFX* gfx;

// ============================================================
//  SPI BUS TREATY FLAG
//  Declared extern in lora_voice.h and elf_loader.h.
//  Defined exactly once, here.
//  Set true during LoRa voice TX/RX to suspend SD card writes.
// ============================================================
volatile bool lora_voice_active = false;

// ============================================================
//  SHARED STUB UI
// ============================================================
static void draw_stub_screen(const char* title, const char* line1,
                              const char* line2, uint16_t accent) {
    gfx->fillScreen(C_BLACK);

    // Header
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 23, 320, accent);
    gfx->setTextSize(1);
    gfx->setTextColor(accent);
    int tw = strlen(title) * 6;
    gfx->setCursor((320 - tw) / 2, 8);
    gfx->print(title);

    // Icon area
    gfx->setTextSize(4);
    gfx->setTextColor(accent & 0x39E7); // dimmed
    gfx->setCursor(136, 60);
    gfx->print("?");

    // Status lines
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREY);
    int w1 = strlen(line1) * 6;
    gfx->setCursor((320 - w1) / 2, 130);
    gfx->print(line1);

    gfx->setTextColor(C_DARK + 0x2104);
    int w2 = strlen(line2) * 6;
    gfx->setCursor((320 - w2) / 2, 148);
    gfx->print(line2);

    // Footer
    gfx->fillRect(0, 210, 320, 30, C_DARK);
    gfx->drawFastHLine(0, 210, 320, accent & 0x2104);
    gfx->setTextColor(accent & 0x39E7);
    gfx->setCursor(72, 220);
    gfx->print("Any key / tap to return");
}

static void stub_wait_exit() {
    while (true) {
        if (get_keypress()) return;
        TrackballState tb = update_trackball();
        if (tb.clicked || tb.x == -1) return;
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); }
            return;
        }
        delay(30);
    }
}


}

// ============================================================
//  run_retro_pack()
//  Full implementation: ROM browser + NES/GB/Atari ELF launcher.
//  Requires: emulator ELF files on SD card:
//    /apps/nofrendo.elf   (NES)
//    /apps/gnuboy.elf     (Game Boy)
//    /apps/stella.elf     (Atari 2600)
//  And ROM files in /roms/nes/, /roms/gb/, /roms/atari/.
//  See PISCES_MOON_MANUAL_ADDENDUM_ELF.md for build instructions.
// ============================================================
void run_retro_pack() {
    draw_stub_screen(
        "RETRO PACK",
        "No emulator ELFs found on SD",
        "Copy nofrendo/gnuboy/stella.elf to /apps/",
        0x07E0   // GAMES green
    );
    stub_wait_exit();
}
#endif
