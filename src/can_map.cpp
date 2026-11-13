#include "can_map.h"
#include "can_map_table.h"
#include "htp_frame.h"
#include "telemetry_config.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>

// Resolved once by canMapInit(); -1 = channel not in set 1 (signal ignored).
static int8_t s_flat[sizeof(kCanSignals) / sizeof(kCanSignals[0])];
static size_t s_signalCount = 0;

static float s_value[HTP_CHANNEL_TOTAL];
static int64_t s_seenUs[HTP_CHANNEL_TOTAL];
static bool s_mapped[HTP_CHANNEL_TOTAL];
static uint32_t s_framesSeen = 0;
static uint32_t s_framesMatched = 0;
// Written by the CAN task (core 0), read by the sampler.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

bool canSignalDecode(const CanSignal &sig, const uint8_t *data, uint8_t len, float *value) {
    if (sig.len == 0 || sig.len > 8 || sig.start + sig.len > len) return false;
    if (sig.muxByte >= 0 && (sig.muxByte >= len || data[sig.muxByte] != sig.muxValue)) return false;
    uint64_t raw = 0;
    for (uint8_t i = 0; i < sig.len; i++) {
        uint8_t b = sig.bigEndian ? data[sig.start + i] : data[sig.start + sig.len - 1 - i];
        raw = (raw << 8) | b;
    }
    int64_t n = (int64_t)raw;
    // Sign-extend from the signal's own width. (The logger only does this for
    // 1, 2 and 4 byte signals; for 3, 5, 6, 7 bytes its values are unsigned.)
    if (sig.isSigned && sig.len < 8 && (raw >> (8 * sig.len - 1)) & 1) {
        n = (int64_t)(raw | (~0ULL << (8 * sig.len)));
    }
    *value = sig.scale * ((float)n - sig.offset);
    return true;
}

void canMapInit() {
    s_signalCount = 0;
    memset(s_mapped, 0, sizeof(s_mapped));
    for (int i = 0; i < HTP_CHANNEL_TOTAL; i++) s_value[i] = NAN;
    for (size_t i = 0; kCanSignals[i].channel != nullptr; i++) {
        int flat = htpFlatIndex(kCanSignals[i].channel);
        s_flat[i] = (int8_t)flat;
        if (flat < 0) {
            Serial.printf("[can-map] signal for unknown channel '%s' (id 0x%lX) ignored\n",
                          kCanSignals[i].channel, (unsigned long)kCanSignals[i].id);
        } else {
            s_mapped[flat] = true;
        }
        s_signalCount = i + 1;
    }
    int mapped = 0;
    for (bool m : s_mapped) mapped += m;
    Serial.printf("[can-map] %s table: %u signals, %d of %d channels mapped\n", CAN_MAP_NAME,
                  (unsigned)s_signalCount, mapped, HTP_CHANNEL_TOTAL);
}

void canMapApply(const CanFrame &f) {
    bool matched = false;
    for (size_t i = 0; i < s_signalCount; i++) {
        const CanSignal &sig = kCanSignals[i];
        if (s_flat[i] < 0 || sig.id != f.id || sig.extended != f.extended) continue;
        float v;
        if (!canSignalDecode(sig, f.data, f.dlc, &v)) continue;
        matched = true;
        portENTER_CRITICAL(&s_lock);
        s_value[s_flat[i]] = v;
        s_seenUs[s_flat[i]] = f.capture_us;
        portEXIT_CRITICAL(&s_lock);
    }
    s_framesSeen++;
    if (matched) s_framesMatched++;
}

void canMapSnapshot(float *out, int64_t nowUs) {
    const int64_t staleUs = (int64_t)CAN_STALE_MS * 1000;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < HTP_CHANNEL_TOTAL; i++) {
        bool fresh = !std::isnan(s_value[i]) && nowUs - s_seenUs[i] <= staleUs;
        out[i] = fresh ? s_value[i] : NAN;
    }
    portEXIT_CRITICAL(&s_lock);
}

void canMapGetStats(CanMapStats &st, int64_t nowUs) {
    float v[HTP_CHANNEL_TOTAL];
    canMapSnapshot(v, nowUs);
    st.framesSeen = s_framesSeen;
    st.framesMatched = s_framesMatched;
    st.channelsMapped = 0;
    st.channelsFresh = 0;
    for (int i = 0; i < HTP_CHANNEL_TOTAL; i++) {
        st.channelsMapped += s_mapped[i];
        st.channelsFresh += s_mapped[i] && !std::isnan(v[i]);
    }
}
