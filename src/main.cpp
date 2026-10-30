// CAN-to-Cellular Telemetry entry point.
// CAN task (core 0) keeps the latest value of each channel, the sampler
// (core 0) builds 10 Hz channel-set-1 windows from them, and the net task
// (core 1) POSTs those windows as HTP/1. With HTP_SYNTH_MODE=1 the samples are
// synthesized instead and the CAN task is not started.

#include <Arduino.h>
#include "can_handler.h"
#include "can_pins.h"
#include "net_task.h"
#include "sampler.h"
#include "telemetry_config.h"

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("=== ESP32 CAN-to-Cellular Telemetry ===");
    Serial.printf("Device ID: %s\n", HTP_DEVICE_ID);
#if HTP_SYNTH_MODE
    Serial.println("Samples: synthesized for channel set 1 (HTP_SYNTH_MODE=1, CAN task not started)");
#else
    Serial.printf("Samples: CAN, %s\n", TELEMETRY_USE_SIMULATED_CAN ? "simulated source"
                                        : CAN_USE_MCP2518FD       ? "MCP2518FD (classic + FD)"
                                                                  : "TWAI (classic only)");
    canHandlerStart();
#endif
    netTaskStart();
    samplerStart();
}

void loop() {
    // All work happens in the CAN, sampler and net tasks.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
