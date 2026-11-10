// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_ai_proto.h — Pisces AI Companion protocol helpers
//
//  The parts of the companion client that don't touch hardware:
//  URL parsing, HTTP response framing (identity or chunked), NDJSON
//  line splitting, JSON encode/decode for protocol v1, and word-wrap
//  for streamed text. No Arduino dependencies (ArduinoJson only), so
//  this file is unit-tested on a desktop compiler.
//
//  Protocol: pisces-ai-companion/docs/PROTOCOL.md
// ─────────────────────────────────────────────

#pragma once

#include <stddef.h>
#include <stdint.h>

// Companion default port when an http URL has none.
#define PM_AI_DEFAULT_PORT 8765

// ── URL ──
struct PmAiUrl {
    bool     tls;
    uint16_t port;
    char     host[96];
    char     base[64];   // path prefix, "" or "/something" (no trailing slash)
};

// Accepts "http://host[:port][/base]", "https://host[:port][/base]" or a
// bare "host[:port]" (treated as http). http without a port uses 8765,
// https without a port uses 443. Returns false if the URL is unusable.
bool pm_ai_parse_url(const char* url, PmAiUrl* out);

// ── HTTP request head ──
// Builds an HTTP/1.0 request head (HTTP/1.0 so the reply is never chunked
// and simply ends when the server closes). key may be NULL or "". A body
// is announced as JSON when content_len > 0. Returns the head length, or
// 0 if it did not fit in out.
size_t pm_ai_build_request(char* out, size_t outsz, const char* method,
                           const PmAiUrl* url, const char* path,
                           const char* key, size_t content_len);

// ── HTTP response head ──
// "HTTP/1.1 200 OK" -> 200. Returns -1 if the line is not a status line.
int  pm_ai_parse_status(const char* line);
// True for a "Transfer-Encoding: chunked" header line (any case).
bool pm_ai_header_is_chunked(const char* line);

// ── Body framing ──
// Decodes an HTTP body (identity or chunked) and splits it into lines.
// The caller supplies the line buffer, so the same class reads small
// NDJSON lines (1 KB) and whole JSON bodies (several KB). A line longer
// than the buffer is dropped, not truncated. '\r' before '\n' is stripped.
typedef void (*PmAiLineFn)(const char* line, size_t len, void* ctx);

class PmAiLineStream {
public:
    void begin(bool chunked, char* buf, size_t cap);
    // Feed raw body bytes. Returns false on a chunked-framing error.
    bool feed(const uint8_t* data, size_t n, PmAiLineFn fn, void* ctx);
    // End of body: deliver a final line that had no trailing newline.
    void finish(PmAiLineFn fn, void* ctx);
    bool chunkedDone() const { return _chunked && _state == ST_DONE; }

private:
    enum { ST_SIZE, ST_DATA, ST_DATA_END, ST_TRAILER, ST_DONE };
    void putByte(char c, PmAiLineFn fn, void* ctx);

    bool     _chunked = false;
    uint8_t  _state = ST_SIZE;
    uint32_t _chunkLeft = 0;
    char     _hex[12] = {0};
    size_t   _hexLen = 0;
    size_t   _trailerLen = 0;
    char*    _buf = nullptr;
    size_t   _cap = 0;
    size_t   _len = 0;
    bool     _overflow = false;
};

// ── Protocol v1 JSON ──
enum PmAiLineType {
    PM_AI_LINE_BAD = 0,    // not JSON / not an object
    PM_AI_LINE_UNKNOWN,    // valid JSON, unknown type (ignore it)
    PM_AI_LINE_ACK,        // text = model name
    PM_AI_LINE_CHUNK,      // text = reply text
    PM_AI_LINE_DONE,
    PM_AI_LINE_ERROR,      // text = error message
};
PmAiLineType pm_ai_parse_line(const char* line, char* text, size_t textsz);

// Build the POST /v1/chat body into out. model/session may be NULL or "".
// Returns the length written, or 0 if it did not fit.
size_t pm_ai_build_chat(char* out, size_t outsz, const char* prompt,
                        const char* model, const char* session,
                        bool reset, bool ascii);

// {"error":{"code":..,"message":..}} -> code/message. False if not that shape.
bool pm_ai_parse_error(const char* body, char* code, size_t codesz,
                       char* msg, size_t msgsz);

// GET /v1/models body -> up to max names (each NAMESZ incl. NUL) plus
// the default model. Returns the number of names, or -1 on bad JSON.
#define PM_AI_MODEL_NAMESZ 48
int pm_ai_parse_models(const char* body, char (*names)[PM_AI_MODEL_NAMESZ],
                       int max, char* def, size_t defsz);

// GET /v1/whoami body -> device name. False on bad JSON.
bool pm_ai_parse_whoami(const char* body, char* name, size_t namesz);

// ── Word wrap for streamed text ──
// Text arrives in arbitrary pieces; complete lines are emitted as soon
// as they are known, and partial() is the line still being built.
typedef void (*PmAiEmitFn)(const char* line, void* ctx);

class PmAiWrap {
public:
    static const int MAX_WIDTH = 63;
    void begin(int width);
    void feed(const char* text, PmAiEmitFn emit, void* ctx);
    void flush(PmAiEmitFn emit, void* ctx);   // emit partial() if non-empty
    const char* partial() const { return _buf; }

private:
    void put(char c, PmAiEmitFn emit, void* ctx);
    char _buf[MAX_WIDTH + 2] = {0};
    int  _len = 0;
    int  _width = 40;
    bool _cont = false;   // current line is a wrapped continuation
};
