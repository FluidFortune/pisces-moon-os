// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// tembed_nfc.cpp — NFC Read for the T-Embed (PN532 over I2C)
//
// Reads ISO 14443A tags: UID, ATQA/SAK and the card family; for NTAG /
// MIFARE Ultralight tags it also reads the NDEF area and decodes the first
// record (URL, text, MIME type, Android app record). Read only — nothing
// is written to the tag, and MIFARE Classic sectors (which need keys) are
// not touched.
//
// DRIVER: raw PN532 frames over I2C, gated on the PN532's IRQ line —
// the same approach as LilyGO's own T-Embed PN532 test, which bypasses
// library I2C read paths that misbehave on this board. 100 kHz, 20 ms
// I2C timeout. Detection is "armed": InListPassiveTarget is sent once
// and the loop just watches IRQ, so the UI stays live while waiting.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <Wire.h>
#include "hal_pins.h"
#include "tembed_ui.h"
#include "tembed_input.h"

using namespace tui;

namespace {

constexpr uint8_t kAddr = I2C_ADDR_NFC;   // 0x24

// ── PN532 framing ────────────────────────────────────────────────
bool waitIrq(uint32_t ms) {
    uint32_t t0 = millis();
    while (digitalRead(NFC_IRQ) != LOW) {
        if (millis() - t0 > ms) return false;
        delay(1);
    }
    return true;
}

bool writeFrame(const uint8_t* cmd, uint8_t len) {
    uint8_t pkt[40];
    if (len + 8 > (int)sizeof(pkt)) return false;
    uint8_t l = len + 1, sum = 0xD4;
    pkt[0] = 0x00; pkt[1] = 0x00; pkt[2] = 0xFF;
    pkt[3] = l;    pkt[4] = (uint8_t)(~l + 1);
    pkt[5] = 0xD4;
    for (uint8_t i = 0; i < len; i++) { pkt[6 + i] = cmd[i]; sum += cmd[i]; }
    pkt[6 + len] = (uint8_t)(~sum + 1);
    pkt[7 + len] = 0x00;
    Wire.beginTransmission(kAddr);
    Wire.write(pkt, 8 + len);
    return Wire.endTransmission() == 0;
}

bool readAck() {
    if (!waitIrq(100)) return false;
    uint8_t b[7];
    if (Wire.requestFrom(kAddr, (uint8_t)7) != 7) return false;
    for (int i = 0; i < 7; i++) b[i] = Wire.read();
    static const uint8_t kAck[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
    return b[0] == 0x01 && memcmp(b + 1, kAck, 6) == 0;
}

// Read a response frame for `cmd` (expects cmd+1). Returns the payload
// length after the response code, or -1.
int readResponse(uint8_t cmd, uint8_t* out, int maxOut, uint32_t timeout) {
    if (!waitIrq(timeout)) return -1;
    uint8_t want = (uint8_t)min(10 + maxOut, 120);
    uint8_t b[120];
    int got = Wire.requestFrom(kAddr, want);
    if (got < 9) return -1;
    for (int i = 0; i < got; i++) b[i] = Wire.read();
    if (b[0] != 0x01) return -1;
    // Find the 00 FF start code (tolerate extra leading zeros).
    int p = 1;
    while (p < got - 1 && !(b[p] == 0x00 && b[p + 1] == 0xFF)) p++;
    p += 2;
    if (p + 3 >= got) return -1;
    uint8_t len = b[p], lcs = b[p + 1];
    if ((uint8_t)(len + lcs) != 0) return -1;
    if (b[p + 2] != 0xD5 || b[p + 3] != (uint8_t)(cmd + 1)) return -1;
    int n = len - 2;
    if (n < 0 || p + 4 + n > got) return -1;
    if (n > maxOut) n = maxOut;
    memcpy(out, b + p + 4, n);
    return n;
}

int command(const uint8_t* c, uint8_t len, uint8_t* out, int maxOut, uint32_t timeout = 200) {
    if (!writeFrame(c, len)) return -1;
    if (!readAck()) return -1;
    return readResponse(c[0], out, maxOut, timeout);
}

// Sending an ACK frame aborts whatever command the PN532 is running.
void abortCommand() {
    static const uint8_t kAckFrame[6] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00 };
    Wire.beginTransmission(kAddr);
    Wire.write(kAckFrame, 6);
    Wire.endTransmission();
    delay(5);
}

uint32_t g_fw = 0;

bool pn532Init() {
    pinMode(NFC_IRQ, INPUT_PULLUP);
    pinMode(NFC_RST, OUTPUT);
    digitalWrite(NFC_RST, LOW);  delay(20);
    digitalWrite(NFC_RST, HIGH); delay(150);
    Wire.setClock(100000);
    Wire.setTimeOut(20);

    uint8_t r[8];
    const uint8_t getFw[] = { 0x02 };
    int n = -1;
    for (int tries = 0; tries < 3 && n < 4; tries++) {   // first frame may only wake it
        n = command(getFw, 1, r, sizeof(r));
        if (n < 4) delay(50);
    }
    if (n < 4) return false;
    g_fw = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3];

    const uint8_t sam[] = { 0x14, 0x01, 0x14, 0x01 };      // normal mode, use IRQ
    if (command(sam, sizeof(sam), r, sizeof(r)) < 0) return false;
    const uint8_t retries[] = { 0x32, 0x05, 0xFF, 0x01, 0xFF };  // passive: retry forever
    if (command(retries, sizeof(retries), r, sizeof(r)) < 0) return false;
    return true;
}

bool armDetect() {
    const uint8_t det[] = { 0x4A, 0x01, 0x00 };           // 1 target, 106 kbps type A
    return writeFrame(det, sizeof(det)) && readAck();
}

// Ultralight/NTAG READ (16 bytes from `page`) via InDataExchange.
bool readPages(uint8_t page, uint8_t* out16) {
    const uint8_t c[] = { 0x40, 0x01, 0x30, page };
    uint8_t r[20];
    int n = command(c, sizeof(c), r, sizeof(r), 100);
    if (n < 17 || r[0] != 0x00) return false;
    memcpy(out16, r + 1, 16);
    return true;
}

// ── Tag model ────────────────────────────────────────────────────
struct Tag {
    uint8_t  uid[10]; uint8_t uidLen;
    uint16_t atqa; uint8_t sak;
    char     family[28];
    char     ndef1[64];     // first decoded record, one line
    char     ndef2[64];     // extra detail line
    uint32_t seenAt;
};

const char* uriPrefix(uint8_t c) {
    static const char* const k[] = {
        "", "http://www.", "https://www.", "http://", "https://", "tel:", "mailto:",
        "ftp://anonymous:anonymous@", "ftp://ftp.", "ftps://", "sftp://", "smb://",
        "nfs://", "ftp://", "dav://", "news:", "telnet://", "imap:", "rtsp://", "urn:",
        "pop:", "sip:", "sips:", "tftp:", "btspp://", "btl2cap://", "btgoep://",
        "tcpobex://", "irdaobex://", "file://", "urn:epc:id:", "urn:epc:tag:",
        "urn:epc:pat:", "urn:epc:raw:", "urn:epc:", "urn:nfc:" };
    return c < sizeof(k) / sizeof(k[0]) ? k[c] : "";
}

void safeCopy(char* dst, size_t cap, const uint8_t* src, size_t n) {
    size_t i = 0;
    for (; i < n && i < cap - 1; i++) dst[i] = (src[i] >= 32 && src[i] < 127) ? (char)src[i] : '.';
    dst[i] = 0;
}

// Decode the first NDEF record of message m[0..len).
void decodeNdef(const uint8_t* m, int len, Tag& t) {
    if (len < 3) { strcpy(t.ndef1, "NDEF: empty message"); return; }
    int records = 0;
    // Count records by walking headers.
    for (int i = 0; i < len && records < 16;) {
        uint8_t h = m[i]; if (i + 1 >= len) break;
        uint8_t tl = m[i + 1]; int j = i + 2;
        uint32_t pl;
        if (h & 0x10) { if (j >= len) break; pl = m[j++]; }
        else { if (j + 3 >= len) break; pl = ((uint32_t)m[j] << 24) | (m[j+1] << 16) | (m[j+2] << 8) | m[j+3]; j += 4; }
        uint8_t il = 0;
        if (h & 0x08) { if (j >= len) break; il = m[j++]; }
        records++;
        if (h & 0x40) break;                         // ME: last record
        i = j + tl + il + (int)pl;
    }

    uint8_t h = m[0], tnf = h & 0x07, tl = m[1];
    int need = 2 + ((h & 0x10) ? 1 : 4) + ((h & 0x08) ? 1 : 0);
    if (len < need) { strcpy(t.ndef1, "NDEF: truncated record"); return; }
    int j = 2; uint32_t pl;
    if (h & 0x10) pl = m[j++];
    else { pl = ((uint32_t)m[j] << 24) | (m[j+1] << 16) | (m[j+2] << 8) | m[j+3]; j += 4; }
    uint8_t il = (h & 0x08) ? m[j++] : 0;
    if (j + tl + il > len) { strcpy(t.ndef1, "NDEF: truncated record"); return; }
    const uint8_t* type = m + j; j += tl + il;
    const uint8_t* pay = m + j;
    if (j + (int)pl > len) pl = (len > j) ? len - j : 0;   // truncated read

    char extra[24] = "";
    if (records > 1) snprintf(extra, sizeof(extra), "  (+%d more)", records - 1);

    if (tnf == 1 && tl == 1 && type[0] == 'U' && pl >= 1) {
        char rest[56]; safeCopy(rest, sizeof(rest), pay + 1, pl - 1);
        snprintf(t.ndef1, sizeof(t.ndef1), "URL %s%s", uriPrefix(pay[0]), rest);
        snprintf(t.ndef2, sizeof(t.ndef2), "NDEF URI record%s", extra);
    } else if (tnf == 1 && tl == 1 && type[0] == 'T' && pl >= 1) {
        uint8_t ll = pay[0] & 0x3F;
        char lang[8]; safeCopy(lang, sizeof(lang), pay + 1, ll);
        char txt[60]; safeCopy(txt, sizeof(txt), pay + 1 + ll, pl > (uint32_t)(1 + ll) ? pl - 1 - ll : 0);
        snprintf(t.ndef1, sizeof(t.ndef1), "\"%s\"", txt);
        snprintf(t.ndef2, sizeof(t.ndef2), "NDEF text (%s)%s", lang, extra);
    } else if (tnf == 2) {
        char mt[48]; safeCopy(mt, sizeof(mt), type, tl);
        snprintf(t.ndef1, sizeof(t.ndef1), "MIME %s", mt);
        snprintf(t.ndef2, sizeof(t.ndef2), "%lu byte payload%s", (unsigned long)pl, extra);
    } else if (tnf == 4) {
        char et[40]; safeCopy(et, sizeof(et), type, tl);
        char pv[40]; safeCopy(pv, sizeof(pv), pay, pl);
        snprintf(t.ndef1, sizeof(t.ndef1), "%s", pv);
        snprintf(t.ndef2, sizeof(t.ndef2), "external: %s%s", et, extra);
    } else {
        snprintf(t.ndef1, sizeof(t.ndef1), "NDEF record TNF %u, %lu bytes", tnf, (unsigned long)pl);
        snprintf(t.ndef2, sizeof(t.ndef2), "%d record(s)", records);
    }
}

// Read CC + NDEF area of an Ultralight/NTAG tag.
void readUltralight(Tag& t) {
    uint8_t cc[16];
    if (!readPages(3, cc)) { strcpy(t.ndef2, "Read failed (moved too soon?)"); return; }
    if (cc[0] != 0xE1) { strcpy(t.ndef1, "No NDEF (blank or unformatted)"); return; }
    int dataBytes = cc[2] * 8;
    const char* model = cc[2] == 0x12 ? "NTAG213" : cc[2] == 0x3E ? "NTAG215" :
                        cc[2] == 0x6D ? "NTAG216" : cc[2] == 0x06 ? "Ultralight" :
                        cc[2] == 0x10 ? "Ultralight C" : nullptr;
    if (model) snprintf(t.family, sizeof(t.family), "%s (%d B)", model, dataBytes);

    // Pages 4.. hold the TLVs. Read up to 160 bytes (10 x READ).
    uint8_t area[160];
    int have = 0;
    int limit = dataBytes < (int)sizeof(area) ? dataBytes : (int)sizeof(area);
    for (uint8_t page = 4; have < limit; page += 4) {
        if (!readPages(page, area + have)) break;
        have += 16;
        // Stop early once the NDEF TLV is fully inside what we have.
        if (have >= 4 && area[0] == 0x03 && area[1] != 0xFF && have >= 2 + area[1]) break;
    }
    if (have > limit) have = limit;
    for (int i = 0; i < have;) {
        uint8_t tt = area[i];
        if (tt == 0x00) { i++; continue; }            // NULL TLV
        if (tt == 0xFE) break;                        // terminator
        if (i + 1 >= have) break;
        int L = area[i + 1], hdr = 2;
        if (L == 0xFF) { if (i + 3 >= have) break; L = (area[i + 2] << 8) | area[i + 3]; hdr = 4; }
        if (tt == 0x03) { decodeNdef(area + i + hdr, min(L, have - i - hdr), t); return; }
        i += hdr + L;                                 // lock / memory control TLVs
    }
    strcpy(t.ndef1, "NDEF formatted, no message");
}

void identify(Tag& t) {
    const char* f;
    switch (t.sak) {
        case 0x00: f = "Ultralight / NTAG"; break;
        case 0x08: f = "MIFARE Classic 1K"; break;
        case 0x18: f = "MIFARE Classic 4K"; break;
        case 0x09: f = "MIFARE Mini"; break;
        case 0x88: f = "MIFARE Classic 1K"; break;
        case 0x10: case 0x11: f = "MIFARE Plus"; break;
        case 0x20: f = "ISO 14443-4 smart card"; break;
        case 0x28: case 0x38: f = "SmartMX / JCOP"; break;
        default:   f = nullptr;
    }
    if (f) strncpy(t.family, f, sizeof(t.family) - 1);
    else   snprintf(t.family, sizeof(t.family), "ISO 14443A SAK %02X", t.sak);
    t.ndef1[0] = t.ndef2[0] = 0;
    if (t.sak == 0x00) readUltralight(t);
    else if (t.sak == 0x08 || t.sak == 0x18 || t.sak == 0x09 || t.sak == 0x88)
        strcpy(t.ndef1, "Sectors are key-protected: UID only");
    else if (t.sak == 0x20) {
        strcpy(t.ndef1, "Payment card, transit card or phone.");
        if (t.uidLen == 4 && t.uid[0] == 0x08)
            strcpy(t.ndef2, "Random UID (phone / card emulation)");
    }
}

// ── UI ───────────────────────────────────────────────────────────
constexpr int kHist = 8;
Tag  g_hist[kHist];
int  g_histN = 0;
bool g_showHist = false;

void uidStr(const Tag& t, char* out) {
    char* p = out;
    for (int i = 0; i < t.uidLen; i++) p += sprintf(p, i ? ":%02X" : "%02X", t.uid[i]);
}

void drawWaiting() {
    clearBand(28, 126);
    gfx->setTextSize(2); gfx->setTextColor(HI);
    gfx->setCursor(6, 56); gfx->print("Hold a tag to the");
    gfx->setCursor(6, 78); gfx->print("reader...");
    gfx->setTextSize(1); gfx->setTextColor(DIM);
    gfx->setCursor(6, 110); gfx->print("Cards, key fobs, stickers, phones (ISO 14443A).");
}

void drawTag(const Tag& t) {
    clearBand(28, 126);
    char u[32]; uidStr(t, u);
    gfx->setTextSize(1); gfx->setTextColor(SUBT);
    gfx->setCursor(6, 30); gfx->printf("UID (%u bytes)", t.uidLen);
    gfx->setTextSize(t.uidLen > 7 ? 1 : 2); gfx->setTextColor(TXT);
    gfx->setCursor(6, 42); gfx->print(u);
    gfx->setTextSize(2); gfx->setTextColor(HI);
    gfx->setCursor(6, 64); gfx->print(t.family);
    gfx->setTextSize(1); gfx->setTextColor(DIM);
    gfx->setCursor(6, 86); gfx->printf("ATQA %04X   SAK %02X", t.atqa, t.sak);
    gfx->setTextColor(t.ndef1[0] == 'U' ? GOOD : TXT);
    gfx->setCursor(6, 104); gfx->printf("%.52s", t.ndef1);
    if (strlen(t.ndef1) > 52) { gfx->setCursor(6, 116); gfx->printf("%.52s", t.ndef1 + 52); }
    gfx->setTextColor(SUBT);
    gfx->setCursor(6, 132); gfx->print(t.ndef2);
}

void drawHistory() {
    clearBand(28, 126);
    gfx->setTextSize(1);
    if (!g_histN) { gfx->setTextColor(DIM); gfx->setCursor(6, 70); gfx->print("No tags read yet."); return; }
    for (int i = 0; i < g_histN; i++) {
        const Tag& t = g_hist[i];
        char u[32]; uidStr(t, u);
        uint32_t ago = (millis() - t.seenAt) / 1000;
        gfx->setTextColor(i == 0 ? TXT : HDR);
        gfx->setCursor(6, 32 + i * 15);
        gfx->printf("%-21s %-18.18s %4lus", u, t.family, (unsigned long)ago);
    }
}

void footerFor() {
    footer(g_showHist ? "TURN=current tag  SIDE=back" : "TURN=history  PUSH=clear  SIDE=back");
}

} // namespace

// Hardware Test hook: reset the PN532 and read its firmware version.
bool tembed_nfc_probe(uint32_t* fw) {
    bool ok = pn532Init();
    if (fw) *fw = ok ? g_fw : 0;
    return ok;
}

void tembed_nfc_run() {
    header("NFC READ");
    footerFor();
    tembed_input_flush();
    gfx->setTextSize(2); gfx->setTextColor(HI);
    gfx->setCursor(6, 70); gfx->print("Starting PN532...");

    if (!pn532Init()) {
        clearBand(28, 126);
        gfx->setTextSize(2); gfx->setTextColor(BAD);
        gfx->setCursor(6, 56); gfx->print("PN532 not responding");
        gfx->setTextSize(1); gfx->setTextColor(DIM);
        gfx->setCursor(6, 84); gfx->print("I2C 0x24 on SDA 8 / SCL 18, IRQ 17, RST 45.");
        footer("SIDE=back");
        while (!tembed_btn_back()) delay(10);
        return;
    }
    char tag[20];
    snprintf(tag, sizeof(tag), "PN5%02X v%u.%u", (unsigned)(g_fw >> 24) & 0xFF,
             (unsigned)(g_fw >> 16) & 0xFF, (unsigned)(g_fw >> 8) & 0xFF);
    headerTag(tag, HI);

    g_showHist = false;
    bool armed = armDetect();
    if (g_histN) drawTag(g_hist[0]); else drawWaiting();

    uint32_t rearmAt = 0;
    bool forceFresh = false;
    for (;;) {
        if (tembed_btn_back()) break;

        if (tembed_encoder_delta() != 0) {
            g_showHist = !g_showHist;
            footerFor();
            if (g_showHist) drawHistory();
            else if (g_histN) drawTag(g_hist[0]); else drawWaiting();
        }
        if (tembed_btn_select() && !g_showHist) {
            forceFresh = true;                        // next detection reads it fresh
            drawWaiting();
        }

        if (!armed && millis() >= rearmAt) armed = armDetect();

        if (armed && digitalRead(NFC_IRQ) == LOW) {
            armed = false;
            uint8_t r[40];
            int n = readResponse(0x4A, r, sizeof(r), 50);
            if (n >= 6 && r[0] >= 1) {
                Tag t = {};
                t.atqa = (r[2] << 8) | r[3];
                t.sak = r[4];
                t.uidLen = r[5] > 10 ? 10 : r[5];
                memcpy(t.uid, r + 6, t.uidLen);
                t.seenAt = millis();
                bool same = g_histN && g_hist[0].uidLen == t.uidLen &&
                            memcmp(g_hist[0].uid, t.uid, t.uidLen) == 0 &&
                            millis() - g_hist[0].seenAt < 3000 && !forceFresh;
                forceFresh = false;
                if (same) {
                    g_hist[0].seenAt = t.seenAt;      // still on the reader
                } else {
                    identify(t);
                    for (int i = min(g_histN, kHist - 1); i > 0; i--) g_hist[i] = g_hist[i - 1];
                    g_hist[0] = t;
                    if (g_histN < kHist) g_histN++;
                    Serial.printf("[NFC] %s SAK %02X\n", t.family, t.sak);
                    if (g_showHist) drawHistory(); else drawTag(t);
                }
            }
            rearmAt = millis() + 300;                 // brief pause, then listen again
        }
        delay(5);
    }
    if (armed) abortCommand();
    // Power the RF field down until next time (SAM normal mode is fine idle).
}

#endif // DEVICE_TEMBED_CC1101
