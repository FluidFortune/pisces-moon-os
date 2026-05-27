// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  rss.cpp — RSS news reader, all devices
//
//  ENTRY POINT: void run_rss();
//
//  Shared: feed list (NoSQL "rss_feeds"), default-seed, fetch+parse.
//  Per-device: layout + input handling.
//
//  PORTRAIT (C28P, Heltec V4 — 240×320):  list scrolls vertically
//  T-DECK PLUS (320×240 landscape):       2-column list, keyboard nav
//  CARDPUTER ADV (240×135 landscape):     compact list, keyboard scroll
//  T-LORA PAGER (480×222 landscape):      wide 2-column, NES+touch
//
//  Tiny RSS parser: <item>...</item> blocks, extracts <title> and
//  <description>, handles CDATA, strips basic HTML.
// ─────────────────────────────────────────────

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "nosql_store.h"
#include "game_input.h"

extern Arduino_GFX *gfx;

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
extern bool c28p_touch_read(int16_t *x, int16_t *y);
#endif

// T-LoRa Pager has no touch hardware — NES buttons (dpad + A/B) only.

// ─── Shared state ───
static constexpr int MAX_HEADLINES = 20;

struct RssItem {
    String title;
    String description;
};

static RssItem g_items[MAX_HEADLINES];
static int g_item_count = 0;
static String g_feed_name;

// ─── Default feeds ───
struct DefaultFeed { const char* name; const char* url; };

static const DefaultFeed DEFAULT_FEEDS[] = {
    { "BBC WORLD",    "https://feeds.bbci.co.uk/news/world/rss.xml" },
    { "NPR",          "https://feeds.npr.org/1001/rss.xml" },
    { "AP TOP",       "https://rsshub.app/apnews/topics/apf-topnews" },
    { "HACKER NEWS",  "https://hnrss.org/frontpage" },
};
static constexpr int DEFAULT_FEEDS_N = sizeof(DEFAULT_FEEDS) / sizeof(DefaultFeed);

static void seed_defaults_if_empty() {
    nosql_init("rss_feeds");
    if (nosql_get_count("rss_feeds") > 0) return;
    for (int i = 0; i < DEFAULT_FEEDS_N; i++) {
        nosql_save_entry("rss_feeds",
                         DEFAULT_FEEDS[i].name,
                         DEFAULT_FEEDS[i].url);
    }
    Serial.println("[RSS] Seeded default feeds");
}

// ─── RSS parser (shared) ───
static int rss_parse(const String& body) {
    g_item_count = 0;
    int cursor = 0;
    while (g_item_count < MAX_HEADLINES) {
        int item_start = body.indexOf("<item>", cursor);
        if (item_start < 0) item_start = body.indexOf("<item ", cursor);
        if (item_start < 0) break;
        int item_end = body.indexOf("</item>", item_start);
        if (item_end < 0) break;
        String item_xml = body.substring(item_start, item_end);

        String title = "";
        int t_start = item_xml.indexOf("<title>");
        if (t_start >= 0) {
            t_start += 7;
            int t_end = item_xml.indexOf("</title>", t_start);
            if (t_end > t_start) {
                title = item_xml.substring(t_start, t_end);
                if (title.startsWith("<![CDATA[")) {
                    title = title.substring(9);
                    int cd = title.indexOf("]]>");
                    if (cd >= 0) title = title.substring(0, cd);
                }
                title.trim();
            }
        }

        String desc = "";
        int d_start = item_xml.indexOf("<description>");
        if (d_start >= 0) {
            d_start += 13;
            int d_end = item_xml.indexOf("</description>", d_start);
            if (d_end > d_start) {
                desc = item_xml.substring(d_start, d_end);
                if (desc.startsWith("<![CDATA[")) {
                    desc = desc.substring(9);
                    int cd = desc.indexOf("]]>");
                    if (cd >= 0) desc = desc.substring(0, cd);
                }
                // Strip basic HTML tags
                while (true) {
                    int lt = desc.indexOf('<');
                    if (lt < 0) break;
                    int gt = desc.indexOf('>', lt);
                    if (gt < 0) break;
                    desc = desc.substring(0, lt) + desc.substring(gt + 1);
                }
                desc.trim();
            }
        }

        if (title.length() > 0) {
            g_items[g_item_count].title = title;
            g_items[g_item_count].description = desc;
            g_item_count++;
        }
        cursor = item_end + 7;
    }
    Serial.printf("[RSS] Parsed %d items\n", g_item_count);
    return g_item_count;
}

static bool fetch_feed(const String& url) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    String body;
    if (url.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure();
        http.begin(client, url);
    } else {
        http.begin(url);
    }
    http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[RSS] HTTP %d\n", code);
        http.end();
        return false;
    }
    body = http.getString();
    http.end();
    rss_parse(body);
    return g_item_count > 0;
}

// ─── Word-wrap helper for displaying titles/descriptions ───
//
// Renders text starting at (x0,y0), wrapping at col_max characters,
// with line_h pixel spacing. Stops at y_max. Returns final y position.
static int draw_wrapped(const String& s, int x0, int y0,
                        int col_max, int line_h, int y_max) {
    gfx->setCursor(x0, y0);
    int col = 0;
    int y = y0;
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (y > y_max) break;
        if (c == '\n' || col >= col_max) {
            y += line_h;
            col = 0;
            gfx->setCursor(x0, y);
            if (c == '\n') continue;
        }
        gfx->write(c);
        col++;
    }
    return y;
}

// ─────────────────────────────────────────────
//  PER-DEVICE IMPLEMENTATIONS
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
// ─── Portrait 240×320 ───

static constexpr int P_PER_PAGE = 5;
static constexpr int P_ROW_H = 52;

static void p_chrome(const char* title) {
    gfx->fillRect(0, 14, 240, 320 - 14, 0x0000);
    gfx->fillRect(0, 14, 240, 22, 0x4800);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 18);
    gfx->print(title);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 22);
    gfx->print("< EXIT");
}

static bool p_pick_feed(String& feed_url, String& feed_name) {
    String titles[16], urls[16];
    int feed_count = 0;
    int total = nosql_get_count("rss_feeds");
    if (total > 16) total = 16;
    String t, u;
    for (int i = 0; i < total; i++) {
        if (nosql_get_entry("rss_feeds", i, t, u)) {
            titles[feed_count] = t;
            urls[feed_count] = u;
            feed_count++;
        }
    }

    p_chrome("RSS FEEDS");
    int top_y = 44, row_h = 40;
    for (int i = 0; i < feed_count; i++) {
        int y = top_y + i * row_h;
        if (y + row_h - 4 > 316) break;
        gfx->fillRect(8, y, 224, row_h - 4, 0x18C3);
        gfx->drawRect(8, y, 224, row_h - 4, 0x4208);
        gfx->setTextSize(2);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(16, y + 10);
        gfx->print(titles[i]);
    }

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < 14) pressed = -1;
            else {
                for (int i = 0; i < feed_count; i++) {
                    int y = top_y + i * row_h;
                    if (ty >= y && ty < y + row_h - 4) { pressed = i; break; }
                }
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) return false;
            if (pressed >= 0 && pressed < feed_count) {
                feed_url = urls[pressed];
                feed_name = titles[pressed];
                return true;
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

static void p_show_article(int idx) {
    if (idx < 0 || idx >= g_item_count) return;
    p_chrome("ARTICLE");
    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    int y = draw_wrapped(g_items[idx].title, 8, 44, 36, 12, 100);
    y += 8;
    gfx->drawFastHLine(0, y, 240, 0x4208);
    y += 4;
    gfx->setTextColor(0xFFFF);
    draw_wrapped(g_items[idx].description, 8, y, 36, 12, 300);

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
            return;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

static void p_show_headlines(const String& feed_name) {
    int page = 0;
    int pages = (g_item_count + P_PER_PAGE - 1) / P_PER_PAGE;
    if (pages < 1) pages = 1;

    auto draw_page = [&]() {
        p_chrome(feed_name.c_str());
        if (g_item_count == 0) {
            gfx->setTextSize(1);
            gfx->setTextColor(0x8410);
            gfx->setCursor(40, 150);
            gfx->print("(no items)");
            return;
        }
        int start = page * P_PER_PAGE;
        int end = min(g_item_count, start + P_PER_PAGE);
        for (int i = start; i < end; i++) {
            int row = i - start;
            int y = 44 + row * P_ROW_H;
            gfx->fillRect(8, y, 224, P_ROW_H - 4, 0x18C3);
            gfx->drawRect(8, y, 224, P_ROW_H - 4, 0x4208);
            gfx->setTextSize(1);
            gfx->setTextColor(0xFFFF);
            draw_wrapped(g_items[i].title, 12, y + 4, 36, 12, y + P_ROW_H - 14);
        }
        if (pages > 1) {
            gfx->setTextSize(1);
            gfx->setTextColor(0x07FF);
            gfx->setCursor(8, 308);
            gfx->printf("PREV   %d/%d   NEXT", page + 1, pages);
        }
    };

    draw_page();
    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < 14) pressed = -1;
            else if (ty >= 304 && pages > 1) {
                if (tx < 80) pressed = -3;
                else if (tx > 160) pressed = -4;
            } else {
                int start = page * P_PER_PAGE;
                int end = min(g_item_count, start + P_PER_PAGE);
                for (int i = start; i < end; i++) {
                    int row = i - start;
                    int y = 44 + row * P_ROW_H;
                    if (ty >= y && ty < y + P_ROW_H - 4) { pressed = i; break; }
                }
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) return;
            else if (pressed == -3) { if (page > 0) { page--; draw_page(); } }
            else if (pressed == -4) { if (page < pages - 1) { page++; draw_page(); } }
            else if (pressed >= 0) { p_show_article(pressed); draw_page(); }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

static void portrait_loop() {
    while (true) {
        String url, name;
        if (!p_pick_feed(url, name)) return;
        g_feed_name = name;
        p_chrome(name.c_str());
        gfx->setTextSize(2);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(40, 150);
        gfx->print("Loading...");
        if (!fetch_feed(url)) {
            gfx->setTextColor(0xF800);
            gfx->setCursor(40, 180);
            gfx->print("Fetch failed");
            delay(2000);
            continue;
        }
        p_show_headlines(name);
    }
}
#endif // portrait

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_CARDPUTER_ADV) || defined(DEVICE_TLORAPAGER)
// ─── Shared keyboard/button list loop helpers ───
//
// For landscape devices we use keyboard up/down + enter or NES dpad +
// A button. Single shared logic, per-device geometry constants below.

static int kb_selected = 0;   // index in current list
static int kb_scroll_top = 0;

static void kb_clamp_scroll(int total, int per_page) {
    if (kb_selected < 0) kb_selected = 0;
    if (kb_selected >= total) kb_selected = total - 1;
    if (kb_selected < kb_scroll_top) kb_scroll_top = kb_selected;
    if (kb_selected >= kb_scroll_top + per_page) kb_scroll_top = kb_selected - per_page + 1;
    if (kb_scroll_top < 0) kb_scroll_top = 0;
}
#endif

#if defined(DEVICE_TDECK_PLUS)
// ─── T-Deck Plus 320×240 landscape ───
//
// Two-column: feed list left, current-feed headlines right.
// Keyboard nav: up/down moves selection, Enter loads, ESC exits.
// On headline screen: up/down moves, Enter shows article, ESC returns.

static constexpr int TD_FEED_W = 120;
static constexpr int TD_FEED_ROW = 20;
static constexpr int TD_HEAD_W = 320 - TD_FEED_W;
static constexpr int TD_HEAD_ROW = 36;
static constexpr int TD_PER_PAGE = (240 - 26) / TD_HEAD_ROW;

static void td_chrome(const char* feed_name) {
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 320, 22, 0x4800);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 4);
    gfx->print("RSS");
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(50, 8);
    gfx->print(feed_name);
    gfx->setTextColor(0x8410);
    gfx->setCursor(200, 8);
    gfx->print("Q=QUIT ENTER=open");
}

static void td_show_article(int idx) {
    if (idx < 0 || idx >= g_item_count) return;
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 320, 22, 0x4800);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 4);
    gfx->print("ARTICLE");
    gfx->setTextColor(0x8410);
    gfx->setTextSize(1);
    gfx->setCursor(220, 8);
    gfx->print("any key=back");

    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    int y = draw_wrapped(g_items[idx].title, 8, 28, 52, 12, 80);
    y += 6;
    gfx->drawFastHLine(0, y, 320, 0x4208);
    y += 4;
    gfx->setTextColor(0xFFFF);
    draw_wrapped(g_items[idx].description, 8, y, 52, 12, 230);

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.key != 0 || input.a || input.b || input.quit) return;
        delay(30); yield();
    }
}

static void td_show_headlines(const String& feed_name) {
    kb_selected = 0;
    kb_scroll_top = 0;
    bool dirty = true;

    while (true) {
        if (dirty) {
            td_chrome(feed_name.c_str());
            kb_clamp_scroll(g_item_count, TD_PER_PAGE);
            int end = min(g_item_count, kb_scroll_top + TD_PER_PAGE);
            for (int i = kb_scroll_top; i < end; i++) {
                int row = i - kb_scroll_top;
                int y = 28 + row * TD_HEAD_ROW;
                bool sel = (i == kb_selected);
                gfx->fillRect(8, y, 304, TD_HEAD_ROW - 4,
                              sel ? 0x4208 : 0x18C3);
                gfx->drawRect(8, y, 304, TD_HEAD_ROW - 4, 0x4208);
                gfx->setTextSize(1);
                gfx->setTextColor(sel ? 0xFFFF : 0xC618);
                draw_wrapped(g_items[i].title, 12, y + 4, 50, 12, y + TD_HEAD_ROW - 6);
            }
            dirty = false;
        }
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return;
        if (input.up)   { kb_selected--; dirty = true; }
        if (input.down) { kb_selected++; dirty = true; }
        if (input.a || (input.key == 13 || input.key == 10)) {
            td_show_article(kb_selected);
            dirty = true;
        }
        delay(60); yield();
    }
}

static void td_pick_feed_and_run() {
    String titles[16], urls[16];
    int feed_count = 0;
    int total = nosql_get_count("rss_feeds");
    if (total > 16) total = 16;
    String t, u;
    for (int i = 0; i < total; i++) {
        if (nosql_get_entry("rss_feeds", i, t, u)) {
            titles[feed_count] = t;
            urls[feed_count] = u;
            feed_count++;
        }
    }

    kb_selected = 0;
    kb_scroll_top = 0;
    bool dirty = true;

    while (true) {
        if (dirty) {
            gfx->fillScreen(0x0000);
            gfx->fillRect(0, 0, 320, 22, 0x4800);
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFFF);
            gfx->setCursor(8, 4);
            gfx->print("RSS FEEDS");
            gfx->setTextColor(0x8410);
            gfx->setTextSize(1);
            gfx->setCursor(200, 8);
            gfx->print("Q=QUIT  ENTER=open");
            int per = (240 - 26) / TD_FEED_ROW;
            kb_clamp_scroll(feed_count, per);
            int end = min(feed_count, kb_scroll_top + per);
            for (int i = kb_scroll_top; i < end; i++) {
                int row = i - kb_scroll_top;
                int y = 28 + row * TD_FEED_ROW;
                bool sel = (i == kb_selected);
                gfx->fillRect(8, y, 304, TD_FEED_ROW - 4,
                              sel ? 0x4208 : 0x18C3);
                gfx->drawRect(8, y, 304, TD_FEED_ROW - 4, 0x4208);
                gfx->setTextSize(1);
                gfx->setTextColor(sel ? 0xFFFF : 0xFFE0);
                gfx->setCursor(16, y + 4);
                gfx->print(titles[i]);
            }
            dirty = false;
        }
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return;
        if (input.up)   { kb_selected--; dirty = true; }
        if (input.down) { kb_selected++; dirty = true; }
        if (input.a || (input.key == 13 || input.key == 10)) {
            // Load this feed
            String url = urls[kb_selected];
            String name = titles[kb_selected];
            gfx->fillScreen(0x0000);
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFE0);
            gfx->setCursor(80, 100);
            gfx->print("Loading...");
            if (!fetch_feed(url)) {
                gfx->setTextColor(0xF800);
                gfx->setCursor(80, 130);
                gfx->print("Fetch failed");
                delay(2000);
            } else {
                td_show_headlines(name);
            }
            dirty = true;
        }
        delay(60); yield();
    }
}
#endif // DEVICE_TDECK_PLUS

#if defined(DEVICE_CARDPUTER_ADV)
// ─── Cardputer 240×135 landscape ───
//
// Compact list view, keyboard-driven. Show 4 headlines per page.

static constexpr int CP_HEAD_ROW = 28;
static constexpr int CP_PER_PAGE = (135 - 16) / CP_HEAD_ROW;

static void cp_show_article(int idx) {
    if (idx < 0 || idx >= g_item_count) return;
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 240, 14, 0x4800);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(4, 4);
    gfx->print("ARTICLE");
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 4);
    gfx->print("any key=back");

    gfx->setTextColor(0x07FF);
    int y = draw_wrapped(g_items[idx].title, 4, 18, 39, 10, 50);
    y += 4;
    gfx->drawFastHLine(0, y, 240, 0x4208);
    y += 2;
    gfx->setTextColor(0xFFFF);
    draw_wrapped(g_items[idx].description, 4, y, 39, 10, 130);

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.key != 0 || input.a || input.b || input.quit) return;
        delay(30); yield();
    }
}

static void cp_show_headlines(const String& feed_name) {
    kb_selected = 0;
    kb_scroll_top = 0;
    bool dirty = true;

    while (true) {
        if (dirty) {
            gfx->fillScreen(0x0000);
            gfx->fillRect(0, 0, 240, 14, 0x4800);
            gfx->setTextSize(1);
            gfx->setTextColor(0xFFFF);
            gfx->setCursor(4, 4);
            gfx->print(feed_name);
            gfx->setTextColor(0x8410);
            gfx->setCursor(170, 4);
            gfx->print("Q=quit");

            kb_clamp_scroll(g_item_count, CP_PER_PAGE);
            int end = min(g_item_count, kb_scroll_top + CP_PER_PAGE);
            for (int i = kb_scroll_top; i < end; i++) {
                int row = i - kb_scroll_top;
                int y = 16 + row * CP_HEAD_ROW;
                bool sel = (i == kb_selected);
                gfx->fillRect(2, y, 236, CP_HEAD_ROW - 2,
                              sel ? 0x4208 : 0x18C3);
                gfx->setTextSize(1);
                gfx->setTextColor(sel ? 0xFFFF : 0xC618);
                draw_wrapped(g_items[i].title, 4, y + 2, 39, 10, y + CP_HEAD_ROW - 4);
            }
            dirty = false;
        }
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return;
        if (input.up)   { kb_selected--; dirty = true; }
        if (input.down) { kb_selected++; dirty = true; }
        if (input.a || (input.key == 13 || input.key == 10)) {
            cp_show_article(kb_selected);
            dirty = true;
        }
        delay(60); yield();
    }
}

static void cp_pick_feed_and_run() {
    String titles[16], urls[16];
    int feed_count = 0;
    int total = nosql_get_count("rss_feeds");
    if (total > 16) total = 16;
    String t, u;
    for (int i = 0; i < total; i++) {
        if (nosql_get_entry("rss_feeds", i, t, u)) {
            titles[feed_count] = t;
            urls[feed_count] = u;
            feed_count++;
        }
    }
    kb_selected = 0;
    kb_scroll_top = 0;
    bool dirty = true;

    int feed_row = 22;
    int per_page = (135 - 16) / feed_row;

    while (true) {
        if (dirty) {
            gfx->fillScreen(0x0000);
            gfx->fillRect(0, 0, 240, 14, 0x4800);
            gfx->setTextSize(1);
            gfx->setTextColor(0xFFFF);
            gfx->setCursor(4, 4);
            gfx->print("RSS FEEDS");
            gfx->setTextColor(0x8410);
            gfx->setCursor(150, 4);
            gfx->print("Q=quit");
            kb_clamp_scroll(feed_count, per_page);
            int end = min(feed_count, kb_scroll_top + per_page);
            for (int i = kb_scroll_top; i < end; i++) {
                int row = i - kb_scroll_top;
                int y = 16 + row * feed_row;
                bool sel = (i == kb_selected);
                gfx->fillRect(2, y, 236, feed_row - 2, sel ? 0x4208 : 0x18C3);
                gfx->setTextColor(sel ? 0xFFFF : 0xFFE0);
                gfx->setCursor(8, y + 6);
                gfx->print(titles[i]);
            }
            dirty = false;
        }
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit) return;
        if (input.up)   { kb_selected--; dirty = true; }
        if (input.down) { kb_selected++; dirty = true; }
        if (input.a || (input.key == 13 || input.key == 10)) {
            String url = urls[kb_selected];
            String name = titles[kb_selected];
            gfx->fillScreen(0x0000);
            gfx->setTextSize(1);
            gfx->setTextColor(0xFFE0);
            gfx->setCursor(80, 60);
            gfx->print("Loading...");
            if (!fetch_feed(url)) {
                gfx->setTextColor(0xF800);
                gfx->setCursor(80, 80);
                gfx->print("Fetch failed");
                delay(2000);
            } else {
                cp_show_headlines(name);
            }
            dirty = true;
        }
        delay(60); yield();
    }
}
#endif // DEVICE_CARDPUTER_ADV

#if defined(DEVICE_TLORAPAGER)
// ─── T-LoRa Pager 480×222 landscape ───
//
// NES buttons + touch. Wide 2-column for headlines.

static constexpr int TLP_HEAD_ROW = 40;
static constexpr int TLP_PER_PAGE = 2 * ((222 - 26) / TLP_HEAD_ROW);  // 2 cols × N

static void tlp_show_article(int idx) {
    if (idx < 0 || idx >= g_item_count) return;
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 480, 22, 0x4800);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 4);
    gfx->print("ARTICLE");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(320, 8);
    gfx->print("B=back  any tap=back");

    gfx->setTextColor(0x07FF);
    int y = draw_wrapped(g_items[idx].title, 8, 30, 78, 12, 80);
    y += 6;
    gfx->drawFastHLine(0, y, 480, 0x4208);
    y += 4;
    gfx->setTextColor(0xFFFF);
    draw_wrapped(g_items[idx].description, 8, y, 78, 12, 210);

    while (true) {
        PMNesInput input = pm_read_nes_input(true);
        if (input.b || input.a || input.quit || input.key != 0) return;
        delay(30); yield();
    }
}

static void tlp_show_headlines(const String& feed_name) {
    kb_selected = 0;
    kb_scroll_top = 0;
    bool dirty = true;
    int cols = 2;
    int col_w = 480 / cols;
    int per_page = TLP_PER_PAGE;

    while (true) {
        if (dirty) {
            gfx->fillScreen(0x0000);
            gfx->fillRect(0, 0, 480, 22, 0x4800);
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFFF);
            gfx->setCursor(8, 4);
            gfx->print(feed_name);
            gfx->setTextColor(0x8410);
            gfx->setTextSize(1);
            gfx->setCursor(320, 8);
            gfx->print("B=quit A=open");

            kb_clamp_scroll(g_item_count, per_page);
            int end = min(g_item_count, kb_scroll_top + per_page);
            for (int i = kb_scroll_top; i < end; i++) {
                int idx_local = i - kb_scroll_top;
                int col = idx_local % cols;
                int row = idx_local / cols;
                int x = col * col_w + 4;
                int y = 28 + row * TLP_HEAD_ROW;
                bool sel = (i == kb_selected);
                gfx->fillRect(x, y, col_w - 8, TLP_HEAD_ROW - 4,
                              sel ? 0x4208 : 0x18C3);
                gfx->drawRect(x, y, col_w - 8, TLP_HEAD_ROW - 4, 0x4208);
                gfx->setTextSize(1);
                gfx->setTextColor(sel ? 0xFFFF : 0xC618);
                draw_wrapped(g_items[i].title, x + 4, y + 4,
                             (col_w - 16) / 6, 12, y + TLP_HEAD_ROW - 10);
            }
            dirty = false;
        }
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit || input.b) return;
        if (input.up)    { kb_selected -= 2; dirty = true; }
        if (input.down)  { kb_selected += 2; dirty = true; }
        if (input.left)  { kb_selected -= 1; dirty = true; }
        if (input.right) { kb_selected += 1; dirty = true; }
        if (input.a || (input.key == 13 || input.key == 10)) {
            tlp_show_article(kb_selected);
            dirty = true;
        }
        delay(40); yield();
    }
}

static void tlp_pick_feed_and_run() {
    String titles[16], urls[16];
    int feed_count = 0;
    int total = nosql_get_count("rss_feeds");
    if (total > 16) total = 16;
    String t, u;
    for (int i = 0; i < total; i++) {
        if (nosql_get_entry("rss_feeds", i, t, u)) {
            titles[feed_count] = t;
            urls[feed_count] = u;
            feed_count++;
        }
    }
    kb_selected = 0;
    kb_scroll_top = 0;
    bool dirty = true;
    int feed_row = 36;
    int per_page = (222 - 26) / feed_row;

    while (true) {
        if (dirty) {
            gfx->fillScreen(0x0000);
            gfx->fillRect(0, 0, 480, 22, 0x4800);
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFFF);
            gfx->setCursor(8, 4);
            gfx->print("RSS FEEDS");
            gfx->setTextSize(1);
            gfx->setTextColor(0x8410);
            gfx->setCursor(320, 8);
            gfx->print("B=quit A=open");
            kb_clamp_scroll(feed_count, per_page);
            int end = min(feed_count, kb_scroll_top + per_page);
            for (int i = kb_scroll_top; i < end; i++) {
                int row = i - kb_scroll_top;
                int y = 28 + row * feed_row;
                bool sel = (i == kb_selected);
                gfx->fillRect(8, y, 464, feed_row - 4, sel ? 0x4208 : 0x18C3);
                gfx->drawRect(8, y, 464, feed_row - 4, 0x4208);
                gfx->setTextSize(2);
                gfx->setTextColor(sel ? 0xFFFF : 0xFFE0);
                gfx->setCursor(20, y + 8);
                gfx->print(titles[i]);
            }
            dirty = false;
        }
        PMNesInput input = pm_read_nes_input(true);
        if (input.quit || input.b) return;
        if (input.up)   { kb_selected--; dirty = true; }
        if (input.down) { kb_selected++; dirty = true; }
        if (input.a || (input.key == 13 || input.key == 10)) {
            String url = urls[kb_selected];
            String name = titles[kb_selected];
            gfx->fillScreen(0x0000);
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFE0);
            gfx->setCursor(160, 100);
            gfx->print("Loading...");
            if (!fetch_feed(url)) {
                gfx->setTextColor(0xF800);
                gfx->setCursor(160, 130);
                gfx->print("Fetch failed");
                delay(2000);
            } else {
                tlp_show_headlines(name);
            }
            dirty = true;
        }
        delay(50); yield();
    }
}
#endif // DEVICE_TLORAPAGER

// ─────────────────────────────────────────────
//  PUBLIC ENTRY
// ─────────────────────────────────────────────
void run_rss() {
    seed_defaults_if_empty();

#if defined(DEVICE_C28P) || defined(DEVICE_HELTEC_V4)
    portrait_loop();
#elif defined(DEVICE_TDECK_PLUS)
    td_pick_feed_and_run();
#elif defined(DEVICE_CARDPUTER_ADV)
    cp_pick_feed_and_run();
#elif defined(DEVICE_TLORAPAGER)
    tlp_pick_feed_and_run();
#else
    gfx->fillScreen(0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xF800);
    gfx->setCursor(10, 10);
    gfx->print("RSS not configured for this device");
    delay(2000);
#endif
}

#ifdef DEVICE_C28P
void c28p_run_rss() { run_rss(); }
#endif