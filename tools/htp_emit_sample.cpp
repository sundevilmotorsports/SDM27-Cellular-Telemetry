// Host stand-in for the first second of htpEmitWindows() in src/net_task.cpp.
// Same encoder, same synth, same channel set. Not flashed, not built by PlatformIO.
//
//   htp_emit_sample <session-uuid> <channel-set-id> <unix-ms> <out-dir>

#include "htp_frame.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: htp_emit_sample <session-uuid> <channel-set-id> <unix-ms> <out-dir>\n");
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

    const uint64_t tStartUs = (sendMs / 1000ULL) * 1000000ULL;
    float samples[22 * 10];
    uint8_t frame[HTP_MAX_FRAME_BYTES];

    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        const HtpGroup *group = &kHtpGroups[g];
        size_t n = 0;
        for (uint8_t c = 0; c < group->channel_count; c++) {
            for (uint16_t i = 0; i < group->rate_hz; i++) {
                float t = 0.0f + (float)i / (float)group->rate_hz;
                samples[n++] = htpSynth(group->channels[c].id, t);
            }
        }
        size_t len = htpEncodeFrame(frame, sizeof(frame), session, (uint16_t)channelSetId,
                                    group, /*seq=*/0, sendMs, 1, &tStartUs, samples);
        if (len == 0) {
            fprintf(stderr, "encode failed for group %u\n", group->key);
            return 1;
        }
        char path[512];
        snprintf(path, sizeof(path), "%s/group-%u.bin", argv[4], group->key);
        FILE *out = fopen(path, "wb");
        if (!out || fwrite(frame, 1, len, out) != len) {
            perror(path);
            return 1;
        }
        fclose(out);
        printf("group %u seq 0 bytes %zu\n", group->key, len);
    }
    return 0;
}
