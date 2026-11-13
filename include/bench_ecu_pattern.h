// Fake ECU traffic for bench tests, laid out per the car logger's config.json
// (sundevilmotorsports/logger @ 9338e3c, the copy the dashboard decodes with),
// so frames from the bench decode into real logger channels on the dashboard.
// Shared by the C5 generator (serial command 'e') and the S3's simulated CAN
// source, so both send identical frames. Header-only and free of Arduino types
// so host tests can include it too.
//
// Step i (one every BENCH_ECU_PERIOD_MS) sends:
//   0x3E8 classic, 8 bytes, mux byte 0 cycling 0, 1, 4 (i % 3):
//     mux 0: [1..2] BE r_engine       1000 + (i * 37) % 11000      rpm
//            [3]    t_eng_coolant    60 + (i / 10) % 40           (raw = C - 50: logger offset -50)
//            [4]    t_oil            70 + (i / 10) % 40           (raw = C - 50)
//            [5..6] BE p_oil         200 + i % 300
//     mux 1: [1]    Lamb_1           (85 + i % 30) / 100          (scale 0.01)
//            [2]    %_TPS            i % 101                      (raw byte)
//            [3]    n-Gear           1 + (i / 50) % 6             (raw byte)
//            [4..5] BE v_trans_out   (i * 7) % 3000
//     mux 4: [6]    %_fuel_left      100 - (i / 100) % 101        (raw byte)
//   0x361 classic, 8 bytes, signed little-endian:
//            [0..1] a_Lat ((i % 400) - 200) * 10, [2..3] a_Long = -a_Lat, [4..5] a_Vert 1000
//   every 10th step, 0x5F1 CAN FD + BRS, 16 bytes: not in the logger config
//            (the dashboard counts it as unknown); it keeps FD on the bench.

#pragma once

#include <stdint.h>
#include <string.h>

#define BENCH_ECU_PERIOD_MS 100
#define BENCH_ECU_MAX_FRAMES 3
#define BENCH_ECU_ID_ENGINE 0x3E8
#define BENCH_ECU_ID_IMU 0x361
#define BENCH_ECU_ID_FD 0x5F1

struct BenchEcuFrame {
    uint32_t id;
    bool fd;
    uint8_t len;
    uint8_t data[16];
};

// Values for step i, in the logger's decoded units.
struct BenchEcuValues {
    int mux; // 0, 1 or 4: which 0x3E8 group step i sends
    int rpm, coolant, oilTemp, oilPressure;
    int lambdaRaw, tps, gear, transOut;
    int fuel;
    int latG, longG, vertG;
};

static inline BenchEcuValues benchEcuValues(uint32_t i) {
    static const int kMux[3] = {0, 1, 4};
    BenchEcuValues v;
    v.mux = kMux[i % 3];
    v.rpm = (int)(1000 + (i * 37) % 11000);
    v.coolant = (int)(60 + (i / 10) % 40);
    v.oilTemp = (int)(70 + (i / 10) % 40);
    v.oilPressure = (int)(200 + i % 300);
    v.lambdaRaw = (int)(85 + i % 30);
    v.tps = (int)(i % 101);
    v.gear = (int)(1 + (i / 50) % 6);
    v.transOut = (int)((i * 7) % 3000);
    v.fuel = (int)(100 - (i / 100) % 101);
    v.latG = ((int)(i % 400) - 200) * 10;
    v.longG = -v.latG;
    v.vertG = 1000;
    return v;
}

static inline void benchPutBe16(uint8_t *p, int x) {
    p[0] = (uint8_t)((uint16_t)x >> 8);
    p[1] = (uint8_t)x;
}

static inline void benchPutLe16(uint8_t *p, int x) {
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)((uint16_t)x >> 8);
}

// Fills out[] for step i and returns how many frames to send (2 or 3).
static inline int benchEcuFrames(uint32_t i, BenchEcuFrame out[BENCH_ECU_MAX_FRAMES]) {
    BenchEcuValues v = benchEcuValues(i);
    memset(out, 0, BENCH_ECU_MAX_FRAMES * sizeof(BenchEcuFrame));

    BenchEcuFrame &e = out[0];
    e.id = BENCH_ECU_ID_ENGINE;
    e.len = 8;
    e.data[0] = (uint8_t)v.mux;
    if (v.mux == 0) {
        benchPutBe16(&e.data[1], v.rpm);
        e.data[3] = (uint8_t)(v.coolant - 50);
        e.data[4] = (uint8_t)(v.oilTemp - 50);
        benchPutBe16(&e.data[5], v.oilPressure);
    } else if (v.mux == 1) {
        e.data[1] = (uint8_t)v.lambdaRaw;
        e.data[2] = (uint8_t)v.tps;
        e.data[3] = (uint8_t)v.gear;
        benchPutBe16(&e.data[4], v.transOut);
    } else {
        e.data[1] = 1; // in_gear
        e.data[6] = (uint8_t)v.fuel;
    }

    BenchEcuFrame &imu = out[1];
    imu.id = BENCH_ECU_ID_IMU;
    imu.len = 8;
    benchPutLe16(&imu.data[0], v.latG);
    benchPutLe16(&imu.data[2], v.longG);
    benchPutLe16(&imu.data[4], v.vertG);

    if (i % 10 != 0) return 2;
    BenchEcuFrame &fd = out[2];
    fd.id = BENCH_ECU_ID_FD;
    fd.fd = true;
    fd.len = 16;
    for (int k = 0; k < 16; k++) fd.data[k] = (uint8_t)(i + k);
    return 3;
}
