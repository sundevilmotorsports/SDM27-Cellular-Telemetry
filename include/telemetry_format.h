// Shared data types and MQTT payload format for the telemetry pipeline.
//
// Payload format (documented for the receiving/broker side too):
//   {
//     "device_id": "esp32-sim7670g-poc-01",
//     "seq": 42,
//     "dropped_frames": 0,
//     "frames": [
//       { "can_id": 256, "extd": false, "dlc": 8, "data": "0102030405060708", "ts_ms": 1735500000123 },
//       ...
//     ]
//   }
// `ts_ms` is Unix epoch milliseconds, computed per-frame at the frame's own
// capture instant -- batching never averages or collapses timestamps.
// `data` is the raw 8-byte CAN payload as lowercase hex, `dlc` bytes long.
//
// JSON is used for PoC clarity. For production, a binary format (CBOR/protobuf)
// would meaningfully cut per-frame overhead on a Cat-1 link -- see README's
// Known Limitations.

#pragma once

#include <Arduino.h>
#include <vector>

struct CanFrame {
    uint32_t id;          // 11-bit or 29-bit CAN identifier
    bool extended;         // true = 29-bit extended ID
    uint8_t dlc;           // 0-8
    uint8_t data[8];
    int64_t capture_us;    // esp_timer_get_time() at the instant the frame left the driver
};

struct TelemetryBatch {
    uint32_t seq = 0;
    uint32_t dropped_frames = 0;
    std::vector<CanFrame> frames;
};

// Serializes a batch to the documented JSON format. `epoch_ms_for` converts a
// frame's monotonic capture_us into wall-clock epoch milliseconds (see time_sync.h) --
// passed in rather than computed here so this module has no clock dependency.
String telemetryFormatBatch(const TelemetryBatch &batch, const char *device_id,
                             int64_t (*epoch_ms_for)(int64_t capture_us));
