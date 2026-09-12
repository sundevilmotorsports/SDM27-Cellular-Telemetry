#include "net_task.h"
#include "board_pins.h"
#include "telemetry_config.h"
#include "sms_config.h"
#include "telemetry_format.h"
#include "time_sync.h"
#include "can_handler.h"

#include <PubSubClient.h>
#include <LittleFS.h>
#include <Arduino.h>
#include <algorithm>
#include <vector>

#if SMS_TEST_ENABLED && TELEMETRY_USE_WIFI
#error "SMS_TEST_ENABLED requires cellular (TELEMETRY_USE_WIFI=0) -- there is no modem to send SMS through in WiFi mode"
#endif

#if MODEM_USB_BENCH_MODE && TELEMETRY_USE_WIFI
#error "MODEM_USB_BENCH_MODE requires cellular (TELEMETRY_USE_WIFI=0) -- there is no modem in WiFi mode"
#endif

#if TELEMETRY_USE_WIFI
#include <WiFi.h>
static WiFiClient netClient;
#else
#include "gsm_modem.h"
static HardwareSerial &SerialAT = Serial1;
static TinyGsm modem(SerialAT);
#if MQTT_USE_TLS
// TLS runs over the modem's +CCH SSL channel rather than a TCP socket. SNI is
// enabled by the driver, which multi-tenant brokers like HiveMQ Cloud require
// to route to the right cluster. Certificate validation is off -- see
// MQTT_USE_TLS in telemetry_config.h.
static TinyGsmClientSecure netClient(modem);
#else
static TinyGsmClient netClient(modem);
#endif
#endif

static PubSubClient mqtt(netClient);

static uint32_t s_batchSeq = 0;
static uint32_t s_mqttBackoffMs = MQTT_BACKOFF_INITIAL_MS;
static uint32_t s_lastMqttAttemptMs = 0;
static uint32_t s_lastRegCheckMs = 0;
static uint32_t s_lastBatchMs = 0;
static uint32_t s_lastTimeSyncMs = 0;
static int s_spoolFileCounter = 0;
static uint32_t s_lastReplayMs = 0;
static uint32_t s_replayBackoffMs = SPOOL_REPLAY_INTERVAL_MS;

#if TELEMETRY_USE_WIFI
#if WIFI_AP_MODE
// ---- WiFi SoftAP bring-up ---------------------------------------------
// The ESP32-S3 hosts its own network instead of joining one. No internet
// uplink exists in this mode, so NTP time sync is skipped entirely -- only
// relative (inter-frame) timing stays accurate, per README's Known
// Limitations. MQTT_BROKER_HOST must point at whatever device (e.g. your
// laptop, once it joins this AP) is running the broker.
static bool s_apStarted = false;

static void wifiConnectBlocking() {
    if (s_apStarted) return;
    Serial.printf("[net] starting WiFi AP '%s'...\n", WIFI_AP_SSID);
    WiFi.mode(WIFI_AP);
    s_apStarted = WiFi.softAP(WIFI_AP_SSID, strlen(WIFI_AP_PASSWORD) ? WIFI_AP_PASSWORD : nullptr);
    if (s_apStarted) {
        Serial.printf("[net] AP up, IP: %s -- point MQTT_BROKER_HOST at a broker reachable "
                      "on this network (e.g. your laptop's IP once it joins)\n",
                      WiFi.softAPIP().toString().c_str());
    } else {
        Serial.println("[net] failed to start WiFi AP");
    }
}

static bool ensureNetworkConnected() {
    if (!s_apStarted) {
        wifiConnectBlocking();
    }
    return s_apStarted;
}
#else
// ---- WiFi station bring-up ---------------------------------------------
static void wifiConnectBlocking() {
    Serial.printf("[net] connecting to WiFi SSID '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
#if WIFI_ENTERPRISE
    // PEAP/MSCHAPv2 (esp_wifi_sta_wpa2_ent_* under the hood) -- covers the
    // large majority of 802.1X networks; EAP-TLS is not supported here.
    WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, WIFI_EAP_IDENTITY, WIFI_EAP_USERNAME, WIFI_EAP_PASSWORD);
#else
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[net] WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
        // Feeds TimeSync::syncFromSystemClock(); the SNTP client keeps this
        // disciplined in the background for as long as WiFi stays connected.
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    } else {
        Serial.println("[net] WiFi connect timed out; will keep retrying");
    }
}

static bool ensureNetworkConnected() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    wifiConnectBlocking();
    return WiFi.status() == WL_CONNECTED;
}
#endif // WIFI_AP_MODE
#else
// ---- Modem power-on -----------------------------------------------------
// Sequence transcribed from LilyGo's ATdebug example (examples/ATdebug/ATdebug.ino)
// for TINY_GSM_MODEM_SIM7670G: reset pulse, DTR low, then a PWRKEY pulse.
static void modemPowerOn() {
    SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

    pinMode(BOARD_LED_PIN, OUTPUT);
    digitalWrite(BOARD_LED_PIN, !LED_ON);

    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
    delay(MODEM_RESET_PULSE_WIDTH_MS);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW); // keep modem out of sleep

    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(MODEM_POWERON_PULSE_WIDTH_MS);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    Serial.println("[net] waiting for modem to respond to AT...");
    if (!modem.testAT(MODEM_START_WAIT_MS)) {
        Serial.println("[net] modem did not respond to AT in time; continuing anyway");
    }
    modem.init();
    Serial.print("[net] modem info: ");
    Serial.println(modem.getModemInfo());
}

// Prints the modem state that actually explains a registration failure. Without
// this, "network registration failed" looks identical whether the antenna is
// unplugged, the SIM is PIN-locked, the carrier is rejecting the device, or the
// modem simply needs longer to scan bands -- and those need opposite fixes.
static void logModemDiagnostics() {
    int simStatus = modem.getSimStatus();
    const char *simText = simStatus == SIM_READY   ? "ready"
                          : simStatus == SIM_LOCKED ? "LOCKED -- set TELEMETRY_GSM_PIN in .env"
                          : simStatus == SIM_ANTITHEFT_LOCKED ? "anti-theft locked"
                                                              : "error / not detected";
    Serial.printf("[net]   SIM: %s (%d)\n", simText, simStatus);

    // CSQ is 0..31, or 99 for "unknown". RSSI dBm = -113 + 2*CSQ, so anything
    // below ~10 (-93 dBm) is marginal and 99 usually means no antenna.
    int16_t csq = modem.getSignalQuality();
    if (csq == 99 || csq < 0) {
        Serial.println("[net]   signal: none (CSQ 99) -- check the antenna is connected");
    } else {
        Serial.printf("[net]   signal: CSQ %d (~%d dBm)%s\n", csq, -113 + 2 * csq,
                      csq < 10 ? " -- marginal" : "");
    }

    RegStatus reg = modem.getRegistrationStatus();
    const char *regText;
    switch (reg) {
        case REG_UNREGISTERED: regText = "not registered, not searching"; break;
        case REG_OK_HOME:      regText = "registered (home)"; break;
        case REG_SEARCHING:    regText = "searching -- may just need more time"; break;
        case REG_DENIED:       regText = "DENIED by carrier -- SIM not provisioned for this device"; break;
        case REG_OK_ROAMING:   regText = "registered (roaming)"; break;
        case REG_SMS_ONLY:     regText = "SMS only -- no data service on this SIM"; break;
        default:               regText = "unknown"; break;
    }
    Serial.printf("[net]   registration (CEREG): %s (%d)\n", regText, (int)reg);

    String op = modem.getOperator();
    Serial.printf("[net]   operator: %s\n", op.length() ? op.c_str() : "(none)");
}

// ---- Registration / GPRS -------------------------------------------------
static bool ensureNetworkConnected() {
    if (modem.isNetworkConnected() && modem.isGprsConnected()) {
        return true;
    }
    Serial.println("[net] (re)connecting to cellular network...");
    // SIM_READY, not the literal 3 -- 3 is SIM_ANTITHEFT_LOCKED (TinyGsmGPRS.tpp),
    // so the old comparison skipped the unlock only in a case that never applies.
    if (strlen(TELEMETRY_GSM_PIN) && modem.getSimStatus() != SIM_READY) {
        modem.simUnlock(TELEMETRY_GSM_PIN);
    }
    if (!modem.waitForNetwork(MODEM_REGISTRATION_TIMEOUT_MS)) {
        Serial.println("[net] network registration failed");
        logModemDiagnostics();
        return false;
    }
    if (!modem.gprsConnect(TELEMETRY_APN, TELEMETRY_APN_USER, TELEMETRY_APN_PASS)) {
        Serial.println("[net] GPRS/PDP context connect failed");
        return false;
    }
    Serial.println("[net] cellular network + PDP context up");
    return true;
}

#if SMS_TEST_ENABLED
// Runs once, the first time the modem is registered on the network --
// confirms the SIM/plan can send texts at all. Deliberately gated on
// modem.isNetworkConnected() only, not ensureNetworkConnected()'s full
// network+GPRS bar: AT+CMGS goes out over plain network registration and
// doesn't need the PDP context that GPRS/MQTT requires.
static void runSmsTest() {
    static bool done = false;
    if (done || !modem.isNetworkConnected()) return;
    done = true;

    if (!strlen(SMS_TEST_NUMBER)) {
        Serial.println("[sms] SMS_TEST_NUMBER is empty; skipping test send");
        return;
    }

    Serial.printf("[sms] sending test SMS to %s...\n", SMS_TEST_NUMBER);
    if (modem.sendSMS(SMS_TEST_NUMBER, SMS_TEST_MESSAGE)) {
        Serial.println("[sms] test SMS sent");
    } else {
        Serial.println("[sms] test SMS send failed");
    }
}
#endif
#endif // TELEMETRY_USE_WIFI

static bool networkBearerUp() {
#if TELEMETRY_USE_WIFI
#if WIFI_AP_MODE
    return s_apStarted;
#else
    return WiFi.status() == WL_CONNECTED;
#endif
#else
    return modem.isGprsConnected();
#endif
}

// ---- MQTT with capped exponential backoff --------------------------------
static bool mqttConnectAttempt() {
    char clientId[48];
    snprintf(clientId, sizeof(clientId), "%s%04X", MQTT_CLIENT_ID_PREFIX,
              (unsigned)(ESP.getEfuseMac() & 0xFFFF));

    bool ok = strlen(MQTT_USERNAME)
                  ? mqtt.connect(clientId, MQTT_USERNAME, MQTT_PASSWORD)
                  : mqtt.connect(clientId);
    return ok;
}

static void maintainMqtt() {
    if (mqtt.connected()) {
        mqtt.loop();
        return;
    }
    uint32_t now = millis();
    if (now - s_lastMqttAttemptMs < s_mqttBackoffMs) {
        return;
    }
    s_lastMqttAttemptMs = now;

    if (!networkBearerUp()) {
        return; // no bearer yet; connection maintenance handles this separately
    }

    Serial.printf("[net] MQTT connect attempt (backoff was %lu ms)...\n",
                  (unsigned long)s_mqttBackoffMs);
    if (mqttConnectAttempt()) {
        Serial.println("[net] MQTT connected");
        s_mqttBackoffMs = MQTT_BACKOFF_INITIAL_MS;
    } else {
        Serial.printf("[net] MQTT connect failed, rc=%d\n", mqtt.state());
        s_mqttBackoffMs = std::min<uint32_t>(s_mqttBackoffMs * 2, MQTT_BACKOFF_MAX_MS);
    }
}

// ---- Offline spooling (LittleFS) -----------------------------------------
// Resume numbering above the highest spool file already on disk. LittleFS
// survives reboots and reflashes, but this counter did not: it restarted at 0
// every boot, so a new run overwrote the batches a previous run had spooled --
// silently destroying exactly the data spooling exists to preserve, and
// leaving a mix of old and new files sharing the same names.
static void initSpoolCounter() {
    File dir = LittleFS.open(SPOOL_DIR);
    if (!dir || !dir.isDirectory()) return;

    int maxIdx = -1;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        String name(f.name());
        int slash = name.lastIndexOf('/');
        if (slash >= 0) {
            name = name.substring(slash + 1);  // some cores report a full path
        }
        int idx = name.toInt();  // "0000000004.json" -> 4
        if (idx > maxIdx) maxIdx = idx;
    }
    s_spoolFileCounter = maxIdx + 1;
    if (s_spoolFileCounter > 0) {
        Serial.printf("[net] resuming spool numbering at %d\n", s_spoolFileCounter);
    }
}

static void spoolBatch(const String &json) {
    // Drop-oldest cap: if the spool directory is full, delete the oldest
    // file before writing a new one, mirroring the in-memory queue's policy.
    File dir = LittleFS.open(SPOOL_DIR);
    if (dir && dir.isDirectory()) {
        std::vector<String> names;
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            names.push_back(String(f.name()));
        }
        if ((int)names.size() >= SPOOL_MAX_FILES) {
            std::sort(names.begin(), names.end());
            LittleFS.remove(String(SPOOL_DIR) + "/" + names.front());
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%010d.json", SPOOL_DIR, s_spoolFileCounter++);
    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.println("[net] failed to open spool file for write");
        return;
    }
    f.print(json);
    f.close();
    Serial.printf("[net] spooled batch to %s\n", path);
}

// Replays a bounded number of spooled batches per call so this never starves
// live traffic if a large backlog has built up.
//
// Rate-limited with backoff. Without it this runs on every pass of the net
// task's 50ms loop, so a batch that cannot publish is retried ~20x/second --
// each attempt shoving kilobytes at the modem over TLS. That saturates the AT
// channel, starves live publishes, and on a metered SIM costs real money to
// achieve nothing.
static void replaySpooledBatches() {
    if (!mqtt.connected()) return;

    uint32_t now = millis();
    if (now - s_lastReplayMs < s_replayBackoffMs) return;
    s_lastReplayMs = now;

    File dir = LittleFS.open(SPOOL_DIR);
    if (!dir || !dir.isDirectory()) return;

    std::vector<String> names;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        names.push_back(String(f.name()));
    }
    std::sort(names.begin(), names.end());

    const int maxPerCall = 5;
    int replayed = 0;
    for (const String &name : names) {
        if (replayed >= maxPerCall || !mqtt.connected()) break;
        String path = String(SPOOL_DIR) + "/" + name;
        File f = LittleFS.open(path, "r");
        if (!f) continue;
        String payload = f.readString();
        f.close();

        // A batch larger than the MQTT buffer can never publish -- PubSubClient
        // rejects it before touching the network. LittleFS survives reflashing,
        // so a file spooled under an older, larger BATCH_MAX_FRAMES outlives the
        // config that produced it. Retrying is futile and, because the loop
        // below breaks on failure, one such file blocks every good batch behind
        // it forever. Drop it and say so loudly.
        if (payload.length() + strlen(MQTT_TOPIC_TELEMETRY) + 16 > MQTT_BUFFER_SIZE) {
            Serial.printf("[net] discarding %s: %u bytes exceeds MQTT buffer (%u) -- "
                          "spooled under a previous config, can never publish\n",
                          path.c_str(), (unsigned)payload.length(),
                          (unsigned)MQTT_BUFFER_SIZE);
            LittleFS.remove(path);
            continue;
        }

        if (mqtt.publish(MQTT_TOPIC_TELEMETRY, payload.c_str())) {
            LittleFS.remove(path);
            replayed++;
            s_replayBackoffMs = SPOOL_REPLAY_INTERVAL_MS;
        } else {
            // Back off rather than spin: the failure is almost always the modem
            // being busy, and retrying immediately guarantees it still is.
            s_replayBackoffMs = std::min<uint32_t>(s_replayBackoffMs * 2,
                                                   SPOOL_REPLAY_MAX_BACKOFF_MS);
            Serial.printf("[net] spool replay failed for %s (%u bytes, mqtt state=%d), "
                          "retry in %lu ms\n",
                          path.c_str(), (unsigned)payload.length(), mqtt.state(),
                          (unsigned long)s_replayBackoffMs);
            break;
        }
    }
    if (replayed > 0) {
        Serial.printf("[net] replayed %d spooled batch(es)\n", replayed);
    }
}

// ---- Uplink stress test ---------------------------------------------------
#if TELEMETRY_STRESS_TEST
// Runs once, the first time MQTT is connected after boot: publishes fixed
// filler payloads back-to-back for STRESS_TEST_DURATION_S, through the same
// mqtt.publish() -> PubSubClient -> TLS -> AT+CCHSEND path real batches use,
// and logs achieved throughput against STRESS_TARGET_BPS. Blocks the net
// task for the duration -- deliberate, so measured throughput reflects only
// this path's own speed, not this loop's normal 50ms tick.
static void runStressTest() {
    static bool done = false;
    if (done || !mqtt.connected()) return;
    done = true;

    String filler;
    filler.reserve(STRESS_FILLER_BYTES);
    for (uint32_t i = 0; i < STRESS_FILLER_BYTES; i++) {
        filler += char('a' + (i % 26));
    }

    Serial.printf("[stress] starting %us burst on '%s', target %lu bps, "
                  "~%u bytes/publish -- this spends real cellular data\n",
                  (unsigned)STRESS_TEST_DURATION_S, MQTT_TOPIC_STRESS,
                  (unsigned long)STRESS_TARGET_BPS, (unsigned)STRESS_FILLER_BYTES);

    uint32_t startMs = millis();
    uint32_t lastLogMs = startMs;
    uint32_t windowBytes = 0;
    uint32_t totalBytes = 0;
    uint32_t seq = 0;
    // Each mqtt.publish() call is chunked to MQTT_MAX_TRANSFER_SIZE bytes,
    // and every chunk is one synchronous AT+CCHSEND round-trip (send AT cmd
    // -> wait for '>' -> write bytes -> wait for the modem's confirmation)
    // before the next chunk can start. Timing publish() itself and dividing
    // by the chunk count it implied tells us whether that round-trip's fixed
    // AT/modem/network turnaround dominates, or the ~22ms/chunk that raw
    // serial transfer of 255 bytes takes at 115200 baud does -- i.e. whether
    // raising MODEM_BAUDRATE would actually help, or whether the fix is
    // fewer, larger chunks instead (which this PubSubClient version can't do
    // -- see MQTT_MAX_TRANSFER_SIZE's comment in platformio.ini).
    uint32_t windowPublishMs = 0;
    uint32_t windowChunks = 0;
    uint64_t totalPublishMs = 0;
    uint32_t totalChunks = 0;

    while (millis() - startMs < STRESS_TEST_DURATION_S * 1000UL) {
        if (!mqtt.connected()) {
            Serial.println("[stress] MQTT dropped mid-burst, stopping early");
            break;
        }

        String json;
        json.reserve(STRESS_FILLER_BYTES + 96);
        json = "{\"device_id\":\"" TELEMETRY_DEVICE_ID "\",\"stress_seq\":";
        json += seq++;
        json += ",\"pad\":\"";
        json += filler;
        json += "\"}";

        uint32_t pubStartMs = millis();
        bool ok = mqtt.publish(MQTT_TOPIC_STRESS, json.c_str());
        uint32_t pubMs = millis() - pubStartMs;
        if (ok) {
            uint32_t chunks = (json.length() + MQTT_MAX_TRANSFER_SIZE - 1) / MQTT_MAX_TRANSFER_SIZE;
            windowBytes += json.length();
            totalBytes += json.length();
            windowPublishMs += pubMs;
            windowChunks += chunks;
            totalPublishMs += pubMs;
            totalChunks += chunks;
        }
        mqtt.loop();
        yield(); // avoid starving the core-1 watchdog across a tight loop

        uint32_t now = millis();
        if (now - lastLogMs >= STRESS_LOG_INTERVAL_MS) {
            uint32_t windowMs = now - lastLogMs;
            uint32_t bps = (uint32_t)((uint64_t)windowBytes * 8000ULL / windowMs);
            uint32_t msPerChunkX10 =
                windowChunks > 0 ? (uint32_t)((uint64_t)windowPublishMs * 10 / windowChunks) : 0;
            Serial.printf("[stress] %lu bps (target %lu), %lu.%lu ms/AT+CCHSEND chunk, "
                          "%lu bytes so far\n",
                          (unsigned long)bps, (unsigned long)STRESS_TARGET_BPS,
                          (unsigned long)(msPerChunkX10 / 10), (unsigned long)(msPerChunkX10 % 10),
                          (unsigned long)totalBytes);
            windowBytes = 0;
            windowPublishMs = 0;
            windowChunks = 0;
            lastLogMs = now;
        }
    }

    uint32_t elapsedMs = millis() - startMs;
    uint32_t avgBps = elapsedMs > 0
                           ? (uint32_t)((uint64_t)totalBytes * 8000ULL / elapsedMs)
                           : 0;
    uint32_t avgMsPerChunkX10 =
        totalChunks > 0 ? (uint32_t)(totalPublishMs * 10 / totalChunks) : 0;
    Serial.printf("[stress] done: %lu bytes over %lu ms, avg %lu bps (target %lu bps)\n",
                  (unsigned long)totalBytes, (unsigned long)elapsedMs,
                  (unsigned long)avgBps, (unsigned long)STRESS_TARGET_BPS);
    Serial.printf("[stress] avg %lu.%lu ms per %d-byte AT+CCHSEND chunk over %lu chunks -- "
                  "~22ms of that is pure serial transfer at 115200 baud; the rest is AT "
                  "command / modem / network round-trip overhead\n",
                  (unsigned long)(avgMsPerChunkX10 / 10), (unsigned long)(avgMsPerChunkX10 % 10),
                  (int)MQTT_MAX_TRANSFER_SIZE, (unsigned long)totalChunks);
    Serial.println("[stress] resuming normal telemetry publishing");
}
#endif

// ---- Batch drain + publish ------------------------------------------------
static void drainAndPublish() {
    TelemetryBatch batch;
    batch.seq = s_batchSeq++;
    batch.dropped_frames = canHandlerTakeDroppedCount();

    CanFrame f;
    while ((int)batch.frames.size() < BATCH_MAX_FRAMES &&
           xQueueReceive(g_canQueue, &f, 0) == pdTRUE) {
        batch.frames.push_back(f);
    }

    if (batch.frames.empty() && batch.dropped_frames == 0) {
        return; // nothing to report this window
    }

    String json = telemetryFormatBatch(batch, TELEMETRY_DEVICE_ID, timeSyncEpochMsFor);

    if (mqtt.connected() && mqtt.publish(MQTT_TOPIC_TELEMETRY, json.c_str())) {
        Serial.printf("[net] published batch seq=%lu frames=%u dropped=%u\n",
                      (unsigned long)batch.seq, (unsigned)batch.frames.size(),
                      (unsigned)batch.dropped_frames);
    } else {
        Serial.printf("[net] publish failed (connected=%d), spooling seq=%lu\n",
                      mqtt.connected(), (unsigned long)batch.seq);
        spoolBatch(json);
    }
}

// ---- Task ------------------------------------------------------------
static void netTask(void *) {
    if (!LittleFS.begin(true)) {
        Serial.println("[net] LittleFS mount/format failed; spooling disabled");
    } else if (!LittleFS.exists(SPOOL_DIR)) {
        LittleFS.mkdir(SPOOL_DIR);
    } else {
        initSpoolCounter();
    }

#if !TELEMETRY_USE_WIFI
    modemPowerOn();
#if MODEM_USB_BENCH_MODE
    Serial.println("[net] MODEM_USB_BENCH_MODE=1 -- modem is powered on and idle, "
                    "UART1 will stay silent from here on. Drive it over its own "
                    "USB port instead (see usb_modem_bench.py).");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
#endif
    mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);

    ensureNetworkConnected();
#if TELEMETRY_USE_WIFI && WIFI_AP_MODE
    Serial.println("[net] AP mode has no internet uplink; skipping NTP time sync "
                    "(inter-frame relative timing is unaffected)");
#else
#if TELEMETRY_USE_WIFI
    bool timeSynced = g_timeSync.syncFromSystemClock();
#else
    bool timeSynced = g_timeSync.sync(modem);
#endif
    Serial.println(timeSynced ? "[net] initial time sync OK"
                               : "[net] initial time sync failed; will retry periodically");
#endif
    s_lastTimeSyncMs = millis();
    s_lastRegCheckMs = millis();

    for (;;) {
        uint32_t now = millis();

        if (now - s_lastRegCheckMs >= MODEM_REGISTRATION_RECHECK_MS) {
            s_lastRegCheckMs = now;
            ensureNetworkConnected();
        }

#if !TELEMETRY_USE_WIFI && SMS_TEST_ENABLED
        runSmsTest(); // no-ops after its first (and only) run
#endif

#if !(TELEMETRY_USE_WIFI && WIFI_AP_MODE)
        if (now - s_lastTimeSyncMs >= TIME_SYNC_INTERVAL_MS) {
            s_lastTimeSyncMs = now;
#if TELEMETRY_USE_WIFI
            if (!g_timeSync.syncFromSystemClock()) {
#else
            if (!g_timeSync.sync(modem)) {
#endif
                Serial.println("[net] periodic time sync failed");
            }
        }
#endif

        maintainMqtt();
#if TELEMETRY_STRESS_TEST
        runStressTest(); // no-ops after its first (and only) run
#endif
        replaySpooledBatches();

        if (now - s_lastBatchMs >= BATCH_WINDOW_MS) {
            s_lastBatchMs = now;
            drainAndPublish();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void netTaskStart() {
    xTaskCreatePinnedToCore(netTask, "net_task", 8192, nullptr, tskIDLE_PRIORITY + 2,
                             nullptr, 1);
}
