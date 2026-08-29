#include "telemetry_format.h"
#include <ArduinoJson.h>

static void hexEncode(const uint8_t *data, uint8_t len, char *out /* len*2+1 bytes */) {
    static const char digits[] = "0123456789abcdef";
    for (uint8_t i = 0; i < len; i++) {
        out[i * 2]     = digits[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

String telemetryFormatBatch(const TelemetryBatch &batch, const char *device_id,
                             int64_t (*epoch_ms_for)(int64_t capture_us)) {
    // Size estimate: ~90 bytes/frame of JSON overhead + hex payload, plus batch header.
    JsonDocument doc;

    doc["device_id"] = device_id;
    doc["seq"] = batch.seq;
    doc["dropped_frames"] = batch.dropped_frames;

    JsonArray frames = doc["frames"].to<JsonArray>();
    char hexbuf[17];
    for (const CanFrame &f : batch.frames) {
        JsonObject jf = frames.add<JsonObject>();
        jf["can_id"] = f.id;
        jf["extd"] = f.extended;
        jf["dlc"] = f.dlc;
        hexEncode(f.data, f.dlc, hexbuf);
        jf["data"] = hexbuf;
        jf["ts_ms"] = epoch_ms_for(f.capture_us);
    }

    String out;
    serializeJson(doc, out);
    return out;
}
