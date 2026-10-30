// Frozen SDM26-cell-v1 wire layout.
// Generated from docs/channel-set-1.json (Helios seed.sql channel_sets id=1).
// Do not reorder channels or groups. Order is the wire order.

#pragma once

#include <stddef.h>
#include <stdint.h>

#define HTP_CHANNEL_SET_ID 1
#define HTP_CHANNEL_SET_NAME "SDM26-cell-v1"
#define HTP_GROUP_COUNT 3

#define HTP_ENC_I16FP 0
#define HTP_ENC_F32 1

struct HtpChannel {
    const char *id;
    uint8_t enc;
    float scale;
    float offset;
};

struct HtpGroup {
    uint8_t key;
    uint16_t rate_hz;
    uint8_t channel_count;
    const HtpChannel *channels;
};

// Group 0: 10 Hz, 22 x i16fp. One window is 8 + 22*10*2 = 448 bytes.
static const HtpChannel kHtpGroup0Channels[] = {
    {"engine.rpm", HTP_ENC_I16FP, 0.5f, 0.0f},
    {"engine.tps", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"engine.aps", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"engine.map", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"engine.lambda", HTP_ENC_I16FP, 0.0001f, 0.6f},
    {"engine.gear", HTP_ENC_I16FP, 1.0f, 0.0f},
    {"chassis.steering_angle", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"imu.lat_g", HTP_ENC_I16FP, 0.001f, 0.0f},
    {"imu.long_g", HTP_ENC_I16FP, 0.001f, 0.0f},
    {"imu.vert_g", HTP_ENC_I16FP, 0.001f, 0.0f},
    {"imu.yaw_rate", HTP_ENC_I16FP, 0.02f, 0.0f},
    {"brake.front_pressure", HTP_ENC_I16FP, 0.5f, 0.0f},
    {"brake.rear_pressure", HTP_ENC_I16FP, 0.5f, 0.0f},
    {"suspension.travel_fl", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"suspension.travel_fr", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"suspension.travel_rl", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"suspension.travel_rr", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"drivetrain.wheel_speed_fl", HTP_ENC_I16FP, 0.125f, 0.0f},
    {"drivetrain.wheel_speed_fr", HTP_ENC_I16FP, 0.125f, 0.0f},
    {"drivetrain.wheel_speed_rl", HTP_ENC_I16FP, 0.125f, 0.0f},
    {"drivetrain.wheel_speed_rr", HTP_ENC_I16FP, 0.125f, 0.0f},
    {"drivetrain.vehicle_speed", HTP_ENC_I16FP, 0.01f, 0.0f},
};

// Group 1: 10 Hz, 5 x f32. One window is 8 + 5*10*4 = 208 bytes.
static const HtpChannel kHtpGroup1Channels[] = {
    {"gps.lat_ref", HTP_ENC_F32, 1.0f, 0.0f},
    {"gps.lat_d", HTP_ENC_F32, 1.0f, 0.0f},
    {"gps.lon_ref", HTP_ENC_F32, 1.0f, 0.0f},
    {"gps.lon_d", HTP_ENC_F32, 1.0f, 0.0f},
    {"gps.speed", HTP_ENC_F32, 1.0f, 0.0f},
};

// Group 2: 1 Hz, 11 x i16fp. One window is 8 + 11*1*2 = 30 bytes.
static const HtpChannel kHtpGroup2Channels[] = {
    {"engine.water_temp", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"engine.oil_temp", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"engine.oil_pressure", HTP_ENC_I16FP, 0.05f, 0.0f},
    {"engine.fuel_pressure", HTP_ENC_I16FP, 0.05f, 0.0f},
    {"engine.battery_voltage", HTP_ENC_I16FP, 0.001f, 0.0f},
    {"power.supply_current", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"tire.surface_temp_fl", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"tire.surface_temp_fr", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"tire.surface_temp_rl", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"tire.surface_temp_rr", HTP_ENC_I16FP, 0.01f, 0.0f},
    {"gps.fix_quality", HTP_ENC_I16FP, 1.0f, 0.0f},
};

static const HtpGroup kHtpGroups[HTP_GROUP_COUNT] = {
    {0, 10, 22, kHtpGroup0Channels},
    {1, 10, 5, kHtpGroup1Channels},
    {2, 1, 11, kHtpGroup2Channels},
};
