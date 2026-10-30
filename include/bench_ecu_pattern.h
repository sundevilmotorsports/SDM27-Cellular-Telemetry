// Fake ECU traffic for bench tests, decoded by include/can_map_bench.h
// (CAN_MAP_BENCH=1). Shared by the C5 generator (serial command 'e') and the
// S3's simulated CAN source, so both send identical frames. Header-only and
// free of Arduino types so the host test can include it too.
//
// Step i (sent every 100 ms) produces two frames: 0x5F0 and 0x5F1 with mux
// i % 2. Values are integer ramps, so a decoded value can be checked exactly:
//   engine.rpm           1000 + (i * 37) % 11000
//   engine.tps           i % 101                      (%)
//   engine.water_temp    60 + (i / 10) % 40           (C)
//   engine.gear          1 + (i / 50) % 6
//   brake.front_pressure (i % 200) * 10               (kPa, even steps)
//   brake.rear_pressure  (i % 200) * 5                (kPa, odd steps)
//   imu.lat_g            ((i % 400) - 200) / 100      (g, even steps)
//   imu.long_g           (200 - (i % 400)) / 100      (g, odd steps)

#pragma once

#include <stdint.h>
#include <string.h>

#define BENCH_ECU_ID_ENGINE 0x5F0
#define BENCH_ECU_ID_CHASSIS 0x5F1
#define BENCH_ECU_PERIOD_MS 100

struct BenchEcuFrame {
    uint32_t id;
    bool fd;
    uint8_t len;
    uint8_t data[16];
};

struct BenchEcuValues {
    float rpm, tps, waterTemp, gear;
    int mux; // 0: front brake + lat g, 1: rear brake + long g
    float brake, g;
};

static inline BenchEcuValues benchEcuValues(uint32_t i) {
    BenchEcuValues v;
    v.rpm = (float)(1000 + (i * 37) % 11000);
    v.tps = (float)(i % 101);
    v.waterTemp = (float)(60 + (i / 10) % 40);
    v.gear = (float)(1 + (i / 50) % 6);
    v.mux = (int)(i % 2);
    int ramp = (int)(i % 400) - 200;
    v.brake = v.mux == 0 ? (float)((i % 200) * 10) : (float)((i % 200) * 5);
    v.g = (float)(v.mux == 0 ? ramp : -ramp) / 100.0f;
    return v;
}

static inline void benchPutLe16(uint8_t *p, uint16_t x) {
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(x >> 8);
}

// Fills out[0] (0x5F0, classic) and out[1] (0x5F1, CAN FD + BRS) for step i.
static inline void benchEcuFrames(uint32_t i, BenchEcuFrame out[2]) {
    BenchEcuValues v = benchEcuValues(i);
    memset(out, 0, 2 * sizeof(BenchEcuFrame));

    out[0].id = BENCH_ECU_ID_ENGINE;
    out[0].len = 8;
    benchPutLe16(&out[0].data[0], (uint16_t)v.rpm);
    benchPutLe16(&out[0].data[2], (uint16_t)(v.tps * 100.0f + 0.5f));
    benchPutLe16(&out[0].data[4], (uint16_t)((v.waterTemp + 40.0f) * 10.0f + 0.5f));
    out[0].data[6] = (uint8_t)v.gear;

    out[1].id = BENCH_ECU_ID_CHASSIS;
    out[1].fd = true;
    out[1].len = 16;
    out[1].data[0] = (uint8_t)v.mux;
    benchPutLe16(&out[1].data[1], (uint16_t)(v.brake * 2.0f + 0.5f));
    int16_t g = (int16_t)(v.mux == 0 ? ((int)(i % 400) - 200) * 10 : (200 - (int)(i % 400)) * 10);
    out[1].data[3] = (uint8_t)((uint16_t)g >> 8); // big-endian on purpose
    out[1].data[4] = (uint8_t)g;
    for (int k = 5; k < 16; k++) out[1].data[k] = (uint8_t)(0xA0 + k); // filler
}
