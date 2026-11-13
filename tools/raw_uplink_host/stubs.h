// Host stand-ins for what the raw CAN batch section of net_task.cpp touches:
// fake clock, SNTP state, FreeRTOS queue, socket and scripted HTTP server.
// See run.sh.
#pragma once
#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <functional>

struct String : std::string {
    String() {}
    String(const std::string &s) : std::string(s) {}
    String(const char *s) : std::string(s) {}
    unsigned length() const { return (unsigned)size(); }
    String substring(size_t a, size_t b) const { return String(std::string::substr(a, std::min(b, size()) - a)); }
    int indexOf(const char *t) const { size_t p = find(t); return p == npos ? -1 : (int)p; }
};

struct FakeSerial {
    bool quiet = false;
    template <typename... A> void printf(const char *f, A... a) { if (!quiet) ::printf(f, a...); }
    void println(const char *s = "") { if (!quiet) ::puts(s); }
    void println(const String &s) { if (!quiet) ::puts(s.c_str()); }
};
static FakeSerial Serial;

static uint64_t g_nowUs = 1000000;
static const int64_t kEpochOffsetMs = 1793320000000LL;
// Runs every 10 ms of fake time, including time spent inside a POST, so the
// CAN "task" keeps producing frames while the net task is blocked.
static std::function<void()> g_onTick;
static void advanceTime(uint64_t us) {
    for (uint64_t end = g_nowUs + us; g_nowUs < end;) {
        g_nowUs += std::min<uint64_t>(10000, end - g_nowUs);
        if (g_onTick) g_onTick();
    }
}
static uint32_t millis() { return (uint32_t)(g_nowUs / 1000); }
static int64_t esp_timer_get_time() { return (int64_t)g_nowUs; }
static uint32_t esp_random() { return (uint32_t)rand(); }
static bool psramFound() { return false; }
#define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n, int) { return malloc(n); }

struct TimeSync {
    bool synced = true;
    bool isSynced() const { return synced; }
    int64_t epochMsFor(int64_t us) const { return us / 1000 + kEpochOffsetMs; }
};
static TimeSync g_timeSync;

static bool g_bearer = true;
static bool networkBearerUp() { return g_bearer; }

struct FakeClient {
    bool conn = false;
    bool connected() { return conn; }
    void stop() { conn = false; }
};
static FakeClient netClient;

// Scripted server: returns HTTP code, fills response, takes g_reqMs.
static uint32_t g_reqMs = 100;
static std::map<std::string, std::string> g_headers; // of the last request
static std::function<int(const uint8_t *, size_t, std::string &)> g_server;
struct HTTPClient {
    std::string resp;
    bool begin(FakeClient &, const char *) { g_headers.clear(); return true; }
    void setConnectTimeout(int) {}
    void setTimeout(int) {}
    void addHeader(const char *k, const char *v) { g_headers[k] = v; }
    int POST(uint8_t *b, size_t n) {
        advanceTime((uint64_t)g_reqMs * 1000);
        int code = g_server(b, n, resp);
        netClient.conn = code > 0;
        return code;
    }
    String getString() { return String(resp); }
    void end() {}
};

typedef int mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 0
static const mbedtls_md_info_t *mbedtls_md_info_from_type(int) { static int x; return &x; }
// Not HMAC, but it depends on key and body, so the fake server can tell a
// signature that doesn't match the bytes it got.
static int mbedtls_md_hmac(const mbedtls_md_info_t *, const unsigned char *key, size_t keyLen,
                           const uint8_t *body, size_t n, uint8_t *mac) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < keyLen; i++) h = (h ^ key[i]) * 1099511628211ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ body[i]) * 1099511628211ULL;
    for (int i = 0; i < 32; i++) mac[i] = (uint8_t)(h >> (8 * (i % 8))) ^ (uint8_t)i;
    return 0;
}

// FreeRTOS queue, single-threaded.
#define pdTRUE 1
#define pdFALSE 0
struct FakeQueue {
    std::deque<std::vector<uint8_t>> items;
    size_t itemSize;
    size_t depth;
};
typedef FakeQueue *QueueHandle_t;
static int xQueueSend(QueueHandle_t q, const void *p, uint32_t) {
    if (q->items.size() >= q->depth) return pdFALSE;
    const uint8_t *b = (const uint8_t *)p;
    q->items.emplace_back(b, b + q->itemSize);
    return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q, void *p, uint32_t) {
    if (q->items.empty()) return pdFALSE;
    memcpy(p, q->items.front().data(), q->itemSize);
    q->items.pop_front();
    return pdTRUE;
}
static int xQueuePeek(QueueHandle_t q, void *p, uint32_t) {
    if (q->items.empty()) return pdFALSE;
    memcpy(p, q->items.front().data(), q->itemSize);
    return pdTRUE;
}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return (unsigned)q->items.size(); }
