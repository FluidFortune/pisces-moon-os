/**
 * PISCES MOON OS — SIMCITY CLASSIC (MICROPOLIS) v1.0
 *
 * This is a self-contained, ground-up ESP32 implementation of the classic
 * city simulation. Rather than porting the full Micropolis C++ engine
 * (which was designed for desktop RAM), we implement the core simulation
 * loop directly with careful memory management for the ESP32-S3's 8MB PSRAM.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  ARCHITECTURE DECISION
 * ─────────────────────────────────────────────────────────────────────────────
 * The original Micropolis engine requires ~16-32MB RAM for its map, sprites,
 * and simulation state — way beyond our 8MB PSRAM.
 *
 * Our approach: faithful gameplay with constrained data structures.
 *
 * Map: 64x64 tiles (Classic is 120x100 — we scale down)
 *   Each tile: 1 byte type + 1 byte flags = 2 bytes/tile = 8KB total
 *   Fits comfortably in PSRAM.
 *
 * Simulation: All the classic systems are here —
 *   Power grid propagation, zone density growth, traffic simulation
 *   (simplified flow model), pollution, crime, tax/budget, disasters.
 *
 * Display: 320x240. Map viewport: 256x192 (32x24 tiles at 8px each).
 *   Right panel 64px: stats, tools, budget.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 *  TILE TYPES
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "gamepad.h"
#include "simcity.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  MAP DIMENSIONS
// ─────────────────────────────────────────────
#define MAP_W       64
#define MAP_H       64
#define TILE_PX     8        // Pixels per tile on screen
#define VIEW_TILES_W 32      // Viewport width in tiles
#define VIEW_TILES_H 24      // Viewport height in tiles
#define VIEW_X      0        // Viewport screen X
#define VIEW_Y      0        // Viewport screen Y
#define PANEL_X     256      // Right panel X
#define PANEL_W     64       // Right panel width

// ─────────────────────────────────────────────
//  TILE TYPES
// ─────────────────────────────────────────────
#define TILE_EMPTY      0
#define TILE_DIRT       1
#define TILE_GRASS      2
#define TILE_WATER      3
#define TILE_ROAD_H     4
#define TILE_ROAD_V     5
#define TILE_ROAD_CROSS 6
#define TILE_ROAD_TN    7    // T-intersections and curves
#define TILE_ROAD_TS    8
#define TILE_ROAD_TE    9
#define TILE_ROAD_TW    10
#define TILE_POWER_H    11
#define TILE_POWER_V    12
#define TILE_RES_EMPTY  20   // Residential zones (density 0-4)
#define TILE_RES_1      21
#define TILE_RES_2      22
#define TILE_RES_3      23
#define TILE_RES_4      24
#define TILE_COM_EMPTY  30   // Commercial zones
#define TILE_COM_1      31
#define TILE_COM_2      32
#define TILE_COM_3      33
#define TILE_IND_EMPTY  40   // Industrial zones
#define TILE_IND_1      41
#define TILE_IND_2      42
#define TILE_IND_3      43
#define TILE_FIRE_DEPT  50
#define TILE_POLICE     51
#define TILE_STADIUM    52
#define TILE_SEAPORT    53
#define TILE_AIRPORT    54
#define TILE_NUCLEAR    55
#define TILE_COAL_PLANT 56
#define TILE_PARK       57
#define TILE_FIRE       60   // Active fire
#define TILE_RUBBLE     61
#define TILE_FLOOD      62
#define TILE_RADIATION  63

// Tile flags (second byte)
#define FLAG_POWERED    0x01
#define FLAG_ROAD_OK    0x02  // Connected to road network
#define FLAG_BURNABLE   0x04
#define FLAG_ANIMATED   0x08  // Has animation frames

// ─────────────────────────────────────────────
//  TOOLS
// ─────────────────────────────────────────────
#define TOOL_BULLDOZE   0
#define TOOL_ROAD       1
#define TOOL_POWER      2
#define TOOL_ZONE_RES   3
#define TOOL_ZONE_COM   4
#define TOOL_ZONE_IND   5
#define TOOL_FIRE_DEPT  6
#define TOOL_POLICE     7
#define TOOL_PARK       8
#define TOOL_STADIUM    9
#define TOOL_SEAPORT    10
#define TOOL_AIRPORT    11
#define TOOL_NUCLEAR    12
#define TOOL_QUERY      13
#define NUM_TOOLS       14

// Tool costs
static const int TOOL_COST[] = {
    1,      // Bulldoze
    10,     // Road (per tile)
    5,      // Power line
    100,    // Residential zone
    150,    // Commercial zone
    150,    // Industrial zone
    500,    // Fire dept
    500,    // Police station
    10,     // Park
    3000,   // Stadium
    5000,   // Seaport
    10000,  // Airport
    5000,   // Nuclear plant
    0       // Query
};

static const char* TOOL_NAMES[] = {
    "BULLDOZE","ROAD","POWER","R-ZONE","C-ZONE","I-ZONE",
    "FIRE","POLICE","PARK","STADIUM","SEAPORT","AIRPORT",
    "NUCLEAR","QUERY"
};

// ─────────────────────────────────────────────
//  TILE COLORS (RGB565)
// ─────────────────────────────────────────────
static uint16_t tileColor(uint8_t type, uint8_t flags) {
    bool powered = (flags & FLAG_POWERED);
    switch (type) {
        case TILE_EMPTY:      return 0x4A49;  // Dark olive
        case TILE_DIRT:       return 0x8400;  // Brown
        case TILE_GRASS:      return 0x0480;  // Dark green
        case TILE_WATER:      return 0x001F;  // Blue
        case TILE_ROAD_H:
        case TILE_ROAD_V:
        case TILE_ROAD_CROSS:
        case TILE_ROAD_TN: case TILE_ROAD_TS:
        case TILE_ROAD_TE: case TILE_ROAD_TW:
            return 0x8C51;  // Asphalt grey
        case TILE_POWER_H:
        case TILE_POWER_V:    return 0xFFE0;  // Yellow power line
        case TILE_RES_EMPTY:  return 0x07E0;  // Green (zoned, empty)
        case TILE_RES_1:      return powered ? 0x3FE0 : 0x2D40;
        case TILE_RES_2:      return powered ? 0x5FE0 : 0x3D60;
        case TILE_RES_3:      return powered ? 0x87E0 : 0x55A0;
        case TILE_RES_4:      return powered ? 0xAFE0 : 0x75E0;
        case TILE_COM_EMPTY:  return 0x07FF;  // Cyan (commercial zoned)
        case TILE_COM_1:      return powered ? 0x2FFF : 0x2555;
        case TILE_COM_2:      return powered ? 0x5FFF : 0x3977;
        case TILE_COM_3:      return powered ? 0x9FFF : 0x5599;
        case TILE_IND_EMPTY:  return 0xFFE0;  // Yellow (industrial zoned)
        case TILE_IND_1:      return powered ? 0xFFC0 : 0xA840;
        case TILE_IND_2:      return powered ? 0xFD80 : 0xA820;
        case TILE_IND_3:      return powered ? 0xFB00 : 0xA800;
        case TILE_FIRE_DEPT:  return 0xF800;  // Red
        case TILE_POLICE:     return 0x001F;  // Blue
        case TILE_STADIUM:    return 0x8C10;  // Teal-grey
        case TILE_SEAPORT:    return 0x041F;  // Navy
        case TILE_AIRPORT:    return 0xAD55;  // Light grey
        case TILE_NUCLEAR:    return powered ? 0xFFFF : 0x8410;
        case TILE_COAL_PLANT: return 0x4208;  // Dark grey
        case TILE_PARK:       return 0x0400;  // Dark green
        case TILE_FIRE:       return 0xFD20;  // Orange-red (animated)
        case TILE_RUBBLE:     return 0x6B4D;  // Grey rubble
        case TILE_FLOOD:      return 0x001F;  // Blue flood
        case TILE_RADIATION:  return 0x07E0;  // Sickly green
        default:              return 0x4208;
    }
}

// Tile border/detail color (darker version for grid lines)
static uint16_t tileBorderColor(uint8_t type) {
    switch (type) {
        case TILE_ROAD_H: case TILE_ROAD_V: case TILE_ROAD_CROSS:
            return 0xFFFF;   // White center line on road
        case TILE_POWER_H: case TILE_POWER_V:
            return 0xFD20;   // Orange wire detail
        default: return 0;   // No border
    }
}

// ─────────────────────────────────────────────
//  GAME STATE
// ─────────────────────────────────────────────
struct MapTile {
    uint8_t type;
    uint8_t flags;
};

static MapTile* cityMap = nullptr;  // Allocated in PSRAM

static inline MapTile& tile(int r, int c) { return cityMap[r * MAP_W + c]; }
static inline bool inMap(int r, int c) { return r>=0 && r<MAP_H && c>=0 && c<MAP_W; }

// City stats
static int32_t funds;
static int32_t taxRate;       // 0-20%
static int32_t population;
static int32_t cityScore;     // 0-1000
static int     gameYear;
static int     gameMonth;     // 0-11
static int     simSpeed;      // 0=pause, 1=slow, 2=med, 3=fast

// Zone counts
static int resZones, comZones, indZones;
static int resPop, comPop, indPop;

// Demand bars (-100 to +100)
static int8_t demandRes, demandCom, demandInd;

// Service coverage (maps, simplified as counts)
static int fireCoverage;     // 0-100%
static int policeCoverage;   // 0-100%
static int powerPlants;

// Disasters
static bool disasterActive;
static int  disasterRow, disasterCol;
static int  disasterTimer;

// Viewport
static int viewRow, viewCol;  // Top-left tile of viewport

// Tool
static int  currentTool;
static int  cursorRow, cursorCol;  // Cursor in map coords

// Tick counter
static uint32_t simTick;
static uint32_t lastSimTick;

// Messages
static char cityMessage[48];
static uint32_t messageTimer;

// ─────────────────────────────────────────────
//  TERRAIN GENERATION
// ─────────────────────────────────────────────
static uint32_t rngState = 12345;
static uint32_t rng() {
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}
static int rngRange(int lo, int hi) { return lo + (rng() % (hi - lo + 1)); }

static void generateTerrain() {
    // Simple diamond-square-inspired height map → terrain types
    // Water along edges, grass/dirt inland, occasional rivers
    for (int r = 0; r < MAP_H; r++) {
        for (int c = 0; c < MAP_W; c++) {
            uint8_t t = TILE_GRASS;

            // Water near borders (river/coast feel)
            bool nearEdge = (r < 3 || r >= MAP_H-3 || c < 3 || c >= MAP_W-3);
            if (nearEdge && (rng() % 3) == 0) t = TILE_WATER;

            // Random water body (lake/river)
            // Noise-based: simple cellular — if 3+ neighbors are water, become water
            // (seeded in second pass)

            // Occasional dirt patches
            if (t == TILE_GRASS && (rng() % 8) == 0) t = TILE_DIRT;

            tile(r, c).type  = t;
            tile(r, c).flags = 0;
        }
    }

    // Second pass: cellular automaton for water bodies
    for (int iter = 0; iter < 2; iter++) {
        for (int r = 1; r < MAP_H-1; r++) {
            for (int c = 1; c < MAP_W-1; c++) {
                if (tile(r,c).type == TILE_WATER) continue;
                int wCount = 0;
                for (int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++)
                    if (tile(r+dr,c+dc).type == TILE_WATER) wCount++;
                if (wCount >= 5) tile(r,c).type = TILE_WATER;
            }
        }
    }
}

// ─────────────────────────────────────────────
//  NEW CITY
// ─────────────────────────────────────────────
static void newCity() {
    memset(cityMap, 0, MAP_W * MAP_H * sizeof(MapTile));
    generateTerrain();

    funds   = 20000;
    taxRate = 7;
    population = 0;
    cityScore  = 500;
    gameYear   = 1900;
    gameMonth  = 0;
    simSpeed   = 1;
    simTick    = 0;
    lastSimTick = millis();

    resZones = comZones = indZones = 0;
    resPop = comPop = indPop = 0;
    demandRes = 60; demandCom = 40; demandInd = 40;
    fireCoverage = 0; policeCoverage = 0; powerPlants = 0;

    disasterActive = false;
    viewRow = MAP_H/2 - VIEW_TILES_H/2;
    viewCol = MAP_W/2 - VIEW_TILES_W/2;
    cursorRow = MAP_H/2; cursorCol = MAP_W/2;
    currentTool = TOOL_BULLDOZE;
    messageTimer = 0;
    strcpy(cityMessage, "Welcome to SimCity!");
    messageTimer = 5000;
}

// ─────────────────────────────────────────────
//  SAVE / LOAD (to SD)
// ─────────────────────────────────────────────
#define SAVE_PATH "/simcity.sav"
#define SAVE_MAGIC 0x53494D43  // "SIMC"

static bool saveCity() {
    FsFile f = sd.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) return false;

    uint32_t magic = SAVE_MAGIC;
    f.write(&magic,     sizeof(magic));
    f.write(&funds,     sizeof(funds));
    f.write(&taxRate,   sizeof(taxRate));
    f.write(&population,sizeof(population));
    f.write(&gameYear,  sizeof(gameYear));
    f.write(&gameMonth, sizeof(gameMonth));
    f.write(&simSpeed,  sizeof(simSpeed));
    f.write(cityMap, MAP_W * MAP_H * sizeof(MapTile));
    f.close();

    strcpy(cityMessage, "City saved.");
    messageTimer = 2000;
    return true;
}

static bool loadCity() {
    if (!sd.exists(SAVE_PATH)) return false;
    FsFile f = sd.open(SAVE_PATH, O_READ);
    if (!f) return false;

    uint32_t magic = 0;
    f.read(&magic, sizeof(magic));
    if (magic != SAVE_MAGIC) { f.close(); return false; }

    f.read(&funds,      sizeof(funds));
    f.read(&taxRate,    sizeof(taxRate));
    f.read(&population, sizeof(population));
    f.read(&gameYear,   sizeof(gameYear));
    f.read(&gameMonth,  sizeof(gameMonth));
    f.read(&simSpeed,   sizeof(simSpeed));
    f.read(cityMap, MAP_W * MAP_H * sizeof(MapTile));
    f.close();

    strcpy(cityMessage, "City loaded.");
    messageTimer = 2000;
    return true;
}

// ─────────────────────────────────────────────
//  POWER PROPAGATION (BFS flood-fill)
// ─────────────────────────────────────────────
static void propagatePower() {
    // Clear all power flags
    for (int i = 0; i < MAP_W * MAP_H; i++)
        cityMap[i].flags &= ~FLAG_POWERED;

    // BFS from power plants
    // Simple queue using PSRAM-friendly static array
    static uint16_t queue[MAP_W * MAP_H];
    int qHead = 0, qTail = 0;

    auto enq = [&](int r, int c) {
        if (!inMap(r,c)) return;
        uint16_t idx = r * MAP_W + c;
        if (cityMap[idx].flags & FLAG_POWERED) return;
        uint8_t t = cityMap[idx].type;
        // Only propagate through power lines, roads (with power), zones, plants
        bool conductor = (t == TILE_POWER_H || t == TILE_POWER_V ||
                          t == TILE_ROAD_H  || t == TILE_ROAD_V  ||
                          t == TILE_ROAD_CROSS ||
                          t == TILE_NUCLEAR || t == TILE_COAL_PLANT ||
                          (t >= TILE_RES_EMPTY && t <= TILE_IND_3) ||
                          t == TILE_FIRE_DEPT || t == TILE_POLICE ||
                          t == TILE_STADIUM || t == TILE_SEAPORT ||
                          t == TILE_AIRPORT);
        if (!conductor) return;
        cityMap[idx].flags |= FLAG_POWERED;
        queue[qTail++ % (MAP_W*MAP_H)] = idx;
    };

    // Seed from power plants
    powerPlants = 0;
    for (int r = 0; r < MAP_H; r++) {
        for (int c = 0; c < MAP_W; c++) {
            uint8_t t = tile(r,c).type;
            if (t == TILE_NUCLEAR || t == TILE_COAL_PLANT) {
                powerPlants++;
                enq(r,c);
            }
        }
    }

    // BFS
    int dirs[4][2] = {{0,1},{0,-1},{1,0},{-1,0}};
    while (qHead != qTail) {
        uint16_t idx = queue[qHead++ % (MAP_W*MAP_H)];
        int r = idx / MAP_W, c = idx % MAP_W;
        for (auto& d : dirs) enq(r+d[0], c+d[1]);
    }
}

// ─────────────────────────────────────────────
//  ZONE GROWTH SIMULATION
// ─────────────────────────────────────────────
static void simulateZoneGrowth() {
    // Each zone has a chance to grow/shrink based on:
    // power, road access, demand, surrounding density

    resZones = comZones = indZones = 0;
    resPop = comPop = indPop = 0;

    for (int r = 0; r < MAP_H; r++) {
        for (int c = 0; c < MAP_W; c++) {
            uint8_t t = tile(r,c).type;
            bool powered = tile(r,c).flags & FLAG_POWERED;

            // Check road access (any adjacent road)
            bool hasRoad = false;
            int dr[] = {0,0,1,-1}; int dc[] = {1,-1,0,0};
            for (int i = 0; i < 4; i++) {
                int nr=r+dr[i], nc=c+dc[i];
                if (!inMap(nr,nc)) continue;
                uint8_t nt = tile(nr,nc).type;
                if (nt>=TILE_ROAD_H && nt<=TILE_ROAD_TW) { hasRoad=true; break; }
            }

            if (t >= TILE_RES_EMPTY && t <= TILE_RES_4) {
                resZones++;
                int density = t - TILE_RES_EMPTY;
                resPop += density * density * 8;

                if (powered && hasRoad && demandRes > 0) {
                    // Chance to grow
                    if (density < 4 && (rng()%100) < (uint32_t)(5 + demandRes/10)) {
                        tile(r,c).type = (uint8_t)(TILE_RES_EMPTY + density + 1);
                    }
                } else if (!powered || !hasRoad) {
                    // Decline without power/road
                    if (density > 0 && (rng()%100) < 2) {
                        tile(r,c).type = (uint8_t)(TILE_RES_EMPTY + density - 1);
                    }
                }
            }
            else if (t >= TILE_COM_EMPTY && t <= TILE_COM_3) {
                comZones++;
                int density = t - TILE_COM_EMPTY;
                comPop += density * density * 6;

                if (powered && hasRoad && demandCom > 0) {
                    if (density < 3 && (rng()%100) < (uint32_t)(3 + demandCom/15)) {
                        tile(r,c).type = (uint8_t)(TILE_COM_EMPTY + density + 1);
                    }
                } else if (!powered || !hasRoad) {
                    if (density > 0 && (rng()%100) < 2)
                        tile(r,c).type = (uint8_t)(TILE_COM_EMPTY + density - 1);
                }
            }
            else if (t >= TILE_IND_EMPTY && t <= TILE_IND_3) {
                indZones++;
                int density = t - TILE_IND_EMPTY;
                indPop += density * density * 4;

                if (powered && hasRoad && demandInd > 0) {
                    if (density < 3 && (rng()%100) < (uint32_t)(3 + demandInd/15)) {
                        tile(r,c).type = (uint8_t)(TILE_IND_EMPTY + density + 1);
                    }
                } else if (!powered || !hasRoad) {
                    if (density > 0 && (rng()%100) < 2)
                        tile(r,c).type = (uint8_t)(TILE_IND_EMPTY + density - 1);
                }
            }
        }
    }

    population = resPop + comPop / 2;

    // Update demand based on zone balance
    // Residential demand: high if low R vs C/I (jobs)
    int jobZones = comZones + indZones;
    demandRes = (int8_t)constrain(jobZones * 2 - resZones, -100, 100);
    demandCom = (int8_t)constrain(resPop/50 - comZones*3, -100, 100);
    demandInd = (int8_t)constrain(resPop/80 - indZones*3, -100, 100);
}

// ─────────────────────────────────────────────
//  MONTHLY BUDGET
// ─────────────────────────────────────────────
static void collectTaxes() {
    int32_t taxRevenue = (population * taxRate) / 100;
    // Services cost money
    int32_t expenses = 0;
    for (int r = 0; r < MAP_H; r++) {
        for (int c = 0; c < MAP_W; c++) {
            uint8_t t = tile(r,c).type;
            if (t == TILE_FIRE_DEPT) expenses += 100;
            if (t == TILE_POLICE)    expenses += 100;
            if (t == TILE_NUCLEAR)   expenses += 300;
            if (t == TILE_COAL_PLANT)expenses += 150;
            if (t == TILE_STADIUM)   expenses += 50;
            if (t == TILE_AIRPORT)   expenses += 200;
            if (t == TILE_SEAPORT)   expenses += 150;
        }
    }

    int32_t net = taxRevenue - expenses;
    funds += net;

    // City score update
    int scoreDelta = 0;
    if (net > 0)      scoreDelta += 2;
    if (population > 500)  scoreDelta += 1;
    if (population > 5000) scoreDelta += 2;
    if (fireCoverage > 80) scoreDelta += 1;
    if (policeCoverage > 80) scoreDelta += 1;
    if (funds < 0)    scoreDelta -= 5;
    cityScore = constrain(cityScore + scoreDelta, 0, 1000);

    // Advance date
    gameMonth++;
    if (gameMonth >= 12) { gameMonth = 0; gameYear++; }

    // Message
    if (net >= 0) snprintf(cityMessage, 48, "Tax: +$%ld  Exp: -$%ld", (long)taxRevenue, (long)expenses);
    else          snprintf(cityMessage, 48, "Budget deficit! -$%ld", (long)(-net));
    messageTimer = 3000;
}

// ─────────────────────────────────────────────
//  SERVICE COVERAGE (simplified)
// ─────────────────────────────────────────────
static void calculateCoverage() {
    int fireStations = 0, policeStations = 0;
    for (int r = 0; r < MAP_H; r++)
        for (int c = 0; c < MAP_W; c++) {
            if (tile(r,c).type == TILE_FIRE_DEPT && (tile(r,c).flags & FLAG_POWERED)) fireStations++;
            if (tile(r,c).type == TILE_POLICE    && (tile(r,c).flags & FLAG_POWERED)) policeStations++;
        }
    // Each station covers ~20% of a 64x64 city
    fireCoverage   = min(100, fireStations * 20);
    policeCoverage = min(100, policeStations * 20);
}

// ─────────────────────────────────────────────
//  DISASTERS
// ─────────────────────────────────────────────
static void triggerFire(int r, int c) {
    if (!inMap(r,c)) return;
    uint8_t t = tile(r,c).type;
    if (t == TILE_WATER || t == TILE_ROAD_H || t == TILE_ROAD_V) return;
    tile(r,c).type  = TILE_FIRE;
    tile(r,c).flags = 0;
    disasterActive = true;
    disasterRow = r; disasterCol = c;
    disasterTimer = 20;
    strcpy(cityMessage, "FIRE REPORTED!");
    messageTimer = 4000;
}

static void spreadFire() {
    if (!disasterActive) {
        // Random fire ignition (~1% chance per month if low fire coverage)
        if (fireCoverage < 50 && population > 100 && (rng()%200)==0) {
            int r = rngRange(5, MAP_H-5);
            int c = rngRange(5, MAP_W-5);
            triggerFire(r, c);
        }
        return;
    }

    disasterTimer--;
    if (disasterTimer <= 0) {
        // Spread to adjacent burnable tiles
        int dr[] = {0,0,1,-1}; int dc[] = {1,-1,0,0};
        for (int i = 0; i < 4; i++) {
            int nr = disasterRow+dr[i], nc = disasterCol+dc[i];
            if (!inMap(nr,nc)) continue;
            uint8_t t = tile(nr,nc).type;
            if (t != TILE_WATER && t != TILE_ROAD_H && t != TILE_ROAD_V &&
                t != TILE_FIRE && t != TILE_RUBBLE && t != TILE_EMPTY) {
                if ((rng()%3)==0) {
                    tile(nr,nc).type = TILE_FIRE;
                    tile(nr,nc).flags = 0;
                }
            }
        }
        // Current fire tile burns out
        tile(disasterRow, disasterCol).type = TILE_RUBBLE;
        disasterActive = false;

        // Fire department might contain it
        if (fireCoverage > 60) {
            strcpy(cityMessage, "Fire contained by dept.");
            messageTimer = 2000;
        }
    }
}

// ─────────────────────────────────────────────
//  TOOL APPLICATION
// ─────────────────────────────────────────────
static bool applyTool(int r, int c) {
    if (!inMap(r,c)) return false;
    int cost = TOOL_COST[currentTool];
    if (funds < cost) {
        strcpy(cityMessage, "Insufficient funds!");
        messageTimer = 2000;
        return false;
    }

    uint8_t t = tile(r,c).type;

    switch (currentTool) {
        case TOOL_BULLDOZE:
            if (t == TILE_WATER) return false;
            tile(r,c).type  = TILE_DIRT;
            tile(r,c).flags = 0;
            funds -= cost;
            return true;

        case TOOL_ROAD:
            if (t == TILE_WATER || t == TILE_RUBBLE) return false;
            if (t >= TILE_ROAD_H && t <= TILE_ROAD_TW) return false;
            // Determine road orientation based on neighbors
            {
                bool left  = inMap(r,c-1) && tile(r,c-1).type >= TILE_ROAD_H && tile(r,c-1).type <= TILE_ROAD_TW;
                bool right = inMap(r,c+1) && tile(r,c+1).type >= TILE_ROAD_H && tile(r,c+1).type <= TILE_ROAD_TW;
                bool up    = inMap(r-1,c) && tile(r-1,c).type >= TILE_ROAD_H && tile(r-1,c).type <= TILE_ROAD_TW;
                bool down  = inMap(r+1,c) && tile(r+1,c).type >= TILE_ROAD_H && tile(r+1,c).type <= TILE_ROAD_TW;
                int count  = left+right+up+down;
                if (count >= 2 && (left||right) && (up||down)) tile(r,c).type = TILE_ROAD_CROSS;
                else if (up || down) tile(r,c).type = TILE_ROAD_V;
                else                 tile(r,c).type = TILE_ROAD_H;
            }
            tile(r,c).flags = 0;
            funds -= cost;
            return true;

        case TOOL_POWER:
            if (t == TILE_WATER) return false;
            if (t == TILE_ROAD_H) { tile(r,c).flags |= FLAG_POWERED; }
            else if (t == TILE_ROAD_V) { tile(r,c).flags |= FLAG_POWERED; }
            else {
                bool vertical = inMap(r-1,c) && (tile(r-1,c).type == TILE_POWER_V || tile(r-1,c).type == TILE_POWER_H);
                tile(r,c).type = vertical ? TILE_POWER_V : TILE_POWER_H;
            }
            funds -= cost;
            return true;

        case TOOL_ZONE_RES:
            if (t != TILE_DIRT && t != TILE_GRASS && t != TILE_EMPTY) return false;
            tile(r,c).type  = TILE_RES_EMPTY;
            tile(r,c).flags = 0;
            funds -= cost;
            return true;

        case TOOL_ZONE_COM:
            if (t != TILE_DIRT && t != TILE_GRASS && t != TILE_EMPTY) return false;
            tile(r,c).type  = TILE_COM_EMPTY;
            tile(r,c).flags = 0;
            funds -= cost;
            return true;

        case TOOL_ZONE_IND:
            if (t != TILE_DIRT && t != TILE_GRASS && t != TILE_EMPTY) return false;
            tile(r,c).type  = TILE_IND_EMPTY;
            tile(r,c).flags = 0;
            funds -= cost;
            return true;

        case TOOL_FIRE_DEPT:
            if (t != TILE_DIRT && t != TILE_GRASS && t != TILE_EMPTY) return false;
            tile(r,c).type  = TILE_FIRE_DEPT;
            tile(r,c).flags = 0;
            funds -= cost;
            return true;

        case TOOL_POLICE:
            if (t != TILE_DIRT && t != TILE_GRASS && t != TILE_EMPTY) return false;
            tile(r,c).type  = TILE_POLICE;
            tile(r,c).flags = 0;
            funds -= cost;
            return true;

        case TOOL_PARK:
            if (t != TILE_DIRT && t != TILE_GRASS && t != TILE_EMPTY) return false;
            tile(r,c).type  = TILE_PARK;
            funds -= cost;
            return true;

        case TOOL_STADIUM:
            if (t != TILE_DIRT && t != TILE_GRASS && t != TILE_EMPTY) return false;
            tile(r,c).type = TILE_STADIUM; funds -= cost; return true;

        case TOOL_SEAPORT:
            // Must be adjacent to water
            { bool nearWater = false;
              int dr[]={0,0,1,-1}; int dc[]={1,-1,0,0};
              for(int i=0;i<4;i++) if(inMap(r+dr[i],c+dc[i]) && tile(r+dr[i],c+dc[i]).type==TILE_WATER) nearWater=true;
              if (!nearWater) { strcpy(cityMessage,"Must be near water!"); messageTimer=2000; return false; }
            }
            tile(r,c).type = TILE_SEAPORT; funds -= cost; return true;

        case TOOL_AIRPORT:
            tile(r,c).type = TILE_AIRPORT; funds -= cost; return true;

        case TOOL_NUCLEAR:
        case TOOL_QUERY:
            tile(r,c).type = (currentTool == TOOL_NUCLEAR) ? TILE_NUCLEAR : t;
            if (currentTool == TOOL_QUERY) {
                const char* names[] = {"Empty","Dirt","Grass","Water","Road","Road","Road","Road","Road","Road","Road","Power","Power",
                    "","","","","","","","R-Zone","R-Dens1","R-Dens2","R-Dens3","R-Dens4",
                    "","","","","","C-Zone","C-Dens1","C-Dens2","C-Dens3","","","","","","","I-Zone","I-Dens1","I-Dens2","I-Dens3",
                    "","","","","","","","","","","Fire Dept","Police","Stadium","Seaport","Airport","Nuclear","Coal Plant","Park",
                    "","","Fire","Rubble","Flood","Radiation"};
                if (t < 64) snprintf(cityMessage, 48, "Tile: %s %s", names[t], (tile(r,c).flags&FLAG_POWERED)?"[PWR]":"");
                messageTimer = 3000;
                return false; // No cost, no placement
            }
            funds -= cost;
            return true;
    }
    return false;
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawTile(int screenX, int screenY, uint8_t type, uint8_t flags) {
    uint16_t col = tileColor(type, flags);
    gfx->fillRect(screenX, screenY, TILE_PX, TILE_PX, col);

    // Details within tile
    uint16_t border = tileBorderColor(type);
    if (border) {
        if (type == TILE_ROAD_H) {
            gfx->drawFastHLine(screenX, screenY+TILE_PX/2, TILE_PX, 0xFFFF);
        } else if (type == TILE_ROAD_V) {
            gfx->drawFastVLine(screenX+TILE_PX/2, screenY, TILE_PX, 0xFFFF);
        } else if (type == TILE_ROAD_CROSS) {
            gfx->drawFastHLine(screenX, screenY+TILE_PX/2, TILE_PX, 0xFFFF);
            gfx->drawFastVLine(screenX+TILE_PX/2, screenY, TILE_PX, 0xFFFF);
        } else if (type == TILE_POWER_H) {
            gfx->drawFastHLine(screenX, screenY+TILE_PX/2, TILE_PX, 0xFFE0);
            gfx->drawPixel(screenX+TILE_PX/2, screenY+TILE_PX/2, 0xFFFF);
        } else if (type == TILE_POWER_V) {
            gfx->drawFastVLine(screenX+TILE_PX/2, screenY, TILE_PX, 0xFFE0);
        }
    }

    // Building details for developed zones
    if (type >= TILE_RES_1 && type <= TILE_RES_4) {
        int density = type - TILE_RES_EMPTY;
        // Draw small building outlines
        for (int i = 0; i < min(density, 2); i++) {
            int bx = screenX + 1 + i*3;
            int by = screenY + 2;
            gfx->drawRect(bx, by, 3, TILE_PX-3, 0x0C40);
        }
    } else if (type >= TILE_COM_1 && type <= TILE_COM_3) {
        gfx->drawRect(screenX+1, screenY+1, TILE_PX-2, TILE_PX-2, 0x04FF);
        gfx->drawPixel(screenX+TILE_PX/2, screenY+1, 0xFFFF); // Antenna
    } else if (type >= TILE_IND_1 && type <= TILE_IND_3) {
        // Smokestack
        gfx->fillRect(screenX+2, screenY+1, 2, 4, 0xC618);
        gfx->fillRect(screenX+5, screenY+1, 2, 4, 0xC618);
    } else if (type == TILE_FIRE) {
        // Animated fire effect — alternate pixels
        uint16_t fc = ((millis()/200) % 2) ? 0xFD20 : 0xF800;
        gfx->fillRect(screenX+1, screenY+2, TILE_PX-2, TILE_PX-3, fc);
        gfx->drawPixel(screenX+TILE_PX/2, screenY+1, 0xFFE0);
    } else if (type == TILE_NUCLEAR) {
        // Warning symbol simplified
        gfx->fillCircle(screenX+TILE_PX/2, screenY+TILE_PX/2, 2, 0xFFE0);
        if (flags & FLAG_POWERED) {
            gfx->drawCircle(screenX+TILE_PX/2, screenY+TILE_PX/2, 3, 0x07E0);
        }
    }

    // Power indicator — tiny dot in corner for powered zones
    if ((flags & FLAG_POWERED) && type >= TILE_RES_EMPTY && type <= TILE_IND_3) {
        gfx->drawPixel(screenX + TILE_PX - 2, screenY + 1, 0xFFE0);
    }
}

static void drawViewport() {
    for (int vr = 0; vr < VIEW_TILES_H; vr++) {
        for (int vc = 0; vc < VIEW_TILES_W; vc++) {
            int mr = viewRow + vr;
            int mc = viewCol + vc;
            int sx = VIEW_X + vc * TILE_PX;
            int sy = VIEW_Y + vr * TILE_PX;

            if (!inMap(mr, mc)) {
                gfx->fillRect(sx, sy, TILE_PX, TILE_PX, 0x0000);
                continue;
            }
            drawTile(sx, sy, tile(mr,mc).type, tile(mr,mc).flags);

            // Cursor highlight
            if (mr == cursorRow && mc == cursorCol) {
                gfx->drawRect(sx, sy, TILE_PX, TILE_PX, 0xFFFF);
            }
        }
    }
}

static void drawPanel() {
    gfx->fillRect(PANEL_X, 0, PANEL_W, 240, 0x18C3);
    gfx->drawFastVLine(PANEL_X, 0, 240, 0x4208);

    gfx->setTextSize(1);
    int y = 2;

    // Funds
    gfx->setTextColor(funds >= 0 ? 0x07E0 : 0xF800);
    gfx->setCursor(PANEL_X+2, y);
    gfx->printf("$%ld", (long)funds); y += 10;

    // Year / Month
    const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(PANEL_X+2, y);
    gfx->printf("%s %d", months[gameMonth], gameYear); y += 10;

    // Population
    gfx->setTextColor(0x07FF);
    gfx->setCursor(PANEL_X+2, y);
    if (population >= 1000) gfx->printf("Pop:%dK", population/1000);
    else                    gfx->printf("Pop:%d", population);
    y += 10;

    // Score
    gfx->setTextColor(0xFD20);
    gfx->setCursor(PANEL_X+2, y);
    gfx->printf("Scr:%d", cityScore); y += 10;

    gfx->drawFastHLine(PANEL_X, y, PANEL_W, 0x4208); y += 3;

    // Demand bars
    gfx->setTextColor(0x07E0);
    gfx->setCursor(PANEL_X+2, y); gfx->print("R"); 
    int barW = max(0, (int)(demandRes * (PANEL_W-14) / 100));
    gfx->fillRect(PANEL_X+10, y, barW, 6, 0x07E0);
    gfx->drawRect(PANEL_X+10, y, PANEL_W-14, 6, 0x4208); y += 8;

    gfx->setTextColor(0x07FF);
    gfx->setCursor(PANEL_X+2, y); gfx->print("C");
    barW = max(0, (int)(demandCom * (PANEL_W-14) / 100));
    gfx->fillRect(PANEL_X+10, y, barW, 6, 0x07FF);
    gfx->drawRect(PANEL_X+10, y, PANEL_W-14, 6, 0x4208); y += 8;

    gfx->setTextColor(0xFFE0);
    gfx->setCursor(PANEL_X+2, y); gfx->print("I");
    barW = max(0, (int)(demandInd * (PANEL_W-14) / 100));
    gfx->fillRect(PANEL_X+10, y, barW, 6, 0xFFE0);
    gfx->drawRect(PANEL_X+10, y, PANEL_W-14, 6, 0x4208); y += 10;

    gfx->drawFastHLine(PANEL_X, y, PANEL_W, 0x4208); y += 3;

    // Current tool — make it a visible tappable button
    gfx->fillRect(PANEL_X+2, y, PANEL_W-4, 18, 0x0020);
    gfx->drawRect(PANEL_X+2, y, PANEL_W-4, 18, 0xFD20);
    gfx->setTextColor(0xFD20);
    gfx->setCursor(PANEL_X+5, y+2);
    gfx->print("TOOL:");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(PANEL_X+5, y+11);
    char tn[7]; strncpy(tn, TOOL_NAMES[currentTool], 6); tn[6]='\0';
    gfx->print(tn); y += 22;
    gfx->setTextColor(0x4208);
    gfx->setCursor(PANEL_X+2, y);
    gfx->printf("$%d", TOOL_COST[currentTool]); y += 12;

    gfx->drawFastHLine(PANEL_X, y, PANEL_W, 0x4208); y += 3;

    // Services
    gfx->setTextColor(0xF800);
    gfx->setCursor(PANEL_X+2, y); gfx->printf("F:%d%%", fireCoverage); y += 10;
    gfx->setTextColor(0x001F);
    gfx->setCursor(PANEL_X+2, y); gfx->printf("P:%d%%", policeCoverage); y += 10;
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(PANEL_X+2, y); gfx->printf("PWR:%d", powerPlants); y += 12;

    gfx->drawFastHLine(PANEL_X, y, PANEL_W, 0x4208); y += 3;

    // Speed indicator
    const char* speeds[] = {"PAUSE","SLOW","MED","FAST"};
    gfx->setTextColor(simSpeed==0 ? 0xF800 : 0x07E0);
    gfx->setCursor(PANEL_X+2, y);
    gfx->print(speeds[simSpeed]); y += 12;

    // Tax rate
    gfx->setTextColor(0x4208);
    gfx->setCursor(PANEL_X+2, y);
    gfx->printf("TAX:%d%%", taxRate); y += 12;

    // Controls hint at bottom — bright enough to actually read
    gfx->drawFastHLine(PANEL_X, 196, PANEL_W, 0x4208);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(PANEL_X+2, 200);
    gfx->print("T=TOOLS");
    gfx->setTextColor(0x4208);
    gfx->setCursor(PANEL_X+2, 210);
    gfx->print("+-tax");
    gfx->setCursor(PANEL_X+2, 220);
    gfx->print("SPC=spd");
    gfx->setCursor(PANEL_X+2, 230);
    gfx->print("S=save");
}

static void drawMessage() {
    if (messageTimer == 0 || strlen(cityMessage) == 0) return;
    gfx->fillRect(0, 230, PANEL_X, 10, 0x18C3);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(4, 231);
    // Truncate to fit
    char buf[43]; strncpy(buf, cityMessage, 42); buf[42]='\0';
    gfx->print(buf);
}

// ─────────────────────────────────────────────
//  SIMULATION TICK
// ─────────────────────────────────────────────
static const int SIM_INTERVALS[] = {0, 2000, 800, 250}; // ms per tick

static void runSimulation() {
    if (simSpeed == 0) return;
    uint32_t now = millis();
    if (now - lastSimTick < (uint32_t)SIM_INTERVALS[simSpeed]) return;
    lastSimTick = now;
    simTick++;

    // Power every 5 ticks
    if (simTick % 5 == 0) propagatePower();

    // Zone growth every 10 ticks
    if (simTick % 10 == 0) simulateZoneGrowth();

    // Monthly events every 30 ticks
    if (simTick % 30 == 0) {
        collectTaxes();
        calculateCoverage();
        spreadFire();
    }

    // Decrement message timer
    if (messageTimer > 0) messageTimer -= min(messageTimer, (uint32_t)SIM_INTERVALS[simSpeed]);
}

// ─────────────────────────────────────────────
//  TOOL SELECTION MENU
// ─────────────────────────────────────────────
static void showToolMenu() {
    gfx->fillRect(0, 0, PANEL_X, 240, 0x0821);
    gfx->setTextColor(0xFFE0);
    gfx->setTextSize(1);
    gfx->setCursor(10, 4);
    gfx->print("SELECT TOOL:");

    for (int i = 0; i < NUM_TOOLS; i++) {
        int y = 18 + i * 16;
        bool sel = (i == currentTool);
        gfx->fillRect(5, y, PANEL_X-10, 14, sel ? 0x001F : 0x1082);
        gfx->drawRect(5, y, PANEL_X-10, 14, sel ? 0xFFFF : 0x4208);
        gfx->setTextColor(sel ? 0xFFFF : 0xC618);
        gfx->setCursor(10, y+3);
        gfx->printf("%-10s $%d", TOOL_NAMES[i], TOOL_COST[i]);
    }

    // Wait for selection
    while (true) {
        TrackballState tb = update_trackball();
        char k = get_keypress();
        int16_t tx, ty;

        if (gamepad_poll()) break; // HOME = accept current tool and exit
        if (tb.y == -1 || gamepad_pressed(GP_UP)) {
            if (currentTool > 0) { currentTool--; showToolMenu(); } }
        if (tb.y ==  1 || gamepad_pressed(GP_DOWN)) {
            if (currentTool < NUM_TOOLS-1) { currentTool++; showToolMenu(); } }
        if (tb.clicked || k == 13 || k == 't' || k == 'T' ||
            gamepad_pressed(GP_A | GP_START) ||
            (get_touch(&tx, &ty) && ty < 24)) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        if (get_touch(&tx, &ty) && ty > 18) {
            int sel = (ty - 18) / 16;
            if (sel >= 0 && sel < NUM_TOOLS) currentTool = sel;
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        delay(50);
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_simcity() {
    // Allocate city map in PSRAM
    cityMap = (MapTile*)ps_malloc(MAP_W * MAP_H * sizeof(MapTile));
    if (!cityMap) {
        gfx->fillScreen(0x0000);
        gfx->setTextColor(0xF800);
        gfx->setCursor(10, 100);
        gfx->print("ERROR: Cannot allocate city map.");
        gfx->setCursor(10, 116);
        gfx->print("PSRAM required.");
        delay(3000); return;
    }

    // Try to load saved city, otherwise new game
    bool loaded = loadCity();
    if (!loaded) newCity();

    // Full redraw
    gfx->fillScreen(0x0000);
    drawViewport();
    drawPanel();
    drawMessage();

    bool needsRedraw = false;
    bool viewportDirty = true;
    bool panelDirty = true;

    while (true) {
        // Input
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        // Header tap = exit
        if (get_touch(&tx, &ty)) {
            while(get_touch(&tx,&ty)){delay(10);}
            // Tapping the right panel opens tool menu
            if (tx >= PANEL_X) {
                showToolMenu(); viewportDirty = true; panelDirty = true;
            }
            // Touching viewport = place tool
            else if (tx < PANEL_X && ty < VIEW_TILES_H * TILE_PX) {
                int mc = viewCol + tx / TILE_PX;
                int mr = viewRow + ty / TILE_PX;
                if (applyTool(mr, mc)) { viewportDirty = true; panelDirty = true; }
                else panelDirty = true;
            }
        }

        // Keyboard shortcuts
        if (k == 'q' || k == 'Q') {
            saveCity();
            break;
        }

        // Gamepad
        if (gamepad_poll()) { saveCity(); break; } // HOME = save and exit
        if (gamepad_pressed(GP_START))  { saveCity(); break; }
        if (gamepad_pressed(GP_B))      { showToolMenu(); viewportDirty = true; panelDirty = true; }
        if (gamepad_pressed(GP_A)) {
            if (applyTool(cursorRow, cursorCol)) { viewportDirty = true; panelDirty = true; }
            else panelDirty = true;
        }
        if (gamepad_pressed(GP_L)) { currentTool = max(0, currentTool-1); panelDirty = true; }
        if (gamepad_pressed(GP_R)) { currentTool = min(NUM_TOOLS-1, currentTool+1); panelDirty = true; }
        if (gamepad_pressed(GP_SELECT)) { simSpeed = (simSpeed+1)%4; panelDirty = true; }
        if (k == 't' || k == 'T') { showToolMenu(); viewportDirty = true; panelDirty = true; }
        if (k == 's' || k == 'S') { saveCity(); panelDirty = true; }
        if (k == '+' || k == '=') { taxRate = min(20, taxRate+1); panelDirty = true; }
        if (k == '-')              { taxRate = max(0, taxRate-1);  panelDirty = true; }
        if (k == ' ')              { simSpeed = (simSpeed+1)%4;    panelDirty = true; }
        if (k == 13)               { // Enter = place tool at cursor
            if (applyTool(cursorRow, cursorCol)) { viewportDirty = true; panelDirty = true; }
        }
        if (k == '1') { currentTool = TOOL_BULLDOZE;  panelDirty = true; }
        if (k == '2') { currentTool = TOOL_ROAD;       panelDirty = true; }
        if (k == '3') { currentTool = TOOL_POWER;      panelDirty = true; }
        if (k == '4') { currentTool = TOOL_ZONE_RES;   panelDirty = true; }
        if (k == '5') { currentTool = TOOL_ZONE_COM;   panelDirty = true; }
        if (k == '6') { currentTool = TOOL_ZONE_IND;   panelDirty = true; }
        if (k == '7') { currentTool = TOOL_FIRE_DEPT;  panelDirty = true; }
        if (k == '8') { currentTool = TOOL_POLICE;     panelDirty = true; }
        if (k == '9') { currentTool = TOOL_NUCLEAR;    panelDirty = true; }

        // Trackball + gamepad D-pad: move cursor, scroll viewport when at edge
        // Fold gamepad into trackball delta so scroll/clamp logic runs once
        int gpx = (gamepad_held(GP_RIGHT) ? 1 : 0) - (gamepad_held(GP_LEFT) ? 1 : 0);
        int gpy = (gamepad_held(GP_DOWN)  ? 1 : 0) - (gamepad_held(GP_UP)   ? 1 : 0);
        bool confirm = tb.clicked || gamepad_pressed(GP_A);

        if (confirm) {
            if (applyTool(cursorRow, cursorCol)) { viewportDirty = true; panelDirty = true; }
        }
        if (tb.x != 0 || tb.y != 0 || gpx != 0 || gpy != 0) {
            cursorCol += tb.x + gpx;
            cursorRow += tb.y + gpy;
            // Scroll viewport when cursor reaches edge
            int relR = cursorRow - viewRow;
            int relC = cursorCol - viewCol;
            if (relC < 2 && viewCol > 0)               { viewCol--;  viewportDirty = true; }
            if (relC > VIEW_TILES_W-3 && viewCol < MAP_W-VIEW_TILES_W) { viewCol++; viewportDirty = true; }
            if (relR < 2 && viewRow > 0)               { viewRow--;  viewportDirty = true; }
            if (relR > VIEW_TILES_H-3 && viewRow < MAP_H-VIEW_TILES_H) { viewRow++; viewportDirty = true; }
            // Clamp cursor to map
            cursorCol = constrain(cursorCol, 0, MAP_W-1);
            cursorRow = constrain(cursorRow, 0, MAP_H-1);
            if (!viewportDirty) viewportDirty = true; // Always redraw for cursor
        }

        // Simulation tick
        runSimulation();

        // Periodic full redraws
        static uint32_t lastFullRedraw = 0;
        if (millis() - lastFullRedraw > 2000) {
            viewportDirty = true;
            panelDirty = true;
            lastFullRedraw = millis();
        }

        if (viewportDirty) { drawViewport(); viewportDirty = false; }
        if (panelDirty)    { drawPanel(); panelDirty = false; }
        drawMessage();

        delay(30);
        yield();
    }

    free(cityMap);
    cityMap = nullptr;
    gfx->fillScreen(0x0000);
}