#define TELEMETRY_USE_WIFI 1
#define WIFI_AP_MODE 0
#define HTTPS_INGEST_URL "https://x.supabase.co/functions/v1/telemetry-ingest"
#define TELEMETRY_HMAC_KEY "k"
#define HTP_DEVICE_ID "bench-s3"
#include "stubs.h"
#include "telemetry_config.h"
#include "telemetry_format.h"
#include "can_handler.h"
#include "bench_ecu_pattern.h"

// can_handler.cpp's side: the queue and its drop-oldest push.
QueueHandle_t g_canQueue = nullptr;
static uint32_t g_canDropped = 0;
uint32_t canHandlerTakeDroppedCount() {
    uint32_t n = g_canDropped;
    g_canDropped = 0;
    return n;
}

#include "section.inc"

static int g_fail = 0;
static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); g_fail++; }
}

// ---- CAN producer: the bench ECU pattern, one step per 100 ms -------------
struct Sent { int64_t tsMs; uint32_t id; std::string hex; };
static std::vector<Sent> g_generated;
static uint32_t g_step = 0;
static bool g_produce = true;
static int g_extraFdPerTick = 0; // 64-byte FD frames added every 10 ms

static std::string hexOf(const uint8_t *d, int n) {
    static const char *x = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; i++) { s += x[d[i] >> 4]; s += x[d[i] & 15]; }
    return s;
}

static void push(const CanFrame &f) {
    while (xQueueSend(g_canQueue, &f, 0) != pdTRUE) {
        CanFrame discard;
        if (xQueueReceive(g_canQueue, &discard, 0) == pdTRUE) g_canDropped++;
    }
    g_generated.push_back({g_timeSync.epochMsFor(f.capture_us), f.id, hexOf(f.data, f.dlc)});
}

static void canTick() {
    if (!g_produce) return;
    for (int k = 0; k < g_extraFdPerTick; k++) {
        CanFrame f{};
        f.id = 0x123;
        f.fd = f.brs = true;
        f.dlc = 64;
        for (int i = 0; i < 64; i++) f.data[i] = (uint8_t)(i + k);
        f.capture_us = esp_timer_get_time();
        push(f);
    }
    if (g_nowUs % (BENCH_ECU_PERIOD_MS * 1000) != 0) return;
    BenchEcuFrame e[BENCH_ECU_MAX_FRAMES];
    int n = benchEcuFrames(g_step++, e);
    for (int k = 0; k < n; k++) {
        CanFrame f{};
        f.id = e[k].id;
        f.fd = f.brs = e[k].fd;
        f.dlc = e[k].len;
        memcpy(f.data, e[k].data, e[k].len);
        f.capture_us = esp_timer_get_time();
        push(f);
    }
}

// ---- Fake server: validates like telemetry-ingest, stores, dedupes --------
struct Stored { uint64_t seq; uint32_t dropped; std::vector<Sent> frames; size_t bytes; };
static std::vector<Stored> g_stored;
static std::set<uint64_t> g_seqs;
static FILE *g_out = nullptr;
static size_t g_maxBody = 0;
static uint32_t g_maxFrames = 0;

static int goodServer(const uint8_t *b, size_t n, std::string &resp) {
    check(g_headers["Content-Type"] == "application/x-sdm-can-batch+json", "content type");
    check(g_headers["x-htp-device"] == HTP_DEVICE_ID, "device header");
    char sig[65];
    hmacHex(b, n, sig);
    check(g_headers["x-htp-signature"] == sig, "signature matches the body");
    JsonDocument doc;
    if (deserializeJson(doc, (const char *)b, n) != DeserializationError::Ok) {
        check(false, "body is JSON");
        resp = "{\"error\":\"bad json\"}";
        return 400;
    }
    check(doc["device_id"] == HTP_DEVICE_ID, "body device_id matches header");
    JsonArray frames = doc["frames"].as<JsonArray>();
    check(frames.size() >= 1 && frames.size() <= 4096, "1..4096 frames");
    uint64_t seq = doc["seq"].as<uint64_t>();
    // The fake POST takes g_reqMs before the server sees it.
    int64_t sentMs = doc["sent_ms"].as<int64_t>();
    check(doc["sent_ms"].is<int64_t>() && sentMs >= (int64_t)seq &&
          sentMs == g_timeSync.epochMsFor(esp_timer_get_time()) - g_reqMs,
          "sent_ms is the time of this attempt");
    g_maxBody = std::max(g_maxBody, n);
    g_maxFrames = std::max<uint32_t>(g_maxFrames, frames.size());
    if (!g_seqs.insert(seq).second) {
        resp = "{\"stored\":false,\"dup\":true}";
        return 200;
    }
    Stored s{seq, doc["dropped_frames"].as<uint32_t>(), {}, n};
    for (JsonObject f : frames) {
        uint32_t dlc = f["dlc"];
        bool fd = f["fd"] | false;
        check(f["data"].as<std::string>().size() == 2 * dlc, "data is dlc bytes of hex");
        check(fd || !f["brs"].is<bool>(), "classic frames omit brs");
        check(fd ? dlc <= 64 : dlc <= 8, "dlc fits frame type");
        check(f["ts_ms"].is<int64_t>() && f["ts_ms"].as<int64_t>() > 1700000000000LL, "ts_ms is epoch ms");
        s.frames.push_back({f["ts_ms"].as<int64_t>(), f["can_id"].as<uint32_t>(), f["data"].as<std::string>()});
    }
    if (g_out) {
        fwrite(b, 1, n, g_out);
        fputc('\n', g_out);
    }
    g_stored.push_back(s);
    resp = "{\"stored\":true,\"dup\":false}";
    return 200;
}

static void run(uint32_t ms) {
    uint64_t end = g_nowUs + (uint64_t)ms * 1000;
    while (g_nowUs < end) {
        rawMaintain();
        advanceTime(50000);
    }
}

// Every generated frame is either delivered once, in order, or counted in
// some batch's dropped_frames.
static void checkDelivery(const char *what) {
    std::vector<Sent> got;
    uint64_t dropped = 0, lastSeq = 0;
    bool seqUp = true;
    for (auto &s : g_stored) {
        got.insert(got.end(), s.frames.begin(), s.frames.end());
        dropped += s.dropped;
        seqUp &= s.seq > lastSeq;
        lastSeq = s.seq;
    }
    dropped += g_canDropped + s_rawCarryDropped; // not yet reported in a batch
    size_t queued = uxQueueMessagesWaiting(g_canQueue) + (s_batch.len ? s_batch.frames : 0);
    char msg[160];
    snprintf(msg, sizeof msg, "%s: delivered %zu + dropped %llu + queued %zu == generated %zu", what,
             got.size(), (unsigned long long)dropped, queued, g_generated.size());
    check(got.size() + dropped + queued == g_generated.size(), msg);
    snprintf(msg, sizeof msg, "%s: seq increases", what);
    check(seqUp, msg);
    // In order, no repeats: a subsequence of what was generated.
    size_t j = 0;
    for (auto &f : got) {
        while (j < g_generated.size() && !(g_generated[j].tsMs == f.tsMs && g_generated[j].id == f.id &&
                                           g_generated[j].hex == f.hex)) j++;
        if (j == g_generated.size()) break;
        j++;
    }
    snprintf(msg, sizeof msg, "%s: frames arrive in order, unchanged, once", what);
    check(j <= g_generated.size() && !got.empty(), msg);
}

int main(int argc, char **argv) {
    // RAW_TEST_VERBOSE=1 prints every [raw] line.
    const bool verbose = getenv("RAW_TEST_VERBOSE") != nullptr;
    Serial.quiet = !verbose;
    g_out = argc > 1 ? fopen(argv[1], "w") : nullptr;
    static FakeQueue q{{}, sizeof(CanFrame), CAN_QUEUE_DEPTH_NO_PSRAM};
    g_canQueue = &q;
    s_rawConfigOk = rawConfigReady() && rawBufferInit();
    check(s_rawConfigOk, "config ok");
    g_server = goodServer;
    g_onTick = canTick;

    // 1. Before SNTP: frames queue, nothing is posted; after it, they go out
    //    with epoch ts_ms from their original capture time.
    g_timeSync.synced = false;
    run(2000);
    check(g_stored.empty(), "pre-sync: nothing posted");
    g_timeSync.synced = true;
    run(1000);
    check(!g_stored.empty() && g_stored[0].frames.front().tsMs == g_generated.front().tsMs,
          "pre-sync frames keep their capture time");

    // 2. Steady state at ~21 frames/s: about one batch per second.
    size_t before = g_stored.size();
    run(20000);
    size_t batches = g_stored.size() - before;
    check(batches >= 18 && batches <= 21, "steady: ~1 batch/s");
    check(g_stored.back().seq > 1700000000000ULL, "seq is epoch ms");
    checkDelivery("steady");
    printf("1-2 pre-sync + steady: %zu batches, %zu frames generated, queued %u\n", g_stored.size(),
           g_generated.size(), uxQueueMessagesWaiting(g_canQueue));

    // 3. No session for 15 s: the same batch is held (409) and retried every
    //    RAW_NO_SESSION_RETRY_MS; once Start is pressed everything arrives.
    int calls = 0;
    std::vector<uint64_t> seqs409;
    bool sessionRunning = false;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        calls++;
        if (!sessionRunning) {
            JsonDocument d;
            deserializeJson(d, (const char *)b, n);
            seqs409.push_back(d["seq"].as<uint64_t>());
            r = "{\"error\":\"no running session for device; start one from the dashboard\"}";
            return 409;
        }
        return goodServer(b, n, r);
    };
    run(15000);
    check(calls >= 6 && calls <= 9, "409: retried every ~2 s");
    check(std::all_of(seqs409.begin(), seqs409.end(), [&](uint64_t s) { return s == seqs409[0]; }),
          "409: the same batch is held");
    check(s_rawWaitingSession, "409: waiting flag");
    sessionRunning = true;
    before = g_stored.size();
    run(5000);
    check(!s_rawWaitingSession, "409: resumes after Start");
    check(g_stored[before].seq == seqs409[0], "409: held batch is the first one stored");
    checkDelivery("no session");
    printf("3 no session 15 s: %d tries of seq %llu, then %zu batches, max %u frames/batch\n", calls,
           (unsigned long long)seqs409[0], g_stored.size() - before, g_maxFrames);

    // 4. Long wait with a small queue: oldest frames dropped and reported.
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        r = "{\"error\":\"no running session for device\"}";
        return 409;
    };
    run(30000); // ~630 frames into a 256-frame queue
    g_server = goodServer;
    before = g_stored.size();
    run(4000);
    uint64_t reported = 0;
    for (size_t i = before; i < g_stored.size(); i++) reported += g_stored[i].dropped;
    check(reported > 300, "overflow: drops reported in dropped_frames");
    checkDelivery("overflow");
    printf("4 overflow: %llu dropped frames reported\n", (unsigned long long)reported);

    // 5. 503 twice, then OK: same seq resent after backoff.
    calls = 0;
    std::vector<uint64_t> seqs;
    std::vector<int64_t> sents;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        JsonDocument d;
        deserializeJson(d, (const char *)b, n);
        seqs.push_back(d["seq"].as<uint64_t>());
        sents.push_back(d["sent_ms"].as<int64_t>());
        if (++calls <= 2) { r = "{\"error\":\"x\"}"; return 503; }
        return goodServer(b, n, r);
    };
    run(6000);
    check(seqs.size() >= 3 && seqs[0] == seqs[1] && seqs[1] == seqs[2], "503: same batch resent");
    check(sents.size() >= 3 && sents[0] < sents[1] && sents[1] < sents[2], "503: each retry has a new sent_ms");
    checkDelivery("503");
    printf("5 503 x2: seq %llu resent, then stored\n", (unsigned long long)seqs[0]);

    // 6. 400: batch dropped, its frames counted in the next batch.
    calls = 0;
    uint32_t lostFrames = 0;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        if (++calls == 1) {
            JsonDocument d;
            deserializeJson(d, (const char *)b, n);
            lostFrames = d["frames"].size();
            r = "{\"error\":\"frames[0].data must be hex\"}";
            return 400;
        }
        return goodServer(b, n, r);
    };
    before = g_stored.size();
    uint32_t rejected = s_rawRejected;
    run(3000);
    check(s_rawRejected == rejected + 1, "400: one batch rejected");
    check(g_stored.size() > before && g_stored[before].dropped >= lostFrames, "400: its frames reported as dropped");
    checkDelivery("400");
    printf("6 400: %u frames reported dropped in the next batch\n", lostFrames);

    // 7. Kept-alive socket closed by the server: retried at once, no backoff.
    calls = 0;
    netClient.conn = true;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        return ++calls == 1 ? -2 : goodServer(b, n, r);
    };
    run(1100);
    check(calls >= 2 && s_rawNextSendMs == 0, "stale socket: retried without backoff");
    printf("7 stale socket: %d server calls, no backoff\n", calls);

    // 8. Outage 10 s with heavy FD traffic (64-byte frames at 300/s): catch-up
    //    batches stay under the byte cap and the frame cap.
    g_server = goodServer;
    g_extraFdPerTick = 3;
    q.depth = 20000;
    g_bearer = false;
    run(10000);
    g_bearer = true;
    g_maxBody = 0;
    run(10000);
    g_extraFdPerTick = 0;
    run(5000);
    check(g_maxBody <= RAW_BATCH_MAX_BYTES, "byte cap respected");
    check(g_maxFrames <= RAW_BATCH_MAX_FRAMES, "frame cap respected");
    check(g_maxBody > RAW_BATCH_MAX_BYTES - 300, "catch-up batches fill to the byte cap");
    checkDelivery("outage");
    printf("8 outage + FD flood: biggest batch %zu B (cap %u), queued %u after\n", g_maxBody,
           (unsigned)RAW_BATCH_MAX_BYTES, uxQueueMessagesWaiting(g_canQueue));

    // 9. Reboot (statics reset, clock keeps going): seq still increases, so
    //    the server never sees a repeat.
    uint64_t lastSeq = g_stored.back().seq;
    s_rawLastSeq = 0;
    run(2000);
    check(g_stored.back().seq > lastSeq, "reboot: seq keeps increasing");
    checkDelivery("reboot");

    // 10. Stats line, printed once.
    Serial.quiet = false;
    s_rawStats = {};
    s_rawStatsStartMs = millis();
    run(10100);
    Serial.quiet = !verbose;

    if (g_out) fclose(g_out);
    printf(g_fail ? "%d FAILED\n" : "uplink: all passed\n", g_fail);
    return g_fail != 0;
}
