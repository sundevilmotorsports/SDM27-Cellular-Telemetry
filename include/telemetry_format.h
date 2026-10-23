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
// `data` is the raw CAN payload as lowercase hex, `dlc` bytes long. `dlc` is
// the payload length in bytes (0-8 classic, up to 64 for CAN FD), not the
// 4-bit DLC code from the wire.
// CAN FD frames add `"fd": true` and `"brs": <bool>` (bit rate switch);
// classic frames omit both, so they cost no extra bytes on the uplink:
//       { "can_id": 291, "extd": false, "dlc": 12, "data": "...", "ts_ms": ..., "fd": true, "brs": true }
//
// JSON is used for PoC clarity. For production, a binary format (CBOR/protobuf)
// would meaningfully cut per-frame overhead on a Cat-1 link -- see README's
// Known Limitations.

#pragma once

#include <Arduino.h>
#include <vector>

#define CAN_MAX_DATA_LEN 64 // CAN FD maximum; classic frames use at most 8

struct CanFrame {
    uint32_t id;          // 11-bit or 29-bit CAN identifier
    bool extended;         // true = 29-bit extended ID
    bool fd;               // true = CAN FD frame (FDF set)
    bool brs;              // CAN FD bit rate switch; always false for classic frames
    uint8_t dlc;           // payload length in bytes: 0-8 classic, 0-64 FD
    uint8_t data[CAN_MAX_DATA_LEN];
    int64_t capture_us;    // esp_timer_get_time() at the instant the frame was received
};

// Upper bound on one frame's serialized JSON, used to keep each batch under
// BATCH_MAX_JSON_BYTES. Worst case is a 29-bit ID, 64-byte FD frame with a
// 13-digit ts_ms: ~100 bytes of keys/punctuation/numbers + 2 hex chars/byte.
inline size_t telemetryFrameJsonMaxBytes(const CanFrame &f) {
    return 112 + 2 * (size_t)f.dlc;
}

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
