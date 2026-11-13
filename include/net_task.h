// Owns the modem (or WiFi) and the uplink: drains g_canQueue into raw CAN
// batches and POSTs them, HMAC-signed, to the telemetry-ingest edge function,
// which files them under the device's running session.

#pragma once

// Creates the net task, pinned to core 1. Call after canHandlerStart().
void netTaskStart();
