// CAN signals on the car -> channel set 1. EMPTY until the car's definitions
// are in hand: regenerate this file from the logger's SD-card config.json:
//
//   python3 tools/logger_config_to_can_map.py config.json --bus Engine > include/can_map_car.h
//
// With no signals every CAN channel posts as a dropout (NaN), never as 0.
// Columns: id, extended, muxByte (-1 = none), muxValue, start, len, signed,
// bigEndian, scale, offset, channel. value = scale * (raw - offset).

#pragma once

static const CanSignal kCanSignals[] = {
    {0, false, -1, 0, 0, 0, false, false, 0.0f, 0.0f, nullptr},
};
