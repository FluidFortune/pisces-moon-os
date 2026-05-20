// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  PISCES MOON OS — CARDPUTER ADV LAUNCHER
//
//  Single-row horizontal side-scrolling launcher for the
//  M5Stack Cardputer ADV (240x135 ST7789V2 IPS landscape).
//
//  Unlike the T-Deck Plus and T-LoRa Pager launchers (which use a
//  two-level grid: category page -> app page), the Cardputer has a
//  flat list of all apps ordered by category. There is no
//  drill-down. The category color of each icon's background tells
//  you implicitly where you are in the ordering.
//
//  Apps excluded on Cardputer (per project scope decision):
//    - DOOM       (keyboard-only Doom on a 240x135 display is a
//                  self-imposed punishment, not a feature)
//    - SIMCITY    (already-barely-playable at 320x240 becomes
//                  unreadable at 240x135 with 1-px residential
//                  zones)
//
//  All other apps are reachable on this launcher. The CYBER apps
//  reflow with column-pruned displays and detail sub-screens; the
//  emulators (GB, C64, Atari 2600, 7800-experimental) each own
//  their own scaler stage. See CARDPUTER_PORT_PLAN.md for the
//  full reflow plan.
//
//  Input contract:
//    Left arrow  / Right arrow  Move selection
//    Enter                      Launch focused app
//    Q / Esc                    Back from app list; no-op at top level
//    1..7                       Jump to start of category
//                               (COMMS, CYBER, TOOLS, GAMES,
//                                INTEL, MEDIA, SYSTEM)
//    SYSTEM -> SLEEP            Display off, low power
//
//  The launcher does not start the Ghost Engine, does not touch
//  WiFi state, does not arm radios. Those are owned by the apps
//  themselves and persist across launcher returns. Wardrive
//  remains active when its app exits — that is the entire point
//  of the Ghost Engine.
// ─────────────────────────────────────────────

#ifdef DEVICE_CARDPUTER_ADV

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <TinyGPSPlus.h>
#include "cardputer_ui.h"
#include "theme.h"
#include "keyboard.h"
#include "pm_input.h"
#include "pm_power.h"

extern Arduino_GFX *gfx;
extern TinyGPSPlus gps;
extern bool g_sd_ready;
extern bool wardrive_active;

// Forward declarations — these app entry points are defined across
// the codebase. Same function names as the Pager and T-Deck Plus
// launchers, so the apps don't need any changes to their externally
// visible API to be reachable from this launcher.
extern void run_wifi_connect();
extern void run_gps();
extern void run_mesh_messenger();
extern void run_voice_terminal();
extern void run_lora_voice();
extern void run_wardrive();
extern void runBluetoothApp();
extern void run_pkt_sniffer();
extern void run_beacon_spotter();
extern void run_net_scanner();
extern void run_hash_tool();
extern void run_ble_gatt_explorer();
extern void run_wpa_handshake();
extern void run_rf_spectrum();
extern void run_probe_intel();
extern void run_offline_pkt_analysis();
extern void run_ble_ducky();
extern void run_usb_ducky();
extern void run_wifi_ducky();
extern void run_notepad();
extern void run_calculator();
extern void run_clock();
extern void run_calendar();
extern void run_etch();
extern void run_snake();
extern void run_pacman();
extern void run_galaga();
extern void run_tetris();
extern void run_pole_position();
extern void run_chess();
extern void run_retro_pack();
extern void run_terminal();
extern void run_data_reader(const char *folder, const char *title);
extern void run_baseball();
extern void run_trails();
extern void run_ssh_client();
extern void run_audio_player();
extern void run_audio_recorder();
extern void keymoteEnter();
extern bool keymoteLoopOnce();
extern void keymoteExit();
extern void run_filesystem();
extern void run_wifi_filemgr();
extern void run_about();
extern void run_system();
extern void run_micropython();
extern void run_elf_browser();
extern void run_gamepad_setup();
extern void run_bridge();
extern bool readPagerBattery(int &percent, uint16_t &mv);

// Local wrappers (matching the Pager pattern for app entry points
// that need argument binding).
static void run_gemini_log() { run_data_reader("gemini", "GEMINI LOG"); }
static void run_ref_med()    { run_data_reader("medical", "MEDICAL REF"); }
static void run_ref_surv()   { run_data_reader("survival", "SURVIVAL"); }
static void run_sleep()      { pm_power_sleep(); }
static void run_ble_keymote() {
    keymoteEnter();
    while (!keymoteLoopOnce()) {
        delay(10);
        yield();
    }
    keymoteExit();
}

// ─────────────────────────────────────────────
//  App table
//
//  Flat sequence of all apps in category order. Each entry knows
//  its category (used to color the icon background and to support
//  the 1..7 number-key category-jump shortcut), its display name,
//  a single-character glyph, and the entry point function.
//
//  Order within a category matches the existing T-Deck and Pager
//  launchers as closely as possible, so muscle memory transfers.
// ─────────────────────────────────────────────

struct CpAppEntry {
    const char *name;       // displayed label (max 14 chars to fit)
    const char *glyph;      // 1-char icon glyph
    int category;           // 0=COMMS, 1=CYBER, 2=TOOLS, 3=GAMES,
                            // 4=INTEL, 5=MEDIA, 6=SYSTEM
    void (*run)();
};

// Category metadata — index matches the `category` field above.
struct CpCategoryMeta {
    const char *name;
    uint16_t color;         // RGB565 icon background color
};

static const CpCategoryMeta CATEGORIES[] = {
    {"COMMS",  0x03EF},
    {"CYBER",  0xF400},
    {"TOOLS",  0xFD20},
    {"GAMES",  0x07E0},
    {"INTEL",  0x001F},
    {"MEDIA",  0xF81F},
    {"SYSTEM", 0x8410},
};
static constexpr int NUM_CATEGORIES = (int)(sizeof(CATEGORIES) / sizeof(CATEGORIES[0]));

static const CpAppEntry APPS[] = {
    // ─── COMMS ──────────────────────────────
    {"WIFI JOIN",  "W", 0, run_wifi_connect},
    {"GPS",        "*", 0, run_gps},
    {"MESH",       "m", 0, run_mesh_messenger},
    {"VOICE",      "V", 0, run_voice_terminal},
    {"LORA PTT",   "P", 0, run_lora_voice},

    // ─── CYBER (14 apps — all reflowed) ─────
    {"WARDRIVE",   "@", 1, run_wardrive},
    {"BT RADAR",   "b", 1, runBluetoothApp},
    {"PKT SNIFF",  "#", 1, run_pkt_sniffer},
    {"BEACON",     "~", 1, run_beacon_spotter},
    {"NET SCAN",   "N", 1, run_net_scanner},
    {"HASH TOOL",  "H", 1, run_hash_tool},
    {"GATT XPLR",  "g", 1, run_ble_gatt_explorer},
    {"WPA HS",     "h", 1, run_wpa_handshake},
    {"RF SPECTRM", "^", 1, run_rf_spectrum},
    {"PROBE INTL", "?", 1, run_probe_intel},
    {"PKT ANLYS",  "%", 1, run_offline_pkt_analysis},
    {"BLE DUCKY",  "D", 1, run_ble_ducky},
    {"USB DUCKY",  "U", 1, run_usb_ducky},
    {"WIFI DUCKY", "F", 1, run_wifi_ducky},

    // ─── TOOLS ──────────────────────────────
    {"JOURNAL",    "J", 2, run_notepad},
    {"CALC",       "=", 2, run_calculator},
    {"CLOCK",      "o", 2, run_clock},
    {"CALENDAR",   "c", 2, run_calendar},
    {"ETCH",       "/", 2, run_etch},

    // ─── GAMES (Doom/SimCity excluded on CP) ─
    {"SNAKE",      "s", 3, run_snake},
    {"PAC-MAN",    "p", 3, run_pacman},
    {"GALAGA",     "G", 3, run_galaga},
    {"TETRIS",     "t", 3, run_tetris},
    {"POLE",       "P", 3, run_pole_position},
    {"CHESS",      "K", 3, run_chess},
    {"RETRO",      "R", 3, run_retro_pack},

    // ─── INTEL ──────────────────────────────
    {"TERMINAL",   ">", 4, run_terminal},
    {"GEMINI LOG", "&", 4, run_gemini_log},
    {"REF: MED",   "+", 4, run_ref_med},
    {"REF: SURV",  "x", 4, run_ref_surv},
    {"BASEBALL",   "B", 4, run_baseball},
    {"TRAILS",     "T", 4, run_trails},
    {"SSH",        "$", 4, run_ssh_client},

    // ─── MEDIA ──────────────────────────────
    {"PLAYER",     ">", 5, run_audio_player},
    {"RECORDER",   "0", 5, run_audio_recorder},
    {"KEYMOTE",    "k", 5, run_ble_keymote},

    // ─── SYSTEM ─────────────────────────────
    {"FILES",      "f", 6, run_filesystem},
    {"SD FILES",   "d", 6, run_wifi_filemgr},
    {"ABOUT",      "i", 6, run_about},
    {"SYSTEM",     "S", 6, run_system},
    {"uPY REPL",   "y", 6, run_micropython},
    {"ELF APPS",   "E", 6, run_elf_browser},
    {"GAMEPAD",    "j", 6, run_gamepad_setup},
    {"BRIDGE",     "<", 6, run_bridge},
    {"SLEEP",      "z", 6, run_sleep},
};
static constexpr int NUM_APPS = (int)(sizeof(APPS) / sizeof(APPS[0]));

// Find the index of the first app in each category. Used by the
// 1..7 number-key shortcut. Computed once on first draw.
static int categoryFirstIndex[NUM_CATEGORIES];
static bool categoryIndexComputed = false;

static void computeCategoryFirstIndex() {
    if (categoryIndexComputed) return;
    for (int c = 0; c < NUM_CATEGORIES; c++) categoryFirstIndex[c] = -1;
    for (int i = 0; i < NUM_APPS; i++) {
        int c = APPS[i].category;
        if (c >= 0 && c < NUM_CATEGORIES && categoryFirstIndex[c] == -1) {
            categoryFirstIndex[c] = i;
        }
    }
    categoryIndexComputed = true;
}


// ─────────────────────────────────────────────
//  TWO-LEVEL LAUNCHER STATE
//
//  The Cardputer launcher operates in two modes:
//
//    LAUNCHER_MODE_CATEGORY — top-level horizontal scroll
//      through the 7 category tiles. ENTER drills into the
//      focused category. This is the initial mode.
//
//    LAUNCHER_MODE_APP_LIST — vertical list of apps within
//      the currently-selected category. ENTER launches the
//      focused app. DEL/BACKSPACE/Q returns to the category
//      level. Top-level Q is intentionally a no-op: sleep is the
//      explicit SYSTEM -> SLEEP launcher item.
//
//  Selection state persists per-category: returning to a
//  category preserves which app was previously focused.
// ─────────────────────────────────────────────

enum CpLauncherMode { LAUNCHER_MODE_CATEGORY, LAUNCHER_MODE_APP_LIST };
static CpLauncherMode launcherMode = LAUNCHER_MODE_CATEGORY;

// Index of focused category at the top level
static int focusedCategory = 0;

// Per-category app-list index (preserved across drill-downs)
static int categoryAppIndex[NUM_CATEGORIES] = {0};

// Compute how many apps belong to each category.
static int countAppsInCategory(int cat) {
    int n = 0;
    for (int i = 0; i < NUM_APPS; i++) {
        if (APPS[i].category == cat) n++;
    }
    return n;
}

// Translate an in-category index (0..N-1) to an absolute APPS[] index.
static int categoryAppToGlobal(int cat, int localIdx) {
    int seen = 0;
    for (int i = 0; i < NUM_APPS; i++) {
        if (APPS[i].category == cat) {
            if (seen == localIdx) return i;
            seen++;
        }
    }
    return -1;
}

// ─────────────────────────────────────────────
//  Geometry constants
// ─────────────────────────────────────────────
#define CP_HDR_H        14
#define CP_FTR_H        12
#define CP_FTR_Y        (135 - CP_FTR_H)
#define CP_BODY_TOP     CP_HDR_H
#define CP_BODY_BOTTOM  CP_FTR_Y
#define CP_DISP_W       240

// Category-screen tile geometry
//   Three tiles visible: previous (peeking), focused (centered), next (peeking)
//   Focused tile: 96×60 centered horizontally
//   Side tiles:   58×40 peeking from left/right edges
#define CAT_TILE_W      96
#define CAT_TILE_H      60
#define CAT_TILE_Y      28
#define CAT_SIDE_W      58
#define CAT_SIDE_H      40
#define CAT_SIDE_Y      (CAT_TILE_Y + (CAT_TILE_H - CAT_SIDE_H) / 2)

// App-list row geometry
#define APP_ROW_H       12
#define APP_ROW_TOP     (CP_HDR_H + 4)

// Header / footer colors
#define CP_HDR_BG       0x10A2
#define CP_HDR_FG       C_GREEN
#define CP_FTR_BG       0x10A2
#define CP_FTR_FG       0x8410
#define CP_BG           0x0000

// ─────────────────────────────────────────────
//  Header / footer
// ─────────────────────────────────────────────

static void drawCpHeader(const char *title, uint16_t titleColor) {
    gfx->fillRect(0, 0, CP_DISP_W, CP_HDR_H, CP_HDR_BG);
    gfx->drawFastHLine(0, CP_HDR_H - 1, CP_DISP_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(titleColor);
    gfx->setCursor(4, 4);
    gfx->print(title);

    // Right-aligned: battery percent + Ghost Engine indicator
    int rx = CP_DISP_W - 4;

    int bat = 0;
    uint16_t mv = 0;
    char batBuf[8];
    if (readPagerBattery(bat, mv)) {
        snprintf(batBuf, sizeof(batBuf), "%d%%", bat);
        rx -= (int)strlen(batBuf) * 6;
        gfx->setTextColor(bat > 50 ? C_GREEN : bat > 20 ? 0xFD20 : C_RED);
        gfx->setCursor(rx, 4);
        gfx->print(batBuf);
        rx -= 6;
    }

    const char *ge = wardrive_active ? "GE" : "--";
    rx -= (int)strlen(ge) * 6;
    gfx->setTextColor(wardrive_active ? C_GREEN : C_GREY);
    gfx->setCursor(rx, 4);
    gfx->print(ge);
}

static void drawCpFooter(const char *hint) {
    gfx->fillRect(0, CP_FTR_Y, CP_DISP_W, CP_FTR_H, CP_FTR_BG);
    gfx->drawFastHLine(0, CP_FTR_Y, CP_DISP_W, 0x2104);
    gfx->setTextSize(1);
    gfx->setTextColor(CP_FTR_FG);
    gfx->setCursor(4, CP_FTR_Y + 3);
    gfx->print(hint);
}

// ─────────────────────────────────────────────
//  CATEGORY SCREEN (top-level)
// ─────────────────────────────────────────────

static void drawCatTile(int cat, int x, int y, int w, int h, bool focused) {
    const CpCategoryMeta &meta = CATEGORIES[cat];
    uint16_t fill   = focused ? meta.color : 0x2104;
    uint16_t border = focused ? C_WHITE     : meta.color;
    uint16_t fg     = focused ? C_BLACK     : meta.color;

    gfx->fillRect(x, y, w, h, fill);
    gfx->drawRect(x, y, w, h, border);

    int textSize = focused ? 2 : 1;
    int charW    = focused ? 12 : 6;
    int nameLen  = (int)strlen(meta.name);
    int nameX    = x + (w - nameLen * charW) / 2;
    int nameY    = focused ? (y + h / 2 - 12) : (y + h / 2 - 8);

    gfx->setTextSize(textSize);
    gfx->setTextColor(fg);
    gfx->setCursor(nameX, nameY);
    gfx->print(meta.name);

    // App count under the name
    int count = countAppsInCategory(cat);
    char cntBuf[12];
    snprintf(cntBuf, sizeof(cntBuf), "%d apps", count);
    int cntLen = (int)strlen(cntBuf);
    gfx->setTextSize(1);
    gfx->setTextColor(focused ? C_BLACK : meta.color);
    gfx->setCursor(x + (w - cntLen * 6) / 2,
                   focused ? (nameY + 18) : (nameY + 10));
    gfx->print(cntBuf);
}

static void drawCategoryScreen() {
    gfx->fillRect(0, CP_BODY_TOP, CP_DISP_W,
                  CP_BODY_BOTTOM - CP_BODY_TOP, CP_BG);
    drawCpHeader("PISCES MOON", C_GREEN);

    // Center focused tile
    int focusedX = (CP_DISP_W - CAT_TILE_W) / 2;
    drawCatTile(focusedCategory, focusedX, CAT_TILE_Y,
                CAT_TILE_W, CAT_TILE_H, true);

    // Left peeking tile (previous category)
    int prevCat = (focusedCategory - 1 + NUM_CATEGORIES) % NUM_CATEGORIES;
    drawCatTile(prevCat, -CAT_SIDE_W / 2, CAT_SIDE_Y,
                CAT_SIDE_W, CAT_SIDE_H, false);

    // Right peeking tile (next category)
    int nextCat = (focusedCategory + 1) % NUM_CATEGORIES;
    drawCatTile(nextCat, CP_DISP_W - CAT_SIDE_W / 2, CAT_SIDE_Y,
                CAT_SIDE_W, CAT_SIDE_H, false);

    // Position indicator under the focused tile
    char posBuf[24];
    snprintf(posBuf, sizeof(posBuf), "%d of %d",
             focusedCategory + 1, NUM_CATEGORIES);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREY);
    int posLen = (int)strlen(posBuf);
    gfx->setCursor((CP_DISP_W - posLen * 6) / 2,
                   CAT_TILE_Y + CAT_TILE_H + 4);
    gfx->print(posBuf);

    drawCpFooter("arrows  ENT open  1-7 jump");
}

// ─────────────────────────────────────────────
//  APP LIST SCREEN (per-category)
// ─────────────────────────────────────────────

static int appListRowsVisible() {
    // Body area: y=CP_BODY_TOP+4 to y=CP_FTR_Y-2
    int avail = (CP_FTR_Y - 2) - (CP_BODY_TOP + 4);
    return avail / APP_ROW_H;
}

static void drawAppListScreen() {
    gfx->fillRect(0, CP_BODY_TOP, CP_DISP_W,
                  CP_BODY_BOTTOM - CP_BODY_TOP, CP_BG);

    const CpCategoryMeta &meta = CATEGORIES[focusedCategory];
    drawCpHeader(meta.name, meta.color);

    int total = countAppsInCategory(focusedCategory);
    int sel   = categoryAppIndex[focusedCategory];
    if (sel >= total) sel = total - 1;
    if (sel < 0) sel = 0;
    categoryAppIndex[focusedCategory] = sel;

    int rows = appListRowsVisible();
    int firstVisible = 0;
    if (sel >= rows) firstVisible = sel - rows + 1;

    int y = APP_ROW_TOP;
    for (int row = 0; row < rows; row++) {
        int localIdx = firstVisible + row;
        if (localIdx >= total) break;
        int globalIdx = categoryAppToGlobal(focusedCategory, localIdx);
        if (globalIdx < 0) break;

        const CpAppEntry &app = APPS[globalIdx];
        bool focused = (localIdx == sel);

        if (focused) {
            gfx->fillRect(0, y - 1, CP_DISP_W, APP_ROW_H, meta.color);
            gfx->setTextColor(C_BLACK);
        } else {
            gfx->setTextColor(C_WHITE);
        }

        gfx->setTextSize(1);
        gfx->setCursor(8, y + 2);
        gfx->print("> ");
        gfx->print(app.name);

        y += APP_ROW_H;
    }

    // Scroll indicator if list is taller than the visible window
    if (total > rows) {
        char scrollBuf[16];
        snprintf(scrollBuf, sizeof(scrollBuf), "%d/%d",
                 sel + 1, total);
        int sLen = (int)strlen(scrollBuf);
        gfx->setTextSize(1);
        gfx->setTextColor(C_GREY);
        gfx->setCursor(CP_DISP_W - sLen * 6 - 4, CP_FTR_Y - 10);
        gfx->print(scrollBuf);
    }

    drawCpFooter("arrows  ENT launch  DEL/Q back");
}

// ─────────────────────────────────────────────
//  Action helpers
// ─────────────────────────────────────────────

static bool isCpBackKey(char k) {
    return k == PM_KEY_BACKSPACE || k == PM_KEY_DEL ||
           k == 8 || k == 127 || k == 'q' || k == 'Q' ||
           k == PM_KEY_ESC;
}

static void launchFocusedApp() {
    int local = categoryAppIndex[focusedCategory];
    int global = categoryAppToGlobal(focusedCategory, local);
    if (global < 0) return;
    if (!APPS[global].run) return;
    APPS[global].run();
    // App returned. Redraw whichever screen we were on.
    if (launcherMode == LAUNCHER_MODE_APP_LIST) {
        drawAppListScreen();
    } else {
        drawCategoryScreen();
    }
}

// ─────────────────────────────────────────────
//  Launcher-only key translation
//
//  On the launcher screens, the unshifted punctuation keys
//  ',', '.', ';', '/' map to arrow events as if Fn were
//  held. This is the launcher's affordance — apps themselves
//  still see the raw characters for typing.
// ─────────────────────────────────────────────

static char launcherKeyTranslate(char k) {
    switch (k) {
        case ',': return PM_KEY_LEFT;
        case '/': return PM_KEY_RIGHT;
        case ';': return PM_KEY_UP;
        case '.': return PM_KEY_DOWN;
        default:  return k;
    }
}

// ─────────────────────────────────────────────
//  Public entry point — called from main.cpp loop()
// ─────────────────────────────────────────────

void run_launcher() {
    computeCategoryFirstIndex();
    launcherMode = LAUNCHER_MODE_CATEGORY;
    drawCategoryScreen();

    uint32_t lastHeaderRefresh = millis();

    while (true) {
        // Periodic header refresh — battery / GE status / clock
        // can change while the launcher is idle.
        uint32_t now = millis();
        if (now - lastHeaderRefresh > 1500) {
            if (launcherMode == LAUNCHER_MODE_CATEGORY) {
                drawCpHeader("PISCES MOON", C_GREEN);
            } else {
                drawCpHeader(CATEGORIES[focusedCategory].name,
                             CATEGORIES[focusedCategory].color);
            }
            lastHeaderRefresh = now;
        }

        char raw = get_keypress();
        if (raw == 0) {
            delay(20);
            yield();
            continue;
        }

        // Translate launcher-mode unshifted punctuation to arrows
        char k = launcherKeyTranslate(raw);

        // ── Per-mode input handling ──
        if (launcherMode == LAUNCHER_MODE_CATEGORY) {
            // Top-level: navigate categories, ENTER drills in

            // Q/Esc at top level means "already home"; do nothing.
            if (isCpBackKey(k)) {
                continue;
            }

            // Left/right with arrow keys OR the D-pad fallback (A/D, H/L)
            if (k == PM_KEY_LEFT || k == 'a' || k == 'A' ||
                k == 'h' || k == 'H') {
                focusedCategory = (focusedCategory - 1 + NUM_CATEGORIES)
                                  % NUM_CATEGORIES;
                drawCategoryScreen();
                continue;
            }
            if (k == PM_KEY_RIGHT || k == 'd' || k == 'D' ||
                k == 'l' || k == 'L') {
                focusedCategory = (focusedCategory + 1) % NUM_CATEGORIES;
                drawCategoryScreen();
                continue;
            }

            // ENTER drills into the focused category
            if (k == '\r' || k == '\n' || k == PM_KEY_ENTER) {
                launcherMode = LAUNCHER_MODE_APP_LIST;
                drawAppListScreen();
                continue;
            }

            // Number keys 1..7 jump directly to a category at the top level
            if (k >= '1' && k <= '7') {
                int target = k - '1';
                if (target >= 0 && target < NUM_CATEGORIES) {
                    focusedCategory = target;
                    drawCategoryScreen();
                }
                continue;
            }

            Serial.printf("[LAUNCHER/CAT] Unhandled key: 0x%02X (%d)\n",
                          (unsigned char)k, (int)k);
            continue;
        }

        // ── LAUNCHER_MODE_APP_LIST ──
        // Up/Down navigates app list, ENTER launches, DEL/BACKSPACE goes back

        // Up
        if (k == PM_KEY_UP || k == 'w' || k == 'W' ||
            k == 'k' || k == 'K') {
            int total = countAppsInCategory(focusedCategory);
            if (total <= 0) continue;
            int sel = categoryAppIndex[focusedCategory];
            sel = (sel - 1 + total) % total;
            categoryAppIndex[focusedCategory] = sel;
            drawAppListScreen();
            continue;
        }

        // Down
        if (k == PM_KEY_DOWN || k == 's' || k == 'S' ||
            k == 'j' || k == 'J') {
            int total = countAppsInCategory(focusedCategory);
            if (total <= 0) continue;
            int sel = categoryAppIndex[focusedCategory];
            sel = (sel + 1) % total;
            categoryAppIndex[focusedCategory] = sel;
            drawAppListScreen();
            continue;
        }

        // Left/Right at app-list level: switch category sideways
        // (preserves the per-category app index on each side).
        if (k == PM_KEY_LEFT || k == 'a' || k == 'A' ||
            k == 'h' || k == 'H') {
            focusedCategory = (focusedCategory - 1 + NUM_CATEGORIES)
                              % NUM_CATEGORIES;
            drawAppListScreen();
            continue;
        }
        if (k == PM_KEY_RIGHT || k == 'd' || k == 'D' ||
            k == 'l' || k == 'L') {
            focusedCategory = (focusedCategory + 1) % NUM_CATEGORIES;
            drawAppListScreen();
            continue;
        }

        // ENTER launches the focused app
        if (k == '\r' || k == '\n' || k == PM_KEY_ENTER) {
            launchFocusedApp();
            lastHeaderRefresh = millis();
            continue;
        }

        // DEL / BACKSPACE returns to the category screen
        if (isCpBackKey(k)) {
            launcherMode = LAUNCHER_MODE_CATEGORY;
            drawCategoryScreen();
            lastHeaderRefresh = millis();
            continue;
        }

        Serial.printf("[LAUNCHER/APP] Unhandled key: 0x%02X (%d)\n",
                      (unsigned char)k, (int)k);
    }
}

#endif // DEVICE_CARDPUTER_ADV
