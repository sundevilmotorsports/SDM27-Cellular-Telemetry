// Encodes one Helios golden fixture with src/htp_frame.cpp for
// tools/check_helios_fixtures.sh.
// stdin: session group nwin seq send_ms, then nwin t_start_us, then all samples
// (window-major, channel-major; "nan" = null). stdout: encoded frame.
#include "htp_frame.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main() {
    char sess[64]; unsigned g, nwin; unsigned long seq; unsigned long long send;
    if (scanf("%63s %u %u %lu %llu", sess, &g, &nwin, &seq, &send) != 5) return 2;
    uint8_t sid[16]; if (!htpParseUuid(sess, sid)) return 3;
    uint64_t t[8]; for (unsigned w = 0; w < nwin; w++) scanf("%llu", (unsigned long long *)&t[w]);
    const HtpGroup *grp = &kHtpGroups[g];
    size_t n = (size_t)nwin * grp->channel_count * grp->rate_hz;
    float *s = (float *)malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) { char tok[64]; scanf("%63s", tok); s[i] = strcmp(tok, "nan") ? (float)strtod(tok, 0) : NAN; }
    static uint8_t out[HTP_MAX_POST_BYTES];
    size_t len = htpEncodeFrame(out, sizeof out, sid, 1, grp, (uint32_t)seq, send, (uint8_t)nwin, t, s);
    if (!len) return 4;
    fwrite(out, 1, len, stdout);
    return 0;
}
