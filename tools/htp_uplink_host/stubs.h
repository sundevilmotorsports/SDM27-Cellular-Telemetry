// Host stand-ins for what the HTP section of net_task.cpp touches. See run.sh.
#pragma once
#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    String substring(size_t a, size_t b) const { return String(std::string::substr(a, b - a)); }
    int indexOf(const char *t) const { size_t p = find(t); return p == npos ? -1 : (int)p; }
};
#define deserializeJson(doc, s) deserializeJson(doc, (s).c_str())

struct FakeSerial {
    bool quiet = false;
    template <typename... A> void printf(const char *f, A... a) { if (!quiet) ::printf(f, a...); }
    void println(const char *s = "") { if (!quiet) ::puts(s); }
    void println(const String &s) { if (!quiet) ::puts(s.c_str()); }
};
static FakeSerial Serial;

static uint64_t g_nowUs = 1000000;
// Runs every 10 ms of fake time, including time spent inside a POST, so the
// sampler "task" keeps ticking while the net task is blocked.
static std::function<void()> g_onTick;
static void advanceTime(uint64_t us) {
    for (uint64_t end = g_nowUs + us; g_nowUs < end;) {
        g_nowUs += std::min<uint64_t>(10000, end - g_nowUs);
        if (g_onTick) g_onTick();
    }
}
static const int64_t kEpochOffsetMs = 1793320000000LL;
static uint32_t millis() { return (uint32_t)(g_nowUs / 1000); }
static int64_t esp_timer_get_time() { return (int64_t)g_nowUs; }
static uint32_t esp_random() { return (uint32_t)rand(); }
static bool psramFound() { return false; }
#define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n, int) { return malloc(n); }

struct TimeSync {
    bool isSynced() const { return true; }
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
static std::function<int(const uint8_t *, size_t, std::string &)> g_server;
struct HTTPClient {
    std::string resp;
    bool begin(FakeClient &, const char *) { return true; }
    void setConnectTimeout(int) {}
    void setTimeout(int) {}
    void addHeader(const char *, const char *) {}
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
static int mbedtls_md_hmac(const mbedtls_md_info_t *, const unsigned char *, size_t,
                           const uint8_t *, size_t, uint8_t *mac) { memset(mac, 0xab, 32); return 0; }

// FreeRTOS, single-threaded: locks are no-ops, tasks are never created.
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) (void)(m)
#define portEXIT_CRITICAL(m) (void)(m)
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) (ms)
#define tskIDLE_PRIORITY 0
static SemaphoreHandle_t xSemaphoreCreateMutex() { static int m; return &m; }
static int xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
static int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
static void vTaskDelay(uint32_t) {}
static void vTaskDelete(void *) {}
static int xTaskCreatePinnedToCore(void (*)(void *), const char *, int, void *, int, void *, int) { return 1; }
