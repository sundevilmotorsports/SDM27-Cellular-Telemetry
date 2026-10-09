#include "time_sync.h"
#include <esp_timer.h>
#include <time.h>

TimeSync g_timeSync;

int64_t timeSyncEpochMsFor(int64_t capture_us) {
    return g_timeSync.epochMsFor(capture_us);
}

bool TimeSync::syncFromSystemClock() {
    time_t now = time(nullptr);
    // Before SNTP's first successful sync, time() reads back ~1970 (or a small
    // offset from it). Treat anything before year 2020 as "not synced yet"
    // rather than latching a bogus offset.
    if (now < 1577836800) { // 2020-01-01T00:00:00Z
        return false;
    }

    int64_t new_offset_ms = ((int64_t)now * 1000LL) - (esp_timer_get_time() / 1000);
    offset_ms_ = new_offset_ms;
    synced_ = true;
    return true;
}

int64_t TimeSync::epochMsFor(int64_t capture_us) const {
    return (capture_us / 1000) + offset_ms_;
}
