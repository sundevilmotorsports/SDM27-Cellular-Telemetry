// Host stand-in for htpEmitWindow() + htpBuildPost() in src/net_task.cpp.
// Same encoder, same synth, same channel set. Not flashed, not built by PlatformIO.
//
//   htp_emit_sample <session-uuid> <channel-set-id> <unix-ms> <out-dir> [windows]
//
// Writes one frame per group holding `windows` (1..8, default 1) consecutive
// windows that end at the last completed second. seq is the window's unix
// second, as on the board. The frame is built the way the firmware builds a
// POST (single-window frames joined under a new header) and checked against a
// direct multi-window encode.

#include "htp_frame.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: htp_emit_sample <session-uuid> <channel-set-id> <unix-ms> <out-dir> [windows]\n");
        return 2;
    }

    uint8_t session[16];
    if (!htpParseUuid(argv[1], session)) {
        fprintf(stderr, "session id is not a UUID\n");
        return 1;
    }
    unsigned channelSetId = 0;
    if (sscanf(argv[2], "%u", &channelSetId) != 1 || channelSetId != 1) {
        fprintf(stderr, "channel set must be 1\n");
        return 1;
    }
    unsigned long long sendMs = 0;
    if (sscanf(argv[3], "%llu", &sendMs) != 1 || sendMs < 1577836800000ULL) {
        fprintf(stderr, "unix-ms looks unset\n");
        return 1;
    }
    unsigned windows = 1;
    if (argc == 6 && (sscanf(argv[5], "%u", &windows) != 1 || windows < 1 || windows > HTP_MAX_WINDOWS)) {
        fprintf(stderr, "windows must be 1..%d\n", HTP_MAX_WINDOWS);
        return 1;
    }

    const uint64_t lastSec = sendMs / 1000ULL - 1;
    const uint64_t firstSec = lastSec - (windows - 1);
    const uint32_t seq = (uint32_t)firstSec;
    uint64_t tStartUs[HTP_MAX_WINDOWS];
    for (unsigned w = 0; w < windows; w++) tStartUs[w] = (firstSec + w) * 1000000ULL;

    static float samples[HTP_MAX_WINDOWS * 22 * 10];
    uint8_t single[HTP_MAX_FRAME_BYTES];
    uint8_t joined[HTP_MAX_POST_BYTES];
    uint8_t direct[HTP_MAX_POST_BYTES];

    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        const HtpGroup *group = &kHtpGroups[g];
        const size_t perWindow = (size_t)group->channel_count * group->rate_hz;
        const size_t windowBytes = htpWindowBytes(group);
        size_t off = HTP_HEADER_BYTES;
        for (unsigned w = 0; w < windows; w++) {
            float relS = (float)(firstSec + w - lastSec);
            float *win = samples + w * perWindow;
            size_t k = 0;
            for (uint8_t c = 0; c < group->channel_count; c++) {
                for (uint16_t i = 0; i < group->rate_hz; i++) {
                    float t = relS + (float)i / (float)group->rate_hz;
                    win[k++] = htpSynth(group->channels[c].id, t);
                }
            }
            size_t len = htpEncodeFrame(single, sizeof(single), session, (uint16_t)channelSetId,
                                        group, seq + w, 0, 1, &tStartUs[w], win);
            if (len != HTP_HEADER_BYTES + windowBytes) {
                fprintf(stderr, "encode failed for group %u\n", group->key);
                return 1;
            }
            memcpy(joined + off, single + HTP_HEADER_BYTES, windowBytes);
            off += windowBytes;
        }
        htpWriteHeader(joined, session, (uint16_t)channelSetId, group->key, (uint8_t)windows, seq, sendMs);

        size_t len = htpEncodeFrame(direct, sizeof(direct), session, (uint16_t)channelSetId,
                                    group, seq, sendMs, (uint8_t)windows, tStartUs, samples);
        if (len != off || memcmp(direct, joined, len) != 0) {
            fprintf(stderr, "group %u: joined frame differs from a direct %u-window encode\n",
                    group->key, windows);
            return 1;
        }

        char path[512];
        snprintf(path, sizeof(path), "%s/group-%u.bin", argv[4], group->key);
        FILE *out = fopen(path, "wb");
        if (!out || fwrite(joined, 1, len, out) != len) {
            perror(path);
            return 1;
        }
        fclose(out);
        printf("group %u seq %lu+%u bytes %zu\n", group->key, (unsigned long)seq, windows, len);
    }
    return 0;
}
