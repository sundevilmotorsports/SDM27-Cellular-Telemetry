// HTP/1 frame encoder. No Arduino types, so the host byte check can compile it.
// Layout matches tools/htp.mjs encodeFrame(), including Math.round (half toward
// +infinity). C lround differs on negative halfway values and is not used.

#pragma once

#include "htp_channel_set_1.h"

#include <stddef.h>
#include <stdint.h>

#define HTP_MAGIC 0x4854
#define HTP_VERSION 1
#define HTP_HEADER_BYTES 36
#define HTP_I16_NULL (-32768)
#define HTP_MAX_WINDOWS 8

// One window of channel set 1 group 0 is the largest single-window frame
// (36 + 8 + 22*10*2 = 484). The retry queue stores one window per slot.
#define HTP_MAX_FRAME_BYTES 512

// Printed UUID order (hyphens removed), not the mixed-endian Windows layout.
bool htpParseUuid(const char *text, uint8_t out[16]);

size_t htpWindowBytes(const HtpGroup *group);
size_t htpFrameBytes(const HtpGroup *group, uint8_t windowCount);

// samples is window-major, then channel-major, rate_hz floats each.
// A non-finite i16fp sample is the dropout sentinel. Returns encoded length,
// or 0 if the buffer is too small or the arguments are invalid.
size_t htpEncodeFrame(uint8_t *out, size_t cap,
                      const uint8_t sessionId[16],
                      uint16_t channelSetId,
                      const HtpGroup *group,
                      uint32_t firstSeq,
                      uint64_t sendTimestampMs,
                      uint8_t windowCount,
                      const uint64_t *tStartUs,
                      const float *samples);

// Bench signal. Same branch order as tools/htp.mjs synth().
float htpSynth(const char *id, float tSeconds);
