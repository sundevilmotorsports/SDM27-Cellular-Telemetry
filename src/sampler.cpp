#include "sampler.h"
#include "can_map.h"
#include "htp_frame.h"
#include "net_task.h"
#include "telemetry_config.h"
#include "time_sync.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <cmath>

#define SAMPLER_TICK_MS 100
#define SAMPLER_TICKS_PER_S (1000 / SAMPLER_TICK_MS)

// Window being filled for each group, channel-major as htpEncodeFrame wants.
static float s_win[HTP_GROUP_COUNT][22 * 10];
static int64_t s_winSec = -1;
static int64_t s_firstSec = -1;
static uint32_t s_windows = 0;
static uint32_t s_lateTicks = 0;

static void fillSnapshot(float *v, int64_t tickMs, int64_t nowUs) {
#if HTP_SYNTH_MODE
    (void)nowUs;
    float t = (float)(tickMs / 1000 - s_firstSec) + (float)(tickMs % 1000) / 1000.0f;
    int i = 0;
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        for (uint8_t c = 0; c < kHtpGroups[g].channel_count; c++) {
            v[i++] = htpSynth(kHtpGroups[g].channels[c].id, t);
        }
    }
#else
    (void)tickMs;
    canMapSnapshot(v, nowUs);
#endif
}

static void startWindow(int64_t sec) {
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        size_t n = (size_t)kHtpGroups[g].channel_count * kHtpGroups[g].rate_hz;
        for (size_t i = 0; i < n; i++) s_win[g][i] = NAN; // a missed tick stays a dropout
    }
    s_winSec = sec;
}

static void finishWindow() {
    if (s_winSec < 0) return;
    uint64_t tStartUs = (uint64_t)s_winSec * 1000000ULL;
    for (int g = 0; g < HTP_GROUP_COUNT; g++) netTaskPushWindow(g, tStartUs, s_win[g]);
    s_windows++;
    s_winSec = -1;
}

static void takeSample(int64_t tickMs, int64_t nowUs) {
    int64_t sec = tickMs / 1000;
    int k = (int)(tickMs % 1000) / SAMPLER_TICK_MS;
    if (sec != s_winSec) {
        finishWindow(); // only if the previous second's last tick was missed
        startWindow(sec);
    }
    float v[HTP_CHANNEL_TOTAL];
    fillSnapshot(v, tickMs, nowUs);
    int flat = 0;
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        const HtpGroup &grp = kHtpGroups[g];
        int step = SAMPLER_TICKS_PER_S / grp.rate_hz;
        if (k % step == 0) {
            int i = k / step;
            for (uint8_t c = 0; c < grp.channel_count; c++) {
                s_win[g][(size_t)c * grp.rate_hz + i] = v[flat + c];
            }
        }
        flat += grp.channel_count;
    }
    if (k == SAMPLER_TICKS_PER_S - 1) finishWindow();
}

static void logCanMap() {
#if !HTP_SYNTH_MODE
    CanMapStats st;
    canMapGetStats(st, esp_timer_get_time());
    float v[HTP_CHANNEL_TOTAL];
    canMapSnapshot(v, esp_timer_get_time());
    Serial.printf("[can-map] frames %lu matched %lu, channels fresh %d/%d mapped",
                  (unsigned long)st.framesSeen, (unsigned long)st.framesMatched,
                  st.channelsFresh, st.channelsMapped);
    int shown = 0;
    for (int g = 0; g < HTP_GROUP_COUNT && shown < 8; g++) {
        for (uint8_t c = 0; c < kHtpGroups[g].channel_count && shown < 8; c++) {
            int i = htpGroupFlatOffset(g) + c;
            if (std::isnan(v[i])) continue;
            Serial.printf("%s %s=%.3g", shown ? "," : ":", kHtpGroups[g].channels[c].id, v[i]);
            shown++;
        }
    }
    Serial.println();
#endif
}

static void samplerTask(void *) {
    static_assert(SAMPLER_TICKS_PER_S == 10, "group rates are checked against 10 ticks/s");
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        if (kHtpGroups[g].rate_hz == 0 || SAMPLER_TICKS_PER_S % kHtpGroups[g].rate_hz) {
            Serial.printf("[sampler] group %u rate %u Hz does not divide %d ticks/s; not sampling\n",
                          kHtpGroups[g].key, kHtpGroups[g].rate_hz, SAMPLER_TICKS_PER_S);
            vTaskDelete(nullptr);
        }
    }
    uint32_t lastLogMs = millis();
    int64_t lastTickMs = -1;
    for (;;) {
        // The CAN map is readable before the clock syncs; windows are not.
        if (millis() - lastLogMs >= 5000) {
            lastLogMs = millis();
            logCanMap();
            if (g_timeSync.isSynced()) {
                Serial.printf("[sampler] windows %lu, late ticks %lu\n", (unsigned long)s_windows,
                              (unsigned long)s_lateTicks);
            }
        }
        if (!g_timeSync.isSynced()) {
            vTaskDelay(pdMS_TO_TICKS(SAMPLER_TICK_MS));
            continue;
        }
        int64_t nowUs = esp_timer_get_time();
        int64_t nowMs = g_timeSync.epochMsFor(nowUs);
        int64_t tickMs = nowMs - nowMs % SAMPLER_TICK_MS;
        if (s_firstSec < 0) s_firstSec = tickMs / 1000;
        if (tickMs != lastTickMs) {
            // More than one tick late: the skipped samples stay NaN.
            if (lastTickMs >= 0 && tickMs - lastTickMs > SAMPLER_TICK_MS) s_lateTicks++;
            lastTickMs = tickMs;
            takeSample(tickMs, nowUs);
        }
        int64_t waitMs = tickMs + SAMPLER_TICK_MS - g_timeSync.epochMsFor(esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(waitMs > 0 ? waitMs : 1));
    }
}

void samplerStart() {
    // Above the net task (it blocks in TLS for seconds), below the CAN task.
    xTaskCreatePinnedToCore(samplerTask, "sampler", 6144, nullptr, tskIDLE_PRIORITY + 4, nullptr, 0);
}
