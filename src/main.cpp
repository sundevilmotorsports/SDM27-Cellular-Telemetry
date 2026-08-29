// CAN-to-Cellular Telemetry PoC entry point.
// Two FreeRTOS tasks + a shared queue, per the architecture in README.md:
//   - CAN task (core 0, high priority): can_handler.cpp
//   - Net task (core 1): net_task.cpp -- modem, MQTT, batching, spooling
// Both feed/consume the same CanFrame queue and format regardless of whether
// TELEMETRY_USE_SIMULATED_CAN selects the synthetic generator or the real
// TWAI driver -- see can_handler.cpp.

#include <Arduino.h>
#include "can_handler.h"
#include "net_task.h"
#include "telemetry_config.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("=== ESP32 CAN-to-Cellular Telemetry PoC ===");
    Serial.printf("Mode: %s CAN\n", TELEMETRY_USE_SIMULATED_CAN ? "simulated" : "real (TWAI)");
    Serial.printf("Device ID: %s\n", TELEMETRY_DEVICE_ID);

    canHandlerStart();
    netTaskStart();
}

void loop() {
    // All work happens in the CAN and net tasks; nothing to do here.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
