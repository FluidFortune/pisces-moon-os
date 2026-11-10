// Host unit tests for pm_ai_proto (desktop g++, C++11).
// Build (from repo root): g++ -std=gnu++11 -I include -I <ArduinoJson>/src src/pm_ai_proto.cpp tools/host_tests/test_pm_ai_proto.cpp -o /tmp/t && /tmp/t

#include "pm_ai_proto.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { std::string _a(a), _b(b); if (_a == _b) g_pass++; else { \
    g_fail++; printf("FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a.c_str(), _b.c_str()); } } while (0)

static void collect(const char* line, size_t len, void* ctx) {
    static_cast<std::vector<std::string>*>(ctx)->push_back(std::string(line, len));
}
static void collect_emit(const char* line, void* ctx) {
    static_cast<std::vector<std::string>*>(ctx)->push_back(line);
}

// Feed a buffer to a stream in pieces of `step` bytes.
static bool feed_steps(PmAiLineStream& s, const std::string& data, size_t step,
                       std::vector<std::string>& out) {
    for (size_t i = 0; i < data.size(); i += step) {
        size_t n = std::min(step, data.size() - i);
        if (!s.feed((const uint8_t*)data.data() + i, n, collect, &out)) return false;
    }
    s.finish(collect, &out);
    return true;
}

// Same framing as pisces_companion.py: each NDJSON line is its own chunk.
static std::string chunk_encode(const std::vector<std::string>& lines) {
    std::string out;
    char head[16];
    for (const std::string& l : lines) {
        std::string data = l + "\n";
        snprintf(head, sizeof(head), "%zx\r\n", data.size());
        out += head;
        out += data;
        out += "\r\n";
    }
    out += "0\r\n\r\n";
    return out;
}

static void test_url() {
    PmAiUrl u;
    CHECK(pm_ai_parse_url("192.168.1.50", &u));
    CHECK(!u.tls); CHECK(u.port == 8765); CHECK_STR(u.host, "192.168.1.50"); CHECK_STR(u.base, "");
    CHECK(pm_ai_parse_url("192.168.1.50:9000", &u)); CHECK(u.port == 9000);
    CHECK(pm_ai_parse_url("http://box.local:8765/", &u));
    CHECK_STR(u.host, "box.local"); CHECK(u.port == 8765); CHECK_STR(u.base, "");
    CHECK(pm_ai_parse_url("https://mac.tail1234.ts.net", &u));
    CHECK(u.tls); CHECK(u.port == 443); CHECK_STR(u.host, "mac.tail1234.ts.net");
    CHECK(pm_ai_parse_url("  HTTPS://Mac.ts.net/pm//  ", &u));
    CHECK(u.tls); CHECK_STR(u.base, "/pm");
    CHECK(pm_ai_parse_url("https://x.ts.net:8443", &u)); CHECK(u.port == 8443);
    CHECK(!pm_ai_parse_url("", &u));
    CHECK(!pm_ai_parse_url("ftp://x", &u));
    CHECK(!pm_ai_parse_url("host:99999", &u));
    CHECK(!pm_ai_parse_url("host:0", &u));
    CHECK(!pm_ai_parse_url("host:abc", &u));
    CHECK(!pm_ai_parse_url("http://[::1]:80", &u));
    CHECK(!pm_ai_parse_url("host junk", &u));
    CHECK(!pm_ai_parse_url("http://", &u));
}

static void test_request() {
    PmAiUrl u;
    char buf[512];
    pm_ai_parse_url("192.168.1.50", &u);
    size_t n = pm_ai_build_request(buf, sizeof(buf), "POST", &u, "/v1/chat", "ABCD", 42);
    CHECK(n == strlen(buf));
    CHECK(strncmp(buf, "POST /v1/chat HTTP/1.0\r\n", 24) == 0);
    CHECK(strstr(buf, "Host: 192.168.1.50:8765\r\n") != nullptr);
    CHECK(strstr(buf, "Authorization: Bearer ABCD\r\n") != nullptr);
    CHECK(strstr(buf, "Content-Length: 42\r\n") != nullptr);
    CHECK(n > 4 && strcmp(buf + n - 4, "\r\n\r\n") == 0);

    pm_ai_parse_url("https://mac.ts.net/pm", &u);
    n = pm_ai_build_request(buf, sizeof(buf), "GET", &u, "/v1/health", "", 0);
    CHECK(strncmp(buf, "GET /pm/v1/health HTTP/1.0\r\n", 28) == 0);
    CHECK(strstr(buf, "Host: mac.ts.net\r\n") != nullptr);
    CHECK(strstr(buf, "Authorization") == nullptr);
    CHECK(strstr(buf, "Content-Length") == nullptr);
    CHECK(pm_ai_build_request(buf, 40, "GET", &u, "/v1/health", "", 0) == 0);
}

static void test_head() {
    CHECK(pm_ai_parse_status("HTTP/1.0 200 OK") == 200);
    CHECK(pm_ai_parse_status("HTTP/1.1 404 Not Found") == 404);
    CHECK(pm_ai_parse_status("HTTP/1.1 502") == 502);
    CHECK(pm_ai_parse_status("http/1.1 401 Unauthorized") == 401);
    CHECK(pm_ai_parse_status("HTTP/2 200") == -1);
    CHECK(pm_ai_parse_status("garbage") == -1);
    CHECK(pm_ai_header_is_chunked("Transfer-Encoding: chunked"));
    CHECK(pm_ai_header_is_chunked("transfer-encoding:Chunked"));
    CHECK(!pm_ai_header_is_chunked("Content-Type: application/x-ndjson"));
    CHECK(!pm_ai_header_is_chunked("Transfer-Encoding: identity"));
}

static const char* REPLY[] = {
    "{\"v\": 1, \"type\": \"ack\", \"id\": \"r_1\", \"session\": \"default\", \"model\": \"llama3.2:3b\"}",
    "{\"type\": \"chunk\", \"text\": \"The sky looks blue because \"}",
    "{\"type\": \"chunk\", \"text\": \"air scatters \\\"short\\\" waves.\\nNew line.\"}",
    "{\"type\": \"done\", \"id\": \"r_1\", \"stats\": {\"eval_count\": 42}}",
};

static void test_stream_identity() {
    std::string data;
    for (const char* l : REPLY) { data += l; data += "\n"; }
    for (size_t step : {1, 2, 3, 7, 64, 4096}) {
        char buf[1025];
        PmAiLineStream s;
        s.begin(false, buf, sizeof(buf));
        std::vector<std::string> lines;
        CHECK(feed_steps(s, data, step, lines));
        CHECK(lines.size() == 4);
        if (lines.size() == 4) CHECK_STR(lines[1], REPLY[1]);
    }
    // CRLF line endings and a final line with no newline.
    char buf[64];
    PmAiLineStream s;
    s.begin(false, buf, sizeof(buf));
    std::vector<std::string> lines;
    feed_steps(s, "one\r\ntwo\n\nthree", 3, lines);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) { CHECK_STR(lines[0], "one"); CHECK_STR(lines[2], "three"); }
}

static void test_stream_chunked() {
    std::vector<std::string> src(REPLY, REPLY + 4);
    std::string data = chunk_encode(src);
    for (size_t step : {1, 5, 13, 4096}) {
        char buf[1025];
        PmAiLineStream s;
        s.begin(true, buf, sizeof(buf));
        std::vector<std::string> lines;
        CHECK(feed_steps(s, data, step, lines));
        CHECK(s.chunkedDone());
        CHECK(lines.size() == 4);
        if (lines.size() == 4) CHECK_STR(lines[3], REPLY[3]);
    }
    // Chunk extensions and uppercase hex are legal.
    char buf[256];
    PmAiLineStream s;
    s.begin(true, buf, sizeof(buf));
    std::vector<std::string> lines;
    std::string ext = "6;name=x\r\nhello\n\r\nA\r\n0123456789\r\n0\r\n\r\n";
    CHECK(feed_steps(s, ext, 2, lines));
    CHECK(lines.size() == 2);
    // Bad framing is reported.
    PmAiLineStream bad;
    bad.begin(true, buf, sizeof(buf));
    std::vector<std::string> junk;
    CHECK(!bad.feed((const uint8_t*)"zz\r\n", 4, collect, &junk));
}

static void test_stream_overflow() {
    char buf[16];
    PmAiLineStream s;
    s.begin(false, buf, sizeof(buf));
    std::vector<std::string> lines;
    feed_steps(s, "short\n" + std::string(40, 'x') + "\nafter\n", 4, lines);
    CHECK(lines.size() == 2);
    if (lines.size() == 2) { CHECK_STR(lines[0], "short"); CHECK_STR(lines[1], "after"); }
}

static void test_lines() {
    char t[128];
    CHECK(pm_ai_parse_line(REPLY[0], t, sizeof(t)) == PM_AI_LINE_ACK); CHECK_STR(t, "llama3.2:3b");
    CHECK(pm_ai_parse_line(REPLY[2], t, sizeof(t)) == PM_AI_LINE_CHUNK);
    CHECK_STR(t, "air scatters \"short\" waves.\nNew line.");
    CHECK(pm_ai_parse_line(REPLY[3], t, sizeof(t)) == PM_AI_LINE_DONE);
    CHECK(pm_ai_parse_line("{\"type\":\"error\",\"code\":\"incomplete\",\"message\":\"Ollama ended the reply early.\"}",
                           t, sizeof(t)) == PM_AI_LINE_ERROR);
    CHECK_STR(t, "Ollama ended the reply early.");
    CHECK(pm_ai_parse_line("{\"type\":\"error\",\"code\":\"backend_timeout\"}", t, sizeof(t)) == PM_AI_LINE_ERROR);
    CHECK_STR(t, "backend_timeout");
    CHECK(pm_ai_parse_line("{\"type\":\"progress\"}", t, sizeof(t)) == PM_AI_LINE_UNKNOWN);
    CHECK(pm_ai_parse_line("not json", t, sizeof(t)) == PM_AI_LINE_BAD);
    CHECK(pm_ai_parse_line("[1,2]", t, sizeof(t)) == PM_AI_LINE_BAD);
    // Truncation into a small buffer stays terminated.
    char small[8];
    CHECK(pm_ai_parse_line(REPLY[1], small, sizeof(small)) == PM_AI_LINE_CHUNK);
    CHECK_STR(small, "The sky");
}

static void test_build_chat() {
    char out[256];
    size_t n = pm_ai_build_chat(out, sizeof(out), "say \"hi\" \\ now", "llama3.2:3b", "default", true, true);
    CHECK(n == strlen(out) && n > 0);
    CHECK_STR(out, "{\"text\":\"say \\\"hi\\\" \\\\ now\",\"model\":\"llama3.2:3b\",\"session\":\"default\",\"reset\":true,\"ascii\":true}");
    n = pm_ai_build_chat(out, sizeof(out), "hi", "", nullptr, false, false);
    CHECK_STR(out, "{\"text\":\"hi\"}");
    CHECK(pm_ai_build_chat(out, 8, "this will not fit", nullptr, nullptr, false, false) == 0);
    CHECK_STR(out, "");
}

static void test_bodies() {
    char code[32], msg[96];
    CHECK(pm_ai_parse_error("{\"error\": {\"code\": \"unauthorized\", \"message\": \"Missing or invalid device key.\"}}",
                            code, sizeof(code), msg, sizeof(msg)));
    CHECK_STR(code, "unauthorized"); CHECK_STR(msg, "Missing or invalid device key.");
    CHECK(!pm_ai_parse_error("{\"ok\":true}", code, sizeof(code), msg, sizeof(msg)));
    CHECK(!pm_ai_parse_error("<html>", code, sizeof(code), msg, sizeof(msg)));

    char names[4][PM_AI_MODEL_NAMESZ];
    char def[PM_AI_MODEL_NAMESZ];
    int n = pm_ai_parse_models(
        "{\"models\": [{\"name\": \"llama3.2:3b\", \"size\": 2019393189, \"family\": \"llama\"},"
        "{\"name\": \"qwen2.5:7b\"}, {\"size\": 1}, {\"name\": \"a\"}, {\"name\": \"b\"}, {\"name\": \"c\"}],"
        " \"default\": \"qwen2.5:7b\"}", names, 4, def, sizeof(def));
    CHECK(n == 4);
    CHECK_STR(names[0], "llama3.2:3b"); CHECK_STR(names[1], "qwen2.5:7b"); CHECK_STR(names[3], "b");
    CHECK_STR(def, "qwen2.5:7b");
    CHECK(pm_ai_parse_models("{\"models\": [], \"default\": null}", names, 4, def, sizeof(def)) == 0);
    CHECK_STR(def, "");
    CHECK(pm_ai_parse_models("oops", names, 4, def, sizeof(def)) == -1);

    char name[33];
    CHECK(pm_ai_parse_whoami("{\"id\": \"d_1\", \"name\": \"Cardputer\", \"permissions\": [\"chat\"]}", name, sizeof(name)));
    CHECK_STR(name, "Cardputer");
    CHECK(!pm_ai_parse_whoami("{}", name, sizeof(name)));
}

static void test_wrap() {
    PmAiWrap w;
    std::vector<std::string> out;
    w.begin(10);
    // Text arriving in uneven pieces, as streamed chunks do.
    const char* pieces[] = {"Hello wo", "rld, this ", "is a te", "st of wra", "pping"};
    for (const char* p : pieces) w.feed(p, collect_emit, &out);
    CHECK_STR(w.partial(), "wrapping");
    w.flush(collect_emit, &out);
    std::vector<std::string> want = {"Hello", "world,", "this is a", "test of", "wrapping"};
    CHECK(out == want);
    if (out != want) for (auto& s : out) printf("  [%s]\n", s.c_str());

    out.clear();
    w.begin(10);
    w.feed("abcdefghijklmnop", collect_emit, &out);   // no spaces: hard break
    w.flush(collect_emit, &out);
    want = {"abcdefghij", "klmnop"};
    CHECK(out == want);

    out.clear();
    w.begin(40);
    w.feed("line one\n\nline   three\r\n", collect_emit, &out);
    want = {"line one", "", "line   three"};
    CHECK(out == want);
    CHECK_STR(w.partial(), "");

    // No line is ever wider than the width, for any split of the input.
    std::string text = "The quick brown fox jumps over the lazy dog while a supercalifragilistic word appears.";
    for (int width = 8; width <= 30; width++) {
        for (size_t step = 1; step <= 9; step += 4) {
            out.clear();
            w.begin(width);
            for (size_t i = 0; i < text.size(); i += step)
                w.feed(text.substr(i, step).c_str(), collect_emit, &out);
            w.flush(collect_emit, &out);
            bool ok = true;
            std::string joined;
            for (auto& s : out) { if ((int)s.size() > width) ok = false; joined += s; }
            CHECK(ok);
            // Nothing but spaces is lost.
            std::string a, b;
            for (char c : text) if (c != ' ') a += c;
            for (char c : joined) if (c != ' ') b += c;
            CHECK(a == b);
        }
    }
}

int main() {
    test_url();
    test_request();
    test_head();
    test_stream_identity();
    test_stream_chunked();
    test_stream_overflow();
    test_lines();
    test_build_chat();
    test_bodies();
    test_wrap();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
