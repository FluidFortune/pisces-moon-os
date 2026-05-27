// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_rss.cpp — RSS news reader for C28P
//
//  PURPOSE:
//
//  Fetches an RSS feed over HTTP, parses headlines, presents a
//  scrollable touch UI for browsing. Tap a headline to read the
//  summary/description text. No HTML rendering — text-only.
//
//  FEED LIST:
//
//  Feed URLs live in NoSQL category "rss_feeds". Each entry's title
//  is the display name, content is the URL. Defaults seeded on
//  first run with a handful of well-known feeds.
//
//  PARSING:
//
//  We don't bring in a full XML parser. RSS is simple enough to
//  parse with string find()/substring() for the tags we care about:
//    <title>, <description>, <link>
//  This is fragile against weird feeds but works for ~90% of them.
//
//  TOUCH UI:
//
//  Screen 1: Feed list (one row per feed, tap to load)
//  Screen 2: Headline list (scrollable, paginated)
//  Screen 3: Article detail (scroll headline + description)
//  Top exit bar returns to previous screen.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "c28p_dpad.h"
#include "nosql_store.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t *x, int16_t *y);

// ─── Limits ───
static constexpr int MAX_HEADLINES = 20;
static constexpr int HEADLINE_PER_PAGE = 5;
static constexpr int HEADLINE_ROW_H = 52;

struct RssItem {
    String title;
    String description;
};

static RssItem g_items[MAX_HEADLINES];
static int g_item_count = 0;
static String g_feed_name;

// ─── Default seed feeds ───
struct DefaultFeed {
    const char* name;
    const char* url;
};

static const DefaultFeed DEFAULT_FEEDS[] = {
    { "BBC WORLD",    "https://feeds.bbci.co.uk/news/world/rss.xml" },
    { "NPR",          "https://feeds.npr.org/1001/rss.xml" },
    { "AP TOP",       "https://rsshub.app/apnews/topics/apf-topnews" },
    { "HACKER NEWS",  "https://hnrss.org/frontpage" },
};
static constexpr int DEFAULT_FEEDS_N = sizeof(DEFAULT_FEEDS) / sizeof(DefaultFeed);

// ─── Seed defaults on first run ───
static void seed_default_feeds_if_empty() {
    nosql_init("rss_feeds");
    int total = nosql_get_count("rss_feeds");
    if (total > 0) return;
    for (int i = 0; i < DEFAULT_FEEDS_N; i++) {
        nosql_save_entry("rss_feeds",
                         DEFAULT_FEEDS[i].name,
                         DEFAULT_FEEDS[i].url);
    }
    Serial.println("[C28P-RSS] Seeded default feeds");
}

// ─── Tiny RSS parser ───
//
// Finds <item>...</item> blocks. Within each, extracts <title> and
// <description>. CDATA-aware (handles <![CDATA[...]]>). Stops at
// MAX_HEADLINES.
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

        // Extract title
        String title = "";
        int t_start = item_xml.indexOf("<title>");
        if (t_start >= 0) {
            t_start += 7;
            int t_end = item_xml.indexOf("</title>", t_start);
            if (t_end > t_start) {
                title = item_xml.substring(t_start, t_end);
                // Strip CDATA
                if (title.startsWith("<![CDATA[")) {
                    title = title.substring(9);
                    int cd = title.indexOf("]]>");
                    if (cd >= 0) title = title.substring(0, cd);
                }
                title.trim();
            }
        }

        // Extract description
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

    Serial.printf("[C28P-RSS] Parsed %d items\n", g_item_count);
    return g_item_count;
}

// ─── Fetch + parse ───
static bool fetch_feed(const String& url) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    if (url.startsWith("https://")) {
        WiFiClientSecure client;
        client.setInsecure();
        http.begin(client, url);
        http.setTimeout(10000);
        int code = http.GET();
        if (code != 200) {
            Serial.printf("[C28P-RSS] HTTP %d\n", code);
            http.end();
            return false;
        }
        String body = http.getString();
        http.end();
        rss_parse(body);
    } else {
        http.begin(url);
        http.setTimeout(10000);
        int code = http.GET();
        if (code != 200) {
            Serial.printf("[C28P-RSS] HTTP %d\n", code);
            http.end();
            return false;
        }
        String body = http.getString();
        http.end();
        rss_parse(body);
    }

    return g_item_count > 0;
}

// ─── Drawing ───
static void rss_draw_chrome(const char* title) {
    gfx->fillRect(0, 14, 240, 320 - 14, 0x0000);
    gfx->fillRect(0, 14, 240, 22, 0x4800);   // dark red header
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 18);
    gfx->print(title);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 22);
    gfx->print("< EXIT");
}

static void rss_draw_loading(const char* feed) {
    gfx->fillRect(0, 40, 240, 280, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(40, 130);
    gfx->print("Loading...");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(40, 160);
    gfx->print(feed);
}

static void rss_draw_error(const char* msg) {
    gfx->fillRect(0, 40, 240, 280, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xF800);
    gfx->setCursor(20, 110);
    gfx->print("ERROR");
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(20, 140);
    gfx->print(msg);
}

// ─── Feed picker screen ───
//
// Returns selected feed URL+name in feed_url/feed_name, or false if
// user exits.
static bool pick_feed(String& feed_url, String& feed_name) {
    String titles[16];
    String urls[16];
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

    rss_draw_chrome("RSS FEEDS");

    int row_h = 40;
    int top_y = 44;
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
            if (ty < 14) { pressed = -1; }
            else {
                for (int i = 0; i < feed_count; i++) {
                    int y = top_y + i * row_h;
                    if (ty >= y && ty < y + row_h - 4 && tx >= 8 && tx < 232) {
                        pressed = i;
                        break;
                    }
                }
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) {
                return false;
            } else if (pressed >= 0 && pressed < feed_count) {
                feed_url = urls[pressed];
                feed_name = titles[pressed];
                return true;
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─── Article detail screen ───
static void show_article(int idx) {
    if (idx < 0 || idx >= g_item_count) return;
    rss_draw_chrome("ARTICLE");

    // Title block — wrap
    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    int y = 44;
    const String& title = g_items[idx].title;
    int col = 0;
    gfx->setCursor(8, y);
    for (size_t i = 0; i < title.length() && y < 100; i++) {
        char c = title[i];
        if (c == '\n' || col >= 36) {
            y += 12;
            col = 0;
            gfx->setCursor(8, y);
            if (c == '\n') continue;
        }
        gfx->write(c);
        col++;
    }

    // Divider
    y += 16;
    gfx->drawFastHLine(0, y, 240, 0x4208);

    // Description — wrap, allow scroll later (v1.2 single-page only)
    y += 8;
    gfx->setTextColor(0xFFFF);
    const String& desc = g_items[idx].description;
    col = 0;
    gfx->setCursor(8, y);
    for (size_t i = 0; i < desc.length() && y < 300; i++) {
        char c = desc[i];
        if (c == '\n' || col >= 36) {
            y += 12;
            col = 0;
            gfx->setCursor(8, y);
            if (c == '\n') continue;
        }
        gfx->write(c);
        col++;
    }

    // Touch loop — any tap returns
    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            // wait for release
            while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
            return;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─── Headline list screen ───
static void show_headlines(const String& feed_name) {
    int page = 0;
    int pages = (g_item_count + HEADLINE_PER_PAGE - 1) / HEADLINE_PER_PAGE;
    if (pages < 1) pages = 1;

    auto draw_page = [&]() {
        rss_draw_chrome(feed_name.c_str());

        if (g_item_count == 0) {
            gfx->setTextSize(1);
            gfx->setTextColor(0x8410);
            gfx->setCursor(40, 150);
            gfx->print("(no items)");
            return;
        }

        int start = page * HEADLINE_PER_PAGE;
        int end = min(g_item_count, start + HEADLINE_PER_PAGE);

        for (int i = start; i < end; i++) {
            int row = i - start;
            int y = 44 + row * HEADLINE_ROW_H;
            gfx->fillRect(8, y, 224, HEADLINE_ROW_H - 4, 0x18C3);
            gfx->drawRect(8, y, 224, HEADLINE_ROW_H - 4, 0x4208);

            // Title (truncate at 64 chars, render across 2-3 lines)
            gfx->setTextSize(1);
            gfx->setTextColor(0xFFFF);
            const String& t = g_items[i].title;
            int col = 0;
            int line_y = y + 4;
            gfx->setCursor(12, line_y);
            for (size_t c = 0; c < t.length() && line_y < y + HEADLINE_ROW_H - 14; c++) {
                if (col >= 36) {
                    line_y += 12;
                    col = 0;
                    gfx->setCursor(12, line_y);
                }
                gfx->write(t[c]);
                col++;
            }
        }

        // Pagination
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
            if (ty < 14) {
                pressed = -1;
            } else if (ty >= 304 && pages > 1) {
                if (tx < 80) pressed = -3;       // PREV
                else if (tx > 160) pressed = -4; // NEXT
            } else {
                int start = page * HEADLINE_PER_PAGE;
                int end = min(g_item_count, start + HEADLINE_PER_PAGE);
                for (int i = start; i < end; i++) {
                    int row = i - start;
                    int y = 44 + row * HEADLINE_ROW_H;
                    if (ty >= y && ty < y + HEADLINE_ROW_H - 4) {
                        pressed = i;
                        break;
                    }
                }
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) {
                return;
            } else if (pressed == -3) {
                if (page > 0) { page--; draw_page(); }
            } else if (pressed == -4) {
                if (page < pages - 1) { page++; draw_page(); }
            } else if (pressed >= 0) {
                show_article(pressed);
                draw_page();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─── Public entry ───
void c28p_run_rss() {
    seed_default_feeds_if_empty();

    while (true) {
        String url, name;
        if (!pick_feed(url, name)) {
            return;
        }
        g_feed_name = name;
        rss_draw_chrome(name.c_str());
        rss_draw_loading(name.c_str());
        if (!fetch_feed(url)) {
            rss_draw_error("Fetch failed");
            // Wait for tap to return to feed list
            delay(2000);
            bool was_touched = false;
            unsigned long t0 = millis();
            while (millis() - t0 < 30000) {
                int16_t tx, ty;
                bool touched = c28p_touch_read(&tx, &ty);
                if (touched && !was_touched) break;
                was_touched = touched;
                delay(20);
                yield();
            }
            continue;
        }
        show_headlines(name);
        // After return from headlines, back to feed picker
    }
}

#endif // DEVICE_C28P