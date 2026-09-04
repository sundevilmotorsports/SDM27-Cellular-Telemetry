// Wall-clock (epoch) time sync, decoupled from CAN frame capture timing.
//
// The CAN task captures esp_timer_get_time() -- a monotonic microsecond
// counter -- at the instant each frame leaves the TWAI driver. That value
// never touches the network. Separately, this module tracks the offset
// between that monotonic clock and true epoch time, synced from the modem's
// network time (AT+CCLK via TinyGSM's getNetworkTime()) at boot and every
// TIME_SYNC_INTERVAL_MS thereafter. epochMsFor() combines the two only at
// serialization time, so queueing/network jitter never pollutes inter-frame
// timing, and a resync only shifts the offset (a step), never the frame's
// original capture instant.

#pragma once

#include <Arduino.h>
#include "gsm_modem.h"

class TimeSync {
public:
    // Attempts one network time sync. Returns true on success. Safe to call
    // repeatedly; updates the offset in place using an atomic-ish assignment
    // (int64_t writes are not torn on the ESP32-S3's 32-bit bus in a way that
    // matters here: readers may see a slightly stale offset for one frame,
    // never a partial one, since this is a single aligned 8-byte write).
    bool sync(TinyGsm &modem);

    // WiFi-mode equivalent of sync() above: reads the ESP32's SNTP-disciplined
    // system clock (set up via configTime() once WiFi is connected) instead of
    // AT+CCLK. Same offset math and same guarantees.
    bool syncFromSystemClock();

    bool isSynced() const { return synced_; }

    // Converts a monotonic esp_timer_get_time() capture into epoch milliseconds.
    int64_t epochMsFor(int64_t capture_us) const;

private:
    volatile int64_t offset_ms_ = 0; // epoch_ms = capture_us/1000 + offset_ms_
    volatile bool synced_ = false;
};

// Global instance + a free-function adapter matching telemetryFormatBatch's
// epoch_ms_for signature (which can't carry a `this` pointer).
extern TimeSync g_timeSync;
int64_t timeSyncEpochMsFor(int64_t capture_us);
