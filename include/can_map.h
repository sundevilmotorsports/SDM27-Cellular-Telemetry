// CAN frames -> latest value per channel set 1 channel.
//
// A signal is decoded the way the car's logger decodes it
// (sundevilmotorsports/logger, src/can.rs), so its SD-card config.json can be
// converted with tools/logger_config_to_can_map.py instead of re-typed:
//   - bytes data[start .. start+len), little-endian unless bigEndian
//   - raw is an integer of that width, sign-extended when isSigned
//   - value = scale * (raw - offset)          (note: offset before scale)
//   - a muxed frame carries a selector byte at muxByte; the signal only
//     applies when that byte equals muxValue
// A frame shorter than start+len does not update the signal.
//
// The CAN task calls canMapApply() for every received frame. The sampler
// reads canMapSnapshot(): a channel that was never received, or not for
// CAN_STALE_MS, reads as NaN, which the encoder sends as a dropout. It never
// reads as 0 or as a frozen last value.

#pragma once

#include "telemetry_format.h"

#include <stddef.h>
#include <stdint.h>

struct CanSignal {
    uint32_t id;
    bool extended;
    int8_t muxByte; // -1 = not muxed
    uint8_t muxValue;
    uint8_t start;
    uint8_t len; // 1..8 bytes
    bool isSigned;
    bool bigEndian;
    float scale;
    float offset;
    const char *channel; // channel set 1 id, e.g. "engine.rpm"; nullptr ends a table
};

struct CanMapStats {
    uint32_t framesSeen;
    uint32_t framesMatched;
    int channelsMapped; // channels with at least one signal in the table
    int channelsFresh;  // of those, received within CAN_STALE_MS
};

// Resolves the table's channel ids. Logs and skips ids that are not in set 1.
void canMapInit();

void canMapApply(const CanFrame &frame);

// out[HTP_CHANNEL_TOTAL], flat channel order (htpFlatIndex).
void canMapSnapshot(float *out, int64_t nowUs);

void canMapGetStats(CanMapStats &st, int64_t nowUs);

// Pure decode of one signal from a frame payload. false when the frame is too
// short or the signal does not apply (mux mismatch). Exposed for host tests.
bool canSignalDecode(const CanSignal &sig, const uint8_t *data, uint8_t len, float *value);
