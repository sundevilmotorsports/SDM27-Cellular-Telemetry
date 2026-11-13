// Host encoder used by tools/check_htp_encode.mjs. Not built into the firmware.
// Reads a request file and writes the HTP frame bytes.

#include "htp_frame.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static bool readExact(FILE *f, void *dst, size_t n) {
    return fread(dst, 1, n, f) == n;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: htp_encode_host request.bin frame.bin\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");
    if (!in) {
        perror(argv[1]);
        return 1;
    }

    uint8_t groupKey = 0;
    uint8_t windowCount = 0;
    uint16_t channelSetId = 0;
    uint32_t seq = 0;
    uint64_t sendMs = 0;
    uint8_t session[16];
    if (!readExact(in, &groupKey, 1) || !readExact(in, &windowCount, 1) ||
        !readExact(in, &channelSetId, 2) || !readExact(in, &seq, 4) ||
        !readExact(in, &sendMs, 8) || !readExact(in, session, 16)) {
        fprintf(stderr, "short header\n");
        return 1;
    }

    const HtpGroup *group = nullptr;
    for (int i = 0; i < HTP_GROUP_COUNT; i++) {
        if (kHtpGroups[i].key == groupKey) group = &kHtpGroups[i];
    }
    if (!group) {
        fprintf(stderr, "unknown group %u\n", groupKey);
        return 1;
    }

    std::vector<uint64_t> starts(windowCount);
    size_t nSamples = (size_t)windowCount * group->channel_count * group->rate_hz;
    std::vector<float> samples(nSamples);
    if (!readExact(in, starts.data(), windowCount * sizeof(uint64_t)) ||
        !readExact(in, samples.data(), nSamples * sizeof(float))) {
        fprintf(stderr, "short body\n");
        return 1;
    }
    fclose(in);

    uint8_t frame[4096];
    size_t n = htpEncodeFrame(frame, sizeof(frame), session, channelSetId, group,
                              seq, sendMs, windowCount, starts.data(), samples.data());
    if (n == 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    FILE *out = fopen(argv[2], "wb");
    if (!out || fwrite(frame, 1, n, out) != n) {
        perror(argv[2]);
        return 1;
    }
    fclose(out);
    return 0;
}
