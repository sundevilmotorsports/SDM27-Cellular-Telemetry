// CAN ingestion: the MCP2518FD (default), the built-in TWAI controller
// (classic only), or a simulated source (TELEMETRY_USE_SIMULATED_CAN=1),
// chosen at compile time. Each frame gets a microsecond capture timestamp the
// moment it leaves the controller, then goes into g_canQueue untouched (no
// decoding: the dashboard decodes with the logger's config.json). Never
// blocks: when the queue is full the oldest frame is dropped and counted.

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "telemetry_format.h"

extern QueueHandle_t g_canQueue;

// Reads and resets the dropped-frame counter. The net task calls it once per
// batch, so each drop is reported in exactly one batch's dropped_frames.
uint32_t canHandlerTakeDroppedCount();

// Creates the queue and starts the CAN task on core 0.
void canHandlerStart();
