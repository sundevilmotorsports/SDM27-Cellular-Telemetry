#include "net_task.h"
#include "board_pins.h"
#include "telemetry_config.h"
#include "telemetry_format.h"
#include "time_sync.h"
#include "can_handler.h"

#include <PubSubClient.h>
#include <LittleFS.h>
#include <Arduino.h>
#include <algorithm>
#include <vector>

#if TELEMETRY_USE_WIFI
#include <WiFi.h>
static WiFiClient netClient;
#else
#include "gsm_modem.h"
static HardwareSerial &SerialAT = Serial1;
static TinyGsm modem(SerialAT);
static TinyGsmClient netClient(modem);
#endif

static PubSubClient mqtt(netClient);

static uint32_t s_batchSeq = 0;
static uint32_t s_mqttBackoffMs = MQTT_BACKOFF_INITIAL_MS;
static uint32_t s_lastMqttAttemptMs = 0;
static uint32_t s_lastRegCheckMs = 0;
static uint32_t s_lastBatchMs = 0;
static uint32_t s_lastTimeSyncMs = 0;
static int s_spoolFileCounter = 0;

#if TELEMETRY_USE_WIFI
// ---- WiFi bring-up --------------------------------------------------------
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

// ---- Registration / GPRS -------------------------------------------------
static bool ensureNetworkConnected() {
    if (modem.isNetworkConnected() && modem.isGprsConnected()) {
        return true;
    }
    Serial.println("[net] (re)connecting to cellular network...");
    if (strlen(TELEMETRY_GSM_PIN) && modem.getSimStatus() != 3) {
        modem.simUnlock(TELEMETRY_GSM_PIN);
    }
    if (!modem.waitForNetwork(30000)) {
        Serial.println("[net] network registration failed");
        return false;
    }
    if (!modem.gprsConnect(TELEMETRY_APN, TELEMETRY_APN_USER, TELEMETRY_APN_PASS)) {
        Serial.println("[net] GPRS/PDP context connect failed");
        return false;
    }
    Serial.println("[net] cellular network + PDP context up");
    return true;
}
#endif // TELEMETRY_USE_WIFI

static bool networkBearerUp() {
#if TELEMETRY_USE_WIFI
    return WiFi.status() == WL_CONNECTED;
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
static void replaySpooledBatches() {
    if (!mqtt.connected()) return;

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

        if (mqtt.publish(MQTT_TOPIC_TELEMETRY, payload.c_str())) {
            LittleFS.remove(path);
            replayed++;
        } else {
            Serial.println("[net] spool replay publish failed, will retry later");
            break;
        }
    }
    if (replayed > 0) {
        Serial.printf("[net] replayed %d spooled batch(es)\n", replayed);
    }
}

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
    }

#if !TELEMETRY_USE_WIFI
    modemPowerOn();
#endif
    mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);

    ensureNetworkConnected();
#if TELEMETRY_USE_WIFI
    bool timeSynced = g_timeSync.syncFromSystemClock();
#else
    bool timeSynced = g_timeSync.sync(modem);
#endif
    Serial.println(timeSynced ? "[net] initial time sync OK"
                               : "[net] initial time sync failed; will retry periodically");
    s_lastTimeSyncMs = millis();
    s_lastRegCheckMs = millis();

    for (;;) {
        uint32_t now = millis();

        if (now - s_lastRegCheckMs >= MODEM_REGISTRATION_RECHECK_MS) {
            s_lastRegCheckMs = now;
            ensureNetworkConnected();
        }

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

        maintainMqtt();
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
