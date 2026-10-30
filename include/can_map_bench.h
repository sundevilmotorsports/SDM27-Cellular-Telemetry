// Bench CAN signals -> channel set 1. Matches the C5 generator's ECU pattern
// (bench/can_tx_c5, serial command 'e') and the simulated CAN source
// (TELEMETRY_USE_SIMULATED_CAN=1). Exercises little/big endian, signed,
// offset, a muxed CAN FD frame, and a channel fed by two muxed signals.
//
//   0x5F0 classic, 8 bytes:
//     [0..1] u16 LE  rpm                     -> engine.rpm
//     [2..3] u16 LE  throttle * 100          -> engine.tps        (%)
//     [4..5] u16 LE  (water C + 40) * 10     -> engine.water_temp (C)
//     [6]    u8      gear                    -> engine.gear
//   0x5F1 CAN FD + BRS, 16 bytes, byte 0 = mux:
//     mux 0: [1..2] u16 LE front brake * 2   -> brake.front_pressure (kPa)
//            [3..4] i16 BE lat g * 1000      -> imu.lat_g
//     mux 1: [1..2] u16 LE rear brake * 2    -> brake.rear_pressure (kPa)
//            [3..4] i16 BE long g * 1000     -> imu.long_g
//   0x100 (simulated CAN source): [0..1] u16 LE counter -> engine.map

#pragma once

static const CanSignal kCanSignals[] = {
    {0x5F0, false, -1, 0, 0, 2, false, false, 1.0f, 0.0f, "engine.rpm"},
    {0x5F0, false, -1, 0, 2, 2, false, false, 0.01f, 0.0f, "engine.tps"},
    {0x5F0, false, -1, 0, 4, 2, false, false, 0.1f, 400.0f, "engine.water_temp"},
    {0x5F0, false, -1, 0, 6, 1, false, false, 1.0f, 0.0f, "engine.gear"},
    {0x5F1, false, 0, 0, 1, 2, false, false, 0.5f, 0.0f, "brake.front_pressure"},
    {0x5F1, false, 0, 0, 3, 2, true, true, 0.001f, 0.0f, "imu.lat_g"},
    {0x5F1, false, 0, 1, 1, 2, false, false, 0.5f, 0.0f, "brake.rear_pressure"},
    {0x5F1, false, 0, 1, 3, 2, true, true, 0.001f, 0.0f, "imu.long_g"},
    {0x100, false, -1, 0, 0, 2, false, false, 1.0f, 0.0f, "engine.map"},
    {0, false, -1, 0, 0, 0, false, false, 0.0f, 0.0f, nullptr},
};
