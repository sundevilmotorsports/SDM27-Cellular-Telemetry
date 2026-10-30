// Owns the modem and the HTP/1 HTTPS POST path.
// Queues 1-second channel-set-1 windows from the sampler and POSTs them to the
// telemetry-ingest edge function, oldest first, several windows per request.

#pragma once

#include <stdint.h>

// Creates the net task, pinned to core 1. Call before samplerStart().
void netTaskStart();

// Encodes one window of one group (samples channel-major, rate_hz per channel,
// NaN = dropout) and queues it. Drops the group's oldest window when full.
// Safe to call from another task; does no network I/O.
void netTaskPushWindow(int group, uint64_t tStartUs, const float *samples);
