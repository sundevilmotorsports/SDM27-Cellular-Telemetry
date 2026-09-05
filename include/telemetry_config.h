// User-editable configuration for the telemetry PoC.
// Fill in your APN and MQTT broker details before flashing the real-CAN or
// cellular-connectivity paths. The public HiveMQ broker below is fine for an
// initial connectivity smoke test ONLY -- do not use it for real vehicle data.
//
// Credentials (WiFi/APN/MQTT) below are wrapped in #ifndef guards: copy
// .env.example to .env (git-ignored) and fill in real values there instead
// of editing them here -- load_env.py injects .env as compiler defines at
// build time, which pre-empts the placeholder #define in each guard. A
// fresh clone with no .env still builds fine using the placeholders as-is.

#pragma once

// ---- CAN source mode --------------------------------------------------
// 1 = simulated CAN frames (no hardware CAN bus required)
// 0 = real CAN frames via the ESP32-S3 TWAI controller + external transceiver
#ifndef TELEMETRY_USE_SIMULATED_CAN
#define TELEMETRY_USE_SIMULATED_CAN 1
#endif

// Simulated CAN generator settings (only used when TELEMETRY_USE_SIMULATED_CAN=1)
// NOTE: these rates are deliberately far below a real bus's. On a metered SIM
// the generator rate, not the batching, sets your data bill: every simulated
// frame costs ~90 bytes of JSON on the wire. 100+5 Hz (the original bench
// values, fine over WiFi) works out to ~34 MB/hour. Raise these only when the
// link is unmetered.
#define SIM_CAN_FAST_ID       0x100   // e.g. wheel speed / RPM, high rate
#define SIM_CAN_FAST_RATE_HZ  1
#define SIM_CAN_SLOW_ID       0x200   // e.g. status frame, low rate
#define SIM_CAN_SLOW_RATE_HZ  1

// ---- Transport ----------------------------------------------------------
// 1 = WiFi (bench testing without a SIM card/cellular plan; uses the
//     ESP32-S3's onboard WiFi radio, not the SIM7670G modem)
// 0 = cellular via the onboard SIM7670G modem (the real deployment path)
#ifndef TELEMETRY_USE_WIFI
#define TELEMETRY_USE_WIFI 0
#endif

// Only used when TELEMETRY_USE_WIFI=1.
// 1 = SoftAP -- the ESP32-S3 hosts its own WiFi network (WIFI_AP_SSID/
//     _PASSWORD below); your laptop/phone joins IT and runs the MQTT broker
//     locally. No existing WiFi network needed at all -- good for field
//     testing with zero WiFi coverage. Timestamps will NOT be network-synced
//     in this mode (no internet uplink for NTP); relative inter-frame timing
//     is unaffected, see README's Known Limitations.
// 0 = station -- the ESP32-S3 joins an existing WiFi network (WIFI_SSID
//     below), same as any laptop/phone would.
#ifndef WIFI_AP_MODE
#define WIFI_AP_MODE 0
#endif

// Used when WIFI_AP_MODE=1. WPA2 requires an 8+ character password; leave
// WIFI_AP_PASSWORD empty ("") for an open (unencrypted) network instead.
#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID           "esp32-telemetry"
#endif
#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD       "telemetry123"
#endif

// Used when WIFI_AP_MODE=0.
#ifndef WIFI_SSID
#define WIFI_SSID              "your-wifi-ssid"
#endif
#define WIFI_CONNECT_TIMEOUT_MS  20000

// 1 = WPA2/WPA3-Enterprise (802.1X) -- networks that ask for a username AND
//     password to join directly (no browser), e.g. eduroam or a corporate
//     network. Uses PEAP/MSCHAPv2, which covers the large majority of
//     enterprise networks; EAP-TLS (client-certificate auth) is not supported.
// 0 = ordinary WPA2/WPA3-Personal -- a single shared network password.
// Only meaningful when WIFI_AP_MODE=0.
#define WIFI_ENTERPRISE 0

// Used when WIFI_AP_MODE=0 and WIFI_ENTERPRISE=0.
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD          "your-wifi-password"
#endif

// Used when WIFI_AP_MODE=0 and WIFI_ENTERPRISE=1. Identity can usually be
// left blank or set equal to the username -- only matters if your network's
// RADIUS server distinguishes outer/inner identity.
#ifndef WIFI_EAP_IDENTITY
#define WIFI_EAP_IDENTITY      ""
#endif
#ifndef WIFI_EAP_USERNAME
#define WIFI_EAP_USERNAME      "your-network-username"
#endif
#ifndef WIFI_EAP_PASSWORD
#define WIFI_EAP_PASSWORD      "your-network-password"
#endif

// ---- Cellular ----------------------------------------------------------
#ifndef TELEMETRY_APN
#define TELEMETRY_APN          "your.apn.here"
#endif
#ifndef TELEMETRY_APN_USER
#define TELEMETRY_APN_USER     ""
#endif
#ifndef TELEMETRY_APN_PASS
#define TELEMETRY_APN_PASS     ""
#endif
#ifndef TELEMETRY_GSM_PIN
#define TELEMETRY_GSM_PIN      ""   // SIM PIN, leave empty if none
#endif

// ---- MQTT ----------------------------------------------------------
// Default broker: if running in SoftAP mode, default to 192.168.4.2 (laptop IP).
// Otherwise, public test broker for initial smoke-testing only.
#ifndef MQTT_BROKER_HOST
#if (TELEMETRY_USE_WIFI && WIFI_AP_MODE)
#define MQTT_BROKER_HOST   "192.168.4.2"
#else
#define MQTT_BROKER_HOST   "broker.hivemq.com"
#endif
#endif
// 1 = MQTT over TLS. Required by hosted brokers (HiveMQ Cloud, EMQX
//     Serverless) which refuse plaintext connections entirely.
// 0 = plaintext MQTT.
//
// Cellular only. The WiFi path uses a plain WiFiClient and would need a
// WiFiClientSecure to match; not wired up, because WiFi mode exists for bench
// testing against a local broker where TLS buys nothing.
//
// Caveat worth knowing: the TinyGSM fork leaves AT+CSSLCFG "authmode" at 0
// (TinyGsmClientSIM7672.h, modemConnect), so the link is encrypted but the
// broker's certificate is NOT validated -- this stops passive eavesdropping,
// not an active man-in-the-middle. Loading a CA cert onto the modem and
// calling setCertificate() would close that gap.
#ifndef MQTT_USE_TLS
#define MQTT_USE_TLS 0
#endif

#ifndef MQTT_BROKER_PORT
#if MQTT_USE_TLS
#define MQTT_BROKER_PORT   8883
#else
#define MQTT_BROKER_PORT   1883
#endif
#endif
#ifndef MQTT_USERNAME
#define MQTT_USERNAME      ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD      ""
#endif
// Guarded so a deployment can pick its own topic from .env without editing a
// tracked file -- useful when one broker carries more than one device, or to
// keep the topic off an easily-guessed default.
#ifndef MQTT_TOPIC_TELEMETRY
#define MQTT_TOPIC_TELEMETRY  "esp32_cellular_telemetry/batch"
#endif
#define MQTT_CLIENT_ID_PREFIX  "esp32-can-telemetry-"
// Must comfortably fit BATCH_MAX_FRAMES worth of serialized JSON (~110 bytes/frame).
// PubSubClient allocates this on the regular heap, not PSRAM -- if you raise
// BATCH_MAX_FRAMES, raise this too and watch for allocation failures at boot.
#define MQTT_BUFFER_SIZE         8192

// ---- Device identity -----------------------------------------------
#define TELEMETRY_DEVICE_ID   "esp32-sim7670g-poc-01"

// ---- Batching / queue -----------------------------------------------
// A wider window amortizes the per-publish overhead (MQTT header + TCP/IP
// segment) over more frames, which matters on a metered link -- but it does
// NOT reduce the per-frame cost, so it can't rescue a high generator rate.
#define CAN_QUEUE_DEPTH        512   // frames; oldest dropped when full
#define BATCH_WINDOW_MS        15000  // how often the net task drains the queue
#define BATCH_MAX_FRAMES         60   // hard cap on frames per published batch

// ---- Wall-clock (epoch) sync -----------------------------------------
#define TIME_SYNC_INTERVAL_MS   (5UL * 60UL * 1000UL)  // resync every 5 minutes

// ---- MQTT reconnect backoff ------------------------------------------
#define MQTT_BACKOFF_INITIAL_MS  1000
#define MQTT_BACKOFF_MAX_MS      60000
#define MODEM_REGISTRATION_RECHECK_MS (30UL * 1000UL)
// How long to wait for the modem to attach to the network. A cold modem on an
// unfamiliar network can spend well over 30s scanning LTE bands before it
// registers, so a short timeout reports a failure that was only slowness.
#define MODEM_REGISTRATION_TIMEOUT_MS (90UL * 1000UL)

// ---- Offline spooling (LittleFS) --------------------------------------
#define SPOOL_DIR               "/spool"
#define SPOOL_MAX_FILES         100   // oldest spooled batch dropped beyond this
// Replay pacing. The net task loops every 50ms, so without a floor here a
// batch that fails to publish is retried 20x/second -- each attempt pushing
// kilobytes at the modem over TLS, saturating the AT channel and burning data
// on a metered SIM to accomplish nothing.
#define SPOOL_REPLAY_INTERVAL_MS    2000
#define SPOOL_REPLAY_MAX_BACKOFF_MS 60000
