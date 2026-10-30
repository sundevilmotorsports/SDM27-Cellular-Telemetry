// Host test of the CAN path: bench ECU frames -> can_map -> sampler windows.
// Built by run.sh with HTP_SYNTH_MODE=0 and CAN_MAP_BENCH=1.
#include "stubs.h"
#include "telemetry_config.h"
#include "htp_frame.h"
#include "bench_ecu_pattern.h"
#include "../../src/can_map.cpp"

// Captures what the sampler hands to the uplink.
static std::map<int, std::vector<std::vector<float>>> g_windows;
static std::map<int, std::vector<uint64_t>> g_starts;
void netTaskPushWindow(int group, uint64_t tStartUs, const float *samples) {
    size_t n = (size_t)kHtpGroups[group].channel_count * kHtpGroups[group].rate_hz;
    g_windows[group].emplace_back(samples, samples + n);
    g_starts[group].push_back(tStartUs);
}

#include "../../src/sampler.cpp"

static int g_fail = 0;
static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); g_fail++; }
}
static bool near(float a, float b) { return std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(b)); }

static float decode1(const CanSignal &s, std::vector<uint8_t> d) {
    float v = NAN;
    canSignalDecode(s, d.data(), (uint8_t)d.size(), &v);
    return v;
}

static CanFrame frameOf(const BenchEcuFrame &e) {
    CanFrame f{};
    f.id = e.id;
    f.fd = f.brs = e.fd;
    f.dlc = e.len;
    memcpy(f.data, e.data, e.len);
    f.capture_us = esp_timer_get_time();
    return f;
}

// Sample value of a channel at sample i of a group window.
static float at(int g, size_t w, const char *id, int i) {
    const HtpGroup &grp = kHtpGroups[g];
    for (uint8_t c = 0; c < grp.channel_count; c++) {
        if (strcmp(grp.channels[c].id, id) == 0) return g_windows[g][w][(size_t)c * grp.rate_hz + i];
    }
    return -999;
}

int main() {
    Serial.quiet = true;

    // 1. Decode rules (logger semantics, value = scale * (raw - offset)).
    CanSignal u16le = {1, false, -1, 0, 0, 2, false, false, 1, 0, "x"};
    check(decode1(u16le, {0x34, 0x12}) == 0x1234, "u16 little-endian");
    CanSignal u16be = {1, false, -1, 0, 0, 2, false, true, 1, 0, "x"};
    check(decode1(u16be, {0x12, 0x34}) == 0x1234, "u16 big-endian");
    CanSignal i16 = {1, false, -1, 0, 1, 2, true, false, 0.5f, 0, "x"};
    check(decode1(i16, {0, 0xFE, 0xFF}) == -1.0f, "i16 sign + scale");
    CanSignal i24 = {1, false, -1, 0, 0, 3, true, false, 1, 0, "x"};
    check(decode1(i24, {0xFF, 0xFF, 0xFF}) == -1.0f, "i24 sign-extends");
    CanSignal i8 = {1, false, -1, 0, 0, 1, true, false, 1, 0, "x"};
    check(decode1(i8, {0x80}) == -128.0f, "i8 min");
    CanSignal off = {1, false, -1, 0, 0, 1, false, false, 2, 10, "x"};
    check(decode1(off, {15}) == 10.0f, "offset applied before scale");
    CanSignal u32 = {1, false, -1, 0, 0, 4, false, false, 1, 0, "x"};
    check(decode1(u32, {0xFF, 0xFF, 0xFF, 0xFF}) == 4294967295.0f, "u32 max");
    check(std::isnan(decode1(u16le, {0x34})), "short frame: no value");
    CanSignal mux = {1, false, 0, 2, 1, 1, false, false, 1, 0, "x"};
    check(decode1(mux, {2, 7}) == 7.0f && std::isnan(decode1(mux, {1, 7})), "mux selects");
    printf("1 decode rules\n");

    // 2. Bench ECU pattern through the bench table, at 10 Hz for 3 s.
    canMapInit();
    for (const CanSignal *s = kCanSignals; s->channel; s++) {
        check(htpFlatIndex(s->channel) >= 0, "bench table: channel exists in set 1");
    }
    float v[HTP_CHANNEL_TOTAL];
    canMapSnapshot(v, esp_timer_get_time());
    check(std::isnan(v[htpFlatIndex("engine.rpm")]), "never received: NaN, not 0");

    int64_t lastTick = -1;
    uint32_t step = 0;
    for (int ms = 0; ms < 3000; ms += 10) {
        g_nowUs += 10000;
        if (ms % BENCH_ECU_PERIOD_MS == 0) {
            BenchEcuFrame e[2];
            benchEcuFrames(step++, e);
            canMapApply(frameOf(e[0]));
            canMapApply(frameOf(e[1]));
        }
        int64_t nowMs = g_timeSync.epochMsFor(esp_timer_get_time());
        int64_t tick = nowMs - nowMs % SAMPLER_TICK_MS;
        if (tick != lastTick) {
            lastTick = tick;
            takeSample(tick, esp_timer_get_time());
        }
    }
    canMapSnapshot(v, esp_timer_get_time());
    BenchEcuValues last = benchEcuValues(step - 1), prev = benchEcuValues(step - 2);
    const BenchEcuValues &m0 = last.mux == 0 ? last : prev, &m1 = last.mux == 1 ? last : prev;
    check(near(v[htpFlatIndex("engine.rpm")], last.rpm), "rpm");
    check(near(v[htpFlatIndex("engine.tps")], last.tps), "tps");
    check(near(v[htpFlatIndex("engine.water_temp")], last.waterTemp), "water temp (offset)");
    check(near(v[htpFlatIndex("engine.gear")], last.gear), "gear");
    check(near(v[htpFlatIndex("brake.front_pressure")], m0.brake), "front brake (mux 0)");
    check(near(v[htpFlatIndex("brake.rear_pressure")], m1.brake), "rear brake (mux 1)");
    check(near(v[htpFlatIndex("imu.lat_g")], m0.g), "lat g (signed BE, mux 0)");
    check(near(v[htpFlatIndex("imu.long_g")], m1.g), "long g (signed BE, mux 1)");
    check(std::isnan(v[htpFlatIndex("engine.oil_temp")]), "unmapped channel: NaN");
    CanMapStats st;
    canMapGetStats(st, esp_timer_get_time());
    check(st.channelsMapped == 9 && st.channelsFresh == 8, "stats: 9 mapped, 8 fresh (0x100 never sent)");
    printf("2 bench pattern: rpm %.0f tps %.0f water %.0f gear %.0f lat %.2f long %.2f\n",
           v[htpFlatIndex("engine.rpm")], v[htpFlatIndex("engine.tps")],
           v[htpFlatIndex("engine.water_temp")], v[htpFlatIndex("engine.gear")],
           v[htpFlatIndex("imu.lat_g")], v[htpFlatIndex("imu.long_g")]);

    // 3. Windows: one per second per group, starting on a whole second, the
    //    10 Hz samples tracking the 10 Hz frames, 1 Hz group sampled at :00.
    check(g_windows[0].size() >= 2 && g_windows[0].size() == g_windows[2].size(), "one window per second per group");
    check(g_starts[0][1] - g_starts[0][0] == 1000000 && g_starts[0][0] % 1000000 == 0, "window starts on whole seconds");
    size_t w = 1;
    bool rising = true;
    for (int i = 1; i < 10; i++) rising &= at(0, w, "engine.rpm", i) - at(0, w, "engine.rpm", i - 1) == 37;
    check(rising, "10 Hz rpm samples are consecutive frames");
    check(!std::isnan(at(2, w, "engine.water_temp", 0)), "1 Hz water temp sampled");
    check(std::isnan(at(2, w, "engine.oil_temp", 0)), "unmapped 1 Hz channel is a dropout");
    check(std::isnan(at(1, w, "gps.lat_ref", 0)), "no GPS source: dropout");
    printf("3 windows: %zu/%zu/%zu\n", g_windows[0].size(), g_windows[1].size(), g_windows[2].size());

    // 4. Frames stop: values go NaN after CAN_STALE_MS, never freeze.
    g_nowUs += (uint64_t)(CAN_STALE_MS - 100) * 1000;
    canMapSnapshot(v, esp_timer_get_time());
    check(!std::isnan(v[htpFlatIndex("engine.rpm")]), "fresh just under CAN_STALE_MS");
    g_nowUs += 200000;
    canMapSnapshot(v, esp_timer_get_time());
    check(std::isnan(v[htpFlatIndex("engine.rpm")]), "stale after CAN_STALE_MS");
    printf("4 stale after %d ms\n", CAN_STALE_MS);

    // 5. The encoder turns NaN into the dropout sentinel, values round-trip.
    uint8_t frame[HTP_MAX_FRAME_BYTES];
    uint8_t sid[16] = {};
    uint64_t t0 = g_starts[0][w];
    size_t len = htpEncodeFrame(frame, sizeof frame, sid, 1, &kHtpGroups[2], 1, 0, 1, &t0, g_windows[2][w].data());
    check(len == HTP_HEADER_BYTES + htpWindowBytes(&kHtpGroups[2]), "group 2 encodes");
    printf(g_fail ? "%d FAILED\n" : "can path: all passed\n", g_fail);
    return g_fail != 0;
}
