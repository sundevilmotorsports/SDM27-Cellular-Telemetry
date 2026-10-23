// User-editable configuration for the telemetry PoC.
// Fill in your APN and HTTPS ingest URL before flashing the real-CAN or
// cellular-connectivity paths. Batches are POSTed as JSON; there is no broker.
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

// Simulated CAN generator settings (only used when
// TELEMETRY_USE_SIMULATED_CAN=1)
// NOTE: these rates are deliberately far below a real bus's. On a metered SIM
// the generator rate, not the batching, sets your data bill: every simulated
// frame costs ~90 bytes of JSON on the wire. 100+5 Hz (the original bench
// values, fine over WiFi) works out to ~34 MB/hour. Raise these only when the
// link is unmetered.
#define SIM_CAN_FAST_ID 0x100 // e.g. wheel speed / RPM, high rate
#define SIM_CAN_FAST_RATE_HZ 1
#define SIM_CAN_SLOW_ID 0x200 // e.g. status frame, low rate
#define SIM_CAN_SLOW_RATE_HZ 1

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
//     needed, but there is no route to the ingest host either, so batches are
//     spooled and not POSTed. Timestamps will NOT be network-synced in this
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

// ---- HTTPS ingest ------------------------------------------------------
// One POST per batch. The body is the JSON from telemetryFormatBatch()
// (device_id, seq, dropped_frames, frames). For Supabase PostgREST the URL is
// https://<project-ref>.supabase.co/rest/v1/<table>
// and HTTPS_API_KEY is the publishable key (sb_publishable_...) or the legacy
// anon JWT. It is sent as the apikey header. Do not put the secret key here.
//
// TLS runs on the ESP32 (mbedTLS over PPP, or WiFiClientSecure on WiFi STA).
// netClient.setInsecure() means the link is encrypted but the certificate is
// not checked. setCACert() would close that gap.
//
// Put the real values in .env, not in this file:
//   HTTPS_INGEST_URL=https://<project-ref>.supabase.co/rest/v1/batches
//   HTTPS_API_KEY=<anon key>
#ifndef HTTPS_INGEST_URL
#define HTTPS_INGEST_URL ""
#endif
#ifndef HTTPS_API_KEY
#define HTTPS_API_KEY ""
#endif
// How long one POST, including the TLS handshake, may take before it is
// treated as a failure and the batch is spooled.
#define HTTPS_POST_TIMEOUT_MS 15000

// ---- Device identity -----------------------------------------------
#define TELEMETRY_DEVICE_ID "esp32-sim7670g-poc-01"

// ---- Batching / queue -----------------------------------------------
// A wider window amortizes the per-POST overhead (HTTP headers + a TLS
// handshake, if the server does not keep the socket) over more frames, but it
// NOT reduce the per-frame cost, so it can't rescue a high generator rate.
#define CAN_QUEUE_DEPTH 512   // frames; oldest dropped when full
#define BATCH_WINDOW_MS 15000 // how often the net task drains the queue
#define BATCH_MAX_FRAMES 60   // hard cap on frames per published batch

// ---- Wall-clock (epoch) sync -----------------------------------------
#define TIME_SYNC_INTERVAL_MS (5UL * 60UL * 1000UL) // resync every 5 minutes

// ---- Link recheck -----------------------------------------------------
#define MODEM_REGISTRATION_RECHECK_MS (30UL * 1000UL)
// How long to wait for the modem to attach to the network. A cold modem on an
// unfamiliar network can spend well over 30s scanning LTE bands before it
// registers, so a short timeout reports a failure that was only slowness.
#define MODEM_REGISTRATION_TIMEOUT_MS (90UL * 1000UL)

// ---- Offline spooling (LittleFS) --------------------------------------
#define SPOOL_DIR "/spool"
#define SPOOL_MAX_FILES 100 // oldest spooled batch dropped beyond this
// Replay pacing. The net task loops every 50ms, so without a floor here a
// batch that fails to publish is retried 20x/second -- each attempt pushing
// kilobytes at the modem over TLS, saturating the AT channel and burning data
// on a metered SIM to accomplish nothing.
#define SPOOL_REPLAY_INTERVAL_MS 2000
#define SPOOL_REPLAY_MAX_BACKOFF_MS 60000
