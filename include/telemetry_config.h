// User-editable configuration for the telemetry PoC.
// Fill in your APN and HTTPS ingest URL before flashing. The uplink is raw
// CAN batches (JSON) POSTed to the telemetry-ingest edge function. There is no
// broker.
//
// Credentials (WiFi/APN/HTTPS) below are wrapped in #ifndef guards: copy
// .env.example to .env (git-ignored) and fill in real values there instead
// of editing them here -- load_env.py injects .env as compiler defines at
// build time, which pre-empts the placeholder #define in each guard. A
// fresh clone with no .env still builds fine using the placeholders as-is.
// An empty HTTPS_INGEST_URL still builds; every post fails until .env is set.

#pragma once

// ---- CAN source mode --------------------------------------------------
// 1 = simulated CAN frames (no hardware CAN bus required)
// 0 = real CAN frames via the ESP32-S3 TWAI controller + external transceiver
#ifndef TELEMETRY_USE_SIMULATED_CAN
#define TELEMETRY_USE_SIMULATED_CAN 1
#endif

// The simulated source sends the bench ECU pattern (include/bench_ecu_pattern.h,
// logger config ids) at 10 Hz, so the dashboard decodes it like the car.

// ---- Uplink stress test ---------------------------------------------------
// Opt-in, off by default. The burst used to publish filler over MQTT. That
// payload is not a telemetry batch, so it is not POSTed to the ingest URL
// (a Supabase table would reject it, or store garbage, and the burst would
// burn metered data doing it). Leave this 0. The net task logs once and
// skips the burst if it is set.
//
// Read this before running it on a metered SIM: at STRESS_TARGET_BPS (5
// Mbps) for STRESS_TEST_DURATION_S this is *designed* to try to move several
// MB. It will very likely fall short -- see the ceiling below -- but whatever
// it manages to push is real billed data, not a simulation.
//
// The likely ceiling is the ESP32<->modem UART: PPP carries every byte over
// it, so MODEM_TARGET_BAUDRATE (board_pins.h) / 10 is the most it can move --
// ~92 KB/s (~740 Kbps) at 921600, less a few percent of PPP/TCP/TLS framing.
// Past that, raise the baud (the handshake falls back if the modem refuses).
#ifndef TELEMETRY_STRESS_TEST
#define TELEMETRY_STRESS_TEST 0
#endif
// Bits/sec this run is being measured against (5 Mbps = the SIM7670G's Cat-1
// uplink spec ceiling). Only affects what's printed, not what's sent.
#define STRESS_TARGET_BPS (5UL * 1000UL * 1000UL)
// Filler bytes the old MQTT burst put in a "pad" field. Unused by the HTTPS
// path; kept so an existing .env entry still compiles.
#define STRESS_FILLER_BYTES 4096
// Hard cap on how long the burst runs before it stops and prints a summary,
// so a run left connected can't keep burning metered data indefinitely.
// Runs once per boot; power-cycle or reflash to run it again.
#define STRESS_TEST_DURATION_S 10
// How often to log the throughput measured so far while the burst runs.
#define STRESS_LOG_INTERVAL_MS 1000


// ---- USB direct-to-modem bench mode ---------------------------------------
// 1 = power the modem on, then stop -- the ESP32 never opens its own AT
//     session past that (no registration, no MQTT, no stress test). Leaves
//     UART1 silent so the SIM7670G's OWN USB port (the separate microUSB
//     next to the modem, wired straight to the modem chip -- not the ESP32's
//     Type-C port) is free for a PC to drive directly, e.g. with
//     usb_modem_bench.py. The point is measuring throughput without the
//     ESP32<->modem UART1 link (115200 baud, see MODEM_BAUDRATE) in the path
//     at all, instead of through PPP over UART1 (MODEM_TARGET_BAUDRATE, see
//     board_pins.h).
// 0 = off (default) -- normal operation.
// Cellular only, same reasoning as SMS_TEST_ENABLED -- build error under
// TELEMETRY_USE_WIFI=1 (see the check in net_task.cpp).
#ifndef MODEM_USB_BENCH_MODE
#define MODEM_USB_BENCH_MODE 0
#endif

// ---- Transport ----------------------------------------------------------
// 1 = WiFi (bench testing without a SIM card/cellular plan; uses the
//     ESP32-S3's onboard WiFi radio, not the SIM7670G modem)
// 0 = cellular via the onboard SIM7670G modem (the real deployment path)
#ifndef TELEMETRY_USE_WIFI
#define TELEMETRY_USE_WIFI 0
#endif

// Only used when TELEMETRY_USE_WIFI=1.
// 1 = SoftAP -- the ESP32-S3 hosts its own WiFi network (WIFI_AP_SSID/
//     _PASSWORD below); your laptop/phone joins IT. No existing WiFi network
//     needed, but there is no route to the ingest host either, so nothing is
//     POSTed. Timestamps will NOT be network-synced in this
//     mode (no internet uplink for NTP); relative inter-frame timing is
//     unaffected, see README's Known Limitations.
// 0 = station -- the ESP32-S3 joins an existing WiFi network (WIFI_SSID
//     below), same as any laptop/phone would.
#ifndef WIFI_AP_MODE
#define WIFI_AP_MODE 0
#endif

// Used when WIFI_AP_MODE=1. WPA2 requires an 8+ character password; leave
// WIFI_AP_PASSWORD empty ("") for an open (unencrypted) network instead.
#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID "esp32-telemetry"
#endif
#ifndef WIFI_AP_PASSWORD
#define WIFI_AP_PASSWORD "telemetry123"
#endif

// Used when WIFI_AP_MODE=0.
#ifndef WIFI_SSID
#define WIFI_SSID "your-wifi-ssid"
#endif
#define WIFI_CONNECT_TIMEOUT_MS 20000

// 1 = WPA2/WPA3-Enterprise (802.1X) -- networks that ask for a username AND
//     password to join directly (no browser), e.g. eduroam or a corporate
//     network. Uses PEAP/MSCHAPv2, which covers the large majority of
//     enterprise networks; EAP-TLS (client-certificate auth) is not supported.
// 0 = ordinary WPA2/WPA3-Personal -- a single shared network password.
// Only meaningful when WIFI_AP_MODE=0.
#define WIFI_ENTERPRISE 0

// Used when WIFI_AP_MODE=0 and WIFI_ENTERPRISE=0.
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "your-wifi-password"
#endif

// Used when WIFI_AP_MODE=0 and WIFI_ENTERPRISE=1. Identity can usually be
// left blank or set equal to the username -- only matters if your network's
// RADIUS server distinguishes outer/inner identity.
#ifndef WIFI_EAP_IDENTITY
#define WIFI_EAP_IDENTITY ""
#endif
#ifndef WIFI_EAP_USERNAME
#define WIFI_EAP_USERNAME "your-network-username"
#endif
#ifndef WIFI_EAP_PASSWORD
#define WIFI_EAP_PASSWORD "your-network-password"
#endif

// ---- Cellular ----------------------------------------------------------
#ifndef TELEMETRY_APN
#define TELEMETRY_APN "your.apn.here"
#endif
#ifndef TELEMETRY_APN_USER
#define TELEMETRY_APN_USER ""
#endif
#ifndef TELEMETRY_APN_PASS
#define TELEMETRY_APN_PASS ""
#endif
#ifndef TELEMETRY_GSM_PIN
#define TELEMETRY_GSM_PIN "" // SIM PIN, leave empty if none
#endif
// esp_modem device profile for the Arduino PPP library. GENERIC sticks to
// standard 3GPP commands; PPP_MODEM_SIM7600 is the closest SIMCom-specific
// profile if GENERIC misbehaves on the SIM7670G.
#define PPP_MODEM_MODEL PPP_MODEM_GENERIC
// PDP context type requested for the PPP session. T-Mobile gives this SIM an
// IPv6-only context whatever is asked for; PPP then runs over IPV6CP (enabled
// via custom_sdkconfig in platformio.ini) and reaches IPv4 hosts via NAT64.
#ifndef PPP_PDP_TYPE
#define PPP_PDP_TYPE "IPV4V6"
#endif
// How long to wait for an IP once the modem has switched to PPP data mode.
#define PPP_CONNECT_TIMEOUT_MS 30000

// ---- HTTPS ingest (raw CAN batches) -----------------------------------
// One POST is one batch: {device_id, seq, dropped_frames, frames[]} as in
// telemetry_format.h, Content-Type application/x-sdm-can-batch+json, stored
// as-is in telemetry.can_batches and decoded by the dashboard with the car
// logger's config.json.
// URL is the edge function, not PostgREST:
//   https://<project-ref>.supabase.co/functions/v1/telemetry-ingest
// Auth is HMAC-SHA256 of the raw body, sent as x-htp-signature, with the
// device id in x-htp-device. Do not send apikey, Authorization, or any
// Supabase secret. The HMAC key is the device key (TELEMETRY_HMAC_KEY), the
// same value set on the function.
//
// Sessions: the board never sends a session id. A signed-in user enters
// HTP_DEVICE_ID on the dashboard and presses Start session
// (telemetry.start_session); the function files each batch under that
// device's running session. With none running it answers 409 and the board
// holds the batch and retries (see RAW_NO_SESSION_RETRY_MS).
//
// TLS runs on the ESP32 (mbedTLS over PPP, or WiFiClientSecure on WiFi STA).
// netClient.setInsecure() means the link is encrypted but the certificate is
// not checked. setCACert() would close that gap.
//
// Put the real values in .env, not in this file. An edit without a reflash
// does nothing. Do not print the HMAC key.
//   HTTPS_INGEST_URL=https://<project-ref>.supabase.co/functions/v1/telemetry-ingest
//   TELEMETRY_HMAC_KEY=<device hmac key>
//   HTP_DEVICE_ID=esp32-sim7670g-poc-01
#ifndef HTTPS_INGEST_URL
#define HTTPS_INGEST_URL ""
#endif
#ifndef TELEMETRY_HMAC_KEY
#define TELEMETRY_HMAC_KEY ""
#endif
// The device id typed into the dashboard. 1-128 characters, no quotes or
// backslashes. The name keeps its HTP_ prefix because the header is x-htp-device.
#ifndef HTP_DEVICE_ID
#define HTP_DEVICE_ID "esp32-sim7670g-poc-01"
#endif
// How often a batch is cut from the queue. The dashboard polls about once a
// second, so 1 s keeps it fed without extra requests. A POST that takes
// longer just makes the next batch bigger.
#ifndef BATCH_WINDOW_MS
#define BATCH_WINDOW_MS 1000
#endif
// Caps on one batch. The server allows 4096 frames; the byte cap keeps one
// POST under ~1 s on the 921600-baud modem UART (~90 KB/s). JSON is ~95 bytes
// per classic frame, up to ~240 per 64-byte CAN FD frame.
#ifndef RAW_BATCH_MAX_FRAMES
#define RAW_BATCH_MAX_FRAMES 2000
#endif
#ifndef RAW_BATCH_MAX_BYTES
#define RAW_BATCH_MAX_BYTES (64 * 1024)
#endif
// CAN frames queued while a POST runs or no session is running; oldest
// dropped beyond this and reported as dropped_frames. ~88 bytes each, in
// PSRAM: 8192 frames is ~720 KB, ~40 s at 200 frames/s.
#ifndef CAN_QUEUE_DEPTH
#define CAN_QUEUE_DEPTH 8192
#endif
#define CAN_QUEUE_DEPTH_NO_PSRAM 256
// Retry period while the server says no session is running (409).
#define RAW_NO_SESSION_RETRY_MS 2000
// Backoff for 408/429/5xx/no response.
#define RAW_BACKOFF_INITIAL_MS 1000
#define RAW_BACKOFF_MAX_MS 8000
// Period of the "[raw] stats" serial line.
#define RAW_STATS_INTERVAL_MS 10000
// How long one POST, including the TLS handshake, may take before it is
// treated as a failure and retried after a backoff.
#define HTTPS_POST_TIMEOUT_MS 15000

// ---- Wall-clock (epoch) sync -----------------------------------------
#define TIME_SYNC_INTERVAL_MS (5UL * 60UL * 1000UL) // resync every 5 minutes

// ---- Link recheck -----------------------------------------------------
#define MODEM_REGISTRATION_RECHECK_MS (30UL * 1000UL)
// How long to wait for the modem to attach to the network. A cold modem on an
// unfamiliar network can spend well over 30s scanning LTE bands before it
// registers, so a short timeout reports a failure that was only slowness.
#define MODEM_REGISTRATION_TIMEOUT_MS (90UL * 1000UL)

