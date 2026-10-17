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
// The modem is only a PPP bearer: TCP runs in the ESP32's lwIP stack and TLS
// in mbedTLS, so a publish is one socket write instead of one AT+CCHSEND /
// AT+CIPSEND round-trip per ~1.4KB chunk.
#include <PPP.h>
#include <esp_netif_ppp.h>
#include <esp_modem_api.h>
static HardwareSerial &SerialAT = Serial1;
#if MQTT_USE_TLS
#include <NetworkClientSecure.h>
static NetworkClientSecure netClient;
#else
#include <NetworkClient.h>
static NetworkClient netClient;
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
// for the SIM7670G: reset pulse, DTR low, then a PWRKEY pulse. Also used to
// recover a wedged PPP session: the reset pulse drops the modem out of CMUX.
static void modemPowerPins() {
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
}

// Minimal AT exchange on Serial1, used only before PPP owns the UART.
static bool rawAt(const char *cmd, uint32_t timeoutMs) {
    while (SerialAT.available()) SerialAT.read();
    SerialAT.print(cmd);
    SerialAT.print("\r");
    String resp;
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        while (SerialAT.available()) {
            resp += (char)SerialAT.read();
            if (resp.endsWith("OK\r\n")) return true;
            if (resp.endsWith("ERROR\r\n")) return false;
        }
        delay(2);
    }
    return false;
}

static bool probeAt(uint32_t baud, uint32_t timeoutMs) {
    SerialAT.updateBaudRate(baud);
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (rawAt("AT", 300)) return true;
    }
    return false;
}

// A cold SIM7670G takes several seconds to answer AT. Both rates are tried
// because SIMCom modems may persist AT+IPR across resets.
static uint32_t detectModemBaud(uint32_t timeoutMs) {
    const uint32_t bauds[] = {MODEM_BAUDRATE, MODEM_TARGET_BAUDRATE};
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        for (uint32_t b : bauds) {
            if (probeAt(b, 300)) return b;
        }
    }
    return 0;
}

// Gets the modem talking at MODEM_TARGET_BAUDRATE if it can and returns the
// baud it ended up at, or 0 if it never answered. The baud is the hard ceiling
// on throughput under PPP, so this matters more than it did over AT sockets.
static uint32_t modemUartHandshake() {
    SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    Serial.println("[net] waiting for modem to respond to AT...");

    uint32_t baud = detectModemBaud(15000);
    if (baud == 0) {
        Serial.println("[net] modem did not respond to AT at either baud");
        SerialAT.end();
        return 0;
    }

    if (baud != MODEM_TARGET_BAUDRATE) {
        char cmd[24];
        snprintf(cmd, sizeof(cmd), "AT+IPR=%lu", (unsigned long)MODEM_TARGET_BAUDRATE);
        if (rawAt(cmd, 1000) && probeAt(MODEM_TARGET_BAUDRATE, 1000)) {
            baud = MODEM_TARGET_BAUDRATE;
        } else {
            // The modem may have switched even though we can't hear it; tell
            // it to come back at the new rate, then listen at the old one.
            SerialAT.updateBaudRate(MODEM_TARGET_BAUDRATE);
            snprintf(cmd, sizeof(cmd), "AT+IPR=%lu", (unsigned long)MODEM_BAUDRATE);
            rawAt(cmd, 300);
            SerialAT.updateBaudRate(MODEM_BAUDRATE);
            Serial.printf("[net] modem would not run at %lu baud, staying at %lu\n",
                          (unsigned long)MODEM_TARGET_BAUDRATE, (unsigned long)MODEM_BAUDRATE);
        }
    }
    Serial.printf("[net] modem UART at %lu baud\n", (unsigned long)baud);
    SerialAT.end(); // PPP installs its own UART driver on the same port
    return baud;
}

// Prints the modem state that actually explains a registration failure. Without
// this, "network registration failed" looks identical whether the antenna is
// unplugged, the SIM is PIN-locked, the carrier is rejecting the device, or the
// modem simply needs longer to scan bands -- and those need opposite fixes.
static void logModemDiagnostics() {
    String sim = PPP.cmd("AT+CPIN?", 1000);
    sim.trim();
    Serial.printf("[net]   SIM: %s\n", sim.length() ? sim.c_str() : "no answer -- SIM missing?");

    // CSQ is 0..31, or 99 for "unknown". RSSI dBm = -113 + 2*CSQ, so anything
    // below ~10 (-93 dBm) is marginal and 99 usually means no antenna.
    int csq = PPP.RSSI();
    if (csq == 99 || csq < 0) {
        Serial.println("[net]   signal: none (CSQ 99) -- check the antenna is connected");
    } else {
        Serial.printf("[net]   signal: CSQ %d (~%d dBm)%s\n", csq, -113 + 2 * csq,
                      csq < 10 ? " -- marginal" : "");
    }

    String cereg = PPP.cmd("AT+CEREG?", 1000);
    int n = -1, stat = -1;
    int idx = cereg.indexOf("+CEREG:");
    if (idx >= 0) sscanf(cereg.c_str() + idx, "+CEREG: %d,%d", &n, &stat);
    const char *regText;
    switch (stat) {
        case 0:  regText = "not registered, not searching"; break;
        case 1:  regText = "registered (home)"; break;
        case 2:  regText = "searching -- may just need more time"; break;
        case 3:  regText = "DENIED by carrier -- SIM not provisioned for this device"; break;
        case 5:  regText = "registered (roaming)"; break;
        case 6:  regText = "SMS only -- no data service on this SIM"; break;
        default: regText = "unknown"; break;
    }
    Serial.printf("[net]   registration (CEREG): %s (%d)\n", regText, stat);

    String op = PPP.operatorName();
    Serial.printf("[net]   operator: %s\n", op.length() ? op.c_str() : "(none)");
}

// ---- Registration / PPP --------------------------------------------------
// PPP runs in CMUX mode: one virtual channel carries PPP data, the other stays
// in AT command mode, so diagnostics and SMS keep working while the link is up.
static bool s_pppStarted = false;
static bool s_sntpStarted = false;

// +CGCONTRDP prints addresses as dot-separated decimal bytes: 4 for IPv4,
// 16 for IPv6.
static bool parseModemAddr(const String &s, esp_ip_addr_t &out) {
    uint8_t b[16];
    int n = 0;
    const char *p = s.c_str();
    while (n < 16) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 255) return false;
        b[n++] = (uint8_t)v;
        p = end;
        if (*p != '.') break;
        p++;
    }
    if (*p) return false;
    memset(&out, 0, sizeof(out));
    if (n == 4) {
        out.type = ESP_IPADDR_TYPE_V4;
        memcpy(&out.u_addr.ip4.addr, b, 4);
        return true;
    }
    if (n == 16) {
        out.type = ESP_IPADDR_TYPE_V6;
        memcpy(out.u_addr.ip6.addr, b, 16);
        return true;
    }
    return false;
}

// lwIP only learns DNS servers over PPP from IPCP, which never completes on an
// IPv6-only context. Take the carrier's servers from the modem instead: on
// T-Mobile they are DNS64, so IPv4-only hosts like the broker come back as
// NAT64 addresses reachable over IPv6.
static void configureDnsFromModem() {
    String r = PPP.cmd("AT+CGCONTRDP=1", 2000);
    // +CGCONTRDP: <cid>,<bearer>,"<apn>","<addr mask>","<gw>","<dns1>","<dns2>",...
    std::vector<String> quoted;
    int line = r.indexOf("+CGCONTRDP:");
    int lineEnd = line >= 0 ? r.indexOf('\n', line) : -1;
    String first = line >= 0 ? r.substring(line, lineEnd >= 0 ? lineEnd : r.length()) : "";
    for (int q = first.indexOf('"'); q >= 0;) {
        int e = first.indexOf('"', q + 1);
        if (e < 0) break;
        quoted.push_back(first.substring(q + 1, e));
        q = first.indexOf('"', e + 1);
    }

    int set = 0;
    for (size_t i = 3; i < quoted.size() && set < 2; i++) {
        esp_netif_dns_info_t dns;
        if (!parseModemAddr(quoted[i], dns.ip)) continue;
        esp_netif_set_dns_info(PPP.netif(), set == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &dns);
        Serial.printf("[net] DNS %d from modem: %s\n", set + 1, quoted[i].c_str());
        set++;
    }
    if (set == 0) {
        // Google's public DNS64, used only if the modem reports nothing.
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V6;
        const uint8_t g[16] = {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x64};
        memcpy(dns.ip.u_addr.ip6.addr, g, 16);
        esp_netif_set_dns_info(PPP.netif(), ESP_NETIF_DNS_MAIN, &dns);
        Serial.println("[net] modem reported no DNS; using 2001:4860:4860::6464");
    }
}

// lwIP's SNTP resolves names IPv4-first, which fails on an IPv6-only link.
// Network.hostByName() prefers AAAA once a global IPv6 exists, so resolve
// here and hand SNTP a literal address.
static void startSntp() {
    static char ntpAddr[48]; // SNTP keeps the pointer
    IPAddress ip;
    if (!Network.hostByName("pool.ntp.org", ip)) {
        Serial.println("[net] could not resolve pool.ntp.org; will retry");
        return;
    }
    strlcpy(ntpAddr, ip.toString().c_str(), sizeof(ntpAddr));
    configTime(0, 0, ntpAddr);
    s_sntpStarted = true;
}

static bool pppStart() {
    uint32_t baud = modemUartHandshake();
    if (baud == 0) return false;

    PPP.setApn(TELEMETRY_APN);
    if (strlen(TELEMETRY_GSM_PIN)) PPP.setPin(TELEMETRY_GSM_PIN);
    PPP.setPins(MODEM_TX_PIN, MODEM_RX_PIN); // no RTS/CTS routed on this board
    if (!PPP.begin(PPP_MODEM_MODEL, 1, baud)) {
        Serial.println("[net] PPP.begin failed (modem sync)");
        return false;
    }
    Serial.printf("[net] modem: %s, IMEI %s\n", PPP.moduleName().c_str(), PPP.IMEI().c_str());

    // Off by default in esp-netif; without them a failed PPP negotiation
    // shows up only as "no IP", with no hint which phase died.
    esp_netif_ppp_config_t pppCfg = {};
    pppCfg.ppp_phase_event_enabled = true;
    pppCfg.ppp_error_event_enabled = true;
    esp_netif_ppp_set_params(PPP.netif(), &pppCfg);

    esp_modem_PdpContext_t pdp = {1, PPP_PDP_TYPE, TELEMETRY_APN};
    if (esp_modem_set_pdp_context(PPP.handle(), &pdp) != ESP_OK) {
        Serial.printf("[net] modem rejected PDP type %s\n", PPP_PDP_TYPE);
    }
    return true;
}

static bool ensureNetworkConnected() {
    if (PPP.connected()) return true;

    if (s_pppStarted && PPP.mode() == ESP_MODEM_MODE_CMUX) {
        // Data channel was up and has since dropped. Restarting from a hard
        // reset is the only recovery that doesn't depend on the modem leaving
        // CMUX cleanly.
        Serial.println("[net] PPP link lost; resetting modem");
        PPP.end();
        s_pppStarted = false;
        modemPowerPins();
    }

    Serial.println("[net] (re)connecting to cellular network...");
    if (!s_pppStarted) {
        s_pppStarted = pppStart();
        if (!s_pppStarted) return false;
    }

    uint32_t start = millis();
    while (!PPP.attached()) {
        if (millis() - start >= MODEM_REGISTRATION_TIMEOUT_MS) {
            Serial.println("[net] network registration failed");
            logModemDiagnostics();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!PPP.mode(ESP_MODEM_MODE_CMUX)) {
        Serial.println("[net] failed to enter CMUX/PPP data mode");
        return false;
    }
    if (!PPP.waitStatusBits(ESP_NETIF_CONNECTED_BIT, PPP_CONNECT_TIMEOUT_MS)) {
        Serial.println("[net] PPP did not get an IP address");
        // CMUX keeps an AT channel open, so the modem can say why the PDP
        // context behind the PPP session didn't come up.
        for (const char *at : {"AT+CGDCONT?", "AT+CGACT?", "AT+CGPADDR", "AT+CEER"}) {
            String r = PPP.cmd(at, 2000);
            r.trim();
            r.replace("\r\n", " | ");
            Serial.printf("[net]   %s -> %s\n", at, r.c_str());
        }
        return false;
    }
    // IPv6 global addresses arrive by router advertisement after the link is up.
    if (!PPP.waitStatusBits(ESP_NETIF_HAS_IP_BIT | ESP_NETIF_HAS_GLOBAL_IP6_BIT, 15000)) {
        Serial.println("[net] PPP link up but no routable address arrived");
        return false;
    }
    configureDnsFromModem();
    String addr = PPP.hasIP() ? "IPv4 " + PPP.localIP().toString()
                              : "IPv6 " + PPP.globalIPv6().toString();
    Serial.printf("[net] PPP up, %s\n", addr.c_str());
    if (!s_sntpStarted) startSntp();
    return true;
}

#if SMS_TEST_ENABLED
// Runs once, the first time the modem is registered on the network --
// confirms the SIM/plan can send texts at all. Gated on registration only,
// not the PPP data link: AT+CMGS goes out over plain network registration.
static void runSmsTest() {
    static bool done = false;
    if (done || !s_pppStarted || !PPP.attached()) return;
    done = true;

    if (!strlen(SMS_TEST_NUMBER)) {
        Serial.println("[sms] SMS_TEST_NUMBER is empty; skipping test send");
        return;
    }

    Serial.printf("[sms] sending test SMS to %s...\n", SMS_TEST_NUMBER);
    if (PPP.sms(SMS_TEST_NUMBER, SMS_TEST_MESSAGE)) {
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
    return PPP.connected();
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

// ---- Raw MQTT PUBLISH framing ---------------------------------------------
// Builds the whole PUBLISH packet and hands it to the socket in one write(),
// so TLS gets full-size records and lwIP full-size segments. PubSubClient's
// publish() would instead copy through its own buffer and split the write.
static void mqttEncodeString(std::vector<uint8_t> &out, const char *s) {
    uint16_t len = (uint16_t)strlen(s);
    out.push_back((uint8_t)(len >> 8));
    out.push_back((uint8_t)(len & 0xFF));
    out.insert(out.end(), s, s + len);
}

static void mqttEncodeRemainingLength(std::vector<uint8_t> &out, size_t n) {
    do {
        uint8_t b = n % 128;
        n /= 128;
        if (n > 0) b |= 0x80;
        out.push_back(b);
    } while (n > 0);
}

static bool rawMqttPublish(const char *topic, const char *payload, size_t payloadLen) {
    std::vector<uint8_t> varHeader;
    mqttEncodeString(varHeader, topic);

    std::vector<uint8_t> packet;
    packet.reserve(5 + varHeader.size() + payloadLen);
    packet.push_back(0x30); // PUBLISH, QoS 0, no DUP/RETAIN
    mqttEncodeRemainingLength(packet, varHeader.size() + payloadLen);
    packet.insert(packet.end(), varHeader.begin(), varHeader.end());
    packet.insert(packet.end(), payload, payload + payloadLen);

    return netClient.write(packet.data(), packet.size()) == packet.size();
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

        if (rawMqttPublish(MQTT_TOPIC_TELEMETRY, payload.c_str(), payload.length())) {
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
    uint32_t windowPublishMs = 0;
    uint32_t windowPublishes = 0;
    uint64_t totalPublishMs = 0;
    uint32_t totalPublishes = 0;

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
        bool ok = rawMqttPublish(MQTT_TOPIC_STRESS, json.c_str(), json.length());
        uint32_t pubMs = millis() - pubStartMs;
        if (ok) {
            windowBytes += json.length();
            totalBytes += json.length();
            windowPublishMs += pubMs;
            windowPublishes++;
            totalPublishMs += pubMs;
            totalPublishes++;
        }
        mqtt.loop();
        yield(); // avoid starving the core-1 watchdog across a tight loop

        uint32_t now = millis();
        if (now - lastLogMs >= STRESS_LOG_INTERVAL_MS) {
            uint32_t windowMs = now - lastLogMs;
            uint32_t bps = (uint32_t)((uint64_t)windowBytes * 8000ULL / windowMs);
            uint32_t msPerPub = windowPublishes > 0 ? windowPublishMs / windowPublishes : 0;
            Serial.printf("[stress] %lu bps (target %lu), %lu ms/publish, %lu bytes so far\n",
                          (unsigned long)bps, (unsigned long)STRESS_TARGET_BPS,
                          (unsigned long)msPerPub, (unsigned long)totalBytes);
            windowBytes = 0;
            windowPublishMs = 0;
            windowPublishes = 0;
            lastLogMs = now;
        }
    }

    uint32_t elapsedMs = millis() - startMs;
    uint32_t avgBps = elapsedMs > 0
                           ? (uint32_t)((uint64_t)totalBytes * 8000ULL / elapsedMs)
                           : 0;
    uint32_t avgMsPerPub = totalPublishes > 0 ? (uint32_t)(totalPublishMs / totalPublishes) : 0;
    Serial.printf("[stress] done: %lu bytes over %lu ms, avg %lu bps (target %lu bps), "
                  "%lu publishes at %lu ms each\n",
                  (unsigned long)totalBytes, (unsigned long)elapsedMs,
                  (unsigned long)avgBps, (unsigned long)STRESS_TARGET_BPS,
                  (unsigned long)totalPublishes, (unsigned long)avgMsPerPub);

    // Publish the final summary report over MQTT so it displays directly in the listener terminal
    String summaryJson = "{\"stress_summary\":true,\"total_bytes\":";
    summaryJson += totalBytes;
    summaryJson += ",\"duration_ms\":";
    summaryJson += elapsedMs;
    summaryJson += ",\"avg_bps\":";
    summaryJson += avgBps;
    summaryJson += ",\"avg_kbps\":";
    summaryJson += (avgBps / 1000);
    summaryJson += ",\"ms_per_publish\":";
    summaryJson += avgMsPerPub;
    summaryJson += "}";
    rawMqttPublish(MQTT_TOPIC_STRESS, summaryJson.c_str(), summaryJson.length());

    Serial.println("[stress] resuming normal telemetry publishing");
}
#endif

// ---- Batch drain + publish ------------------------------------------------
// Builds and publishes one batch of at most maxFrames frames, also capped by
// BATCH_MAX_FRAMES and by serialized size: replaySpooledBatches() discards
// any spooled batch larger than MQTT_BUFFER_SIZE, and with 64-byte CAN FD
// frames a frame-count cap alone would overshoot it ~2x. Returns the number
// of frames taken off the queue, or -1 if the publish failed (batch spooled).
static int publishOneBatch(int maxFrames) {
    // Batch header (device_id, seq, dropped_frames, brackets) plus the
    // topic and the replay check's own margin.
    static const size_t kBudget = MQTT_BUFFER_SIZE - strlen(MQTT_TOPIC_TELEMETRY) - 16 -
                                  strlen(TELEMETRY_DEVICE_ID) - 96;

    TelemetryBatch batch;
    batch.dropped_frames = canHandlerTakeDroppedCount();

    size_t bytes = 0;
    CanFrame f;
    while ((int)batch.frames.size() < std::min(maxFrames, BATCH_MAX_FRAMES) &&
           xQueuePeek(g_canQueue, &f, 0) == pdTRUE) {
        size_t frameBytes = telemetryFrameJsonMaxBytes(f);
        if (!batch.frames.empty() && bytes + frameBytes > kBudget) break;
        xQueueReceive(g_canQueue, &f, 0); // sole consumer: this is the frame just peeked
        batch.frames.push_back(f);
        bytes += frameBytes;
    }

    if (batch.frames.empty() && batch.dropped_frames == 0) {
        return 0; // nothing to report this window
    }
    batch.seq = s_batchSeq++;

    String json = telemetryFormatBatch(batch, TELEMETRY_DEVICE_ID, timeSyncEpochMsFor);

    if (mqtt.connected() &&
        rawMqttPublish(MQTT_TOPIC_TELEMETRY, json.c_str(), json.length())) {
        Serial.printf("[net] published batch seq=%lu frames=%u bytes=%u dropped=%u\n",
                      (unsigned long)batch.seq, (unsigned)batch.frames.size(),
                      (unsigned)json.length(), (unsigned)batch.dropped_frames);
        return (int)batch.frames.size();
    }
    Serial.printf("[net] publish failed (connected=%d), spooling seq=%lu\n",
                  mqtt.connected(), (unsigned long)batch.seq);
    spoolBatch(json);
    return -1;
}

// Publishes everything that was queued when the window opened, in as many
// batches as it takes. One capped batch per window bounded sustained
// throughput at BATCH_MAX_FRAMES / BATCH_WINDOW_MS (4 frames/s at the
// defaults) however fast the uplink was, silently overflowing the queue
// above that. Frames arriving mid-drain wait for the next window, so a bus
// faster than the uplink can't pin the net task here; and the first failed
// publish stops the drain, so an outage still spools one batch per window.
static void drainAndPublish() {
    int pending = (int)uxQueueMessagesWaiting(g_canQueue);
    do {
        int taken = publishOneBatch(pending);
        if (taken <= 0) break;
        pending -= taken;
        mqtt.loop();
    } while (pending > 0);
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
    modemPowerPins();
#if MODEM_USB_BENCH_MODE
    Serial.println("[net] MODEM_USB_BENCH_MODE=1 -- modem is powered on and idle, "
                    "UART1 will stay silent from here on. Drive it over its own "
                    "USB port instead (see usb_modem_bench.py).");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
#endif
#if !TELEMETRY_USE_WIFI && MQTT_USE_TLS
    // Certificate validation stays off, same as the modem-side TLS it replaces
    // -- see MQTT_USE_TLS in telemetry_config.h.
    netClient.setInsecure();
#endif
    mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);

    ensureNetworkConnected();
#if TELEMETRY_USE_WIFI && WIFI_AP_MODE
    Serial.println("[net] AP mode has no internet uplink; skipping NTP time sync "
                    "(inter-frame relative timing is unaffected)");
#else
    // SNTP was only just started by ensureNetworkConnected(); give its first
    // reply a moment instead of waiting a full TIME_SYNC_INTERVAL_MS.
    bool timeSynced = false;
    for (uint32_t start = millis(); !timeSynced && millis() - start < 10000;) {
        timeSynced = g_timeSync.syncFromSystemClock();
        if (!timeSynced) vTaskDelay(pdMS_TO_TICKS(250));
    }
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
#if !TELEMETRY_USE_WIFI
            if (!s_sntpStarted && PPP.connected()) startSntp();
#endif
            if (!g_timeSync.syncFromSystemClock()) {
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
    // TLS handshakes now run in mbedTLS on this task's stack, not in the modem.
    xTaskCreatePinnedToCore(netTask, "net_task", 12288, nullptr, tskIDLE_PRIORITY + 2,
                             nullptr, 1);
}
