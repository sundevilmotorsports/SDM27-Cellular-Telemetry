// Owns the modem, MQTT connection, and all reconnect/backoff/retry logic.
// Drains the shared CAN queue into time-windowed batches, serializes them,
// and publishes over MQTT via the LTE link. Never touches CAN capture timing.

#pragma once

// Creates the net task, pinned to core 1.
void netTaskStart();
