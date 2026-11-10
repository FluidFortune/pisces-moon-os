// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// pm_ai.cpp — see pm_ai.h.
//
// Transport notes:
//   - Requests are HTTP/1.0, so the companion replies with a plain stream
//     that ends when it closes the connection (no chunked decoding needed).
//     Chunked replies are still decoded in case a proxy rewrites them.
//   - HTTPS (Tailscale Funnel) verifies the server against the Let's
//     Encrypt roots in pm_ai_roots.h. No setInsecure().
//   - Cancel = close the socket. The companion stops Ollama and does not
//     add the partial exchange to history.
//   - All buffers are heap-allocated for the duration of one call and
//     freed after, so nothing stays resident on the no-PSRAM Cardputer.

#include "pm_ai.h"
#include "pm_ai_proto.h"
#include "pm_ai_roots.h"
#include "wifi_mode.h"
#include "gemini_client.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <new>
#include <time.h>

static const char*    NVS_NS            = "pm_ai";
static const char*    SESSION           = "default";
static const uint32_t SMALL_TIMEOUT_MS  = 15000;    // health, whoami, models
static const uint32_t HEAD_TIMEOUT_MS   = 180000;   // first reply may wait for a model to load
static const uint32_t IDLE_TIMEOUT_MS   = 120000;   // silence in the middle of a reply
static const uint32_t CANCEL_POLL_MS    = 40;
static const size_t   LINE_CAP          = 2049;     // one NDJSON line
static const size_t   BODY_CAP          = 6145;     // a whole JSON body (model list)
static const size_t   ERROR_BODY_CAP    = 1025;
static const int      MAX_MODELS        = 16;

// ─── Settings (NVS) ──────────────────────────────────────────

void pm_ai_load_config(PmAiConfig& cfg) {
    cfg = PmAiConfig();
    Preferences p;
    if (!p.begin(NVS_NS, true)) return;     // namespace not created yet: defaults
    cfg.backend = p.getUChar("backend", PM_AI_BACKEND_GEMINI);
    cfg.url     = p.getString("url", "");
    cfg.key     = p.getString("key", "");
    cfg.model   = p.getString("model", "");
    p.end();
}

bool pm_ai_save_config(const PmAiConfig& cfg) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    p.putUChar("backend", cfg.backend);
    p.putString("url", cfg.url);
    p.putString("key", cfg.key);
    p.putString("model", cfg.model);
    p.end();
    return true;
}

bool pm_ai_companion_configured(const PmAiConfig& cfg) {
    return cfg.url.length() > 0 && cfg.key.length() > 0;
}

// ─── WiFi ────────────────────────────────────────────────────

bool pm_ai_ensure_wifi(String& err, uint32_t timeout_ms) {
    if (WiFi.status() == WL_CONNECTED) return true;
    // Cardputer: may ask to stop a running wardrive (scanner and client
    // WiFi can't share the no-PSRAM heap). Other devices: no prompt.
    if (!request_wifi_mode(WIFI_MODE_PM_CLIENT, "AI Chat")) {
        err = "WiFi not switched to client mode.";
        return false;
    }
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout_ms) {
        delay(100);
        yield();
    }
    if (WiFi.status() == WL_CONNECTED) return true;
    err = "No WiFi. Join a network in the WIFI app.";
    return false;
}

// ─── Connection ──────────────────────────────────────────────

// Certificate checks need a real clock if mbedTLS validates dates. Sync
// once if the clock was never set (it is a no-op when time is known).
static void sync_time_for_tls() {
    const time_t SANE = 1700000000;   // Nov 2023
    if (time(nullptr) > SANE) return;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    uint32_t t0 = millis();
    while (time(nullptr) < SANE && millis() - t0 < 4000) delay(100);
}

// Owns the socket for one request; closes it on scope exit.
struct Conn {
    WiFiClient*       plain  = nullptr;
    WiFiClientSecure* secure = nullptr;
    Client*           io     = nullptr;
    ~Conn() {
        if (io) io->stop();
        delete secure;
        delete plain;
    }
};

static bool conn_open(Conn& conn, const PmAiUrl& u, String& err) {
    if (u.tls) {
        sync_time_for_tls();
        conn.secure = new (std::nothrow) WiFiClientSecure();
        if (!conn.secure) { err = "Out of memory for HTTPS."; return false; }
        conn.secure->setCACert(PM_AI_ROOT_CAS);
        conn.io = conn.secure;
    } else {
        conn.plain = new (std::nothrow) WiFiClient();
        if (!conn.plain) { err = "Out of memory."; return false; }
        conn.io = conn.plain;
    }
    if (!conn.io->connect(u.host, u.port)) {
        err = String("Can't reach ") + u.host + ":" + String(u.port);
        err += u.tls ? " (HTTPS: certificate or network)." : ". Is the companion running?";
        return false;
    }
    return true;
}

static bool conn_send(Conn& conn, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = conn.io->write((const uint8_t*)data + sent, len - sent);
        if (n == 0) return false;
        sent += n;
    }
    return true;
}

// Reads the status line and headers. Returns the HTTP status, or
// -1 dropped / bad reply, -2 cancelled, -3 timed out.
static int read_head(Conn& conn, bool* chunked, uint32_t timeout_ms,
                     PmAiCancelFn cancel, void* ctx) {
    char line[256];
    size_t len = 0;
    int status = -1;
    bool first = true;
    *chunked = false;
    uint32_t t0 = millis();
    uint32_t last_poll = 0;
    while (true) {
        if (conn.io->available() <= 0) {
            if (!conn.io->connected()) return -1;
            if (millis() - t0 > timeout_ms) return -3;
            if (cancel && millis() - last_poll >= CANCEL_POLL_MS) {
                last_poll = millis();
                if (cancel(ctx)) return -2;
            }
            delay(5);
            continue;
        }
        int ch = conn.io->read();
        if (ch < 0) continue;
        if (ch == '\n') {
            if (len > 0 && line[len - 1] == '\r') len--;
            line[len] = '\0';
            if (first) {
                first = false;
                status = pm_ai_parse_status(line);
                if (status < 0) return -1;
            } else if (len == 0) {
                return status;                       // blank line: end of head
            } else if (pm_ai_header_is_chunked(line)) {
                *chunked = true;
            }
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)ch;
        }
    }
}

static void append_line(const char* line, size_t len, void* ctx) {
    (void)len;
    String* out = static_cast<String*>(ctx);
    if (out->length()) *out += '\n';
    *out += line;
}

// Reads a whole (small) body into out. False on timeout, framing error
// or memory.
static bool read_body(Conn& conn, bool chunked, size_t cap, String& out) {
    char* buf = (char*)malloc(cap);
    if (!buf) return false;
    PmAiLineStream ls;
    ls.begin(chunked, buf, cap);
    uint8_t tmp[128];
    uint32_t last = millis();
    bool ok = true;
    while (true) {
        int a = conn.io->available();
        if (a > 0) {
            int n = conn.io->read(tmp, a > (int)sizeof(tmp) ? sizeof(tmp) : (size_t)a);
            if (n > 0) {
                last = millis();
                if (!ls.feed(tmp, (size_t)n, append_line, &out)) { ok = false; break; }
                if (ls.chunkedDone()) break;
            }
        } else if (!conn.io->connected()) {
            break;
        } else if (millis() - last > SMALL_TIMEOUT_MS) {
            ok = false;
            break;
        } else {
            delay(5);
        }
    }
    ls.finish(append_line, &out);
    free(buf);
    return ok;
}

// Turns a non-200 reply into one screen-sized sentence.
static String describe_error(int status, const String& body, const char* host) {
    char code[32], msg[160];
    if (!pm_ai_parse_error(body.c_str(), code, sizeof(code), msg, sizeof(msg))) {
        return String("HTTP ") + status + " from " + host + ". Is the companion running?";
    }
    if (status == 401) return "Device key rejected. Run /setup or pair again.";
    if (status == 429) return "Too many bad keys. Wait 5 minutes, then retry.";
    if (strcmp(code, "model_not_found") == 0) return String(msg) + " Pick one with /model.";
    if (strcmp(code, "backend_unreachable") == 0) return "Companion can't reach Ollama. Is Ollama running?";
    return String(msg[0] ? msg : code);
}

static bool head_error(int status, String& err) {
    if (status == -2) { err = "Cancelled."; return true; }
    if (status == -3) { err = "No answer from the companion."; return true; }
    if (status < 0)   { err = "Connection dropped."; return true; }
    return false;
}

// One GET returning a small JSON body. Returns the HTTP status (or <0).
static int http_get(const PmAiConfig& cfg, const char* path, bool with_key,
                    String& body, String& err) {
    PmAiUrl u;
    if (!pm_ai_parse_url(cfg.url.c_str(), &u)) { err = "Bad companion URL. Run /setup."; return -1; }
    if (WiFi.status() != WL_CONNECTED) { err = "No WiFi."; return -1; }
    Conn conn;
    if (!conn_open(conn, u, err)) return -1;
    char head[384];
    size_t hn = pm_ai_build_request(head, sizeof(head), "GET", &u, path,
                                    with_key ? cfg.key.c_str() : "", 0);
    if (!hn || !conn_send(conn, head, hn)) { err = "Send failed."; return -1; }
    bool chunked = false;
    int status = read_head(conn, &chunked, SMALL_TIMEOUT_MS, nullptr, nullptr);
    if (head_error(status, err)) return -1;
    if (!read_body(conn, chunked, BODY_CAP, body)) { err = "Reply was cut off."; return -1; }
    return status;
}

// ─── Companion: check + models ───────────────────────────────

bool pm_ai_check(const PmAiConfig& cfg, String& device_name, String& err) {
    String body;
    int status = http_get(cfg, "/v1/health", false, body, err);
    if (status < 0) return false;
    if (status != 200 || body.indexOf("pisces-ai-companion") < 0) {
        err = "That URL answered, but it isn't the Pisces AI Companion.";
        return false;
    }
    body = "";
    PmAiUrl u;
    pm_ai_parse_url(cfg.url.c_str(), &u);
    status = http_get(cfg, "/v1/whoami", true, body, err);
    if (status < 0) return false;
    if (status != 200) { err = describe_error(status, body, u.host); return false; }
    char name[40];
    if (!pm_ai_parse_whoami(body.c_str(), name, sizeof(name))) { err = "Odd reply from companion."; return false; }
    device_name = name;
    return true;
}

int pm_ai_models(const PmAiConfig& cfg, String* names, int max, String& def, String& err) {
    String body;
    int status = http_get(cfg, "/v1/models", true, body, err);
    if (status < 0) return -1;
    PmAiUrl u;
    pm_ai_parse_url(cfg.url.c_str(), &u);
    if (status != 200) { err = describe_error(status, body, u.host); return -1; }
    if (max > MAX_MODELS) max = MAX_MODELS;
    char (*buf)[PM_AI_MODEL_NAMESZ] =
        (char (*)[PM_AI_MODEL_NAMESZ])malloc((size_t)max * PM_AI_MODEL_NAMESZ);
    if (!buf) { err = "Out of memory."; return -1; }
    char d[PM_AI_MODEL_NAMESZ];
    int n = pm_ai_parse_models(body.c_str(), buf, max, d, sizeof(d));
    if (n < 0) {
        err = "Odd model list from companion.";
    } else {
        for (int i = 0; i < n; i++) names[i] = buf[i];
        def = d;
    }
    free(buf);
    return n;
}

// ─── Chat ────────────────────────────────────────────────────

struct StreamState {
    PmAiChunkFn on_chunk;
    void*       ctx;
    char*       text;
    size_t      textsz;
    bool        done;
    bool        failed;
    String      err;
};

static void on_stream_line(const char* line, size_t len, void* p) {
    (void)len;
    StreamState* st = static_cast<StreamState*>(p);
    if (st->done || st->failed) return;
    switch (pm_ai_parse_line(line, st->text, st->textsz)) {
    case PM_AI_LINE_CHUNK:
        if (st->text[0] && st->on_chunk) st->on_chunk(st->text, st->ctx);
        break;
    case PM_AI_LINE_DONE:
        st->done = true;
        break;
    case PM_AI_LINE_ERROR:
        st->failed = true;
        st->err = String("AI error: ") + st->text;
        break;
    default:
        break;   // ack, unknown future line types, stray non-JSON
    }
}

static PmAiResult chat_gemini(const char* prompt, bool reset, PmAiChunkFn on_chunk,
                              void* ctx, String& err) {
    if (reset) reset_gemini_memory();
    String reply = ask_gemini(String(prompt));          // blocks; no cancel
    if (reply.startsWith("ERROR")) {
        err = reply;
        return PM_AI_FAILED;
    }
    if (on_chunk) on_chunk(reply.c_str(), ctx);
    return PM_AI_OK;
}

static PmAiResult chat_companion(const PmAiConfig& cfg, const char* prompt, bool reset,
                                 PmAiChunkFn on_chunk, PmAiCancelFn cancel, void* ctx,
                                 String& err) {
    PmAiUrl u;
    if (!pm_ai_parse_url(cfg.url.c_str(), &u)) { err = "Bad companion URL. Run /setup."; return PM_AI_FAILED; }
    if (WiFi.status() != WL_CONNECTED) { err = "No WiFi."; return PM_AI_FAILED; }

    // Request body. Typed prompts are ASCII, so 2x covers JSON escaping;
    // retry at 6x for control characters (\u00XX) just in case.
    size_t plen = strlen(prompt);
    char* body = nullptr;
    size_t blen = 0;
    for (size_t mult = 2; mult <= 6 && !blen; mult += 4) {
        free(body);
        size_t cap = plen * mult + 256;
        body = (char*)malloc(cap);
        if (!body) { err = "Out of memory."; return PM_AI_FAILED; }
        blen = pm_ai_build_chat(body, cap, prompt, cfg.model.c_str(), SESSION, reset, true);
    }
    if (!blen) { free(body); err = "Prompt too long."; return PM_AI_FAILED; }

    Conn conn;
    if (!conn_open(conn, u, err)) { free(body); return PM_AI_FAILED; }
    char head[384];
    size_t hn = pm_ai_build_request(head, sizeof(head), "POST", &u, "/v1/chat",
                                    cfg.key.c_str(), blen);
    bool sent = hn && conn_send(conn, head, hn) && conn_send(conn, body, blen);
    free(body);
    if (!sent) { err = "Send failed."; return PM_AI_FAILED; }

    bool chunked = false;
    int status = read_head(conn, &chunked, HEAD_TIMEOUT_MS, cancel, ctx);
    if (status == -2) return PM_AI_CANCELLED;
    if (head_error(status, err)) return PM_AI_FAILED;
    if (status != 200) {
        String ebody;
        read_body(conn, chunked, ERROR_BODY_CAP, ebody);
        err = describe_error(status, ebody, u.host);
        return PM_AI_FAILED;
    }

    char* line = (char*)malloc(LINE_CAP);
    char* text = (char*)malloc(LINE_CAP);
    if (!line || !text) {
        free(line);
        free(text);
        err = "Out of memory.";
        return PM_AI_FAILED;
    }
    StreamState st;
    st.on_chunk = on_chunk;
    st.ctx = ctx;
    st.text = text;
    st.textsz = LINE_CAP;
    st.done = false;
    st.failed = false;

    PmAiLineStream ls;
    ls.begin(chunked, line, LINE_CAP);
    uint8_t tmp[256];
    uint32_t last_data = millis();
    uint32_t last_poll = 0;
    bool cancelled = false, framing = false, timed_out = false;

    while (!st.done && !st.failed) {
        if (cancel && millis() - last_poll >= CANCEL_POLL_MS) {
            last_poll = millis();
            if (cancel(ctx)) { cancelled = true; break; }
        }
        int a = conn.io->available();
        if (a > 0) {
            int n = conn.io->read(tmp, a > (int)sizeof(tmp) ? sizeof(tmp) : (size_t)a);
            if (n > 0) {
                last_data = millis();
                if (!ls.feed(tmp, (size_t)n, on_stream_line, &st)) { framing = true; break; }
            }
            continue;
        }
        if (!conn.io->connected()) break;
        if (millis() - last_data > IDLE_TIMEOUT_MS) { timed_out = true; break; }
        delay(5);
        yield();
    }
    if (!cancelled && !framing) ls.finish(on_stream_line, &st);
    free(line);
    free(text);

    if (cancelled) return PM_AI_CANCELLED;     // Conn closes the socket
    if (st.done) return PM_AI_OK;
    if (st.failed) { err = st.err; return PM_AI_FAILED; }
    if (framing) err = "Garbled reply from companion.";
    else if (timed_out) err = "Companion went quiet. Try again.";
    else err = "Reply was cut off.";
    return PM_AI_FAILED;
}

PmAiResult pm_ai_chat(const PmAiConfig& cfg, const char* prompt, bool reset,
                      PmAiChunkFn on_chunk, PmAiCancelFn cancel, void* ctx,
                      String& err) {
    if (!prompt || !prompt[0]) { err = "Empty prompt."; return PM_AI_FAILED; }
    if (cfg.backend == PM_AI_BACKEND_COMPANION) {
        if (!pm_ai_companion_configured(cfg)) { err = "Companion not set up. Run /setup."; return PM_AI_FAILED; }
        return chat_companion(cfg, prompt, reset, on_chunk, cancel, ctx, err);
    }
    return chat_gemini(prompt, reset, on_chunk, ctx, err);
}
