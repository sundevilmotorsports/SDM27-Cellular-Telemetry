#include "time_sync.h"
#include <esp_timer.h>
#include <time.h>

TimeSync g_timeSync;

int64_t timeSyncEpochMsFor(int64_t capture_us) {
    return g_timeSync.epochMsFor(capture_us);
}

// The ESP32 Arduino toolchain's libc doesn't provide timegm(), so UTC calendar
// -> epoch-seconds is computed directly (Howard Hinnant's days_from_civil).
static int64_t daysFromCivilUtc(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = (int64_t)yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static time_t timegmPortable(const struct tm *t) {
    int64_t days = daysFromCivilUtc(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    return (time_t)(days * 86400LL + t->tm_hour * 3600LL + t->tm_min * 60LL + t->tm_sec);
}

bool TimeSync::sync(TinyGsm &modem) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    float timezone = 0;

    // AT+CCLK via TinyGSM's shared getNetworkTime() (TinyGsmTime mixin, inherited
    // by TinyGsmClientSIM7672.h, which TINY_GSM_MODEM_SIM7670G maps to).
    if (!modem.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &timezone)) {
        return false;
    }

    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;

    // Treats the struct as UTC; `timezone` (hours east of UTC, from AT+CCLK's
    // quarter-hour field) shifts local wall-clock -> true UTC.
    time_t local_as_utc = timegmPortable(&t);
    time_t epoch_s = local_as_utc - (time_t)(timezone * 3600.0f);

    int64_t new_offset_ms = ((int64_t)epoch_s * 1000LL) - (esp_timer_get_time() / 1000);

    offset_ms_ = new_offset_ms;
    synced_ = true;
    return true;
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
