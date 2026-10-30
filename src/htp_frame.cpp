#include "htp_frame.h"

#include <cmath>
#include <cstring>

static int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool htpParseUuid(const char *text, uint8_t out[16]) {
    if (!text || !out) return false;
    // 8-4-4-4-12
    static const int hyphenAt[] = {8, 13, 18, 23};
    int hi = 0;
    for (int i = 0; i < 36; i++) {
        char c = text[i];
        if (c == '\0') return false;
        bool expectHyphen = false;
        for (int h : hyphenAt) {
            if (i == h) expectHyphen = true;
        }
        if (expectHyphen) {
            if (c != '-') return false;
            continue;
        }
        int n = hexNibble(c);
        if (n < 0) return false;
        if ((hi & 1) == 0) {
            out[hi / 2] = (uint8_t)(n << 4);
        } else {
            out[hi / 2] |= (uint8_t)n;
        }
        hi++;
    }
    return text[36] == '\0' && hi == 32;
}

static void writeU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

static void writeU32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)(v >> 24);
}

static void writeU64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
    }
}

static uint8_t sampleWidth(const HtpChannel *ch) {
    return ch->enc == HTP_ENC_F32 ? 4 : 2;
}

size_t htpWindowBytes(const HtpGroup *group) {
    size_t n = 8;
    for (uint8_t i = 0; i < group->channel_count; i++) {
        n += (size_t)group->rate_hz * sampleWidth(&group->channels[i]);
    }
    return n;
}

size_t htpFrameBytes(const HtpGroup *group, uint8_t windowCount) {
    return HTP_HEADER_BYTES + (size_t)windowCount * htpWindowBytes(group);
}

// tools/htp.mjs uses Math.round, which is floor(x + 0.5): half toward +infinity.
// A finite value that rounds to -32768 is left as the dropout sentinel, matching
// the JS check `if (raw !== I16_NULL) clamp`.
static int16_t encodeI16(float value, float scale, float offset) {
    if (!std::isfinite(value) || !(scale > 0.0f) || !std::isfinite(scale) || !std::isfinite(offset)) {
        return (int16_t)HTP_I16_NULL;
    }
    // Divide in double. float32 division does not match Math.round of the same
    // float32 operands, which is what tools/htp.mjs does.
    double scaled = (static_cast<double>(value) - static_cast<double>(offset)) / static_cast<double>(scale);
    if (!std::isfinite(scaled)) return (int16_t)HTP_I16_NULL;
    if (scaled >= 32767.5) return 32767;
    if (scaled < -32768.5) return -32767;
    int32_t raw = static_cast<int32_t>(std::floor(scaled + 0.5));
    if (raw == HTP_I16_NULL) return (int16_t)HTP_I16_NULL;
    if (raw < -32767) return -32767;
    if (raw > 32767) return 32767;
    return (int16_t)raw;
}

void htpWriteHeader(uint8_t *out,
                    const uint8_t sessionId[16],
                    uint16_t channelSetId,
                    uint8_t groupKey,
                    uint8_t windowCount,
                    uint32_t firstSeq,
                    uint64_t sendTimestampMs) {
    writeU16(out + 0, HTP_MAGIC);
    out[2] = HTP_VERSION;
    out[3] = 0;
    memcpy(out + 4, sessionId, 16);
    writeU16(out + 20, channelSetId);
    out[22] = groupKey;
    out[23] = windowCount;
    writeU32(out + 24, firstSeq);
    writeU64(out + 28, sendTimestampMs);
}

size_t htpEncodeFrame(uint8_t *out, size_t cap,
                      const uint8_t sessionId[16],
                      uint16_t channelSetId,
                      const HtpGroup *group,
                      uint32_t firstSeq,
                      uint64_t sendTimestampMs,
                      uint8_t windowCount,
                      const uint64_t *tStartUs,
                      const float *samples) {
    if (!out || !sessionId || !group || !tStartUs || !samples) return 0;
    if (windowCount < 1 || windowCount > HTP_MAX_WINDOWS) return 0;
    if (group->channel_count == 0 || group->rate_hz == 0) return 0;

    size_t need = htpFrameBytes(group, windowCount);
    if (need > cap) return 0;

    htpWriteHeader(out, sessionId, channelSetId, group->key, windowCount, firstSeq, sendTimestampMs);

    size_t off = HTP_HEADER_BYTES;
    size_t sampleBase = 0;
    const size_t perWindowSamples = (size_t)group->channel_count * group->rate_hz;
    for (uint8_t w = 0; w < windowCount; w++) {
        writeU64(out + off, tStartUs[w]);
        off += 8;
        for (uint8_t c = 0; c < group->channel_count; c++) {
            const HtpChannel *ch = &group->channels[c];
            for (uint16_t i = 0; i < group->rate_hz; i++) {
                float v = samples[sampleBase++];
                if (ch->enc == HTP_ENC_F32) {
                    float f = v;
                    memcpy(out + off, &f, 4);
                    off += 4;
                } else {
                    int16_t raw = encodeI16(v, ch->scale, ch->offset);
                    writeU16(out + off, (uint16_t)raw);
                    off += 2;
                }
            }
        }
    }
    if (sampleBase != (size_t)windowCount * perWindowSamples) return 0;
    return off == need ? need : 0;
}

int htpGroupFlatOffset(int group) {
    int off = 0;
    for (int g = 0; g < group && g < HTP_GROUP_COUNT; g++) off += kHtpGroups[g].channel_count;
    return off;
}

int htpFlatIndex(const char *id) {
    if (!id) return -1;
    int i = 0;
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        for (uint8_t c = 0; c < kHtpGroups[g].channel_count; c++, i++) {
            if (std::strcmp(kHtpGroups[g].channels[c].id, id) == 0) return i;
        }
    }
    return -1;
}

static bool idHas(const char *id, const char *part) {
    return id && part && std::strstr(id, part) != nullptr;
}

float htpSynth(const char *id, float t) {
    if (!id) return 0.0f;
    if (idHas(id, "rpm")) return 3000.0f + 9000.0f * (std::fmod(t, 6.0f) / 6.0f);
    if (std::strcmp(id, "gps.lat_ref") == 0) return 33.42f;
    if (std::strcmp(id, "gps.lon_ref") == 0) return -111.93f;
    if (std::strcmp(id, "gps.lat_d") == 0) {
        return 0.002f * std::cos((2.0f * 3.14159265f * std::fmod(t, 90.0f)) / 90.0f);
    }
    if (std::strcmp(id, "gps.lon_d") == 0) {
        return 0.002f * std::sin((2.0f * 3.14159265f * std::fmod(t, 90.0f)) / 90.0f);
    }
    if (idHas(id, "fix_quality")) return 4.0f;
    if (idHas(id, "speed")) return 20.0f + 15.0f * std::sin((2.0f * 3.14159265f * t) / 30.0f);
    if (idHas(id, "temp")) return 80.0f + 15.0f * (1.0f - std::exp(-t / 120.0f)) + 0.5f * std::sin(t);
    if (idHas(id, "pressure")) return 300.0f + 150.0f * (std::fmod(t, 6.0f) / 6.0f);
    if (idHas(id, "voltage")) return 13.8f + 0.1f * std::sin(t / 5.0f);
    if (idHas(id, "gear")) return 1.0f + std::floor(std::fmod(t, 6.0f));
    if (idHas(id, "lambda")) return 0.9f + 0.05f * std::sin(t);
    if (idHas(id, "_g")) return 1.2f * std::sin(t / 3.0f);
    return 10.0f + std::sin(t + (float)std::strlen(id));
}
