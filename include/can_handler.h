// CAN ingestion: reads either the real TWAI controller or a synthetic frame
// generator (compile-time switch, TELEMETRY_USE_SIMULATED_CAN), and feeds a
// shared FreeRTOS queue that the net task drains. Both paths capture a
// monotonic microsecond timestamp at the moment the frame is pulled off its
// source -- before it touches the queue -- so queueing/network jitter never
// pollutes inter-frame timing. Never blocks: if the queue is full (the net
// task/cellular link can't keep up), the oldest queued frame is dropped and
// a running counter is incremented so the next successful batch can report
// the gap.

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "telemetry_format.h"

extern QueueHandle_t g_canQueue;

// Atomically reads and resets the dropped-frame counter. Call this once per
// batch when building a TelemetryBatch, so each drop is reported exactly once.
uint32_t canHandlerTakeDroppedCount();

// Creates the shared queue and starts the CAN ingestion task on core 0.
void canHandlerStart();
