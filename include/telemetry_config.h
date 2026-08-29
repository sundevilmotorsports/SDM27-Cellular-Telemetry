// User-editable configuration for the telemetry PoC.
// Fill in your APN and MQTT broker details before flashing the real-CAN or
// cellular-connectivity paths. The public HiveMQ broker below is fine for an
// initial connectivity smoke test ONLY -- do not use it for real vehicle data.

#pragma once

// ---- CAN source mode --------------------------------------------------
// 1 = simulated CAN frames (no hardware CAN bus required)
// 0 = real CAN frames via the ESP32-S3 TWAI controller + external transceiver
#define TELEMETRY_USE_SIMULATED_CAN 1

// Simulated CAN generator settings (only used when TELEMETRY_USE_SIMULATED_CAN=1)
#define SIM_CAN_FAST_ID       0x100   // e.g. wheel speed / RPM, high rate
#define SIM_CAN_FAST_RATE_HZ  100
#define SIM_CAN_SLOW_ID       0x200   // e.g. status frame, low rate
#define SIM_CAN_SLOW_RATE_HZ  5

// ---- Cellular ----------------------------------------------------------
#define TELEMETRY_APN          "your.apn.here"
#define TELEMETRY_APN_USER     ""
#define TELEMETRY_APN_PASS     ""
#define TELEMETRY_GSM_PIN      ""   // SIM PIN, leave empty if none

// ---- MQTT ----------------------------------------------------------
// Public test broker for initial smoke-testing only.
#define MQTT_BROKER_HOST   "broker.hivemq.com"
#define MQTT_BROKER_PORT   1883
#define MQTT_USERNAME      ""
#define MQTT_PASSWORD      ""
#define MQTT_TOPIC_TELEMETRY  "esp32_cellular_telemetry/batch"
#define MQTT_CLIENT_ID_PREFIX  "esp32-can-telemetry-"
// Must comfortably fit BATCH_MAX_FRAMES worth of serialized JSON (~110 bytes/frame).
// PubSubClient allocates this on the regular heap, not PSRAM -- if you raise
// BATCH_MAX_FRAMES, raise this too and watch for allocation failures at boot.
#define MQTT_BUFFER_SIZE        16384

// ---- Device identity -----------------------------------------------
#define TELEMETRY_DEVICE_ID   "esp32-sim7670g-poc-01"

// ---- Batching / queue -----------------------------------------------
#define CAN_QUEUE_DEPTH        512   // frames; oldest dropped when full
#define BATCH_WINDOW_MS         1000  // how often the net task drains the queue
#define BATCH_MAX_FRAMES        200   // hard cap on frames per published batch

// ---- Wall-clock (epoch) sync -----------------------------------------
#define TIME_SYNC_INTERVAL_MS   (5UL * 60UL * 1000UL)  // resync every 5 minutes

// ---- MQTT reconnect backoff ------------------------------------------
#define MQTT_BACKOFF_INITIAL_MS  1000
#define MQTT_BACKOFF_MAX_MS      60000
#define MODEM_REGISTRATION_RECHECK_MS (30UL * 1000UL)

// ---- Offline spooling (LittleFS) --------------------------------------
#define SPOOL_DIR               "/spool"
#define SPOOL_MAX_FILES         100   // oldest spooled batch dropped beyond this
