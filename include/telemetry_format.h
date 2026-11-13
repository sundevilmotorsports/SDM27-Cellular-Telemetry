// CAN frame type and the raw CAN batch format posted to telemetry-ingest
// (content type application/x-sdm-can-batch+json, stored as-is in
// telemetry.can_batches and decoded by the dashboard with the logger's
// config.json -- the board never decodes or scales anything):
//   {
//     "device_id": "esp32-sim7670g-poc-01",
//     "seq": 1794533525123,
//     "sent_ms": 1794533525140,
//     "dropped_frames": 0,
//     "frames": [
//       { "can_id": 256, "extd": false, "dlc": 8, "data": "0102030405060708", "ts_ms": 1794533525001 },
//       ...
//     ]
//   }
// `ts_ms` is Unix epoch milliseconds at the frame's own capture instant.
// `sent_ms` is Unix epoch milliseconds when this POST attempt left the board
// (rewritten on every retry; seq stays the same). Stored as sent_at; the
// dashboard compares it with its own clock for latency.
// `data` is the payload as lowercase hex, `dlc` bytes long. `dlc` is the
// payload length in bytes (0-8 classic; 0-8, 12, 16, 20, 24, 32, 48, 64 FD),
// not the 4-bit DLC code from the wire. CAN FD frames add `"fd": true` and
// `"brs": <bool>`; classic frames omit both. `device_id` must equal the signed
// x-htp-device header. At most 4096 frames per batch (server limit).

#pragma once

#include <Arduino.h>

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

// Upper bound on one frame's serialized JSON: a 29-bit ID, 64-byte FD frame
// with a 13-digit ts_ms is ~100 bytes of keys/punctuation/numbers + 2 hex
// chars per byte.
inline size_t telemetryFrameJsonMaxBytes(const CanFrame &f) {
    return 112 + 2 * (size_t)f.dlc;
}

// Writes one frame as a JSON object into out (no trailing comma, no NUL
// needed). Returns the bytes written, or 0 if it does not fit in cap.
size_t telemetryFormatFrame(char *out, size_t cap, const CanFrame &f, int64_t ts_ms);
