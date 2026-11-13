// CAN-to-Cellular Telemetry entry point.
// CAN task (core 0) timestamps every received frame and queues it untouched;
// the net task (core 1) posts the queue as raw CAN batches once a second.

#include <Arduino.h>
#include "can_handler.h"
#include "can_pins.h"
#include "net_task.h"
#include "telemetry_config.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("=== ESP32 CAN-to-Cellular Telemetry ===");
    Serial.printf("Device ID: %s\n", HTP_DEVICE_ID);
    Serial.printf("CAN source: %s\n", TELEMETRY_USE_SIMULATED_CAN ? "simulated (bench ECU pattern)"
                                     : CAN_USE_MCP2518FD       ? "MCP2518FD (classic + FD)"
                                                               : "TWAI (classic only)");
    canHandlerStart();
    netTaskStart();
}

void loop() {
    // All work happens in the CAN and net tasks.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
