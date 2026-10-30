// CAN ingestion: the MCP2518FD (default), the built-in TWAI controller
// (classic only), or a simulated source (TELEMETRY_USE_SIMULATED_CAN=1),
// chosen at compile time. Each frame gets a microsecond capture timestamp and
// is decoded straight into the CAN map (can_map.h); nothing is queued.

#pragma once

// Builds the CAN map and starts the CAN task on core 0.
void canHandlerStart();
