// Picks the CAN signal table compiled in. Each table is a kCanSignals[] array
// ending with an entry whose channel is nullptr.
//   CAN_MAP_BENCH=0 (default): include/can_map_car.h, the car. Generate it from
//                              the logger's config.json with
//                              tools/logger_config_to_can_map.py.
//   CAN_MAP_BENCH=1:           include/can_map_bench.h, frames from the C5
//                              generator's ECU pattern and the simulated CAN source.

#pragma once

#include "can_map.h"

#ifndef CAN_MAP_BENCH
#define CAN_MAP_BENCH 0
#endif

#if CAN_MAP_BENCH
#define CAN_MAP_NAME "bench"
#include "can_map_bench.h"
#else
#define CAN_MAP_NAME "car"
#include "can_map_car.h"
#endif
