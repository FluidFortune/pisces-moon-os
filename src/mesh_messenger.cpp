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
 * T-Deck Plus SX1262 pins:
 *   CS   = GPIO9   RST  = GPIO17
 *   DIO1 = GPIO45  BUSY = GPIO13
 *   SPI  = shared (MOSI:41, MISO:38, SCK:40)
 *
 * Protocol: Meshtastic LongFast preset
 *   Freq:  906.875 MHz (US) / 869.525 MHz (EU — change LORA_FREQ)
 *   BW:    250 kHz
 *   SF:    11
 *   CR:    4/8
 *   Sync:  0x2B   (Meshtastic network ID)
 *   Preamble: 16 symbols
 *
 * Packet format (Meshtastic-compatible):
 *   [0..3]  dest    uint32_le  destination node ID (0xFFFFFFFF = broadcast)
 *   [4..7]  from    uint32_le  sender node ID
 *   [8..11] id      uint32_le  unique packet ID
 *   [12..15] flags  uint32_le  hop_limit(3b) | want_ack(1b) | hop_start(3b) | ...
 *   [16..]  payload             protobuf-encoded Data message (unencrypted default channel)
 *
 * Data protobuf (TEXT_MESSAGE_APP, portnum=1):
 *   0x08 0x01               field 1 (portnum) = 1
 *   0x12 <len> <text bytes> field 2 (payload) = UTF-8 text
 *
 * Channels: mapped to Meshtastic channel index 0-7
 *   Channel 0 = LongFast default (most Meshtastic nodes)
 *   Channels 1-7 = custom secondary channels
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
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "mesh_messenger.h"

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  SX1262 HARDWARE PINS
// ─────────────────────────────────────────────
#define LORA_CS    9
#define LORA_RST   17
#define LORA_DIO1  45
#define LORA_BUSY  13

// ─────────────────────────────────────────────
//  RADIO SETTINGS — Meshtastic LongFast US
//  Change LORA_FREQ to 869.525 for EU
// ─────────────────────────────────────────────
#define LORA_FREQ       906.875f   // MHz — US LongFast ch 0
#define LORA_BW         250.0f     // kHz
#define LORA_SF         11
#define LORA_CR         8          // 4/8
#define LORA_SYNC       0x2B       // Meshtastic sync word
#define LORA_PREAMBLE   16
#define LORA_POWER      22         // dBm — max legal for SX1262

// Meshtastic broadcast address
#define MESH_BROADCAST  0xFFFFFFFF
#define MESH_HOP_LIMIT  3

// ─────────────────────────────────────────────
//  CHANNELS
// ─────────────────────────────────────────────
#define MAX_CHANNELS    4
static const char* CHANNEL_NAMES[MAX_CHANNELS] = {
    "#general",     // ch 0 — Meshtastic default
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

static MeshMsg msgStore[MAX_CHANNELS][MAX_MSGS_PER_CH];
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

// Seen packet IDs for dedup (rolling 16-entry cache)
#define SEEN_IDS_SIZE  16
static uint32_t seenIds[SEEN_IDS_SIZE] = {0};
static int      seenIdx = 0;

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
// Channel tab bar: y=0..22
// Message area:   y=23..208
// Input bar:      y=209..239
#define HDR_H       23
#define INPUT_Y     209
#define INPUT_H     31
#define MSG_Y       HDR_H
#define MSG_H       (INPUT_Y - HDR_H)
#define MSG_ROW_H   12
#define MSG_ROWS    ((MSG_H - 2) / MSG_ROW_H)

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
    uint32_t flags;    // bits[2:0]=hop_limit, bit[3]=want_ack, bits[6:4]=hop_start
};

static uint32_t makeFlags(int hopLimit, bool wantAck, int hopStart) {
    return ((uint32_t)(hopLimit & 0x07)) |
           ((uint32_t)(wantAck ? 1 : 0) << 3) |
           ((uint32_t)(hopStart & 0x07) << 4);
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
    msgScroll[ch] = max(0, total - MSG_ROWS);
}

// ─────────────────────────────────────────────
//  RADIO INIT
// ─────────────────────────────────────────────
static bool initRadio() {
    // SPI bus already running from main.cpp — just reference it
    loraModule = new Module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY,
                            SPI, lspSettings);
    radio = new SX1262(loraModule);

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
        Serial.printf("[MESH] Radio init failed: %d\n", state);
        return false;
    }

    // SX1262-specific: set DIO2 as RF switch
    radio->setDio2AsRfSwitch(true);
    // Set current limit
    radio->setCurrentLimit(140.0f);
    // Set RX boosted gain
    radio->setRxBoostedGainMode(true);

    Serial.printf("[MESH] Radio ready. NodeID: %08x  Freq: %.3f MHz\n",
                  (unsigned)myNodeId, LORA_FREQ);
    return true;
}

static void deinitRadio() {
    if (radio) {
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

    memcpy(pkt + sizeof(MeshHeader), payload, payloadLen);
    int totalLen = sizeof(MeshHeader) + payloadLen;

    // Tune to channel frequency
    radio->setFrequency(channelFreq(ch));

    transmitting = true;
    int state = radio->transmit(pkt, totalLen);
    transmitting = false;

    if (state == RADIOLIB_ERR_NONE) {
        markSeen(hdr->id);
        char senderStr[MAX_SENDER_LEN];
        snprintf(senderStr, sizeof(senderStr), "~me");
        addMsg(ch, senderStr, text, myNodeId, true);
        Serial.printf("[MESH] TX ch%d id=%08x: %s\n", ch, (unsigned)hdr->id, text);
        return true;
    } else {
        Serial.printf("[MESH] TX failed: %d\n", state);
        return false;
    }
}

// ─────────────────────────────────────────────
//  RECEIVE — non-blocking poll
// ─────────────────────────────────────────────
static void pollReceive() {
    if (!radio || transmitting) return;

    uint8_t buf[256];
    int state = radio->receive(buf, sizeof(buf));

    if (state == RADIOLIB_ERR_NONE) {
        int rxLen = radio->getPacketLength();
        if (rxLen <= (int)sizeof(MeshHeader)) return;

        MeshHeader* hdr = (MeshHeader*)buf;
        uint32_t destId = hdr->dest;
        uint32_t fromId = hdr->from;
        uint32_t pktId  = hdr->id;

        // Ignore our own packets
        if (fromId == myNodeId) return;

        // Deduplicate
        if (alreadySeen(pktId)) return;
        markSeen(pktId);

        // Only show broadcast or packets destined for us
        if (destId != MESH_BROADCAST && destId != myNodeId) return;

        uint8_t* payload = buf + sizeof(MeshHeader);
        int payloadLen   = rxLen - sizeof(MeshHeader);

        char textBuf[MAX_MSG_LEN] = "";
        int portnum = 0;
        bool hasText = decodeTextPayload(payload, payloadLen,
                                          textBuf, sizeof(textBuf), &portnum);

        if (!hasText || portnum != 1) return;  // Only TEXT_MESSAGE_APP

        // Determine which channel to show on based on flags channel bits
        // Meshtastic uses a channel hash — we simplify to hop_start as channel hint
        int hopStart = (hdr->flags >> 4) & 0x07;
        int rxCh = min(hopStart, MAX_CHANNELS - 1);

        char senderStr[MAX_SENDER_LEN];
        snprintf(senderStr, sizeof(senderStr), "!%06x",
                 (unsigned)(fromId & 0xFFFFFF));

        float snr  = radio->getSNR();
        float rssi = radio->getRSSI();

        addMsg(rxCh, senderStr, textBuf, fromId, false);
        Serial.printf("[MESH] RX ch%d from %s: %s  RSSI=%.0f SNR=%.1f\n",
                      rxCh, senderStr, textBuf, rssi, snr);

        // Brief notification if we're on a different channel
        if (rxCh != currentCh) {
            char notif[48];
            snprintf(notif, sizeof(notif), "New msg in %s", CHANNEL_NAMES[rxCh]);
            setStatus(notif, COL_YELLOW);
        }

    } else if (state != RADIOLIB_ERR_RX_TIMEOUT &&
               state != RADIOLIB_ERR_CRC_MISMATCH) {
        // Ignore timeouts and CRC mismatches silently
        Serial.printf("[MESH] RX state: %d\n", state);
    }

    // Retune to current channel after receive
    radio->setFrequency(channelFreq(currentCh));
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawHeader() {
    gfx->fillRect(0, 0, 320, HDR_H, COL_HDR);
    gfx->drawFastHLine(0, HDR_H-1, 320, COL_ACCENT);

    // Channel tabs
    int tabW = 80;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        int tx = i * tabW;
        bool active = (i == currentCh);
        gfx->fillRect(tx, 0, tabW-1, HDR_H-1, active ? 0x0020 : COL_HDR);
        if (active) gfx->drawFastHLine(tx, HDR_H-1, tabW-1, COL_ACCENT);
        gfx->setTextSize(1);
        gfx->setTextColor(active ? COL_CH_ACTIVE : COL_CH_INACTIVE);
        gfx->setCursor(tx+4, 7);
        // Short name for tab
        const char* name = CHANNEL_NAMES[i] + 1; // skip '#'
        char shortName[8];
        strncpy(shortName, name, 7); shortName[7] = '\0';
        gfx->print(shortName);

        // Unread dot
        if (!active && msgCount[i] > 0) {
            gfx->fillCircle(tx + tabW - 6, 5, 3, COL_YELLOW);
        }
    }

    // RSSI/status in top-right corner if space
    // (overlaps last tab slightly — acceptable tradeoff)
}

static void drawMessages() {
    gfx->fillRect(0, MSG_Y, 320, MSG_H, COL_BG);

    int ch = currentCh;
    int total = min(msgCount[ch], MAX_MSGS_PER_CH);
    if (total == 0) {
        gfx->setTextSize(1); gfx->setTextColor(COL_DIM);
        gfx->setCursor(8, MSG_Y + MSG_H/2 - 6);
        gfx->print("No messages. Type below and press ENTER.");
        return;
    }

    // Draw from scroll position
    int startIdx = msgScroll[ch];
    int y = MSG_Y + 2;

    for (int i = startIdx; i < total && y < INPUT_Y - MSG_ROW_H; i++) {
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
        int maxChars = (318 - senderPixels) / 6;
        char display[64];
        strncpy(display, m.text, min(maxChars, 63));
        display[min(maxChars, 63)] = '\0';
        gfx->print(display);

        y += MSG_ROW_H;
    }

    // Scroll indicator
    if (total > MSG_ROWS) {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(310, MSG_Y + 2);
        gfx->print("^");
        gfx->setCursor(310, INPUT_Y - 10);
        gfx->print("v");
    }
}

static void drawInput() {
    gfx->fillRect(0, INPUT_Y, 320, INPUT_H, COL_INPUT_BG);
    gfx->drawFastHLine(0, INPUT_Y, 320, COL_ACCENT);

    // Channel prefix
    gfx->setTextSize(1);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(2, INPUT_Y + 10);
    gfx->print(CHANNEL_NAMES[currentCh]);
    gfx->print(">");

    int prefixW = (strlen(CHANNEL_NAMES[currentCh]) + 1) * 6 + 2;

    // Input text
    gfx->setTextColor(COL_INPUT_FG);
    gfx->setCursor(prefixW + 2, INPUT_Y + 10);
    if (inputLen > 0) {
        // Show last N chars that fit
        int maxChars = (318 - prefixW) / 6 - 1;
        int start = max(0, inputLen - maxChars);
        gfx->print(inputBuf + start);
        // Cursor blink approximation
        gfx->print("_");
    } else {
        gfx->setTextColor(COL_DIM);
        gfx->print("type message...");
    }

    // Status bar (right side of input)
    if (millis() - lastStatusMs < 3000) {
        gfx->setTextColor(statusColor);
        gfx->setCursor(200, INPUT_Y + 10);
        gfx->print(statusMsg);
    } else if (!radioReady) {
        gfx->setTextColor(COL_RED);
        gfx->setCursor(240, INPUT_Y + 10);
        gfx->print("[NO RADIO]");
    } else {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(260, INPUT_Y + 10);
        gfx->print("[MESH]");
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

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_mesh_messenger() {
    myNodeId = getNodeId();

    // Show init screen
    gfx->fillScreen(COL_BG);
    gfx->fillRect(0, 0, 320, HDR_H, COL_HDR);
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
            if (get_touch(&tx,&ty) && ty<HDR_H) {
                while(get_touch(&tx,&ty)){delay(10);}
                break;
            }
            char k = get_keypress();
            if (k=='q'||k=='Q') break;
            delay(50);
        }
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
    addMsg(0, "~sys", "TAB=switch channel  ENTER=send  Q=quit", 0, false);

    drawFull();

    // Start async RX
    radio->startReceive();

    bool running = true;
    uint32_t lastRx    = millis();
    uint32_t lastDraw  = millis();

    while (running) {

        // ── Poll for received packets ──
        if (millis() - lastRx > 50) {
            pollReceive();
            radio->startReceive();
            lastRx = millis();
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

        // Header tap = exit
        if (get_touch(&tx, &ty) && ty < HDR_H) {
            while(get_touch(&tx,&ty)){delay(10);}
            // Check if tapping a channel tab
            int tappedCh = tx / 80;
            if (tappedCh >= 0 && tappedCh < MAX_CHANNELS && tappedCh != currentCh) {
                currentCh = tappedCh;
                inputLen = 0; inputBuf[0] = '\0';
                radio->setFrequency(channelFreq(currentCh));
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
            currentCh = (currentCh + 1) % MAX_CHANNELS;
            inputLen = 0; inputBuf[0] = '\0';
            radio->setFrequency(channelFreq(currentCh));
            drawFull();
            continue;
        }

        // Trackball scroll messages
        if (tb.y == -1) {
            int total = min(msgCount[currentCh], MAX_MSGS_PER_CH);
            if (msgScroll[currentCh] > 0) {
                msgScroll[currentCh]--;
                drawMessages();
            }
        }
        if (tb.y == 1) {
            int total = min(msgCount[currentCh], MAX_MSGS_PER_CH);
            if (msgScroll[currentCh] < total - MSG_ROWS) {
                msgScroll[currentCh]++;
                drawMessages();
            }
        }

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
            radio->startReceive();
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

    // Cleanup
    if (radio) radio->standby();
    deinitRadio();
    gfx->fillScreen(COL_BG);
}