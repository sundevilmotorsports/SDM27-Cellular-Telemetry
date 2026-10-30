#define TELEMETRY_USE_WIFI 1
#define WIFI_AP_MODE 0
#define HTTPS_INGEST_URL "https://x.supabase.co/functions/v1/telemetry-ingest"
#define TELEMETRY_HMAC_KEY "k"
#define HTP_SESSION_ID "3c70901f-0eeb-41c9-914d-621ddbf38774"
#include "stubs.h"
#include "telemetry_config.h"
#include "htp_frame.h"
#include "section.inc"

// Server state: what got staged, per group.
static std::map<int, std::set<uint32_t>> g_staged;
static std::vector<std::pair<int,int>> g_posts; // (group, window_count)
static int g_fail = 0;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }

static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); g_fail++; }
}

static int goodServer(const uint8_t *b, size_t n, std::string &resp) {
    check(b[0] == 0x54 && b[1] == 0x48 && b[2] == 1, "magic/version");
    int g = b[22], wc = b[23];
    uint32_t seq = rd32(b + 24);
    size_t wb = htpWindowBytes(&kHtpGroups[g]);
    check(n == HTP_HEADER_BYTES + wc * wb, "frame length");
    check(rd64(b + 28) > 0, "send timestamp set");
    std::string acked, dup;
    for (int w = 0; w < wc; w++) {
        uint64_t t = rd64(b + HTP_HEADER_BYTES + w * wb);
        check(t == (uint64_t)(seq + w) * 1000000ULL, "t_start matches seq");
        bool fresh = g_staged[g].insert(seq + w).second;
        std::string &l = fresh ? acked : dup;
        if (!l.empty()) l += ",";
        l += std::to_string(seq + w);
        if (!fresh) { acked += acked.empty() ? "" : ","; acked += std::to_string(seq + w); }
    }
    g_posts.push_back({g, wc});
    resp = "{\"acked\":[" + acked + "],\"dup\":[" + dup + "],\"live_published\":true,"
           "\"server_recv_ms\":1000,\"server_send_ms\":1042}";
    return 200;
}

// Runs the net loop (50 ms ticks) for `ms` of fake time.
static void run(uint32_t ms) {
    uint64_t end = g_nowUs + (uint64_t)ms * 1000;
    while (g_nowUs < end) {
        htpMaintain();
        g_nowUs += 50000;
    }
}

static int queued() { return s_htpQ[0].count + s_htpQ[1].count + s_htpQ[2].count; }

static bool contiguous(int g, uint32_t *lo, uint32_t *hi) {
    auto &s = g_staged[g];
    if (s.empty()) return false;
    *lo = *s.begin(); *hi = *s.rbegin();
    return s.size() == *hi - *lo + 1;
}

int main() {
    Serial.quiet = true;
    s_htpSessionOk = htpParseUuid(HTP_SESSION_ID, s_htpSession);
    s_htpConfigOk = htpConfigReady() && htpQueuesInit();
    check(s_htpConfigOk, "config ok");
    g_server = goodServer;

    // 1. Steady state, 100 ms requests: one window per POST, nothing backs up.
    run(20000);
    uint32_t lo, hi;
    for (int g = 0; g < 3; g++) check(contiguous(g, &lo, &hi), "steady: contiguous seqs");
    check(g_staged[0].size() >= 18, "steady: ~1 window/s");
    check(lo > 1700000000u, "seq is unix seconds");
    bool allSingle = true;
    for (auto &p : g_posts) allSingle &= p.second == 1;
    check(allSingle, "steady: single-window POSTs");
    check(queued() <= 3, "steady: queue near empty");
    printf("1 steady: %zu windows/group, %zu posts, queued %d\n", g_staged[0].size(), g_posts.size(), queued());

    // 2. 20 s outage: backlog drains in 8-window POSTs, nothing lost or dropped.
    g_bearer = false;
    run(20000);
    check(queued() >= 57, "outage: backlog builds");
    g_bearer = true;
    size_t before = g_posts.size();
    run(5000);
    int maxWc = 0;
    for (size_t i = before; i < g_posts.size(); i++) maxWc = std::max(maxWc, g_posts[i].second);
    check(maxWc == 8, "outage: catch-up uses 8-window POSTs");
    for (int g = 0; g < 3; g++) check(contiguous(g, &lo, &hi), "outage: no gap after recovery");
    check(s_htpDroppedOldest == 0, "outage: nothing dropped");
    printf("2 outage 20s: catch-up %zu posts, max %d windows, queued %d, dropped %lu\n",
           g_posts.size() - before, maxWc, queued(), (unsigned long)s_htpDroppedOldest);

    // 3. 50 s outage: queue caps at 32/group and drops the oldest.
    g_bearer = false;
    run(50000);
    check(s_htpQ[0].count == HTP_RETRY_QUEUE_MAX, "long outage: queue capped");
    check(s_htpDroppedOldest > 0, "long outage: oldest dropped");
    g_bearer = true;
    run(8000);
    check(queued() <= 3, "long outage: drained");
    printf("3 outage 50s: dropped %lu windows total, queued %d after recovery\n",
           (unsigned long)s_htpDroppedOldest, queued());

    g_staged.clear(); // the dropped windows above are a real gap
    // 4. Kept-alive socket closed by server: first write fails, retried at once.
    int calls = 0;
    netClient.conn = true;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        return ++calls == 1 ? -2 : goodServer(b, n, r);
    };
    uint32_t failedBefore = s_htpStats.postsFailed;
    run(1100);
    check(calls >= 2 && s_htpStats.postsFailed == failedBefore, "stale socket: retried without failure");
    check(s_htpNextSendMs == 0, "stale socket: no backoff");
    printf("4 stale socket: %d server calls, no backoff\n", calls);

    // 5. 503 then OK: same seqs resent after backoff, none lost.
    size_t staged0 = g_staged[0].size();
    calls = 0;
    std::vector<uint32_t> seqs;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        seqs.push_back(rd32(b + 24));
        if (++calls <= 2) { r = "{\"error\":\"x\"}"; return 503; }
        return goodServer(b, n, r);
    };
    run(6000);
    check(seqs.size() >= 3 && seqs[0] == seqs[1] && seqs[1] == seqs[2], "503: same frame resent");
    for (int g = 0; g < 3; g++) check(contiguous(g, &lo, &hi), "503: no gap");
    check(g_staged[0].size() > staged0, "503: progress after recovery");
    printf("5 503 x2: resent seq %lu, then recovered\n", (unsigned long)seqs[0]);

    // 6. 400: frame dropped, counted, queue keeps moving.
    calls = 0;
    uint32_t perm = s_htpPermanentErrors;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        if (++calls == 1) { r = "{\"error\":\"bad\"}"; return 400; }
        return goodServer(b, n, r);
    };
    run(3000);
    check(s_htpPermanentErrors == perm + 1, "400: one window counted as rejected");
    check(queued() <= 3, "400: queue still drains");
    printf("6 400: rejected %lu\n", (unsigned long)(s_htpPermanentErrors - perm));

    // 7. Slow uplink, 1.2 s per POST (3.6 s per second of data): the emitter
    //    backfills the seconds a POST blocked, POSTs grow to keep up.
    g_server = goodServer;
    g_reqMs = 1200;
    before = g_posts.size();
    g_staged.clear();
    run(60000);
    for (int g = 0; g < 3; g++) check(contiguous(g, &lo, &hi), "slow: no gap from blocked seconds");
    maxWc = 0;
    double avgWc = 0;
    for (size_t i = before; i < g_posts.size(); i++) { maxWc = std::max(maxWc, g_posts[i].second); avgWc += g_posts[i].second; }
    avgWc /= (g_posts.size() - before);
    check(avgWc > 1.5, "slow: POSTs batch windows");
    check(s_htpQ[0].count < HTP_RETRY_QUEUE_MAX, "slow: keeps up without dropping");
    printf("7 slow 1.2 s/POST: %zu posts in 60 s, avg %.1f windows/POST, queued %d/%d/%d\n",
           g_posts.size() - before, avgWc, s_htpQ[0].count, s_htpQ[1].count, s_htpQ[2].count);

    // 8. Stats line format, printed once.
    Serial.quiet = false;
    g_reqMs = 300;
    run(1000);
    Serial.quiet = true;
    s_htpStats = {};
    s_htpStatsStartMs = millis();
    Serial.quiet = false;
    run(10200);

    // 10. Session row missing: 500 FK error is retried, not dropped.
    Serial.quiet = true;
    calls = 0;
    perm = s_htpPermanentErrors;
    g_reqMs = 100;
    g_server = [&](const uint8_t *b, size_t n, std::string &r) {
        if (++calls <= 3) {
            r = "{\"error\":\"insert failed: insert or update on table \\\"staging_chunks\\\" violates "
                "foreign key constraint \\\"staging_chunks_session_id_fkey\\\"\"}";
            return 500;
        }
        return goodServer(b, n, r);
    };
    run(20000);
    check(s_htpPermanentErrors == perm, "missing session: nothing dropped");
    check(queued() <= 3, "missing session: drains once the row exists");
    printf("10 missing session: %d calls, recovered, queued %d\n", calls, queued());

    // 9. ackedCount: partial ack, dup-only, garbage.
    int32_t sm;
    check(ackedCount(String("{\"acked\":[5,6],\"dup\":[7],\"server_recv_ms\":1,\"server_send_ms\":3}"), 5, 3, &sm) == 3 && sm == 2, "ack: acked+dup");
    check(ackedCount(String("{\"acked\":[5],\"dup\":[]}"), 5, 3, &sm) == 1, "ack: partial");
    check(ackedCount(String("{\"acked\":[4,9,4294967295],\"dup\":[]}"), 5, 3, &sm) == 0, "ack: out of range");
    check(ackedCount(String("<html>"), 5, 3, &sm) == -1, "ack: not JSON");

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
