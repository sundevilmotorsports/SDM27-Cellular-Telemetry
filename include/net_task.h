// Owns the modem, the HTTPS POST path, and spool replay.
// Drains the shared CAN queue into time-windowed batches, serializes them,
// and POSTs the JSON to HTTPS_INGEST_URL. Never touches CAN capture timing.

#pragma once

// Creates the net task, pinned to core 1.
void netTaskStart();
