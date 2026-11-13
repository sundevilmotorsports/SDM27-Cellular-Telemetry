#include "telemetry_format.h"
#include <cstdio>

size_t telemetryFormatFrame(char *out, size_t cap, const CanFrame &f, int64_t ts_ms) {
    if (f.dlc > CAN_MAX_DATA_LEN || cap < telemetryFrameJsonMaxBytes(f)) return 0;
    int n = snprintf(out, cap, "{\"can_id\":%lu,\"extd\":%s,\"dlc\":%u,\"data\":\"",
                     (unsigned long)f.id, f.extended ? "true" : "false", (unsigned)f.dlc);
    if (n <= 0) return 0;
    static const char digits[] = "0123456789abcdef";
    size_t off = (size_t)n;
    for (uint8_t i = 0; i < f.dlc; i++) {
        out[off++] = digits[f.data[i] >> 4];
        out[off++] = digits[f.data[i] & 0x0f];
    }
    n = snprintf(out + off, cap - off, "\",\"ts_ms\":%lld%s%s}", (long long)ts_ms,
                 f.fd ? ",\"fd\":true,\"brs\":" : "", f.fd ? (f.brs ? "true" : "false") : "");
    if (n <= 0 || off + (size_t)n >= cap) return 0;
    return off + (size_t)n;
}
