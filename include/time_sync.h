// Wall-clock (epoch) time sync, decoupled from CAN frame capture timing.
//
// The CAN task captures esp_timer_get_time() -- a monotonic microsecond
// counter -- at the instant each frame leaves the TWAI driver. That value
// never touches the network. Separately, this module tracks the offset
// between that monotonic clock and true epoch time, read from the SNTP-
// disciplined system clock at boot and every TIME_SYNC_INTERVAL_MS
// thereafter. epochMsFor() combines the two only at
// serialization time, so queueing/network jitter never pollutes inter-frame
// timing, and a resync only shifts the offset (a step), never the frame's
// original capture instant.

#pragma once

#include <Arduino.h>

class TimeSync {
public:
    // Reads the ESP32's SNTP-disciplined system clock (configTime() is called
    // once WiFi or PPP is up). Returns false until SNTP has synced. Safe to
    // call repeatedly: offset_ms_ is a single aligned 8-byte write, so readers
    // may see a slightly stale offset for one frame, never a torn one.
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
