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
 * PISCES MOON OS — MESH MESSENGER v1.0
 * IRC-style multi-channel LoRa messenger
 * Meshtastic-compatible packet format
 *
 * Meshtastic attribution:
 *   This file implements interoperability logic for the Meshtastic
 *   LongFast RF lane and packet/protobuf layout. Protocol constants,
 *   field names, packet framing, and protobuf field semantics are based
 *   on public Meshtastic documentation and the upstream Meshtastic
 *   firmware/protobuf repositories:
 *
 *     https://github.com/meshtastic/firmware
 *     https://github.com/meshtastic/protobufs
 *
 *   Meshtastic firmware and protobuf definitions are licensed upstream
 *   under GPL-3.0. Pisces Moon OS is licensed under AGPL-3.0-or-later.
 *   No upstream Meshtastic source file is vendored here; if future work
 *   imports or adapts Meshtastic implementation code directly, preserve
 *   the upstream copyright notices and GPL-3.0 license terms alongside
 *   this project's AGPL notice.
 *
 * v1.1 — reliable same-channel receive
 *   TX now encodes the active app channel into the header's hop_start
 *   hint instead of duplicating hop_limit there. RX already uses this
 *   field as the display-channel hint, so the old encoding made every
 *   message appear on channel MESH_HOP_LIMIT (#pisces).
 *
 *   RX is interrupt-driven now: DIO1 sets a flag, the UI loop reads
 *   packets with readData() only when one is actually present, then
 *   returns the radio to continuous receive. This avoids holding the
 *   shared SPI mutex inside RadioLib's blocking receive() timeout.
 *
 * v1.2 — MicroSD transcript
 *   Sent and received messages are appended to /mesh_logs/messages.csv
 *   with an immediate close after every row. This keeps the on-screen
 *   ring buffer light while preserving a durable transcript on SD.
 *
 * T-Deck Plus SX1262 pins:
 *   CS   = GPIO9   RST  = GPIO17
 *   DIO1 = GPIO45  BUSY = GPIO13
 *   SPI  = shared (MOSI:41, MISO:38, SCK:40)
 *
 * Protocol: Meshtastic LongFast preset
 *   Freq:  906.875 MHz (US) / 869.525 MHz (EU — change LORA_FREQ)
 *   BW:    250 kHz
 *   SF:    11
 *   CR:    4/5
 *   Sync:  0x2B   (Meshtastic network ID)
 *   Preamble: 16 symbols
 *
 * Packet format (Meshtastic-compatible):
 *   [0..3]  dest    uint32_le  destination node ID (0xFFFFFFFF = broadcast)
 *   [4..7]  from    uint32_le  sender node ID
 *   [8..11] id      uint32_le  unique packet ID
 *   [12]    flags   uint8      hop_limit(3b) | want_ack(1b) | via_mqtt(1b) | hop_start(3b)
 *   [13]    channel uint8      Meshtastic channel hash
 *   [14]    next    uint8      next-hop low byte
 *   [15]    relay   uint8      relay-node low byte
 *   [16..]  payload             protobuf-encoded Data message (unencrypted default channel)
 *
 * Data protobuf (TEXT_MESSAGE_APP, portnum=1):
 *   0x08 0x01               field 1 (portnum) = 1
 *   0x12 <len> <text bytes> field 2 (payload) = UTF-8 text
 *
 * Channels:
 *   Channel 0 = LongFast default RF lane (most Meshtastic nodes)
 *   Channels 1-3 = Pisces private offset lanes
 *
 * Node ID: derived from ESP32 MAC address (lower 4 bytes)
 *
 * Controls:
 *   Keyboard     = type message
 *   Enter/Click  = send
 *   Tab          = switch channel
 *   Q / header   = exit
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "mesh_messenger.h"
#include "pm_lora_pins.h"

// Forward declaration: cardputerSdSPI lives in main.cpp and is the
// HSPI instance on Cardputer's peripheral SPI bus (pins 14/39/40),
// shared by SD card AND the Cap LoRa SX1262. On Cardputer the
// default `SPI` instance is bound to the LCD pins (35/36) and
// cannot reach the radio. See main.cpp for the routing comment.
#ifdef DEVICE_CARDPUTER_ADV
extern SPIClass cardputerSdSPI;
#endif
extern volatile bool wardrive_bridge_streaming;
#include <TinyGPSPlus.h>
extern TinyGPSPlus gps;

extern Arduino_GFX *gfx;
extern SemaphoreHandle_t spi_mutex;
extern SdFat sd;
extern volatile bool g_sd_ready;

// ─────────────────────────────────────────────
//  SX1262 HARDWARE PINS
// ─────────────────────────────────────────────
#define MESH_LORA_CS    PM_LORA_CS
#define MESH_LORA_RST   PM_LORA_RST
#define MESH_LORA_DIO1  PM_LORA_IRQ
#define MESH_LORA_BUSY  PM_LORA_BUSY

// ─────────────────────────────────────────────
//  RADIO SETTINGS — Meshtastic LongFast US
//  Change LORA_FREQ to 869.525 for EU
// ─────────────────────────────────────────────
#define LORA_FREQ       906.875f   // MHz — US LongFast ch 0
#define LORA_BW         250.0f     // kHz
#define LORA_SF         11
#define LORA_CR         5          // 4/5 — Meshtastic LongFast
#define LORA_SYNC       0x2B       // Meshtastic sync word
#define LORA_PREAMBLE   16
#define LORA_POWER      22         // dBm — max legal for SX1262

// Meshtastic broadcast address
#define MESH_BROADCAST  0xFFFFFFFF
#define MESH_HOP_LIMIT  3
#define MESH_LONGFAST_CHANNEL_HASH 0x02

// ─────────────────────────────────────────────
//  CHANNELS
// ─────────────────────────────────────────────
#define MAX_CHANNELS    4
static const char* CHANNEL_NAMES[MAX_CHANNELS] = {
    "#LongFast",    // ch 0 — Meshtastic default RF lane
    "#local",       // ch 1
    "#emergency",   // ch 2
    "#pisces",      // ch 3 — our own channel
};

// ─────────────────────────────────────────────
//  MESSAGE STORE
// ─────────────────────────────────────────────
#define MAX_MSGS_PER_CH  32
#define MAX_MSG_LEN      200
#define MAX_SENDER_LEN   12

struct MeshMsg {
    char sender[MAX_SENDER_LEN];
    char text[MAX_MSG_LEN];
    uint32_t nodeId;
    bool      outgoing;
    bool      valid;
};

static MeshMsg (*msgStore)[MAX_MSGS_PER_CH] = nullptr;
static int     msgCount[MAX_CHANNELS] = {0};
static int     msgScroll[MAX_CHANNELS] = {0};

// ─────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────
static uint32_t myNodeId      = 0;
static int      currentCh     = 0;
static char     inputBuf[MAX_MSG_LEN] = "";
static int      inputLen       = 0;
static bool     radioReady     = false;
static bool     transmitting   = false;
static uint32_t lastStatusMs   = 0;
static char     statusMsg[48]  = "";
static uint16_t statusColor    = 0xFFFF;
static volatile bool rxFlag     = false;
static uint32_t meshLogLastWarnMs = 0;

struct HeardNode {
    uint32_t nodeId;
    uint8_t  channelHash;
    float    rssi;
    float    snr;
    uint32_t lastSeenMs;
    uint32_t lastAnnounceMs;
    bool     valid;
};

#define HEARD_NODES_SIZE 16
#define HEARD_NODE_ANNOUNCE_MS 60000UL
static HeardNode heardNodes[HEARD_NODES_SIZE];
static int heardNodeReplaceIdx = 0;

#define MESH_LOG_DIR   "/mesh_logs"
#define MESH_LOG_PATH  "/mesh_logs/messages.csv"

// Seen packet IDs for dedup (rolling 16-entry cache)
#define SEEN_IDS_SIZE  16
static uint32_t seenIds[SEEN_IDS_SIZE] = {0};
static int      seenIdx = 0;

static bool ensureMsgStore() {
    if (msgStore) return true;
    msgStore = (MeshMsg (*)[MAX_MSGS_PER_CH])calloc(MAX_CHANNELS, sizeof(*msgStore));
    if (!msgStore) {
        Serial.println("[MESH] Message store allocation failed");
        return false;
    }
    memset(msgCount, 0, sizeof(msgCount));
    memset(msgScroll, 0, sizeof(msgScroll));
    return true;
}

static void freeMsgStore() {
    if (!msgStore) return;
    free(msgStore);
    msgStore = nullptr;
    memset(msgCount, 0, sizeof(msgCount));
    memset(msgScroll, 0, sizeof(msgScroll));
    memset(heardNodes, 0, sizeof(heardNodes));
    heardNodeReplaceIdx = 0;
}

// ─────────────────────────────────────────────
//  COLORS (cyberpunk theme)
// ─────────────────────────────────────────────
#define COL_BG          0x0000
#define COL_HDR         0x18C3
#define COL_ACCENT      0x07E0   // green
#define COL_CYAN        0x07FF
#define COL_YELLOW      0xFFE0
#define COL_RED         0xF800
#define COL_DIM         0x4208
#define COL_SEL         0x0010   // dark blue highlight
#define COL_CH_ACTIVE   0x07E0
#define COL_CH_INACTIVE 0x4208
#define COL_MSG_OUT     0x07FF   // cyan — our messages
#define COL_MSG_IN      0xFFFF   // white — incoming
#define COL_MSG_SYS     0xFD20   // orange — system
#define COL_INPUT_BG    0x0841
#define COL_INPUT_FG    0xFFFF

// ─────────────────────────────────────────────
//  LAYOUT
// ─────────────────────────────────────────────
// Layout is derived from the active display at runtime:
//   T-Deck Plus:    320x240
//   T-LoRa Pager:   480x222
//   Cardputer ADV:  240x135
static constexpr int MESH_HDR_H     = 23;
static constexpr int MESH_MSG_ROW_H = 12;

static int meshDispW() {
    return gfx ? gfx->width() : 320;
}

static int meshDispH() {
    return gfx ? gfx->height() : 240;
}

static int meshInputH() {
    return (meshDispH() <= 135) ? 32 : 31;
}

static int meshInputY() {
    return max(MESH_HDR_H + MESH_MSG_ROW_H, meshDispH() - meshInputH());
}

static int meshMsgY() {
    return MESH_HDR_H;
}

static int meshMsgH() {
    return max(MESH_MSG_ROW_H, meshInputY() - meshMsgY());
}

static int meshRows() {
    return max(1, (meshMsgH() - 2) / MESH_MSG_ROW_H);
}

static int meshTabW() {
    return max(1, meshDispW() / MAX_CHANNELS);
}

// ─────────────────────────────────────────────
//  RADIOLIB INSTANCE
//  Use the shared SPI bus already initialized in main.cpp.
//  T-Deck Plus routes SX1262, TFT, and SD all through the
//  same MOSI:41/MISO:38/SCK:40 bus — RadioLib manages CS
//  (GPIO9) itself so there is no bus contention.
//  Do NOT call SPI.begin() again here — it's already running.
// ─────────────────────────────────────────────
static SPISettings lspSettings(2000000, MSBFIRST, SPI_MODE0);

static Module*  loraModule = nullptr;
static SX1262*  radio      = nullptr;

static void IRAM_ATTR meshRxISR() {
    rxFlag = true;
}

// ─────────────────────────────────────────────
//  NODE ID
// ─────────────────────────────────────────────
static uint32_t getNodeId() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    // Meshtastic uses bottom 4 bytes of BT MAC
    return ((uint32_t)mac[2] << 24) |
           ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] << 8)  |
            (uint32_t)mac[5];
}

static String nodeIdToStr(uint32_t id) {
    char buf[12];
    snprintf(buf, sizeof(buf), "!%08x", (unsigned int)id);
    return String(buf);
}

// ─────────────────────────────────────────────
//  PROTOBUF ENCODING (minimal hand-rolled)
//  Data message:
//    field 1 (portnum): varint = 1  → 0x08 0x01
//    field 2 (payload): bytes       → 0x12 <varint len> <bytes>
// ─────────────────────────────────────────────
static int encodeTextPayload(const char* text, uint8_t* out, int outLen) {
    int textLen = strlen(text);
    if (textLen > 228) textLen = 228; // max payload

    int idx = 0;
    // field 1: portnum = 1 (TEXT_MESSAGE_APP)
    out[idx++] = 0x08;
    out[idx++] = 0x01;
    // field 2: payload bytes
    out[idx++] = 0x12;
    out[idx++] = (uint8_t)textLen;  // varint (works for len < 128)
    memcpy(out + idx, text, textLen);
    idx += textLen;

    return idx;
}

// ─────────────────────────────────────────────
//  PROTOBUF DECODING (minimal)
//  Returns text if field 2 found, else ""
// ─────────────────────────────────────────────
static bool decodeTextPayload(const uint8_t* data, int len,
                               char* textOut, int textOutLen,
                               int* portnumOut) {
    *portnumOut = 0;
    textOut[0] = '\0';

    int i = 0;
    while (i < len) {
        if (i >= len) break;
        uint8_t tag = data[i++];
        uint8_t fieldNum  = tag >> 3;
        uint8_t wireType  = tag & 0x07;

        if (wireType == 0) {
            // varint
            uint32_t val = 0;
            int shift = 0;
            while (i < len) {
                uint8_t b = data[i++];
                val |= ((uint32_t)(b & 0x7F)) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            if (fieldNum == 1) *portnumOut = val;
        } else if (wireType == 2) {
            // length-delimited
            uint32_t blen = 0;
            int shift = 0;
            while (i < len) {
                uint8_t b = data[i++];
                blen |= ((uint32_t)(b & 0x7F)) << shift;
                if (!(b & 0x80)) break;
                shift += 7;
            }
            if (fieldNum == 2 && blen > 0) {
                int copyLen = min((int)blen, textOutLen - 1);
                memcpy(textOut, data + i, copyLen);
                textOut[copyLen] = '\0';
            }
            i += blen;
        } else {
            // unknown wire type — stop parsing
            break;
        }
    }
    return (textOut[0] != '\0');
}

// ─────────────────────────────────────────────
//  PACKET HEADER (16 bytes, little-endian)
// ─────────────────────────────────────────────
struct __attribute__((packed)) MeshHeader {
    uint32_t dest;
    uint32_t from;
    uint32_t id;
    uint8_t  flags;       // bits[2:0]=hop_limit, bit[3]=want_ack, bit[4]=via_mqtt, bits[7:5]=hop_start
    uint8_t  channel;     // Meshtastic channel hash
    uint8_t  nextHop;     // next-hop node-id low byte, or 0
    uint8_t  relayNode;   // relay-node node-id low byte, or 0
};

static uint8_t makeFlags(int hopLimit, bool wantAck, int hopStart, bool viaMqtt = false) {
    return (uint8_t)(((hopLimit & 0x07)) |
                     ((wantAck ? 1 : 0) << 3) |
                     ((viaMqtt ? 1 : 0) << 4) |
                     ((hopStart & 0x07) << 5));
}

static uint8_t meshChannelHashForAppChannel(int ch) {
    return (ch == 0) ? MESH_LONGFAST_CHANNEL_HASH : (uint8_t)(0x80 | (ch & 0x7F));
}

static bool alreadySeen(uint32_t id) {
    for (int i = 0; i < SEEN_IDS_SIZE; i++)
        if (seenIds[i] == id) return true;
    return false;
}

static void markSeen(uint32_t id) {
    seenIds[seenIdx] = id;
    seenIdx = (seenIdx + 1) % SEEN_IDS_SIZE;
}

// ─────────────────────────────────────────────
//  CHANNEL → FREQUENCY
//  Meshtastic channels offset from base freq
//  Channel 0 = base, ch N = base + N * (BW/1000) MHz approx
//  (Simple approximation — real Meshtastic uses hash-based channel)
// ─────────────────────────────────────────────
static float channelFreq(int ch) {
    // Meshtastic US LongFast: 902.0–928.0 MHz, 8 channels
    // ch0=906.875, step ~3.125 MHz
    return LORA_FREQ + (ch * 3.125f);
}

static int armReceiveLocked(int ch) {
    if (!radio) return RADIOLIB_ERR_UNKNOWN;
    int state = radio->setFrequency(channelFreq(ch));
    if (state != RADIOLIB_ERR_NONE) return state;
    rxFlag = false;
    return radio->startReceive();
}

static bool armReceive(int ch, uint32_t wait_ms = 500) {
    if (!radio || !radioReady) return false;
    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        Serial.println("[MESH] RX arm: SPI mutex timeout");
        return false;
    }
    int state = armReceiveLocked(ch);
    xSemaphoreGiveRecursive(spi_mutex);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[MESH] RX arm failed: %d\n", state);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────
//  STATUS BAR
// ─────────────────────────────────────────────
static void setStatus(const char* msg, uint16_t col = COL_ACCENT) {
    strncpy(statusMsg, msg, sizeof(statusMsg)-1);
    statusColor  = col;
    lastStatusMs = millis();
}

// ─────────────────────────────────────────────
//  ADD MESSAGE TO CHANNEL
// ─────────────────────────────────────────────
static void addMsg(int ch, const char* sender, const char* text,
                   uint32_t nodeId, bool outgoing) {
    if (ch < 0 || ch >= MAX_CHANNELS) return;
    if (!ensureMsgStore()) return;
    int idx = msgCount[ch] % MAX_MSGS_PER_CH;
    MeshMsg& m = msgStore[ch][idx];
    strncpy(m.sender, sender, MAX_SENDER_LEN-1);
    m.sender[MAX_SENDER_LEN-1] = '\0';
    strncpy(m.text, text, MAX_MSG_LEN-1);
    m.text[MAX_MSG_LEN-1] = '\0';
    m.nodeId   = nodeId;
    m.outgoing = outgoing;
    m.valid    = true;
    msgCount[ch]++;
    // Auto-scroll to bottom
    int total = min(msgCount[ch], MAX_MSGS_PER_CH);
    msgScroll[ch] = max(0, total - meshRows());
}

static bool appendMeshTranscript(int ch, int headerCh, const char* sender,
                                 const char* text, uint32_t nodeId,
                                 bool outgoing, float rssi, float snr);

static int meshtasticHashToDisplayChannel(uint8_t channelHash) {
    if (channelHash == MESH_LONGFAST_CHANNEL_HASH) return 0;
    if ((channelHash & 0x80) != 0) {
        int ch = channelHash & 0x7F;
        if (ch > 0 && ch < MAX_CHANNELS) return ch;
    }
    return currentCh;
}

static void noteHeardNode(uint32_t nodeId, uint8_t channelHash, float rssi, float snr,
                          bool decodedText, size_t payloadLen) {
    if (nodeId == 0 || nodeId == myNodeId) return;

    uint32_t now = millis();
    int idx = -1;
    for (int i = 0; i < HEARD_NODES_SIZE; i++) {
        if (heardNodes[i].valid && heardNodes[i].nodeId == nodeId) {
            idx = i;
            break;
        }
    }

    bool isNew = false;
    if (idx < 0) {
        idx = heardNodeReplaceIdx;
        heardNodeReplaceIdx = (heardNodeReplaceIdx + 1) % HEARD_NODES_SIZE;
        heardNodes[idx].valid = true;
        heardNodes[idx].nodeId = nodeId;
        heardNodes[idx].lastAnnounceMs = 0;
        isNew = true;
    }

    HeardNode& n = heardNodes[idx];
    n.channelHash = channelHash;
    n.rssi = rssi;
    n.snr = snr;
    n.lastSeenMs = now;

    if (decodedText && !isNew) return;
    if (!isNew && now - n.lastAnnounceMs < HEARD_NODE_ANNOUNCE_MS) return;
    n.lastAnnounceMs = now;

    char senderStr[MAX_SENDER_LEN];
    snprintf(senderStr, sizeof(senderStr), "!%06x", (unsigned)(nodeId & 0xFFFFFF));

    char msg[96];
    snprintf(msg, sizeof(msg), "heard %s %s h=%02X len=%u r=%.0f s=%.1f",
             senderStr,
             decodedText ? "text" : "raw/enc",
             channelHash,
             (unsigned)payloadLen,
             rssi,
             snr);

    int ch = meshtasticHashToDisplayChannel(channelHash);
    addMsg(ch, "~node", msg, nodeId, false);
    appendMeshTranscript(ch, ch, "~node", msg, nodeId, false, rssi, snr);
}

static void csvPrintEscaped(FsFile& file, const char* text) {
    file.print('"');
    for (const char* p = text; p && *p; ++p) {
        if (*p == '"') {
            file.print("\"\"");
        } else if (*p == '\r' || *p == '\n') {
            file.print(' ');
        } else {
            file.print(*p);
        }
    }
    file.print('"');
}

static bool appendMeshTranscript(int ch, int headerCh, const char* sender,
                                 const char* text, uint32_t nodeId,
                                 bool outgoing, float rssi, float snr) {
    if (!g_sd_ready) return false;
    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }

    bool ok = false;
    bool existed = sd.exists(MESH_LOG_PATH);
    if (!sd.exists(MESH_LOG_DIR)) {
        sd.mkdir(MESH_LOG_DIR);
    }

    FsFile file = sd.open(MESH_LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
    if (file) {
        if (!existed || file.size() == 0) {
            file.println("millis,direction,channel,header_channel,node_id,sender,rssi,snr,text");
        }

        file.print(millis());
        file.print(',');
        file.print(outgoing ? "tx" : "rx");
        file.print(',');
        file.print(ch);
        file.print(',');
        file.print(headerCh);
        file.print(',');
        file.print("0x");
        file.print(nodeId, HEX);
        file.print(',');
        csvPrintEscaped(file, sender);
        file.print(',');
        if (outgoing) {
            file.print(',');
        } else {
            file.print(rssi, 0);
            file.print(',');
            file.print(snr, 1);
        }
        file.print(',');
        csvPrintEscaped(file, text);
        file.println();
        file.sync();
        file.close();
        ok = true;
    }

    xSemaphoreGiveRecursive(spi_mutex);

    if (!ok && millis() - meshLogLastWarnMs > 5000) {
        Serial.println("[MESH] SD transcript write failed");
        meshLogLastWarnMs = millis();
    }
    return ok;
}

// ─────────────────────────────────────────────
//  RADIO INIT
// ─────────────────────────────────────────────
static bool initRadio() {
    // SPI bus selection per device:
    //   Cardputer: SX1262 lives on HSPI (peripheral bus 14/39/40)
    //   T-Deck / Pager: SX1262 shares the same FSPI bus as TFT/SD
    //
    // On Cardputer, the default `SPI` instance was bound to the LCD
    // pins (35/36) during display init — talking to SX1262 through
    // that bus reaches no one. The Cap is on the HSPI bus alongside
    // SD; both share via SPI Bus Treaty mutex with different CS.
#ifdef DEVICE_CARDPUTER_ADV
    loraModule = new Module(MESH_LORA_CS, MESH_LORA_DIO1, MESH_LORA_RST, MESH_LORA_BUSY,
                            cardputerSdSPI, lspSettings);
#else
    loraModule = new Module(MESH_LORA_CS, MESH_LORA_DIO1, MESH_LORA_RST, MESH_LORA_BUSY,
                            SPI, lspSettings);
#endif
    radio = new SX1262(loraModule);

    // SPI Bus Treaty: Take mutex during full radio initialization
    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        Serial.println("[MESH] initRadio: SPI mutex timeout");
        delete radio; radio = nullptr;
        delete loraModule; loraModule = nullptr;
        return false;
    }

    int state = radio->begin(
        LORA_FREQ,
        LORA_BW,
        LORA_SF,
        LORA_CR,
        LORA_SYNC,
        LORA_POWER,
        LORA_PREAMBLE
    );

    if (state != RADIOLIB_ERR_NONE) {
        xSemaphoreGiveRecursive(spi_mutex);
        Serial.printf("[MESH] Radio init failed: %d\n", state);
        // Tear down both radio and module so the exit path doesn't
        // try to standby()/delete a half-initialized SX1262 object.
        // Without this, exiting Mesh Messenger after a failed init
        // calls radio->standby() on a chip that never completed
        // begin(), which can hang an SPI transaction or dereference
        // an internal field that init never populated. Setting both
        // pointers to nullptr makes the cleanup at end of loop into
        // a no-op, and any later code that null-checks `radio` will
        // skip its work cleanly.
        delete radio;       radio = nullptr;
        delete loraModule;  loraModule = nullptr;
        return false;
    }

    // SX1262-specific: set DIO2 as RF switch
    radio->setDio2AsRfSwitch(true);
    // Set current limit
    radio->setCurrentLimit(140.0f);
    // Set RX boosted gain
    radio->setRxBoostedGainMode(true);
    radio->setPacketReceivedAction(meshRxISR);
    rxFlag = false;

    xSemaphoreGiveRecursive(spi_mutex);

    Serial.printf("[MESH] Radio ready. NodeID: %08x  Freq: %.3f MHz\n",
                  (unsigned)myNodeId, LORA_FREQ);
    return true;
}

static void deinitRadio() {
    if (radio) {
        radio->clearPacketReceivedAction();
        rxFlag = false;
        radio->standby();
        delete radio;
        radio = nullptr;
    }
    if (loraModule) {
        delete loraModule;
        loraModule = nullptr;
    }
    // Do NOT call SPI.end() — the shared bus is still needed by TFT and SD
}

// ─────────────────────────────────────────────
//  SEND TEXT MESSAGE
// ─────────────────────────────────────────────
static bool sendText(int ch, const char* text) {
    if (!radio || !radioReady) return false;

    // Build payload
    uint8_t payload[256];
    int payloadLen = encodeTextPayload(text, payload, sizeof(payload));

    // Build full packet
    uint8_t pkt[256 + sizeof(MeshHeader)];
    MeshHeader* hdr = (MeshHeader*)pkt;

    hdr->dest  = MESH_BROADCAST;
    hdr->from  = myNodeId;
    hdr->id    = (uint32_t)(esp_random());
    hdr->flags = makeFlags(MESH_HOP_LIMIT, false, MESH_HOP_LIMIT);
    hdr->channel = meshChannelHashForAppChannel(ch);
    hdr->nextHop = 0;
    hdr->relayNode = 0;

    memcpy(pkt + sizeof(MeshHeader), payload, payloadLen);
    int totalLen = sizeof(MeshHeader) + payloadLen;

    // SPI Bus Treaty: Take mutex before LoRa SPI access
    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        Serial.println("[MESH] TX: SPI mutex timeout");
        return false;
    }

    // Tune to channel frequency
    int tuneState = radio->setFrequency(channelFreq(ch));
    if (tuneState != RADIOLIB_ERR_NONE) {
        armReceiveLocked(currentCh);
        xSemaphoreGiveRecursive(spi_mutex);
        Serial.printf("[MESH] TX tune failed: %d\n", tuneState);
        return false;
    }

    rxFlag = false;
    transmitting = true;
    int state = radio->transmit(pkt, totalLen);
    transmitting = false;
    rxFlag = false;

    int rxState = armReceiveLocked(currentCh);

    xSemaphoreGiveRecursive(spi_mutex);

    if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("[MESH] RX re-arm after TX failed: %d\n", rxState);
    }

    if (state == RADIOLIB_ERR_NONE) {
        markSeen(hdr->id);
        char senderStr[MAX_SENDER_LEN];
        snprintf(senderStr, sizeof(senderStr), "~me");
        addMsg(ch, senderStr, text, myNodeId, true);
        appendMeshTranscript(ch, ch, senderStr, text, myNodeId, true, 0.0f, 0.0f);
        Serial.printf("[MESH] TX ch%d id=%08x: %s\n", ch, (unsigned)hdr->id, text);
        return true;
    } else {
        Serial.printf("[MESH] TX failed: %d\n", state);
        return false;
    }
}

// ─────────────────────────────────────────────
//  RECEIVE — interrupt flag + non-blocking read
// ─────────────────────────────────────────────
static int pollReceive() {
    if (!radio || transmitting || !rxFlag) return 0;
    rxFlag = false;

    // SPI Bus Treaty: try mutex non-blocking. If the bus is busy,
    // keep the flag set so the next loop can drain the packet.
    if (!spi_mutex || xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        rxFlag = true;
        return 0;
    }

    uint8_t buf[256];
    size_t rxLen = radio->getPacketLength();
    size_t readLen = (rxLen > 0) ? min(rxLen, sizeof(buf)) : sizeof(buf);
    int state = radio->readData(buf, readLen);
    float snr = 0.0f;
    float rssi = 0.0f;
    if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_CRC_MISMATCH) {
        rssi = radio->getRSSI();
        snr  = radio->getSNR();
    }
    int rxState = armReceiveLocked(currentCh);

    xSemaphoreGiveRecursive(spi_mutex);

    if (rxState != RADIOLIB_ERR_NONE) {
        Serial.printf("[MESH] RX re-arm failed: %d\n", rxState);
    }

    if (state == RADIOLIB_ERR_NONE) {
        if (rxLen > sizeof(buf)) {
            Serial.printf("[MESH] RX packet too large: %u\n", (unsigned)rxLen);
            return 0;
        }
        if (rxLen <= (int)sizeof(MeshHeader)) return 0;

        MeshHeader* hdr = (MeshHeader*)buf;
        uint32_t destId = hdr->dest;
        uint32_t fromId = hdr->from;
        uint32_t pktId  = hdr->id;
        uint8_t channelHash = hdr->channel;

        // Ignore our own packets
        if (fromId == myNodeId) return 0;

        // Deduplicate
        if (alreadySeen(pktId)) return 0;
        markSeen(pktId);

        // Only show broadcast or packets destined for us
        if (destId != MESH_BROADCAST && destId != myNodeId) return 0;

        uint8_t* payload = buf + sizeof(MeshHeader);
        int payloadLen   = rxLen - sizeof(MeshHeader);

        char textBuf[MAX_MSG_LEN] = "";
        int portnum = 0;
        bool hasText = decodeTextPayload(payload, payloadLen,
                                          textBuf, sizeof(textBuf), &portnum);

        bool isText = hasText && portnum == 1;
        noteHeardNode(fromId, channelHash, rssi, snr, isText, payloadLen);
        if (!isText) {
            Serial.printf("[MESH] RX raw/enc hash=%02X from !%06x len=%d RSSI=%.0f SNR=%.1f\n",
                          channelHash, (unsigned)(fromId & 0xFFFFFF), payloadLen, rssi, snr);
            return 2;
        }

        // We can only receive on the frequency the radio is currently tuned to,
        // so display on currentCh. The Meshtastic channel hash is logged as
        // the network-layer diagnostic; channel 0 should be LongFast hash 0x02.
        int hopStart = (hdr->flags >> 5) & 0x07;
        int headerCh = meshtasticHashToDisplayChannel(channelHash);
        int rxCh = currentCh;

        char senderStr[MAX_SENDER_LEN];
        snprintf(senderStr, sizeof(senderStr), "!%06x",
                 (unsigned)(fromId & 0xFFFFFF));

        addMsg(rxCh, senderStr, textBuf, fromId, false);
        appendMeshTranscript(rxCh, headerCh, senderStr, textBuf, fromId, false, rssi, snr);
        Serial.printf("[MESH] RX ch%d hash=%02X hop=%d from %s: %s  RSSI=%.0f SNR=%.1f\n",
                      rxCh, channelHash, hopStart, senderStr, textBuf, rssi, snr);

        // ── Bridge streaming hook ─────────────────────────────────────
        // Emit mesh_link JSON so pm_bridge.py can log to lora_*.csv
        // and The Clinician can receive live LoRa topology data.
        if (wardrive_bridge_streaming) {
            // Our node ID — last 6 hex chars of MAC
            char myNodeStr[12];
            snprintf(myNodeStr, sizeof(myNodeStr), "!%06x",
                     (unsigned)(getNodeId() & 0xFFFFFF));
            // Get frequency from radio config
            float freq_mhz = 915.0f;  // default; override if radio exposes it
            Serial.printf(
                "{\"event\":\"mesh_link\","
                "\"from\":\"%s\",\"to\":\"%s\","
                "\"rssi\":%.0f,\"snr\":%.1f,"
                "\"freq\":%.1f,\"sf\":7,\"bw\":125,"
                "\"quality\":%d,"
                "\"lat\":%.6f,\"lng\":%.6f}\n",
                senderStr, myNodeStr,
                rssi, snr, freq_mhz,
                (int)max(0.0f, min(100.0f, (rssi + 120.0f) * 100.0f / 80.0f)),
                gps.location.isValid() ? gps.location.lat() : 0.0,
                gps.location.isValid() ? gps.location.lng() : 0.0);
        }

        return 2;

    } else if (state != RADIOLIB_ERR_RX_TIMEOUT &&
               state != RADIOLIB_ERR_CRC_MISMATCH) {
        // Ignore timeouts and CRC mismatches silently
        Serial.printf("[MESH] RX state: %d\n", state);
    }

    // No retune needed — pollReceive doesn't change channel
    return 0;
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawHeader() {
    const int w = meshDispW();
    const int tabW = meshTabW();
    gfx->fillRect(0, 0, w, MESH_HDR_H, COL_HDR);
    gfx->drawFastHLine(0, MESH_HDR_H - 1, w, COL_ACCENT);

    // Channel tabs
    for (int i = 0; i < MAX_CHANNELS; i++) {
        int tx = i * tabW;
        bool active = (i == currentCh);
        int tabRight = (i == MAX_CHANNELS - 1) ? w : tx + tabW;
        int tabPixels = max(1, tabRight - tx);
        gfx->fillRect(tx, 0, tabPixels - 1, MESH_HDR_H - 1, active ? 0x0020 : COL_HDR);
        if (active) gfx->drawFastHLine(tx, MESH_HDR_H - 1, tabPixels - 1, COL_ACCENT);
        gfx->setTextSize(1);
        gfx->setTextColor(active ? COL_CH_ACTIVE : COL_CH_INACTIVE);
        gfx->setCursor(tx+4, 7);
        // Short name for tab
        const char* name = CHANNEL_NAMES[i] + 1; // skip '#'
        char shortName[16];
        int maxChars = min((int)sizeof(shortName) - 1, max(1, (tabPixels - 12) / 6));
        strncpy(shortName, name, maxChars);
        shortName[maxChars] = '\0';
        gfx->print(shortName);

        // Unread dot
        if (!active && msgCount[i] > 0) {
            gfx->fillCircle(tabRight - 6, 5, 3, COL_YELLOW);
        }
    }

    // RSSI/status in top-right corner if space
    // (overlaps last tab slightly — acceptable tradeoff)
}

static void drawMessages() {
    const int w = meshDispW();
    const int msgY = meshMsgY();
    const int msgH = meshMsgH();
    const int inputY = meshInputY();
    gfx->fillRect(0, msgY, w, msgH, COL_BG);

    int ch = currentCh;
    int total = min(msgCount[ch], MAX_MSGS_PER_CH);
    if (total == 0) {
        gfx->setTextSize(1); gfx->setTextColor(COL_DIM);
        gfx->setCursor(8, msgY + msgH / 2 - 6);
        gfx->print("No messages. Type below and press ENTER.");
        return;
    }

    // Draw from scroll position
    int startIdx = msgScroll[ch];
    int y = msgY + 2;

    for (int i = startIdx; i < total && y < inputY - MESH_MSG_ROW_H; i++) {
        MeshMsg& m = msgStore[ch][i % MAX_MSGS_PER_CH];
        if (!m.valid) continue;

        // Sender tag
        uint16_t senderCol = m.outgoing ? COL_MSG_OUT : COL_CYAN;
        gfx->setTextSize(1);
        gfx->setTextColor(senderCol);
        gfx->setCursor(2, y);
        gfx->print(m.sender);
        gfx->print(" ");

        int senderPixels = (strlen(m.sender) + 1) * 6;

        // Message text — word wrap
        uint16_t textCol = m.outgoing ? COL_MSG_OUT : COL_MSG_IN;
        gfx->setTextColor(textCol);
        gfx->setCursor(2 + senderPixels, y);

        // Simple truncation to fit on one line
        int maxChars = max(1, (w - 4 - senderPixels) / 6);
        char display[64];
        strncpy(display, m.text, min(maxChars, 63));
        display[min(maxChars, 63)] = '\0';
        gfx->print(display);

        y += MESH_MSG_ROW_H;
    }

    // Scroll indicator
    if (total > meshRows()) {
        int indicatorX = max(0, w - 10);
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(indicatorX, msgY + 2);
        gfx->print("^");
        gfx->setCursor(indicatorX, inputY - 10);
        gfx->print("v");
    }
}

static void drawInput() {
    const int w = meshDispW();
    const int inputY = meshInputY();
    const int inputH = meshInputH();
    gfx->fillRect(0, inputY, w, inputH, COL_INPUT_BG);
    gfx->drawFastHLine(0, inputY, w, COL_ACCENT);

    if (w <= 240) {
        gfx->setTextSize(1);
        gfx->setTextColor(COL_ACCENT);
        gfx->setCursor(2, inputY + 3);
        gfx->print(CHANNEL_NAMES[currentCh]);
        gfx->print(">");

        if (millis() - lastStatusMs < 3000) {
            int statusChars = min((int)strlen(statusMsg), max(0, (w - 92) / 6));
            if (statusChars > 0) {
                char compactStatus[16];
                strncpy(compactStatus, statusMsg, min(statusChars, (int)sizeof(compactStatus) - 1));
                compactStatus[min(statusChars, (int)sizeof(compactStatus) - 1)] = '\0';
                gfx->setTextColor(statusColor);
                gfx->setCursor(w - (int)strlen(compactStatus) * 6 - 4, inputY + 3);
                gfx->print(compactStatus);
            }
        }

        gfx->setCursor(2, inputY + 17);
        if (inputLen > 0) {
            int maxChars = max(2, (w - 6) / 6 - 1); // leave room for cursor
            bool clipped = inputLen > maxChars;
            int shownChars = clipped ? maxChars - 1 : maxChars;
            int start = max(0, inputLen - shownChars);
            gfx->setTextColor(COL_INPUT_FG);
            if (clipped) gfx->print("<");
            gfx->print(inputBuf + start);
            gfx->print("_");
        } else {
            gfx->setTextColor(COL_DIM);
            gfx->print("type message...");
        }
        return;
    }

    // Channel prefix
    gfx->setTextSize(1);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(2, inputY + 10);
    gfx->print(CHANNEL_NAMES[currentCh]);
    gfx->print(">");

    int prefixW = (strlen(CHANNEL_NAMES[currentCh]) + 1) * 6 + 2;

    // Input text
    gfx->setTextColor(COL_INPUT_FG);
    gfx->setCursor(prefixW + 2, inputY + 10);
    if (inputLen > 0) {
        // Show last N chars that fit
        int reserve = (w >= 300) ? 82 : 4;
        int maxChars = max(1, (w - prefixW - reserve) / 6 - 1);
        int start = max(0, inputLen - maxChars);
        gfx->print(inputBuf + start);
        // Cursor blink approximation
        gfx->print("_");
    } else {
        gfx->setTextColor(COL_DIM);
        gfx->print("type message...");
    }

    // Status bar (right side of input)
    if (w >= 300) {
        if (millis() - lastStatusMs < 3000) {
            gfx->setTextColor(statusColor);
            gfx->setCursor(max(prefixW + 48, w - 120), inputY + 10);
            gfx->print(statusMsg);
        } else if (!radioReady) {
            gfx->setTextColor(COL_RED);
            gfx->setCursor(max(prefixW + 48, w - 72), inputY + 10);
            gfx->print("[NO RADIO]");
        } else {
            gfx->setTextColor(COL_DIM);
            gfx->setCursor(max(prefixW + 48, w - 42), inputY + 10);
            gfx->print("[MESH]");
        }
    }
}

static void drawFull() {
    gfx->fillScreen(COL_BG);
    drawHeader();
    drawMessages();
    drawInput();
}

static void drawMessagesAndInput() {
    drawMessages();
    drawInput();
}

static void meshSwitchChannel(int delta) {
    int next = (currentCh + delta + MAX_CHANNELS) % MAX_CHANNELS;
    if (next == currentCh) return;
    currentCh = next;
    inputLen = 0;
    inputBuf[0] = '\0';
    armReceive(currentCh);
    drawFull();
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_mesh_messenger() {
    myNodeId = getNodeId();
    if (!ensureMsgStore()) {
        gfx->fillScreen(COL_BG);
        gfx->setTextSize(1);
        gfx->setTextColor(COL_RED);
        gfx->setCursor(8, 40);
        gfx->print("Mesh message memory unavailable.");
        delay(1200);
        return;
    }

    // Show init screen
    gfx->fillScreen(COL_BG);
    gfx->fillRect(0, 0, meshDispW(), MESH_HDR_H, COL_HDR);
    gfx->setTextSize(1); gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, 7); gfx->print("MESH MESSENGER — INITIALIZING");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 40);
    gfx->printf("Node ID: !%08x", (unsigned)myNodeId);
    gfx->setCursor(8, 56);
    gfx->print("Starting SX1262 radio...");

    radioReady = initRadio();

    if (!radioReady) {
        gfx->setTextColor(COL_RED);
        gfx->setCursor(8, 80);
        gfx->print("Radio init FAILED.");
        gfx->setCursor(8, 96);
        gfx->print("Check LoRa module (CS:9 RST:17 DIO1:45 BUSY:13)");
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(8, 120);
        gfx->print("Tap header to exit.");
        while (true) {
            int16_t tx, ty;
            if (get_touch(&tx,&ty) && ty < MESH_HDR_H) {
                while(get_touch(&tx,&ty)){delay(10);}
                break;
            }
            char k = get_keypress();
            if (k=='q'||k=='Q') break;
            delay(50);
        }
        freeMsgStore();
        return;
    }

    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, 80);
    gfx->printf("OK — %.3f MHz  SF%d  BW%.0fk", LORA_FREQ, LORA_SF, LORA_BW);
    gfx->setCursor(8, 96);
    gfx->print("Meshtastic LongFast compatible.");
    delay(1200);

    // System welcome messages
    addMsg(0, "~sys", "Mesh Messenger ready. Listening on LongFast.", 0, false);
    char idMsg[64];
    snprintf(idMsg, sizeof(idMsg), "Your node ID: !%08x", (unsigned)myNodeId);
    addMsg(0, "~sys", idMsg, 0, false);
    addMsg(0, "~sys",
           g_sd_ready ? "SD transcript: /mesh_logs/messages.csv"
                      : "SD not ready; transcript disabled this run.",
           0, false);
#ifdef DEVICE_TLORAPAGER
    addMsg(0, "~sys", "WHEEL=switch channel  CLICK=send  Q=quit", 0, false);
#elif defined(DEVICE_CARDPUTER_ADV)
    addMsg(0, "~sys", "TAB=switch channel  ENTER=send  Q=quit", 0, false);
#else
    addMsg(0, "~sys", "TAB=switch channel  ENTER=send  Q=quit", 0, false);
#endif

    drawFull();

    armReceive(currentCh);

    bool running = true;
    uint32_t lastDraw  = millis();

    while (running) {

        // ── Drain received packets only when DIO1 has fired ──
        int meshUiUpdate = pollReceive();
        if (meshUiUpdate == 2) {
            drawMessagesAndInput();
        }

        // ── Periodic UI refresh ──
        if (millis() - lastDraw > 500) {
            drawInput();   // Refresh status/cursor blink
            lastDraw = millis();
        }

        // ── Input handling ──
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        // Header tap — top half = exit, bottom half = channel switch
        // Use ty < 40 for GT911 calibration tolerance
        if (get_touch(&tx, &ty) && ty < MESH_HDR_H + 17) {
            while(get_touch(&tx,&ty)){delay(10);}
            // Top portion of header = exit
            if (ty < 20) {
                running = false;
                continue;
            }
            // Bottom portion of header = channel tab switch
            int tappedCh = tx / meshTabW();
            if (tappedCh >= 0 && tappedCh < MAX_CHANNELS && tappedCh != currentCh) {
                currentCh = tappedCh;
                inputLen = 0; inputBuf[0] = '\0';
                armReceive(currentCh);
                drawFull();
            }
            continue;
        }

        // Q = quit
        if (k == 'q' || k == 'Q') {
            running = false;
            continue;
        }

        // Tab = switch channel
        if (k == 9) {
            meshSwitchChannel(1);
            continue;
        }

#ifdef DEVICE_TLORAPAGER
        // Pager encoder: scroll up moves one tab left, scroll down moves one tab right.
        if (tb.y == -1) {
            meshSwitchChannel(-1);
            continue;
        }
        if (tb.y == 1) {
            meshSwitchChannel(1);
            continue;
        }
#else
        // T-Deck trackball: vertical movement scrolls the message history.
        if (tb.y == -1) {
            int total = min(msgCount[currentCh], MAX_MSGS_PER_CH);
            if (msgScroll[currentCh] > 0) {
                msgScroll[currentCh]--;
                drawMessages();
            }
        }
        if (tb.y == 1) {
            int total = min(msgCount[currentCh], MAX_MSGS_PER_CH);
            if (msgScroll[currentCh] < total - meshRows()) {
                msgScroll[currentCh]++;
                drawMessages();
            }
        }
#endif

        // Enter / trackball click = send
        if ((k == 13 || tb.clicked) && inputLen > 0) {
            if (!radioReady) {
                setStatus("No radio!", COL_RED);
            } else {
                setStatus("Sending...", COL_YELLOW);
                drawInput();
                if (sendText(currentCh, inputBuf)) {
                    setStatus("Sent!", COL_ACCENT);
                } else {
                    setStatus("TX failed", COL_RED);
                }
            }
            inputLen = 0;
            inputBuf[0] = '\0';
            drawMessagesAndInput();
            continue;
        }

        // Backspace
        if ((k == 8 || k == 127) && inputLen > 0) {
            inputBuf[--inputLen] = '\0';
            drawInput();
            continue;
        }

        // Printable characters
        if (k >= 32 && k < 127 && inputLen < (int)sizeof(inputBuf) - 1) {
            inputBuf[inputLen++] = k;
            inputBuf[inputLen]   = '\0';
            drawInput();
            continue;
        }

        delay(10);
        yield();
    }

    // Cleanup.
    // radio may be nullptr if initRadio() failed; deinitRadio() and the
    // standby() call above must both null-check. (Pre-fix: standby() was
    // called unconditionally and could crash on a half-initialized radio
    // returning to the launcher after init_failed -2.)
    if (radio && radioReady) {
        // Take the SPI mutex briefly so standby() doesn't collide with
        // any other Treaty participant (SD writes, display refresh on
        // shared bus, etc).
        if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            radio->standby();
            xSemaphoreGiveRecursive(spi_mutex);
        }
    }
    deinitRadio();
    freeMsgStore();
    gfx->fillScreen(COL_BG);
}
