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

    // AT+CCLK answers from the modem's RTC, which holds a default date (often
    // 1980 or 2000) until the network supplies real time. Parsing therefore
    // succeeds while the modem is still unregistered, and without this check a
    // bogus epoch gets latched into offset_ms_ and stamped onto every frame --
    // reported as a successful sync. Mirrors syncFromSystemClock()'s guard.
    if (year < 2020) {
        return false;
    }

    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon  = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = minute;
    t.tm_sec  = second;

    // AT+CCLK's time fields are already UTC on this modem, so no timezone shift
    // is applied. The `timezone` field still reports the *local* zone, and
    // applying it (as this code originally did) double-corrects.
    //
    // Measured on SIM7670G-MNGV / T-Mobile US: a batch stamped 2026-09-06
    // 01:28:46Z was published at roughly 2026-09-05 18:30Z -- exactly +7h, the
    // magnitude of the local UTC-7 offset that had been subtracted from an
    // already-UTC value. Reading `timezone` here would reintroduce that.
    //
    // Note this is a modem/carrier behaviour, not a standard: SIMCom parts
    // differ on whether +CCLK reports UTC or local time. If timestamps land a
    // whole number of hours off on other hardware, this is the line to revisit.
    (void)timezone;
    time_t epoch_s = timegmPortable(&t);

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
