// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  terminal_app.cpp — AI terminal (T-Deck Plus, T-LoRa Pager, Cardputer ADV)
//
//  Type a prompt, get an answer. Two backends (pm_ai.h):
//    - Pisces AI Companion on your own computer (Ollama), streamed
//      over WiFi: home network or Tailscale Funnel from anywhere.
//    - Gemini, direct (key in secrets.h).
//
//  Commands (type at the prompt):
//    /help    list commands          /setup   connect to the companion
//    /model   pick a model           /new     start a new conversation
//    /local   use the companion      /gemini  use Gemini
//    /status  show settings          /clear   clear the screen
//
//  While a reply streams: Q or ESC stops it (T-Deck: tap the header).
//  Scroll: trackball (T-Deck, Pager) or Fn+; / Fn+. (Cardputer arrows).
//
//  Text is wrapped to the real screen width (the shared TextBuffer wraps
//  at 55 columns, wider than the Cardputer's 38).
// ─────────────────────────────────────────────

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "keyboard.h"
#include "pm_input.h"
#include "apps.h"
#include "gemini_client.h"
#include "text_buffer.h"
#include "pm_ai.h"
#include "pm_ai_proto.h"
#include <WiFi.h>

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 135;
#else
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
#endif

#define TERM_HDR_H      24
#define TERM_BODY_Y     26
static constexpr int TERM_INPUT_H = 22;
static constexpr int TERM_INPUT_Y = DISP_H - TERM_INPUT_H;
static constexpr int TERM_BODY_H  = TERM_INPUT_Y - TERM_BODY_Y;

// Columns that fit on screen (6 px font, margins + scrollbar), capped by
// the TextBuffer line size.
static constexpr int TERM_COLS_SCREEN = (DISP_W - 8) / 6;
static constexpr int TERM_COLS = TERM_COLS_SCREEN < (TB_LINE_WIDTH - 1)
                                 ? TERM_COLS_SCREEN : (TB_LINE_WIDTH - 1);

static constexpr int MAX_PROMPT = 1000;
static constexpr int MAX_PICK   = 9;

static const uint16_t COL_USER = 0x07E0;
static const uint16_t COL_AI   = 0xFFFF;
static const uint16_t COL_SYS  = C_GREY;
static const uint16_t COL_OK   = 0x07FF;
static const uint16_t COL_WARN = 0xFD20;
static const uint16_t COL_ERR  = 0xF800;

static PmAiConfig g_cfg;
static bool       g_reset_next = false;

// ─── Drawing ─────────────────────────────────────────────────

static String backend_label() {
    if (g_cfg.backend == PM_AI_BACKEND_COMPANION) {
        return g_cfg.model.length() ? g_cfg.model : String("LOCAL AI");
    }
    return "GEMINI";
}

static void drawHeader() {
    gfx->fillRect(0, 0, DISP_W, TERM_HDR_H, C_DARK);
    gfx->drawFastHLine(0, TERM_HDR_H, DISP_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(6, 8);
    gfx->print("AI | ");
    const char* hint =
#ifdef DEVICE_TLORAPAGER
        "/help";
#else
        "/help  Q exit";
#endif
    int hint_w = (int)strlen(hint) * 6;
    int max_label = (DISP_W - 6 - 30 - hint_w - 12) / 6;
    String label = backend_label();
    if ((int)label.length() > max_label) label = label.substring(0, max_label);
    gfx->setTextColor(C_WHITE);
    gfx->print(label);
    gfx->setTextColor(C_GREY);
    gfx->setCursor(DISP_W - hint_w - 6, 8);
    gfx->print(hint);
}

static void drawInputBar(const char* label, const String& input, bool active) {
    gfx->fillRect(0, TERM_INPUT_Y, DISP_W, TERM_INPUT_H, 0x0841);
    gfx->drawFastHLine(0, TERM_INPUT_Y, DISP_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(5, TERM_INPUT_Y + 7);
    gfx->print(label);
    int x = 5 + ((int)strlen(label) + 1) * 6;
#ifdef DEVICE_TLORAPAGER
    if (!active && input.length() == 0) {
        gfx->setTextColor(C_GREY);
        gfx->setCursor(x, TERM_INPUT_Y + 7);
        gfx->print("CLK TYPE | Q EXIT");
        return;
    }
#endif
    gfx->setTextColor(active ? C_WHITE : C_GREY);
    gfx->setCursor(x, TERM_INPUT_Y + 7);
    int maxChars = (DISP_W - x - 8) / 6;
    String visible = input;
    if ((int)visible.length() > maxChars) visible = visible.substring(visible.length() - maxChars);
    gfx->print(visible);
    if (active) gfx->print("_");
}

static void drawBody(TextBuffer& tb) {
    tb.draw(0, TERM_BODY_Y, DISP_W, TERM_BODY_H, 1);
}

struct PrintCtx {
    TextBuffer* tb;
    uint16_t    color;
};

static void emit_line(const char* line, void* ctx) {
    PrintCtx* pc = static_cast<PrintCtx*>(ctx);
    pc->tb->append(line, pc->color);
}

// Word-wrap text to the screen width into the scrollback, then show it.
static void say(TextBuffer& tb, const char* text, uint16_t color) {
    PmAiWrap w;
    w.begin(TERM_COLS);
    PrintCtx pc = { &tb, color };
    w.feed(text, emit_line, &pc);
    w.flush(emit_line, &pc);
    tb.scroll_to_bottom();
    drawBody(tb);
}

static void say(TextBuffer& tb, const String& text, uint16_t color) {
    say(tb, text.c_str(), color);
}

static void redrawAll(TextBuffer& tb, const String& prompt, bool active) {
    gfx->fillScreen(C_BLACK);
    drawHeader();
    drawBody(tb);
    drawInputBar("USER>", prompt, active);
}

// ─── Modal line input (setup, model pick) ────────────────────

// Edits `out` in the input bar. ENTER confirms (true); ESC or ` cancels.
static bool read_line(const char* label, String& out, int maxlen) {
    drawInputBar(label, out, true);
    while (true) {
        char k = get_keypress();
        if (!k) { delay(20); yield(); continue; }
        if (k == PM_KEY_ESC || k == '`') return false;
        if (k == PM_KEY_ENTER || k == '\n') return true;
        if (k == PM_KEY_BACKSPACE || k == 127) {
            if (out.length()) out.remove(out.length() - 1);
        } else if (k >= 32 && k <= 126 && (int)out.length() < maxlen) {
            out += k;
        }
        drawInputBar(label, out, true);
    }
}

// ─── Streaming display ───────────────────────────────────────

struct StreamView {
    TextBuffer* tb;
    PmAiWrap    wrap;
    PrintCtx    pc;
    uint32_t    last_draw;
    bool        any;
};

// Show committed lines plus the line still being built.
static void render_stream(StreamView& sv) {
    TextBuffer& tb = *sv.tb;
    tb.append(sv.wrap.partial(), COL_AI);   // temporary line
    tb.scroll_to_bottom();
    drawBody(tb);
    if (tb.count > 0) tb.count--;          // remove it again
    sv.last_draw = millis();
}

static void on_chunk(const char* text, void* ctx) {
    StreamView* sv = static_cast<StreamView*>(ctx);
    sv->any = true;
    sv->wrap.feed(text, emit_line, &sv->pc);
    if (millis() - sv->last_draw >= 90) render_stream(*sv);
}

static bool on_cancel(void* ctx) {
    (void)ctx;
    char k = get_keypress();
    if (k == PM_KEY_ESC || k == '`' || pm_is_exit_key(k)) return true;
#ifndef DEVICE_TLORAPAGER
    int16_t tx, ty;
    if (get_touch(&tx, &ty) && ty < TERM_HDR_H) {
        while (get_touch(&tx, &ty)) { delay(10); yield(); }
        return true;
    }
#endif
    return false;
}

// ─── Commands ────────────────────────────────────────────────

static void cmd_help(TextBuffer& tb) {
    say(tb, "/setup  connect to your computer", COL_SYS);
    say(tb, "/model  pick a model", COL_SYS);
    say(tb, "/new    start a new conversation", COL_SYS);
    say(tb, "/local  use your computer's AI", COL_SYS);
    say(tb, "/gemini use Gemini", COL_SYS);
    say(tb, "/status show settings", COL_SYS);
    say(tb, "/clear  clear the screen", COL_SYS);
    say(tb, "While a reply streams, Q or ESC stops it.", COL_SYS);
}

static void cmd_status(TextBuffer& tb) {
    say(tb, String("Backend: ") +
            (g_cfg.backend == PM_AI_BACKEND_COMPANION ? "companion" : "Gemini"), COL_SYS);
    say(tb, String("URL: ") + (g_cfg.url.length() ? g_cfg.url : String("(not set)")), COL_SYS);
    String key = "(not set)";
    if (g_cfg.key.length() >= 4) key = "****" + g_cfg.key.substring(g_cfg.key.length() - 4);
    say(tb, "Key: " + key, COL_SYS);
    say(tb, "Model: " + (g_cfg.model.length() ? g_cfg.model : String("companion default")), COL_SYS);
    say(tb, String("WiFi: ") + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                                               : String("not connected")), COL_SYS);
}

// Connects WiFi, repainting afterwards (the Cardputer may show a
// "stop wardrive?" modal). Prints the error and returns false on failure.
static bool wifi_up(TextBuffer& tb, const String& prompt) {
    if (WiFi.status() == WL_CONNECTED) return true;
    say(tb, "Connecting WiFi...", COL_SYS);
    String err;
    bool ok = pm_ai_ensure_wifi(err);
    redrawAll(tb, prompt, false);
    if (!ok) say(tb, err, COL_ERR);
    return ok;
}

static void cmd_model(TextBuffer& tb) {
    if (!pm_ai_companion_configured(g_cfg)) { say(tb, "Run /setup first.", COL_WARN); return; }
    if (!wifi_up(tb, "")) return;
    say(tb, "Asking for models...", COL_SYS);
    String names[MAX_PICK];
    String def, err;
    int n = pm_ai_models(g_cfg, names, MAX_PICK, def, err);
    if (n < 0) { say(tb, err, COL_ERR); return; }
    if (n == 0) { say(tb, "No models. On the computer: ollama pull llama3.2", COL_WARN); return; }
    String row0 = "0  companion default";
    if (def.length()) row0 += " (" + def + ")";
    say(tb, row0, COL_SYS);
    for (int i = 0; i < n; i++) {
        String row = String(i + 1) + "  " + names[i];
        say(tb, row, names[i] == g_cfg.model ? COL_OK : COL_SYS);
    }
    String pick;
    if (!read_line("PICK>", pick, 2)) { say(tb, "Unchanged.", COL_SYS); return; }
    int choice = pick.toInt();
    if (pick == "0") {
        g_cfg.model = "";
    } else if (choice >= 1 && choice <= n) {
        g_cfg.model = names[choice - 1];
    } else {
        say(tb, "Unchanged.", COL_SYS);
        return;
    }
    pm_ai_save_config(g_cfg);
    drawHeader();
    say(tb, "Model: " + (g_cfg.model.length() ? g_cfg.model : String("companion default")), COL_OK);
}

static void cmd_setup(TextBuffer& tb) {
    say(tb, "Connect to the Pisces AI Companion on your computer. ESC cancels.", COL_SYS);
    say(tb, "URL: the LAN address 'serve' prints (192.168.1.50:8765) or your https://...ts.net Funnel address.", COL_SYS);
    String url = g_cfg.url;
    if (!read_line("URL>", url, 95)) { say(tb, "Setup cancelled.", COL_SYS); return; }
    url.trim();
    PmAiUrl parsed;
    if (!pm_ai_parse_url(url.c_str(), &parsed)) { say(tb, "That URL doesn't look right.", COL_ERR); return; }

    say(tb, g_cfg.key.length() ? "Device key (Enter keeps the current one):"
                               : "Device key from 'pair' (dashes optional):", COL_SYS);
    String key;
    if (!read_line("KEY>", key, 40)) { say(tb, "Setup cancelled.", COL_SYS); return; }
    key.trim();
    if (!key.length()) key = g_cfg.key;
    if (!key.length()) { say(tb, "A device key is required.", COL_ERR); return; }

    if (url != g_cfg.url) g_cfg.model = "";   // different computer: its default model
    g_cfg.url = url;
    g_cfg.key = key;
    g_cfg.backend = PM_AI_BACKEND_COMPANION;
    pm_ai_save_config(g_cfg);
    drawHeader();
    say(tb, "Saved.", COL_OK);

    if (!wifi_up(tb, "")) return;
    say(tb, "Checking...", COL_SYS);
    String name, err;
    if (!pm_ai_check(g_cfg, name, err)) { say(tb, err, COL_ERR); return; }
    say(tb, "Paired as " + name + ".", COL_OK);
    cmd_model(tb);
}

// Returns true if the input was a command (handled here).
static bool run_command(TextBuffer& tb, const String& line) {
    if (!line.startsWith("/")) return false;
    String cmd = line;
    cmd.trim();
    cmd.toLowerCase();
    if (cmd == "/help" || cmd == "/?") {
        cmd_help(tb);
    } else if (cmd == "/setup") {
        cmd_setup(tb);
    } else if (cmd == "/model" || cmd == "/models") {
        cmd_model(tb);
    } else if (cmd == "/new" || cmd == "/reset") {
        g_reset_next = true;
        say(tb, "Next message starts a new conversation.", COL_OK);
    } else if (cmd == "/local" || cmd == "/companion") {
        if (!pm_ai_companion_configured(g_cfg)) {
            say(tb, "Run /setup first.", COL_WARN);
        } else {
            g_cfg.backend = PM_AI_BACKEND_COMPANION;
            pm_ai_save_config(g_cfg);
            drawHeader();
            say(tb, "Using your computer's AI.", COL_OK);
        }
    } else if (cmd == "/gemini") {
        g_cfg.backend = PM_AI_BACKEND_GEMINI;
        pm_ai_save_config(g_cfg);
        drawHeader();
        say(tb, gemini_has_key() ? "Using Gemini." : "Using Gemini, but no key is set in secrets.h.",
            gemini_has_key() ? COL_OK : COL_WARN);
    } else if (cmd == "/status") {
        cmd_status(tb);
    } else if (cmd == "/clear") {
        tb.clear();
        drawBody(tb);
    } else {
        say(tb, "Unknown command. Try /help.", COL_WARN);
    }
    return true;
}

// ─── Chat ────────────────────────────────────────────────────

static void send_prompt(TextBuffer& tb, const String& prompt) {
    say(tb, "USER> " + prompt, COL_USER);
    if (!wifi_up(tb, "")) { say(tb, "", COL_AI); return; }

    StreamView sv;
    sv.tb = &tb;
    sv.wrap.begin(TERM_COLS);
    sv.pc.tb = &tb;
    sv.pc.color = COL_AI;
    sv.last_draw = 0;
    sv.any = false;
    sv.wrap.feed("AI> ", emit_line, &sv.pc);
    render_stream(sv);
    drawInputBar("WAIT>", "Q or ESC stops", false);

    String err;
    PmAiResult r = pm_ai_chat(g_cfg, prompt.c_str(), g_reset_next,
                              on_chunk, on_cancel, &sv, err);
    if (!sv.any) sv.wrap.begin(TERM_COLS);   // drop the bare "AI> " prefix
    sv.wrap.flush(emit_line, &sv.pc);

    if (r == PM_AI_OK) {
        g_reset_next = false;
        if (!sv.any) say(tb, "AI> (empty reply)", COL_SYS);
    } else if (r == PM_AI_CANCELLED) {
        g_reset_next = false;
        say(tb, "[stopped]", COL_WARN);
    } else {
        say(tb, err, COL_ERR);
    }
    say(tb, "", COL_AI);
}

// ─── App entry ───────────────────────────────────────────────

void run_terminal() {
    TextBuffer tb;
    if (!tb.init()) {
        gfx->fillScreen(C_BLACK);
        gfx->setTextColor(0xF800);
        gfx->setCursor(10, 50);
        gfx->print("ERROR: Could not allocate text buffer.");
        delay(3000);
        return;
    }
    tb.visible_rows = TERM_BODY_H / 10;

    pm_ai_load_config(g_cfg);
    // A companion choice that lost its settings falls back to Gemini.
    if (g_cfg.backend == PM_AI_BACKEND_COMPANION && !pm_ai_companion_configured(g_cfg)) {
        g_cfg.backend = PM_AI_BACKEND_GEMINI;
    }

    String prompt = "";
    bool inputActive =
#ifdef DEVICE_TLORAPAGER
        false;
#else
        true;
#endif
    redrawAll(tb, prompt, inputActive);

    say(tb, "AI Terminal ready.", COL_USER);
    if (g_cfg.backend == PM_AI_BACKEND_COMPANION) {
        say(tb, "Using your computer's AI (" + g_cfg.url + ").", COL_SYS);
    } else if (gemini_has_key()) {
        say(tb, "Using Gemini. /setup connects your computer's AI instead.", COL_SYS);
    } else {
        say(tb, "No AI set up yet. Type /setup to connect the Pisces AI Companion on your computer.", COL_WARN);
    }
    say(tb, "Type /help for commands.", COL_SYS);
    say(tb, "", COL_AI);
    drawInputBar("USER>", prompt, inputActive);

    while (true) {
#ifndef DEVICE_TLORAPAGER
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < TERM_HDR_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            tb.free_mem();
            return;
        }
#endif

        TrackballState tbs = update_trackball();
        char k = get_keypress();
        uint8_t uk = (uint8_t)k;

#ifdef DEVICE_TLORAPAGER
        if (tbs.clicked) {
            inputActive = !inputActive;
            drawInputBar("USER>", prompt, inputActive);
            delay(120);
            yield();
            continue;
        }
        if (!inputActive && pm_is_exit_key(k)) {
            tb.free_mem();
            return;
        }
#endif

        // Scrolling: trackball while not typing; arrow/page keys any time.
        if (!inputActive && tbs.y == -1) { tb.scroll_up(2); drawBody(tb); }
        else if (!inputActive && tbs.y == 1) { tb.scroll_down(2); drawBody(tb); }
        if (uk == PM_KEY_UP)        { tb.scroll_up(1); drawBody(tb); continue; }
        if (uk == PM_KEY_DOWN)      { tb.scroll_down(1); drawBody(tb); continue; }
        if (uk == PM_KEY_PGUP)      { tb.scroll_up(tb.visible_rows); drawBody(tb); continue; }
        if (uk == PM_KEY_PGDN)      { tb.scroll_down(tb.visible_rows); drawBody(tb); continue; }

#ifndef DEVICE_TLORAPAGER
        if (pm_is_exit_key(k) && prompt.length() == 0) {
            tb.free_mem();
            return;
        }
#endif

        bool submit = false;
        if (inputActive && k) {
            if (k == PM_KEY_BACKSPACE || k == 127) {
                if (prompt.length() > 0) prompt.remove(prompt.length() - 1);
                drawInputBar("USER>", prompt, inputActive);
            } else if (k == PM_KEY_ENTER || k == '\n') {
                if (prompt.length() > 0) submit = true;   // empty ENTER: ignore
            } else if (k >= 32 && k <= 126 && prompt.length() < (unsigned)MAX_PROMPT) {
                prompt += k;
                drawInputBar("USER>", prompt, inputActive);
            }
        }

        if (!submit) {
            delay(20);
            yield();
            continue;
        }

        String line = prompt;
        prompt = "";
        if (!run_command(tb, line)) send_prompt(tb, line);
#ifdef DEVICE_TLORAPAGER
        inputActive = false;
#else
        inputActive = true;
#endif
        drawInputBar("USER>", prompt, inputActive);
        yield();
    }
}
