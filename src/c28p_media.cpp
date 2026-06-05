// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_media.cpp — Media app for C28P
//
//  CONTAINS:
//   1. Voice recorder — record audio from on-board ES8311 mic input,
//      save as raw PCM WAV to /recordings/ on SD
//   2. Recordings list — browse past recordings, tap to play back
//   3. Image viewer stub — placeholder for v1.3 JPEG decode on SD
//
//  The recorder uses the ES8311 codec on the C28P (same chip the
//  c28p_audio.cpp game-tone driver initializes). For v1.2.1 the
//  recorder writes raw PCM at 16kHz mono 16-bit into a WAV header.
//  No compression. ~32KB per second of audio. A 30-second memo is
//  about 1MB on SD.
//
//  TOUCH UI (240×320 portrait):
//
//    y=  0..14   Exit bar (owned by dpad)
//    y= 16..40   Title bar
//    y= 44..80   Mode tabs: [RECORD] [LIBRARY] [PHOTOS]
//    y= 84..304  Active panel
//    y=308..318  Status line
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include <SD_MMC.h>
#include <driver/i2s.h>     // for the i2s_read() polling loop
#include "Audio.h"           // ESP32-audioI2S — WAV playback from SD_MMC
#include "c28p_dpad.h"
#include "hal_pins.h"
#include "c28p_audio.h"      // v1.3 HAL — modes + ownership tracking

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t *x, int16_t *y);

// ─── State ───
enum MediaTab {
    TAB_RECORD = 0,
    TAB_LIBRARY = 1,
    TAB_PHOTOS = 2,
};
static MediaTab g_tab = TAB_RECORD;

// ─── Recording state ───
static bool g_recording = false;
static uint32_t g_record_start_ms = 0;
static uint32_t g_record_bytes = 0;
static File g_record_file;
static char g_record_filename[64];
static bool g_last_record_valid = false;
static uint32_t g_last_record_bytes = 0;
static uint32_t g_last_record_ms = 0;
static char g_last_record_filename[64];

// ─── Playback state ───
// Up to 5 filenames cached from the last library panel draw,
// so a tap on a row can start playback without re-scanning the dir.
static Audio*  g_play_audio    = nullptr;
static bool    g_play_active   = false;
static int     g_play_row      = -1;
static char    lib_filenames[5][64];   // full SD_MMC paths, e.g. /recordings/rec_0.wav
static int     lib_file_count  = 0;

static void stop_media_playback();

// ─── Recording I2S RX ──────────────────────────────
static bool g_rx_active = false;

// v1.3: route through audio HAL so RECORD mode is tracked
// alongside PLAYBACK_TONE and PLAYBACK_AUDIO. No more stale
// flag desync after the mic probe.
static bool recording_i2s_rx_init() {
    if (!c28p_audio_enter(C28P_AUDIO_RECORD)) {
        Serial.println("[C28P-MEDIA] audio HAL RECORD entry failed");
        return false;
    }
    g_rx_active = true;
    return true;
}

static void recording_i2s_rx_stop() {
    if (!g_rx_active) return;
    c28p_audio_release();   // HAL stops + uninstalls driver, disables amp
    g_rx_active = false;
}

// ─── WAV header for 16kHz mono 16-bit PCM ───
//
// PCM WAV format. Header is 44 bytes, fields little-endian.
// data length is patched at stop time once total recording size is known.
static void write_wav_header(File& f, uint32_t data_bytes) {
    uint32_t file_size = data_bytes + 36;
    uint32_t sample_rate = 16000;
    uint16_t channels = 1;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    uint8_t hdr[44];
    memcpy(hdr +  0, "RIFF", 4);
    memcpy(hdr +  4, &file_size, 4);
    memcpy(hdr +  8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(hdr + 16, &fmt_size, 4);
    uint16_t audio_format = 1;   // PCM
    memcpy(hdr + 20, &audio_format, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &sample_rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits_per_sample, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_bytes, 4);

    f.seek(0);
    f.write(hdr, 44);
}

static bool ensure_recordings_dir() {
    if (!SD_MMC.exists("/recordings")) {
        if (!SD_MMC.mkdir("/recordings")) {
            Serial.println("[C28P-MEDIA] mkdir /recordings failed");
            return false;
        }
    }
    return true;
}

static bool start_recording() {
    if (g_recording) return true;
    if (!ensure_recordings_dir()) return false;
    if (g_play_active) stop_media_playback();

    uint32_t secs = millis() / 1000;
    snprintf(g_record_filename, sizeof(g_record_filename),
             "/recordings/rec_%lu.wav", (unsigned long)secs);

    // The audio HAL handles codec init + I2S RX install + amp gating
    // (amp stays disabled in RECORD mode to avoid speaker-to-mic
    // feedback). One call replaces the v1.2.x dance of c28p_audio_begin
    // → i2s_driver_uninstall → recording_i2s_rx_init → amp-disable.
    if (!recording_i2s_rx_init()) {
        Serial.println("[C28P-MEDIA] HAL RECORD init failed");
        return false;
    }

    g_record_file = SD_MMC.open(g_record_filename, FILE_WRITE);
    if (!g_record_file) {
        Serial.printf("[C28P-MEDIA] Failed to open %s\n", g_record_filename);
        recording_i2s_rx_stop();
        return false;
    }

    // Reserve 44 bytes for the WAV header; patched at stop
    uint8_t empty_hdr[44] = {0};
    g_record_file.write(empty_hdr, 44);

    g_record_bytes    = 0;
    g_record_start_ms = millis();
    g_recording       = true;
    Serial.printf("[C28P-MEDIA] Recording: %s\n", g_record_filename);
    return true;
}

static void stop_recording() {
    if (!g_recording) return;
    g_recording = false;
    uint32_t elapsed_ms = millis() - g_record_start_ms;

    // Stop the I2S RX driver
    recording_i2s_rx_stop();

    // Patch the WAV header with actual data size
    write_wav_header(g_record_file, g_record_bytes);
    g_record_file.close();

    if (g_record_bytes > 0) {
        strncpy(g_last_record_filename, g_record_filename,
                sizeof(g_last_record_filename) - 1);
        g_last_record_filename[sizeof(g_last_record_filename) - 1] = 0;
        g_last_record_bytes = g_record_bytes;
        g_last_record_ms = elapsed_ms;
        g_last_record_valid = true;
    }

    uint32_t duration = elapsed_ms / 1000;
    Serial.printf("[C28P-MEDIA] Stopped: %s (%u sec, %u bytes)\n",
                  g_record_filename, duration, g_record_bytes);
}

// ─── Drawing ───
static void media_draw_chrome() {
    gfx->fillRect(0, 14, 240, 320 - 14, 0x0000);
    gfx->fillRect(0, 14, 240, 22, 0x780F);   // magenta header
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 18);
    gfx->print("MEDIA");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 22);
    gfx->print("< EXIT");
}

static void media_draw_tabs() {
    gfx->fillRect(0, 40, 240, 40, 0x0000);
    const char* labels[3] = { "RECORD", "LIBRARY", "PHOTOS" };
    for (int i = 0; i < 3; i++) {
        int x = 8 + i * 78;
        bool active = (g_tab == (MediaTab)i);
        gfx->fillRect(x, 44, 72, 30, active ? 0xF81F : 0x18C3);
        gfx->drawRect(x, 44, 72, 30, active ? 0xFFFF : 0x4208);
        gfx->setTextSize(1);
        gfx->setTextColor(active ? 0xFFFF : 0x8410);
        // Center text in tab — 6px/char approximately
        int tw = strlen(labels[i]) * 6;
        gfx->setCursor(x + (72 - tw) / 2, 56);
        gfx->print(labels[i]);
    }
}

static void media_draw_record_panel() {
    gfx->fillRect(0, 84, 240, 220, 0x0000);

    // Record button — big and obvious
    if (g_recording) {
        // Pulsing red square indicator at top
        gfx->fillRect(20, 92, 200, 80, 0x4000);
        gfx->drawRect(20, 92, 200, 80, 0xF800);
        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(56, 110);
        gfx->print("RECORDING");

        uint32_t elapsed = (millis() - g_record_start_ms) / 1000;
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(80, 140);
        gfx->printf("%02lu:%02lu",
                    (unsigned long)(elapsed / 60),
                    (unsigned long)(elapsed % 60));

        // STOP button
        gfx->fillRect(40, 200, 160, 60, 0x4000);
        gfx->drawRect(40, 200, 160, 60, 0xF800);
        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(82, 222);
        gfx->print("STOP");
    } else {
        bool playing_last = (g_play_active && g_play_row < 0);

        gfx->fillRect(20, 92, 200, 80, playing_last ? 0x0320 : 0x18C3);
        gfx->drawRect(20, 92, 200, 80, playing_last ? 0x07E0 : 0x4208);
        gfx->setTextSize(2);
        gfx->setTextColor(playing_last ? 0x07E0 : 0x8410);
        gfx->setCursor(playing_last ? 54 : 70, 110);
        gfx->print(playing_last ? "PLAYING" : "READY");
        gfx->setTextSize(1);
        gfx->setCursor(40, 140);
        if (playing_last) {
            gfx->print("Playing last recording.");
        } else if (g_last_record_valid) {
            gfx->printf("Last: %lu.%lus  %lu bytes",
                        (unsigned long)(g_last_record_ms / 1000),
                        (unsigned long)((g_last_record_ms % 1000) / 100),
                        (unsigned long)g_last_record_bytes);
        } else {
            gfx->print("Tap below to begin recording.");
        }
        gfx->setCursor(40, 154);
        gfx->print("Recordings save to /recordings");

        if (playing_last) {
            gfx->fillRect(40, 210, 160, 56, 0x4000);
            gfx->drawRect(40, 210, 160, 56, 0xF800);
            gfx->setTextSize(2);
            gfx->setTextColor(0xF800);
            gfx->setCursor(82, 230);
            gfx->print("STOP");
            return;
        }

        if (g_last_record_valid) {
            gfx->fillRect(40, 176, 160, 28, 0x0320);
            gfx->drawRect(40, 176, 160, 28, 0x07E0);
            gfx->setTextSize(1);
            gfx->setTextColor(0x07E0);
            gfx->setCursor(84, 186);
            gfx->print("PLAY LAST");
        }

        // RECORD button
        gfx->fillRect(40, 212, 160, 56, 0x2104);
        gfx->drawRect(40, 212, 160, 56, 0xF800);
        gfx->fillCircle(120, 236, 14, 0xF800);   // red dot
        gfx->setTextSize(1);
        gfx->setTextColor(0xF800);
        gfx->setCursor(96, 258);
        gfx->print("RECORD");
    }
}

static void media_draw_library_panel() {
    gfx->fillRect(0, 84, 240, 220, 0x0000);

    if (!ensure_recordings_dir()) {
        gfx->setTextSize(1);
        gfx->setTextColor(0xF800);
        gfx->setCursor(20, 140);
        gfx->print("SD card not available");
        lib_file_count = 0;
        return;
    }

    File dir = SD_MMC.open("/recordings");
    if (!dir) {
        gfx->setTextSize(1);
        gfx->setTextColor(0x8410);
        gfx->setCursor(20, 140);
        gfx->print("No recordings yet");
        lib_file_count = 0;
        return;
    }

    // Header — show NOW PLAYING info when something is active
    if (g_play_active && g_play_row >= 0) {
        gfx->setTextSize(1);
        gfx->setTextColor(0x07E0);
        gfx->setCursor(8, 88);
        gfx->print("\xBB PLAYING");
        // STOP button
        gfx->fillRect(8, 272, 224, 28, 0x4000);
        gfx->drawRect(8, 272, 224, 28, 0xF800);
        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(88, 280);
        gfx->print("STOP");
    } else {
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(8, 88);
        gfx->print("RECORDINGS  (tap to play)");
    }

    lib_file_count = 0;
    int row = 0;
    File entry = dir.openNextFile();
    while (entry && row < 5) {
        if (!entry.isDirectory()) {
            // Cache full path under /recordings/ for playback.
            //
            // entry.name() behavior is version-dependent in SD_MMC:
            // some builds return basenames ("rec_13.wav"), others
            // full paths ("/recordings/rec_13.wav"). Pre-fix builds
            // stored whatever came back, then handed it to the Audio
            // library which couldn't find files when given basenames
            // (it looked at SD root, not /recordings/). Normalize
            // here so playback is unambiguous regardless of SD_MMC
            // version.
            const char* raw = entry.name();
            const char* basename = strrchr(raw, '/');
            basename = basename ? basename + 1 : raw;
            snprintf(lib_filenames[row], sizeof(lib_filenames[row]),
                     "/recordings/%s", basename);
            lib_file_count++;

            int y = 104 + row * 34;
            bool is_playing = (g_play_active && g_play_row == row);
            uint16_t bg = is_playing ? 0x0720 : 0x18C3;
            uint16_t border = is_playing ? 0x07E0 : 0x4208;
            gfx->fillRect(8, y, 224, 30, bg);
            gfx->drawRect(8, y, 224, 30, border);

            if (is_playing) {
                gfx->setTextColor(0x07E0);
                gfx->setCursor(14, y + 6);
                gfx->print("\xBB ");   // play arrow
            } else {
                gfx->setTextColor(0xFFFF);
                gfx->setCursor(14, y + 6);
            }

            const char* name = entry.name();
            const char* slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            // Truncate to fit display width
            char disp[28];
            strncpy(disp, name, 27); disp[27] = 0;
            gfx->setTextColor(is_playing ? 0x07E0 : 0xFFFF);
            gfx->setCursor(26, y + 6);
            gfx->print(disp);
            gfx->setTextColor(0x8410);
            gfx->setCursor(14, y + 18);
            gfx->printf("%u bytes", (unsigned)entry.size());
            row++;
        }
        entry = dir.openNextFile();
    }
    dir.close();

    if (row == 0) {
        gfx->setTextColor(0x8410);
        gfx->setCursor(20, 180);
        gfx->print("No recordings yet.");
        gfx->setCursor(20, 196);
        gfx->print("Switch to RECORD tab");
        gfx->setCursor(20, 210);
        gfx->print("to make one.");
    }
}

static void media_draw_photos_panel() {
    gfx->fillRect(0, 84, 240, 220, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor(40, 140);
    gfx->print("PHOTOS");
    gfx->setTextSize(1);
    gfx->setCursor(20, 180);
    gfx->print("Image viewer is v1.3.");
    gfx->setCursor(20, 196);
    gfx->print("Will display JPEGs");
    gfx->setCursor(20, 210);
    gfx->print("from /photos/ on SD.");
}

static void media_redraw_panel() {
    if (g_tab == TAB_RECORD) media_draw_record_panel();
    else if (g_tab == TAB_LIBRARY) media_draw_library_panel();
    else media_draw_photos_panel();
}

// ─── Playback ───────────────────────────────
// start_media_playback() uses the ESP32-audioI2S Audio library to
// stream a WAV file from SD_MMC through the ES8311 codec. Before
// handing I2S over to the library we:
//   1. Init the codec (es8311_init() over I2C) via c28p_audio_begin().
//   2. Explicitly uninstall the legacy IDF I2S driver that
//      c28p_audio_begin() installed, so the Audio library can
//      install its own driver on I2S_NUM_0.
// On stop, the Audio instance is deleted and the amp disabled. The
// next game to run will call pm_game_audio_begin() → c28p_audio_begin()
// which reinstalls the driver cleanly from scratch.

static void stop_media_playback() {
    if (g_play_audio) {
        g_play_audio->stopSong();
        delay(30);
        delete g_play_audio;
        g_play_audio = nullptr;
    }
    if (g_play_active) {
        // The Audio library uninstalled its I2S driver in its dtor.
        // Tell the HAL so it doesn't try to uninstall again, then
        // drop to IDLE (amp off).
        c28p_audio_release_driver();
    }
    g_play_active = false;
    g_play_row    = -1;
}

static bool start_media_playback_path(const char* path, int row) {
    if (!path || !path[0]) return false;

    // Stop any in-progress recording FIRST. If recording is active
    // when the user taps a library row, the c28p_audio_enter call
    // below will tear down the RX I2S driver — but g_recording stays
    // true, and the main loop will keep calling i2s_read() on the
    // now-uninstalled driver, which crashes with "RX mode is not
    // enabled" / LoadProhibited. stop_recording() patches the WAV
    // header, closes the file, clears g_recording / g_rx_active, and
    // leaves the HAL in IDLE — then PLAYBACK_AUDIO can enter cleanly.
    if (g_recording) {
        stop_recording();
    }

    // Stop any in-progress playback first
    stop_media_playback();

    // Hand the HAL into PLAYBACK_AUDIO mode — codec + amp on, no
    // driver installed on our side. The Audio library will install
    // its own via setPinout() below.
    if (!c28p_audio_enter(C28P_AUDIO_PLAYBACK_AUDIO)) {
        Serial.println("[C28P-MEDIA] HAL PLAYBACK_AUDIO entry failed");
        return false;
    }

    g_play_audio = new Audio();
    g_play_audio->setPinout(PIN_I2S_SCLK, PIN_I2S_LRCK, PIN_I2S_DOUT, I2S_PIN_NO_CHANGE);
    // Manually route MCLK — ESP32-audioI2S v2.1.0's setPinout doesn't
    // touch mck_io_num, and its i2s_mclk_pin_select() is ESP32-only
    // and only accepts pins 0/1/3. We need GPIO4 on ESP32-S3, so we
    // call i2s_set_pin() directly with everything else as
    // I2S_PIN_NO_CHANGE to leave the library's other pin routing alone.
    {
        i2s_pin_config_t mclk_cfg = {};
        mclk_cfg.mck_io_num   = PIN_I2S_MCLK;
        mclk_cfg.bck_io_num   = I2S_PIN_NO_CHANGE;
        mclk_cfg.ws_io_num    = I2S_PIN_NO_CHANGE;
        mclk_cfg.data_out_num = I2S_PIN_NO_CHANGE;
        mclk_cfg.data_in_num  = I2S_PIN_NO_CHANGE;
        i2s_set_pin(I2S_NUM_0, &mclk_cfg);
    }
    g_play_audio->setVolume(15);   // 0-21
    if (!g_play_audio->connecttoFS(SD_MMC, path)) {
        Serial.printf("[C28P-MEDIA] connecttoFS failed: %s\n", path);
        delete g_play_audio;
        g_play_audio = nullptr;
        c28p_audio_release_driver();
        return false;
    }

    g_play_active = true;
    g_play_row    = row;

    Serial.printf("[C28P-MEDIA] Playing: %s\n", path);
    return true;
}

static void start_media_playback(int row) {
    if (row < 0 || row >= lib_file_count) return;
    start_media_playback_path(lib_filenames[row], row);
}

static void start_last_recording() {
    if (!g_last_record_valid) return;
    start_media_playback_path(g_last_record_filename, -1);
}

// ─── Public entry ───
void c28p_run_media() {
    media_draw_chrome();
    media_draw_tabs();
    media_redraw_panel();

    uint32_t last_clock_update = 0;
    bool was_touched = false;
    int pressed = -2;

    while (true) {
        // Poll I2S RX and write PCM samples to the WAV file while recording.
        // 512 bytes = 256 samples = 16ms of audio at 16kHz. This keeps the
        // loop tight enough that DMA buffers don't overflow (6 * 256 samples
        // = ~96ms of headroom) while not blocking the UI for long stretches.
        if (g_recording && g_rx_active) {
            static uint8_t cap_buf[512];
            size_t bytes_read = 0;
            esp_err_t err = i2s_read(I2S_NUM_0, cap_buf, sizeof(cap_buf),
                                     &bytes_read, pdMS_TO_TICKS(10));
            if (err == ESP_OK && bytes_read > 0) {
                g_record_file.write(cap_buf, bytes_read);
                g_record_bytes += bytes_read;
            }
        }

        // Service the Audio decoder every loop iteration.
        // The Audio library streams PCM into I2S DMA from here;
        // skipping loop() for too long causes buffer underruns and
        // audible glitches or silence.
        if (g_play_active && g_play_audio) {
            g_play_audio->loop();
            // Auto-stop when the file finishes
            if (!g_play_audio->isRunning()) {
                stop_media_playback();
                media_redraw_panel();   // remove playing indicator
            }
        }

        // Update the elapsed clock if recording
        if (g_recording && millis() - last_clock_update > 1000) {
            media_draw_record_panel();
            last_clock_update = millis();
        }

        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            if (ty < 14) {
                pressed = -1;   // exit
            }
            // Tabs
            else if (ty >= 44 && ty < 74) {
                if (tx < 86) pressed = -10;
                else if (tx < 164) pressed = -11;
                else pressed = -12;
            }
            // RECORD / STOP / PLAY LAST buttons (RECORD panel)
            else if (g_tab == TAB_RECORD) {
                if (g_recording && ty >= 200 && ty < 260) {
                    pressed = -20;
                } else if (g_play_active && g_play_row < 0 &&
                           ty >= 210 && ty < 268) {
                    pressed = -60;
                } else if (!g_recording && g_last_record_valid &&
                           ty >= 176 && ty < 204) {
                    pressed = -21;
                } else if (!g_recording && ty >= 212 && ty < 268) {
                    pressed = -20;
                }
            }
            // Library row taps (y=104..269, each row 34px tall)
            else if (g_tab == TAB_LIBRARY && ty >= 104 && ty < 274) {
                int row = (ty - 104) / 34;
                if (row >= 0 && row < lib_file_count) {
                    pressed = -(50 + row);   // -50 = row 0, -51 = row 1, ...
                }
            }
            // STOP playback button in library panel (y=272..299)
            else if (g_tab == TAB_LIBRARY && g_play_active && ty >= 272 && ty < 300) {
                pressed = -60;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) {
                if (g_recording) stop_recording();
                stop_media_playback();
                return;
            } else if (pressed == -10) {
                g_tab = TAB_RECORD;  media_draw_tabs(); media_redraw_panel();
            } else if (pressed == -11) {
                g_tab = TAB_LIBRARY; media_draw_tabs(); media_redraw_panel();
            } else if (pressed == -12) {
                g_tab = TAB_PHOTOS;  media_draw_tabs(); media_redraw_panel();
            } else if (pressed == -20) {
                if (g_recording) {
                    if (millis() - g_record_start_ms >= 1000) {
                        stop_recording();
                        media_draw_record_panel();
                    } else {
                        Serial.println("[C28P-MEDIA] Ignoring early stop touch");
                    }
                } else {
                    if (start_recording()) {
                        media_draw_record_panel();
                    }
                }
            } else if (pressed == -21) {
                start_last_recording();
                media_draw_record_panel();
            } else if (pressed <= -50 && pressed >= -54) {
                // Library row tap — start playback of that recording
                int row = (-pressed) - 50;
                start_media_playback(row);
                media_draw_library_panel();   // redraw with playing indicator
            } else if (pressed == -60) {
                // Explicit STOP tap
                stop_media_playback();
                media_redraw_panel();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(8);   // tighter loop when audio is playing to avoid buffer underruns
        yield();
    }
}

#endif // DEVICE_C28P
