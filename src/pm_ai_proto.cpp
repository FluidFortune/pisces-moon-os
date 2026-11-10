// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// pm_ai_proto.cpp — see pm_ai_proto.h. No Arduino dependencies.

#include "pm_ai_proto.h"

#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

// ─── small helpers ───────────────────────────────────────────

static void copy_str(char* dst, size_t sz, const char* src) {
    if (!dst || sz == 0) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= sz) n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Case-insensitive prefix test.
static bool starts_with_ci(const char* s, const char* prefix) {
    while (*prefix) {
        if (lower(*s) != lower(*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static bool contains_ci(const char* s, const char* needle) {
    size_t n = strlen(needle);
    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && lower(s[i]) == lower(needle[i])) i++;
        if (i == n) return true;
    }
    return false;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool host_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) ||
           c == '.' || c == '-' || c == '_';
}

// ─── URL ─────────────────────────────────────────────────────

bool pm_ai_parse_url(const char* url, PmAiUrl* out) {
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));
    while (*url == ' ') url++;

    const char* p = url;
    bool tls = false;
    if (starts_with_ci(p, "https://")) {
        tls = true;
        p += 8;
    } else if (starts_with_ci(p, "http://")) {
        p += 7;
    } else if (strstr(p, "://")) {
        return false;                        // some other scheme
    }

    size_t hl = 0;
    while (p[hl] && p[hl] != ':' && p[hl] != '/' && p[hl] != ' ') {
        if (!host_char_ok(p[hl])) return false;   // includes IPv6 '['
        hl++;
    }
    if (hl == 0 || hl >= sizeof(out->host)) return false;
    memcpy(out->host, p, hl);
    out->host[hl] = '\0';
    p += hl;

    uint32_t port = tls ? 443 : PM_AI_DEFAULT_PORT;
    if (*p == ':') {
        p++;
        if (!is_digit(*p)) return false;
        port = 0;
        while (is_digit(*p)) {
            port = port * 10 + (uint32_t)(*p - '0');
            if (port > 65535) return false;
            p++;
        }
        if (port == 0) return false;
    }

    if (*p == '/') {
        size_t bl = 0;
        while (p[bl] && p[bl] != ' ' && p[bl] != '?' && p[bl] != '#') bl++;
        size_t keep = bl;
        while (keep > 0 && p[keep - 1] == '/') keep--;
        if (keep >= sizeof(out->base)) return false;
        memcpy(out->base, p, keep);
        out->base[keep] = '\0';
        p += bl;
    }
    while (*p == ' ') p++;
    if (*p) return false;                    // junk after the URL

    out->tls = tls;
    out->port = (uint16_t)port;
    return true;
}

// ─── HTTP head ───────────────────────────────────────────────

size_t pm_ai_build_request(char* out, size_t outsz, const char* method,
                           const PmAiUrl* url, const char* path,
                           const char* key, size_t content_len) {
    if (!out || outsz == 0 || !method || !url || !path) return 0;
    char hostport[112];
    bool default_port = url->tls ? (url->port == 443) : (url->port == 80);
    if (default_port) snprintf(hostport, sizeof(hostport), "%s", url->host);
    else snprintf(hostport, sizeof(hostport), "%s:%u", url->host, (unsigned)url->port);

    char auth[96] = "";
    if (key && key[0]) {
        int a = snprintf(auth, sizeof(auth), "Authorization: Bearer %s\r\n", key);
        if (a < 0 || (size_t)a >= sizeof(auth)) return 0;
    }
    char body_hdr[80] = "";
    if (content_len > 0) {
        snprintf(body_hdr, sizeof(body_hdr),
                 "Content-Type: application/json\r\nContent-Length: %u\r\n",
                 (unsigned)content_len);
    }
    int n = snprintf(out, outsz,
                     "%s %s%s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: PiscesMoon\r\n"
                     "Accept: application/x-ndjson, application/json\r\n"
                     "%s%s"
                     "Connection: close\r\n\r\n",
                     method, url->base, path, hostport, auth, body_hdr);
    if (n < 0 || (size_t)n >= outsz) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

int pm_ai_parse_status(const char* line) {
    if (!line || !starts_with_ci(line, "HTTP/1.")) return -1;
    const char* p = line + 7;
    if (!is_digit(*p)) return -1;
    p++;
    if (*p != ' ') return -1;
    p++;
    if (!is_digit(p[0]) || !is_digit(p[1]) || !is_digit(p[2])) return -1;
    if (p[3] && p[3] != ' ' && p[3] != '\r') return -1;
    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

bool pm_ai_header_is_chunked(const char* line) {
    if (!line || !starts_with_ci(line, "transfer-encoding")) return false;
    const char* colon = strchr(line, ':');
    return colon && contains_ci(colon + 1, "chunked");
}

// ─── Body framing ────────────────────────────────────────────

void PmAiLineStream::begin(bool chunked, char* buf, size_t cap) {
    _chunked = chunked;
    _state = ST_SIZE;
    _chunkLeft = 0;
    _hexLen = 0;
    _trailerLen = 0;
    _buf = buf;
    _cap = cap;
    _len = 0;
    _overflow = false;
}

void PmAiLineStream::putByte(char c, PmAiLineFn fn, void* ctx) {
    if (!_buf || _cap < 2) return;
    if (c == '\n') {
        if (!_overflow && _len > 0) {
            size_t n = _len;
            if (_buf[n - 1] == '\r') n--;
            _buf[n] = '\0';
            if (n > 0) fn(_buf, n, ctx);
        }
        _len = 0;
        _overflow = false;
        return;
    }
    if (_overflow) return;
    if (_len + 1 < _cap) {
        _buf[_len++] = c;
    } else {
        _overflow = true;    // too long: drop this whole line
    }
}

bool PmAiLineStream::feed(const uint8_t* data, size_t n, PmAiLineFn fn, void* ctx) {
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (!_chunked) {
            putByte(c, fn, ctx);
            continue;
        }
        switch (_state) {
        case ST_SIZE:
            if (c == '\r') break;
            if (c == '\n') {
                uint32_t size = 0;
                size_t digits = 0;
                for (size_t k = 0; k < _hexLen && _hex[k] != ';'; k++) {
                    char h = lower(_hex[k]);
                    uint32_t v;
                    if (is_digit(h)) v = (uint32_t)(h - '0');
                    else if (h >= 'a' && h <= 'f') v = (uint32_t)(h - 'a' + 10);
                    else if (h == ' ') continue;
                    else return false;
                    if (size > 0x0FFFFFFF) return false;
                    size = size * 16 + v;
                    digits++;
                }
                if (digits == 0) return false;
                _hexLen = 0;
                if (size == 0) {
                    _state = ST_TRAILER;
                    _trailerLen = 0;
                } else {
                    _chunkLeft = size;
                    _state = ST_DATA;
                }
                break;
            }
            if (_hexLen + 1 < sizeof(_hex)) _hex[_hexLen++] = c;
            else return false;
            break;
        case ST_DATA:
            putByte(c, fn, ctx);
            if (--_chunkLeft == 0) _state = ST_DATA_END;
            break;
        case ST_DATA_END:
            if (c == '\r') break;
            if (c == '\n') { _state = ST_SIZE; break; }
            return false;
        case ST_TRAILER:
            if (c == '\r') break;
            if (c == '\n') {
                if (_trailerLen == 0) _state = ST_DONE;
                _trailerLen = 0;
            } else {
                _trailerLen++;
            }
            break;
        default:
            break;   // ST_DONE: ignore anything after the last chunk
        }
    }
    return true;
}

void PmAiLineStream::finish(PmAiLineFn fn, void* ctx) {
    if (_len > 0 && !_overflow) putByte('\n', fn, ctx);
    _len = 0;
    _overflow = false;
}

// ─── Protocol v1 JSON ────────────────────────────────────────

PmAiLineType pm_ai_parse_line(const char* line, char* text, size_t textsz) {
    if (text && textsz) text[0] = '\0';
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) return PM_AI_LINE_BAD;
    if (!doc.is<JsonObject>()) return PM_AI_LINE_BAD;

    const char* type = doc["type"] | "";
    if (strcmp(type, "chunk") == 0) {
        copy_str(text, textsz, doc["text"] | "");
        return PM_AI_LINE_CHUNK;
    }
    if (strcmp(type, "ack") == 0) {
        copy_str(text, textsz, doc["model"] | "");
        return PM_AI_LINE_ACK;
    }
    if (strcmp(type, "done") == 0) return PM_AI_LINE_DONE;
    if (strcmp(type, "error") == 0) {
        const char* msg = doc["message"] | "";
        copy_str(text, textsz, msg[0] ? msg : (doc["code"] | "error"));
        return PM_AI_LINE_ERROR;
    }
    return PM_AI_LINE_UNKNOWN;
}

size_t pm_ai_build_chat(char* out, size_t outsz, const char* prompt,
                        const char* model, const char* session,
                        bool reset, bool ascii) {
    if (!out || outsz == 0 || !prompt) return 0;
    JsonDocument doc;
    doc["text"] = prompt;
    if (model && model[0]) doc["model"] = model;
    if (session && session[0]) doc["session"] = session;
    if (reset) doc["reset"] = true;
    if (ascii) doc["ascii"] = true;
    size_t need = measureJson(doc);
    if (need + 1 > outsz) {
        out[0] = '\0';
        return 0;
    }
    return serializeJson(doc, out, outsz);
}

bool pm_ai_parse_error(const char* body, char* code, size_t codesz,
                       char* msg, size_t msgsz) {
    copy_str(code, codesz, "");
    copy_str(msg, msgsz, "");
    if (!body) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
    JsonObject err = doc["error"];
    if (err.isNull()) return false;
    copy_str(code, codesz, err["code"] | "");
    copy_str(msg, msgsz, err["message"] | "");
    return true;
}

int pm_ai_parse_models(const char* body, char (*names)[PM_AI_MODEL_NAMESZ],
                       int max, char* def, size_t defsz) {
    copy_str(def, defsz, "");
    if (!body) return -1;
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return -1;
    JsonArray arr = doc["models"];
    if (arr.isNull()) return -1;
    int n = 0;
    for (JsonObject m : arr) {
        if (n >= max) break;
        const char* name = m["name"] | "";
        if (!name[0]) continue;
        copy_str(names[n], PM_AI_MODEL_NAMESZ, name);
        n++;
    }
    copy_str(def, defsz, doc["default"] | "");
    return n;
}

bool pm_ai_parse_whoami(const char* body, char* name, size_t namesz) {
    copy_str(name, namesz, "");
    if (!body) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;
    const char* n = doc["name"] | "";
    if (!n[0]) return false;
    copy_str(name, namesz, n);
    return true;
}

// ─── Word wrap ───────────────────────────────────────────────

void PmAiWrap::begin(int width) {
    if (width < 8) width = 8;
    if (width > MAX_WIDTH) width = MAX_WIDTH;
    _width = width;
    _len = 0;
    _buf[0] = '\0';
    _cont = false;
}

void PmAiWrap::put(char c, PmAiEmitFn emit, void* ctx) {
    if (c == '\r') return;
    if (c == '\n') {
        _buf[_len] = '\0';
        emit(_buf, ctx);
        _len = 0;
        _buf[0] = '\0';
        _cont = false;
        return;
    }
    if (c == '\t') c = ' ';
    if ((unsigned char)c < 0x20) return;
    if (_cont && _len == 0 && c == ' ') return;   // no leading space after a wrap

    _buf[_len++] = c;
    _buf[_len] = '\0';
    if (_len <= _width) return;

    // Line is one character too long: break at the last space that
    // keeps a reasonable amount on this line, else hard-break.
    int sp = -1;
    for (int i = _width; i > 0; i--) {
        if (_buf[i] == ' ') { sp = i; break; }
    }
    int take, rest;
    if (sp > _width / 3) {
        take = sp;
        rest = sp + 1;
    } else {
        take = _width;
        rest = _width;
    }
    char line[MAX_WIDTH + 2];
    memcpy(line, _buf, (size_t)take);
    while (take > 0 && line[take - 1] == ' ') take--;
    line[take] = '\0';
    emit(line, ctx);

    int remain = _len - rest;
    memmove(_buf, _buf + rest, (size_t)remain);
    _len = remain;
    int lead = 0;
    while (lead < _len && _buf[lead] == ' ') lead++;
    if (lead) {
        memmove(_buf, _buf + lead, (size_t)(_len - lead));
        _len -= lead;
    }
    _buf[_len] = '\0';
    _cont = true;
}

void PmAiWrap::feed(const char* text, PmAiEmitFn emit, void* ctx) {
    if (!text) return;
    for (const char* p = text; *p; p++) put(*p, emit, ctx);
}

void PmAiWrap::flush(PmAiEmitFn emit, void* ctx) {
    if (_len > 0) {
        _buf[_len] = '\0';
        emit(_buf, ctx);
    }
    _len = 0;
    _buf[0] = '\0';
    _cont = false;
}
