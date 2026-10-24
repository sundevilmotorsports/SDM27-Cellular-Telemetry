// Owns the modem and the HTP/1 HTTPS POST path.
// Builds 1-second channel-set-1 windows and POSTs them to the telemetry-ingest
// edge function. Does not drain the CAN queue; real CAN mapping is not wired.

#pragma once

// Creates the net task, pinned to core 1.
void netTaskStart();
