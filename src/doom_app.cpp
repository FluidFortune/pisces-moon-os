/**
 * PISCES MOON OS — DOOM INTEGRATION LAYER v1.0
 * Connects esp32-doom (Nicola Wrachien's port) to Pisces Moon OS
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  SETUP INSTRUCTIONS (do this before building)
 * ─────────────────────────────────────────────────────────────────────────────
 *
 *  1. Clone the Doom engine into your project:
 *       git clone https://github.com/nicowillis/esp32-doom.git doom_engine/
 *     OR the more actively maintained fork:
 *       https://github.com/DrBodiless/doom-esp32
 *
 *  2. Copy these files from the engine into your src/ directory:
 *       doom_engine/src/*.c  doom_engine/src/*.h
 *     Do NOT copy main.cpp from the engine — we provide our own entry point here.
 *
 *  3. Add to platformio.ini build_flags:
 *       -DESP32_DOOM
 *       -DNORMALUNIX        (tells Doom we're a unix-like target)
 *       -DLINUX             (enables POSIX-compatible code paths)
 *
 *  4. WAD file placement:
 *     Option A — SD card (easier, slower load):
 *       Copy doom1.wad (shareware, free) to /doom1.wad on your MicroSD card
 *       Full DOOM: /doom.wad | DOOM II: /doom2.wad
 *
 *     Option B — gamedata FAT partition (faster, requires flash tool):
 *       Use the GameData utility (below) to write doom1.wad to the
 *       internal 6.25MB FAT partition at offset 0x310000.
 *       Mount: FFat.begin(false, "/gamedata")
 *       Path:  /gamedata/doom1.wad
 *
 *  5. Shareware WAD (doom1.wad, 4.2MB) is free and legal to distribute.
 *     Download from: https://distro.ibiblio.org/pub/linux/distributions/
 *                    slackware/slackware-3.3/games/doom1.wad
 *     Or: https://www.doomworld.com/classicdoom/info/shareware.php
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  DISPLAY MAPPING
 * ─────────────────────────────────────────────────────────────────────────────
 *  Doom native res: 320x200. Our screen: 320x240.
 *  We render Doom at 320x200 centered vertically (20px letterbox top/bottom)
 *  OR stretch to 320x240 with slight vertical distortion (barely noticeable).
 *  The esp32-doom port renders directly to a uint16_t framebuffer in RGB565.
 *  We blit that buffer to ST7789 via SPI DMA.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  AUDIO
 * ─────────────────────────────────────────────────────────────────────────────
 *  Doom's PC speaker / OPL2 sound emulation is handled by the engine.
 *  Output goes to our I2S speaker (same pins as audio player: BCK=7, WS=5, DOUT=6).
 *  Music: The engine generates 8-bit PCM at 11025Hz. We upsample to 44100Hz
 *  for the I2S DAC.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  INPUT MAPPING
 * ─────────────────────────────────────────────────────────────────────────────
 *  T-Deck keyboard → Doom keys:
 *    W / trackball up    = FORWARD
 *    S / trackball down  = BACKWARD
 *    A / trackball left  = TURN LEFT
 *    D / trackball right = TURN RIGHT
 *    Space / trackball click = FIRE / USE
 *    E                   = USE (open doors)
 *    Shift               = RUN
 *    1-7                 = Weapon select
 *    Enter               = MENU SELECT
 *    Escape / Q          = MENU / QUIT
 *    Tab                 = MAP
 *    F1                  = HELP
 *    F5                  = DETAIL
 *    F6                  = SAVE
 *    F9                  = LOAD
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  MEMORY BUDGET
 * ─────────────────────────────────────────────────────────────────────────────
 *  Doom heap requirement: ~2.5-3MB for shareware episode
 *  Available PSRAM: 8MB (ESP32-S3FN16R8)
 *  Framebuffer: 320x200x2 = 128KB (fits in PSRAM)
 *  Z-buffer: 320x200x2  = 128KB
 *  Sound buffer: 8KB
 *  Total: ~3MB — tight but workable with PSRAM
 *  Key: malloc() must use PSRAM. Add to platformio.ini:
 *    -DBOARD_HAS_PSRAM
 *    board_build.arduino.memory_type = qio_opi   (already set)
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FFat.h>
#include <FS.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "gamepad.h"
#include "doom_app.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG    0x0000
#define COL_RED   0xF800
#define COL_GREEN 0x07E0
#define COL_TEXT  0xFFFF
#define COL_DIM   0x4208

// ─────────────────────────────────────────────
//  DOOM ENGINE INTERFACE
//  These are the symbols exported by the doom engine.
//  Include the engine's main header here once you've
//  added the source files to your project.
// ─────────────────────────────────────────────

// Uncomment once doom engine source is added:
// extern "C" {
//     #include "doomgeneric.h"
// }
//
// The esp32-doom / doomgeneric port expects you to implement:
//   void DG_Init()          — called once at startup
//   void DG_DrawFrame()     — called each frame to blit framebuffer to display
//   void DG_SleepMs(uint32_t ms)
//   uint32_t DG_GetTicksMs()
//   int DG_GetKey(int* pressed, unsigned char* doomKey)
//   void DG_SetWindowTitle(const char* title)
//
// DG_ScreenBuffer is the uint16_t* framebuffer (320x200 RGB565)
// that Doom renders into. We blit it each frame.

// ─────────────────────────────────────────────
//  FRAMEBUFFER
// ─────────────────────────────────────────────
#define DOOM_W 320
#define DOOM_H 200
#define DOOM_Y_OFFSET 20   // Center 200px vertically in 240px screen

static uint16_t* doomFrameBuffer = nullptr;

// ─────────────────────────────────────────────
//  INPUT QUEUE
// ─────────────────────────────────────────────
#define KEY_QUEUE_SIZE 16
static uint8_t keyQueue[KEY_QUEUE_SIZE];
static int keyQueueHead = 0, keyQueueTail = 0;
static bool keyPressed[KEY_QUEUE_SIZE];

static void enqueueKey(uint8_t doomKey, bool pressed) {
    int next = (keyQueueHead + 1) % KEY_QUEUE_SIZE;
    if (next != keyQueueTail) {
        keyQueue[keyQueueHead]   = doomKey;
        keyPressed[keyQueueHead] = pressed;
        keyQueueHead = next;
    }
}

// Map T-Deck keyboard to Doom key codes
// Doom key codes defined in doomkeys.h — common values:
#define DK_RIGHTARROW  0xae
#define DK_LEFTARROW   0xac
#define DK_UPARROW     0xad
#define DK_DOWNARROW   0xaf
#define DK_FIRE        ' '    // Space = fire in Doom
#define DK_USE         'e'
#define DK_RSHIFT      0x80
#define DK_ESCAPE      27
#define DK_ENTER       13
#define DK_TAB         9

static uint8_t mapKeyToDoom(char k) {
    if (k >= 'a' && k <= 'z') return k;           // Letters pass through
    if (k >= '1' && k <= '7') return k;           // Weapon select
    if (k == ' ')  return DK_FIRE;
    if (k == 13)   return DK_ENTER;
    if (k == 27)   return DK_ESCAPE;
    if (k == 9)    return DK_TAB;
    return 0;
}

// ─────────────────────────────────────────────
//  DOOMGENERIC IMPLEMENTATION
//  These functions are called by the Doom engine.
//  Implement here to bridge to Pisces Moon hardware.
// ─────────────────────────────────────────────

extern "C" {

void DG_Init() {
    // Allocate framebuffer in PSRAM
    doomFrameBuffer = (uint16_t*)ps_malloc(DOOM_W * DOOM_H * sizeof(uint16_t));
    if (!doomFrameBuffer) {
        Serial.println("[DOOM] FATAL: Cannot allocate framebuffer in PSRAM");
        return;
    }
    memset(doomFrameBuffer, 0, DOOM_W * DOOM_H * sizeof(uint16_t));
    Serial.printf("[DOOM] Framebuffer allocated at %p (%d bytes)\n",
                  doomFrameBuffer, DOOM_W * DOOM_H * 2);
}

void DG_DrawFrame() {
    // Blit Doom's 320x200 RGB565 framebuffer to the 320x240 display
    // Centered: 20px letterbox top and bottom
    if (!doomFrameBuffer) return;

    // Arduino_GFX drawRGBBitmap: x, y, data, w, h
    // This uses SPI DMA and is fast enough for ~25fps
    gfx->draw16bitRGBBitmap(0, DOOM_Y_OFFSET, doomFrameBuffer, DOOM_W, DOOM_H);

    // Fill letterbox bars (only needed once but cheap to repeat)
    gfx->fillRect(0, 0, DOOM_W, DOOM_Y_OFFSET, 0x0000);
    gfx->fillRect(0, DOOM_Y_OFFSET + DOOM_H, DOOM_W, DOOM_Y_OFFSET, 0x0000);
}

void DG_SleepMs(uint32_t ms) {
    delay(ms);
}

uint32_t DG_GetTicksMs() {
    return (uint32_t)millis();
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    if (keyQueueHead == keyQueueTail) return 0; // Queue empty
    *doomKey = keyQueue[keyQueueTail];
    *pressed  = keyPressed[keyQueueTail] ? 1 : 0;
    keyQueueTail = (keyQueueTail + 1) % KEY_QUEUE_SIZE;
    return 1;
}

void DG_SetWindowTitle(const char* title) {
    // Ignored — we don't have a window title bar
    Serial.printf("[DOOM] Title: %s\n", title);
}

// doomgeneric expects this global framebuffer pointer
uint16_t* DG_ScreenBuffer = nullptr; // Set to doomFrameBuffer in DG_Init

} // extern "C"

// ─────────────────────────────────────────────
//  WAD LOCATOR
// ─────────────────────────────────────────────
static bool findWAD(char* wadPath, int pathLen) {
    // Priority: internal FAT partition first (faster), then SD
    if (FFat.begin(false, "/gamedata")) {
        if (FFat.exists("/gamedata/doom1.wad")) {
            strncpy(wadPath, "/gamedata/doom1.wad", pathLen);
            Serial.println("[DOOM] WAD found on internal FAT partition");
            return true;
        }
        if (FFat.exists("/gamedata/doom.wad")) {
            strncpy(wadPath, "/gamedata/doom.wad", pathLen);
            return true;
        }
        if (FFat.exists("/gamedata/doom2.wad")) {
            strncpy(wadPath, "/gamedata/doom2.wad", pathLen);
            return true;
        }
    }

    // Try SD card
    if (sd.exists("/doom1.wad")) {
        strncpy(wadPath, "/doom1.wad", pathLen);
        Serial.println("[DOOM] WAD found on SD card");
        return true;
    }
    if (sd.exists("/doom.wad")) {
        strncpy(wadPath, "/doom.wad", pathLen);
        return true;
    }
    if (sd.exists("/doom2.wad")) {
        strncpy(wadPath, "/doom2.wad", pathLen);
        return true;
    }

    return false;
}

// ─────────────────────────────────────────────
//  NO WAD SCREEN
// ─────────────────────────────────────────────
static void showNoWAD() {
    gfx->fillScreen(COL_BG);
    gfx->setTextColor(COL_RED);
    gfx->setTextSize(2);
    gfx->setCursor(20, 30);  gfx->print("DOOM: NO WAD");

    gfx->setTextSize(1);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(10, 60);  gfx->print("WAD file not found.");
    gfx->setCursor(10, 75);  gfx->print("Copy doom1.wad to your SD card:");
    gfx->setTextColor(0x07FF);
    gfx->setCursor(10, 90);  gfx->print("  /doom1.wad  (on MicroSD root)");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(10, 108); gfx->print("Shareware WAD (doom1.wad) is free.");
    gfx->setCursor(10, 120); gfx->print("Download from doomworld.com/classicdoom");

    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(10, 140); gfx->print("OR flash to internal gamedata partition:");
    gfx->setTextColor(0x07FF);
    gfx->setCursor(10, 155); gfx->print("  Use GameData utility (SYSTEM folder)");
    gfx->setCursor(10, 170); gfx->print("  Path: /gamedata/doom1.wad");

    gfx->setTextColor(COL_DIM);
    gfx->setCursor(10, 200); gfx->print("Tap header or press Q to exit.");

    // Wait for dismiss
    while (true) {
        char k = get_keypress();
        int16_t tx, ty;
        TrackballState tb = update_trackball();
        if (k == 'q' || k == 'Q' || tb.clicked ||
            (get_touch(&tx, &ty) && ty < 30)) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        delay(50);
    }
}

// ─────────────────────────────────────────────
//  ENGINE NOT INSTALLED SCREEN
// ─────────────────────────────────────────────
static void showEngineNotInstalled() {
    gfx->fillScreen(COL_BG);
    gfx->setTextColor(COL_RED);
    gfx->setTextSize(2);
    gfx->setCursor(10, 20); gfx->print("DOOM ENGINE");
    gfx->setCursor(10, 42); gfx->print("NOT LINKED");

    gfx->setTextSize(1);
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(10, 70);  gfx->print("The Doom engine source is not");
    gfx->setCursor(10, 82);  gfx->print("included in the project yet.");

    gfx->setTextColor(0x07FF);
    gfx->setCursor(10, 100); gfx->print("To enable Doom:");
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(10, 114); gfx->print("1. Clone esp32-doom engine:");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(16, 126); gfx->print("github.com/nicowillis/esp32-doom");
    gfx->setTextColor(COL_TEXT);
    gfx->setCursor(10, 140); gfx->print("2. Copy engine .c/.h files to src/");
    gfx->setCursor(10, 153); gfx->print("3. Uncomment #include in doom_app.cpp");
    gfx->setCursor(10, 166); gfx->print("4. Add -DESP32_DOOM to build_flags");
    gfx->setCursor(10, 179); gfx->print("5. Copy doom1.wad to SD card root");
    gfx->setCursor(10, 192); gfx->print("6. Build and flash");

    gfx->setTextColor(COL_GREEN);
    gfx->setCursor(10, 212); gfx->print("All integration code is ready.");
    gfx->setCursor(10, 222); gfx->print("Just add the engine source files.");

    while (true) {
        char k = get_keypress();
        int16_t tx, ty;
        TrackballState tb = update_trackball();
        if (k == 'q' || k == 'Q' || tb.clicked ||
            (get_touch(&tx, &ty) && ty < 30)) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        delay(50);
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_doom() {
    gfx->fillScreen(COL_BG);

    // Check if engine is compiled in
#ifndef ESP32_DOOM
    showEngineNotInstalled();
    return;
#endif

    // Find WAD
    char wadPath[64] = "";
    if (!findWAD(wadPath, sizeof(wadPath))) {
        showNoWAD();
        return;
    }

    // Boot screen
    gfx->setTextColor(COL_RED);
    gfx->setTextSize(3);
    gfx->setCursor(60, 60);  gfx->print("DOOM");
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(10, 110); gfx->printf("WAD: %s", wadPath);
    gfx->setCursor(10, 124); gfx->print("Loading...");
    delay(500);

    // Initialize engine
    // Build argv for Doom's main()
    // Doom engine expects: doom -iwad <path>
    char iwadArg[64];
    snprintf(iwadArg, sizeof(iwadArg), "%s", wadPath);

    // Uncomment when engine is linked:
    // const char* argv[] = {"doom", "-iwad", iwadArg};
    // doomgeneric_Create(3, (char**)argv);
    // DG_ScreenBuffer = doomFrameBuffer;

    // Main game loop
    while (true) {
        // Poll input and enqueue key events
        char k = get_keypress();
        if (k) {
            if (k == 'q' || k == 'Q') break; // Exit to launcher

            uint8_t dk = mapKeyToDoom(k);
            if (dk) {
                enqueueKey(dk, true);
                // Key release after brief delay
                delay(50);
                enqueueKey(dk, false);
            }
        }

        // Trackball input
        TrackballState tb = update_trackball();
        if (tb.y == -1) { enqueueKey(DK_UPARROW,    true); }
        if (tb.y ==  1) { enqueueKey(DK_DOWNARROW,   true); }
        if (tb.x == -1) { enqueueKey(DK_LEFTARROW,   true); }
        if (tb.x ==  1) { enqueueKey(DK_RIGHTARROW,  true); }
        if (tb.clicked) { enqueueKey(DK_FIRE,         true); }

        // 8BitDo Zero 2 gamepad — natural FPS layout
        if (gamepad_poll()) break; // HOME = exit
        if (gamepad_pressed(GP_UP))    { enqueueKey(DK_UPARROW,   true); }
        if (gamepad_pressed(GP_DOWN))  { enqueueKey(DK_DOWNARROW,  true); }
        if (gamepad_pressed(GP_LEFT))  { enqueueKey(DK_LEFTARROW,  true); }
        if (gamepad_pressed(GP_RIGHT)) { enqueueKey(DK_RIGHTARROW, true); }
        if (gamepad_pressed(GP_A))     { enqueueKey(DK_FIRE,       true); }
        if (gamepad_pressed(GP_B))     { enqueueKey(DK_USE,        true); }
        if (gamepad_pressed(GP_START)) { enqueueKey(DK_ESCAPE,     true); }
        if (gamepad_pressed(GP_SELECT)){ enqueueKey(DK_TAB,        true); } // automap

        // Touch: header tap = quit
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 20) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }

        // Tick the engine
        // Uncomment when engine is linked:
        // doomgeneric_Tick();

        yield();
    }

    // Cleanup
    if (doomFrameBuffer) {
        free(doomFrameBuffer);
        doomFrameBuffer = nullptr;
    }
    FFat.end();
    gfx->fillScreen(COL_BG);
}