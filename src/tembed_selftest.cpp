// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// tembed_selftest.cpp — Hardware Test for the T-Embed CC1101 (Plus)
//
// One screen that checks every part of the board, so "X doesn't work"
// can be split into hardware vs firmware without flashing LilyGO's test
// binaries. Each line is PASS / WARN / FAIL with the raw evidence, and
// the whole report is echoed to USB serial for pasting.
//
//   Power rail     GPIO15 (peripheral power enable) is driven HIGH
//   nRF24 SPI      raw register write + read-back on CS 44 / CE 43,
//                  independent of RadioLib (retries at a slower clock)
//   nRF24 driver   RadioLib begin() result, as the nRF24 Scan app uses it
//   Pins 43/44     IO-MUX function: GPIO, or still owned by UART0
//   CC1101         PARTNUM / VERSION status registers over raw SPI
//   SD card        mount state, size, FAT type
//   I2C bus        address scan (expects 0x24 PN532, 0x55, 0x6B)
//   PN532          firmware version via the NFC app's driver
//   Battery        BQ27220 gauge reading;  Charger: BQ25896 ACK
//   Memory         internal heap and PSRAM
//
// Receive / read only: nothing is transmitted and no tag is written.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SdFat.h>
#include <esp_heap_caps.h>
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "hal_pins.h"
#include "tembed_ui.h"
#include "tembed_input.h"

using namespace tui;

extern volatile bool g_sd_ready;
extern SdFat sd;

namespace {

enum Verdict : uint8_t { V_PASS, V_WARN, V_FAIL, V_INFO };
struct Row { const char* name; Verdict v; char detail[44]; };
constexpr int kRows = 12;
Row g_rows[kRows];
int g_rowN = 0;

constexpr int kRowY = 30, kRowH = 10;

void drawRow(int i, bool running = false) {
    const Row& r = g_rows[i];
    int y = kRowY + i * kRowH;
    gfx->fillRect(0, y, W, kRowH, BG);
    gfx->setTextSize(1);
    gfx->setCursor(4, y + 1);
    if (running) { gfx->setTextColor(DIM); gfx->print("[....]"); }
    else switch (r.v) {
        case V_PASS: gfx->setTextColor(GOOD); gfx->print("[PASS]"); break;
        case V_WARN: gfx->setTextColor(WARN); gfx->print("[WARN]"); break;
        case V_FAIL: gfx->setTextColor(BAD);  gfx->print("[FAIL]"); break;
        default:     gfx->setTextColor(HI);   gfx->print("[INFO]"); break;
    }
    gfx->setTextColor(TXT);
    gfx->setCursor(46, y + 1);  gfx->print(r.name);
    gfx->setTextColor(running ? DIM : SUBT);
    gfx->setTextWrap(false);              // long details clip, never spill into the next row
    gfx->setCursor(130, y + 1); gfx->print(running ? "testing..." : r.detail);
    gfx->setTextWrap(true);
}

// Begin a row (shows "testing..."), returns its index.
int begin(const char* name) {
    int i = g_rowN++;
    g_rows[i].name = name; g_rows[i].v = V_INFO; g_rows[i].detail[0] = 0;
    drawRow(i, true);
    return i;
}

void finish(int i, Verdict v, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_rows[i].detail, sizeof(g_rows[i].detail), fmt, ap);
    va_end(ap);
    g_rows[i].v = v;
    drawRow(i);
    static const char* const kTag[] = { "PASS", "WARN", "FAIL", "INFO" };
    Serial.printf("[HWTEST] %-4s %-12s %s\n", kTag[v], g_rows[i].name, g_rows[i].detail);
}

// ── Raw SPI helpers (all under the Bus Treaty mutex) ─────────────
void deselectAll() {
    digitalWrite(BOARD_TFT_CS, HIGH);
    digitalWrite(CC1101_CS,    HIGH);
    digitalWrite(NRF24_CS,     HIGH);
    digitalWrite(BOARD_SD_CS,  HIGH);
}

// nRF24: R_REGISTER = 0x00|reg, W_REGISTER = 0x20|reg. The STATUS
// register is shifted out during the command byte.
uint8_t nrfRead(uint8_t reg, uint8_t* status, uint32_t hz) {
    SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
    digitalWrite(NRF24_CS, LOW);
    uint8_t st = SPI.transfer(0x00 | (reg & 0x1F));
    uint8_t v  = SPI.transfer(0xFF);
    digitalWrite(NRF24_CS, HIGH);
    SPI.endTransaction();
    if (status) *status = st;
    return v;
}
void nrfWrite(uint8_t reg, uint8_t val, uint32_t hz) {
    SPI.beginTransaction(SPISettings(hz, MSBFIRST, SPI_MODE0));
    digitalWrite(NRF24_CS, LOW);
    SPI.transfer(0x20 | (reg & 0x1F));
    SPI.transfer(val);
    digitalWrite(NRF24_CS, HIGH);
    SPI.endTransaction();
}

// Write a scratch value to SETUP_AW (0x03) and RF_CH (0x05), read back,
// restore. Only a live chip can echo both. Returns true on match.
bool nrfReadback(uint32_t hz, uint8_t& status, uint8_t& aw, uint8_t& ch) {
    aw = nrfRead(0x03, &status, hz);
    ch = nrfRead(0x05, nullptr, hz);
    nrfWrite(0x03, 0x01, hz);
    nrfWrite(0x05, 0x2A, hz);
    uint8_t aw2 = nrfRead(0x03, nullptr, hz);
    uint8_t ch2 = nrfRead(0x05, nullptr, hz);
    nrfWrite(0x03, (aw >= 1 && aw <= 3) ? aw : 0x03, hz);   // restore
    nrfWrite(0x05, ch <= 125 ? ch : 0x02, hz);
    return aw2 == 0x01 && ch2 == 0x2A;
}

// CC1101 status registers need the burst bit: 0xC0 | addr.
// After CS goes low the chip pulls SO low when its oscillator is ready.
uint8_t ccStatusReg(uint8_t addr, bool& ready) {
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CC1101_CS, LOW);
    uint32_t t0 = micros();
    ready = true;
    while (digitalRead(SPI_MISO) == HIGH) {
        if (micros() - t0 > 2000) { ready = false; break; }
    }
    SPI.transfer(0xC0 | addr);
    uint8_t v = SPI.transfer(0x00);
    digitalWrite(CC1101_CS, HIGH);
    SPI.endTransaction();
    return v;
}

bool i2cAck(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// IO-MUX function of a pad: 1 = plain GPIO, 0 = its UART0 function.
const char* muxName(uint32_t regAddr) {
    uint32_t f = REG_GET_FIELD(regAddr, MCU_SEL);
    return f == PIN_FUNC_GPIO ? "GPIO" : (f == 0 ? "UART0" : "other");
}

// ── The test run ─────────────────────────────────────────────────
void runAll() {
    g_rowN = 0;
    gfx->fillRect(0, 28, W, 128, BG);
    Serial.println("[HWTEST] ===== T-Embed hardware test =====");

    // 1. Power rail
    int r = begin("Power rail");
    bool pwr = (REG_READ(GPIO_OUT_REG) >> BOARD_PWR_EN) & 1;
    finish(r, pwr ? V_PASS : V_FAIL, pwr ? "GPIO15 enable HIGH" : "GPIO15 is LOW: radios unpowered");

    // 2. nRF24 raw SPI
    r = begin("nRF24 SPI");
    uint8_t st = 0xFF, aw = 0xFF, ch = 0xFF, stFirst = 0xFF, awFirst = 0xFF;
    bool nrfOk = false; uint32_t okHz = 0;
    if (busTake(1000)) {
        deselectAll();
        pinMode(NRF24_CE, OUTPUT); digitalWrite(NRF24_CE, LOW);   // standby
        pinMode(NRF24_CS, OUTPUT); digitalWrite(NRF24_CS, HIGH);
        delay(5);
        // The very first transaction after screen traffic is the one that
        // breaks if the bus disagrees on SPI mode (see main.cpp, panel
        // mode). Read SETUP_AW once here, then again inside the read-back;
        // a mismatch means the first read after a redraw is corrupted.
        stFirst = 0xFF;
        awFirst = nrfRead(0x03, &stFirst, 1000000);
        const uint32_t kHz[] = { 1000000, 250000 };
        for (uint32_t hz : kHz) {
            if (nrfReadback(hz, st, aw, ch)) { nrfOk = true; okHz = hz; break; }
        }
        busGive();
    }
    if (nrfOk && (awFirst != aw || stFirst != st))
        finish(r, V_FAIL, "1st read glitch AW %02X then %02X", awFirst, aw);
    else if (nrfOk)
        finish(r, okHz == 1000000 ? V_PASS : V_WARN, "ok  ST %02X AW %02X%s",
               st, aw, okHz == 1000000 ? "" : " slow clk only");
    else if ((st == 0xFF && aw == 0xFF) || (st == 0x00 && aw == 0x00))
        finish(r, V_FAIL, "silent ST %02X AW %02X (seated?)", st, aw);
    else
        finish(r, V_FAIL, "garbled ST %02X AW %02X (pins?)", st, aw);

    // 3. nRF24 via RadioLib (what the scanner uses)
    r = begin("nRF24 driver");
    int code = nrf24_radiolib_begin_code();
    finish(r, code == 0 ? V_PASS : V_FAIL, code == 0 ? "RadioLib begin() ok" : "RadioLib begin() = %d", code);

    // 4. Pin ownership of 43/44 (after the probes configured them)
    r = begin("Pins 43/44");
    const char* m43 = muxName(IO_MUX_GPIO43_REG);
    const char* m44 = muxName(IO_MUX_GPIO44_REG);
    uint32_t o43 = GPIO.func_out_sel_cfg[43].func_sel, o44 = GPIO.func_out_sel_cfg[44].func_sel;
    bool pinsOk = strcmp(m43, "GPIO") == 0 && strcmp(m44, "GPIO") == 0 &&
                  o43 == SIG_GPIO_OUT_IDX && o44 == SIG_GPIO_OUT_IDX;
    finish(r, pinsOk ? V_PASS : V_FAIL, "CE43 %s/%lu  CS44 %s/%lu", m43, (unsigned long)o43,
           m44, (unsigned long)o44);

    // 5. CC1101
    r = begin("CC1101");
    uint8_t part = 0xFF, ver = 0xFF; bool rdy = false;
    if (busTake(1000)) {
        deselectAll();
        part = ccStatusReg(0x30, rdy);
        ver  = ccStatusReg(0x31, rdy);
        busGive();
    }
    bool ccOk = part == 0x00 && (ver == 0x14 || ver == 0x04 || ver == 0x08);
    finish(r, ccOk ? V_PASS : V_FAIL, "PART %02X VER %02X%s", part, ver, rdy ? "" : " (not ready)");

    // 6. SD card
    r = begin("SD card");
    if (g_sd_ready && busTake(1000)) {
        uint64_t sectors = sd.card() ? sd.card()->sectorCount() : 0;
        uint8_t ft = sd.fatType();
        busGive();
        finish(r, V_PASS, "mounted %.1f GB %s", sectors * 512.0 / 1e9,
               ft == 64 ? "exFAT" : ft == 32 ? "FAT32" : ft == 16 ? "FAT16" : "?");
    } else {
        finish(r, V_WARN, "no card: saving disabled");
    }

    // 7. PN532
    r = begin("PN532 NFC");
    uint32_t fw = 0;
    if (tembed_nfc_probe(&fw))
        finish(r, V_PASS, "PN5%02X firmware %u.%u", (unsigned)(fw >> 24) & 0xFF,
               (unsigned)(fw >> 16) & 0xFF, (unsigned)(fw >> 8) & 0xFF);
    else
        finish(r, V_FAIL, "no response (I2C 0x24, IRQ 17)");

    // 8. I2C scan (after the PN532 probe — it sleeps and ignores a bare
    //    address scan until a real command wakes it)
    r = begin("I2C bus");
    char found[44] = ""; int nf = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2cAck(a)) {
            size_t l = strlen(found);
            if (l < sizeof(found) - 6) snprintf(found + l, sizeof(found) - l, "%s%02X", nf ? " " : "", a);
            nf++;
        }
    }
    finish(r, nf ? V_INFO : V_FAIL, nf ? "found %s" : "no devices answer%s", nf ? found : "");

    // 9. Battery gauge
    r = begin("Battery");
    int soc = 0, mv = 0, ma = 0;
    if (tembed_battery_read(&soc, &mv, &ma))
        finish(r, V_PASS, "%d%%  %d.%02dV  %+d mA", soc, mv / 1000, (mv % 1000) / 10, ma);
    else
        finish(r, V_FAIL, "BQ27220 (0x55) not responding");

    // 10. Charger
    r = begin("Charger");
    bool chg = i2cAck(I2C_ADDR_PMU);
    finish(r, chg ? V_PASS : V_FAIL, chg ? "BQ25896 (0x6B) present" : "BQ25896 (0x6B) missing");

    // 11. Memory
    r = begin("Memory");
    finish(r, V_INFO, "heap %uK  PSRAM %u/%uK",
           (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getPsramSize() / 1024));

    int fails = 0;
    for (int i = 0; i < g_rowN; i++) if (g_rows[i].v == V_FAIL) fails++;
    Serial.printf("[HWTEST] ===== %d failure(s) =====\n", fails);
    char tag[16]; snprintf(tag, sizeof(tag), fails ? "%d FAIL" : "ALL PASS", fails);
    headerTag(tag, fails ? BAD : GOOD);
}

} // namespace

void tembed_selftest_run() {
    header("HARDWARE TEST");
    footer("PUSH=run again  SIDE=back  (also on USB serial)");
    tembed_input_flush();
    runAll();
    for (;;) {
        if (tembed_btn_back()) break;
        if (tembed_btn_select()) { headerTag("", BG); runAll(); tembed_input_flush(); }
        delay(10);
    }
}

#endif // DEVICE_TEMBED_CC1101
