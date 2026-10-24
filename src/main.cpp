// CAN-to-Cellular Telemetry entry point.
// Net task (core 1) posts HTP/1. With HTP_SYNTH_MODE=1 the CAN task is not
// started; samples are synthesized for channel set 1.

#include <Arduino.h>
#include "can_handler.h"
#include "can_pins.h"
#include "net_task.h"
#include "telemetry_config.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("=== ESP32 CAN-to-Cellular Telemetry PoC ===");
    Serial.printf("Mode: %s CAN\n", TELEMETRY_USE_SIMULATED_CAN ? "simulated"
                                     : CAN_USE_MCP2518FD       ? "real (MCP2518FD, classic + FD)"
                                                               : "real (TWAI, classic only)");
    Serial.printf("Device ID: %s\n", HTP_DEVICE_ID);
#if HTP_SYNTH_MODE
    Serial.println("Uplink: HTP/1 synth for channel set 1 (CAN task not started)");
#else
    Serial.println("Uplink: HTP/1, but HTP_SYNTH_MODE=0 and CAN is not mapped onto channels");
    canHandlerStart();
#endif
    netTaskStart();
}

void loop() {
    // All work happens in the CAN and net tasks; nothing to do here.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
