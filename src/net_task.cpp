#include "net_task.h"
#include "board_pins.h"
#include "telemetry_config.h"
#include "sms_config.h"
#include "time_sync.h"
#include "can_handler.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/md.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#if SMS_TEST_ENABLED && TELEMETRY_USE_WIFI
#error "SMS_TEST_ENABLED requires cellular (TELEMETRY_USE_WIFI=0) -- there is no modem to send SMS through in WiFi mode"
#endif

#if MODEM_USB_BENCH_MODE && TELEMETRY_USE_WIFI
#error "MODEM_USB_BENCH_MODE requires cellular (TELEMETRY_USE_WIFI=0) -- there is no modem in WiFi mode"
#endif

#if TELEMETRY_USE_WIFI
#include <WiFi.h>
// SoftAP has no route to the ingest host. Station mode posts over TLS.
#if !WIFI_AP_MODE
#include <WiFiClientSecure.h>
static WiFiClientSecure netClient;
#endif
#else
// The modem is only a PPP bearer: TCP runs in the ESP32's lwIP stack and TLS
// in mbedTLS. A batch is one HTTPS POST on that socket.
#include <PPP.h>
#include <esp_netif_ppp.h>
#include <esp_modem_api.h>
#include <NetworkClientSecure.h>
static HardwareSerial &SerialAT = Serial1;
static NetworkClientSecure netClient;
#endif

static uint32_t s_lastRegCheckMs = 0;
static uint32_t s_lastTimeSyncMs = 0;

#if TELEMETRY_USE_WIFI
#if WIFI_AP_MODE
// ---- WiFi SoftAP bring-up ---------------------------------------------
// The ESP32-S3 hosts its own network instead of joining one. No internet
// uplink exists in this mode, so NTP time sync is skipped entirely -- only
// relative (inter-frame) timing stays accurate, per README's Known
// Limitations. There is also no route to HTTPS_INGEST_URL, so nothing is
// POSTed in this mode.
static bool s_apStarted = false;

static void wifiConnectBlocking() {
    if (s_apStarted) return;
    Serial.printf("[net] starting WiFi AP '%s'...\n", WIFI_AP_SSID);
    WiFi.mode(WIFI_AP);
    s_apStarted = WiFi.softAP(WIFI_AP_SSID, strlen(WIFI_AP_PASSWORD) ? WIFI_AP_PASSWORD : nullptr);
    if (s_apStarted) {
        Serial.printf("[net] AP up, IP: %s -- no route to the ingest host from here; "
                      "nothing is posted in AP mode\n",
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

#if !TELEMETRY_USE_WIFI
// PPP.connected() only says the netif is up. On the bench a session stayed
// "up" while passing no traffic -- every connect then timed out for minutes
// and nothing ever tore the link down. After a few straight POSTs that fail
// to connect, log what the modem says and rebuild PPP from a modem reset.
#define POST_FAILS_BEFORE_PPP_RESET 3
static int s_postFailStreak = 0;

static void restartStalePpp() {
    Serial.printf("[net] %d POSTs failed over a 'connected' PPP link; resetting modem\n",
                  POST_FAILS_BEFORE_PPP_RESET);
    logModemDiagnostics();
    PPP.end();
    s_pppStarted = false;
    modemPowerPins();
    ensureNetworkConnected();
}
#endif

// ---- Raw CAN batch POST ----------------------------------------------------
// Frames wait in g_canQueue (can_handler.cpp). Once per BATCH_WINDOW_MS this
// task drains the queue into one batch (telemetry_format.h) and POSTs it to
// telemetry-ingest as application/x-sdm-can-batch+json, signed like HTP. The
// server files it under the device's running session, which a signed-in user
// starts from the dashboard; the board never knows a session id.
//
// One batch is in flight at a time and is kept until the server has it (2xx,
// including a dup), so a retry carries the same seq and frames. Only sent_ms
// changes: it is the Unix ms of each attempt, written into the body (fixed
// 13 digits, in place) and signed just before the POST, so the dashboard can
// compare it with received_at for uplink latency. Meanwhile
// new frames wait in the CAN queue and the next batch takes as many as fit, so
// a slow or stalled uplink makes fewer, bigger batches instead of a backlog of
// small ones. When the queue overflows the oldest frames go, and the count
// rides in the next batch's dropped_frames.
//
// seq is the batch's build time in Unix ms. The server dedupes on
// (session, device, seq), so a seq counter that restarted at 0 after a power
// cut would have its first batches acked as dups and lost; a clock-based seq
// never repeats. Batches are only built once SNTP has synced, which also gives
// every frame (even ones captured before the sync) an epoch ts_ms.
//
// Server answers: 2xx stored or dup; 409 no running session (hold the batch,
// retry every RAW_NO_SESSION_RETRY_MS); 408/429/5xx/no response (retry with
// backoff); any other 4xx (malformed or unauthorized: drop the batch, count
// its frames as dropped).

#define RAW_CONTENT_TYPE "application/x-sdm-can-batch+json"

// Reset every RAW_STATS_INTERVAL_MS by rawLogStats().
struct RawStats {
    uint32_t postsOk;
    uint32_t postsFailed;
    uint32_t framesSent;
    uint32_t bytesSent;
    uint32_t reqMsSum;
    uint32_t reqMsMax;
    uint32_t reused;
};

struct RawBatch {
    char *body;        // RAW_BATCH_MAX_BYTES, allocated once
    size_t len;        // 0 = no batch pending
    uint64_t seq;
    uint32_t frames;
    uint32_t dropped;  // dropped_frames written into this batch
    int64_t oldestMs;  // ts_ms of the first frame
    size_t sentOff;    // offset of the RAW_SENT_DIGITS digits of sent_ms in body
};

// Unix ms has 13 digits from 2001 to 2286.
#define RAW_SENT_DIGITS 13

static RawBatch s_batch = {};
static RawStats s_rawStats = {};
static uint32_t s_rawStatsStartMs = 0;
static uint32_t s_rawLastBuildMs = 0;
static uint32_t s_rawBackoffMs = RAW_BACKOFF_INITIAL_MS;
static uint32_t s_rawNextSendMs = 0;
static uint32_t s_rawCarryDropped = 0; // frames of discarded batches, for the next batch
static uint32_t s_rawDroppedTotal = 0; // queue overflow + discarded batches, since boot
static uint32_t s_rawRejected = 0;     // batches dropped on a 4xx
static uint64_t s_rawLastSeq = 0;
static bool s_rawWaitingSession = false;
static bool s_rawLastReused = false;
static bool s_rawConfigOk = false;
static CanFrame s_rawPeek;

// The device id goes into the body as a JSON string unescaped.
static bool rawDeviceIdOk(const char *id) {
    size_t n = strlen(id);
    if (n < 1 || n > 128) return false;
    for (size_t i = 0; i < n; i++) {
        if (id[i] < 0x20 || id[i] == '"' || id[i] == '\\' || id[i] == 0x7f) return false;
    }
    return true;
}

static bool rawConfigReady() {
    return strlen(HTTPS_INGEST_URL) > 0 && strlen(TELEMETRY_HMAC_KEY) > 0 &&
           rawDeviceIdOk(HTP_DEVICE_ID);
}

static void rawLogConfigOnce() {
    static bool logged = false;
    if (logged) return;
    logged = true;
    if (!strlen(HTTPS_INGEST_URL) || !strlen(TELEMETRY_HMAC_KEY)) {
        Serial.println("[raw] HTTPS_INGEST_URL or TELEMETRY_HMAC_KEY is empty; "
                       "no posts until .env is set and the board is reflashed");
        return;
    }
    if (!rawDeviceIdOk(HTP_DEVICE_ID)) {
        Serial.println("[raw] HTP_DEVICE_ID must be 1-128 printable characters without \" or \\; no posts");
        return;
    }
    if (!s_batch.body) {
        Serial.printf("[raw] no memory for the %u-byte batch buffer; no posts\n",
                      (unsigned)RAW_BATCH_MAX_BYTES);
        return;
    }
    if (strstr(HTTPS_INGEST_URL, "/rest/") != nullptr) {
        Serial.println("[raw] URL looks like PostgREST, not telemetry-ingest; posts will be rejected");
    }
    Serial.printf("[raw] ingest %s\n", HTTPS_INGEST_URL);
    Serial.printf("[raw] device=%s (enter this id on the dashboard and press Start session)\n",
                  HTP_DEVICE_ID);
}

static bool rawBufferInit() {
    s_batch.body = psramFound() ? (char *)heap_caps_malloc(RAW_BATCH_MAX_BYTES, MALLOC_CAP_SPIRAM)
                                : nullptr;
    if (!s_batch.body) s_batch.body = (char *)malloc(RAW_BATCH_MAX_BYTES);
    return s_batch.body != nullptr;
}

static int64_t rawNowMs() {
    return g_timeSync.epochMsFor(esp_timer_get_time());
}

// Overwrites the batch's sent_ms digits with the current time. The width
// never changes, so neither does the body length.
static void rawStampSent() {
    char digits[24];
    int n = snprintf(digits, sizeof digits, "%lld", (long long)rawNowMs());
    if (n == RAW_SENT_DIGITS) memcpy(s_batch.body + s_batch.sentOff, digits, RAW_SENT_DIGITS);
}

// Drains up to RAW_BATCH_MAX_FRAMES frames (and RAW_BATCH_MAX_BYTES of JSON)
// from the CAN queue into s_batch. Leaves s_batch empty if the queue was.
static void rawBuildBatch() {
    if (!g_canQueue || uxQueueMessagesWaiting(g_canQueue) == 0) return;
    // Taken before the frames, so a drop that happens while building lands in
    // the next batch rather than going uncounted.
    uint32_t overflow = canHandlerTakeDroppedCount();
    s_rawDroppedTotal += overflow;
    uint32_t dropped = overflow + s_rawCarryDropped;
    s_rawCarryDropped = 0;
    uint64_t seq = (uint64_t)rawNowMs();
    if (seq <= s_rawLastSeq) seq = s_rawLastSeq + 1; // two builds in one ms, or a clock step back
    char *b = s_batch.body;
    const size_t cap = RAW_BATCH_MAX_BYTES;
    int n = snprintf(b, cap, "{\"device_id\":\"%s\",\"seq\":%llu,\"sent_ms\":",
                     HTP_DEVICE_ID, (unsigned long long)seq);
    if (n <= 0 || (size_t)n + RAW_SENT_DIGITS >= cap) return;
    size_t sentOff = (size_t)n;
    memset(b + sentOff, '0', RAW_SENT_DIGITS);
    n = snprintf(b + sentOff + RAW_SENT_DIGITS, cap - sentOff - RAW_SENT_DIGITS,
                 ",\"dropped_frames\":%lu,\"frames\":[", (unsigned long)dropped);
    if (n <= 0 || sentOff + RAW_SENT_DIGITS + (size_t)n >= cap) return;
    size_t off = sentOff + RAW_SENT_DIGITS + (size_t)n;
    uint32_t frames = 0;
    int64_t oldestMs = 0;
    while (frames < RAW_BATCH_MAX_FRAMES && xQueuePeek(g_canQueue, &s_rawPeek, 0) == pdTRUE) {
        int64_t tsMs = g_timeSync.epochMsFor(s_rawPeek.capture_us);
        // 2 bytes kept for "]}".
        size_t room = cap - off - 2 - (frames ? 1 : 0);
        char *at = b + off + (frames ? 1 : 0);
        size_t w = telemetryFormatFrame(at, room, s_rawPeek, tsMs);
        if (w == 0) break; // full; the frame stays queued for the next batch
        if (frames) b[off] = ',';
        off += w + (frames ? 1 : 0);
        if (frames == 0) oldestMs = tsMs;
        frames++;
        CanFrame done;
        xQueueReceive(g_canQueue, &done, 0);
    }
    if (frames == 0) {
        s_rawCarryDropped += dropped;
        return;
    }
    b[off++] = ']';
    b[off++] = '}';
    s_batch.len = off;
    s_batch.seq = seq;
    s_batch.frames = frames;
    s_batch.dropped = dropped;
    s_batch.oldestMs = oldestMs;
    s_batch.sentOff = sentOff;
    rawStampSent();
    s_rawLastSeq = seq;
}

#if TELEMETRY_USE_WIFI && WIFI_AP_MODE
static int httpPostRaw(String &response) {
    response = "";
    return 0;
}
#else
static void hmacHex(const uint8_t *body, size_t len, char out[65]) {
    uint8_t mac[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int rc = mbedtls_md_hmac(info,
                             reinterpret_cast<const unsigned char *>(TELEMETRY_HMAC_KEY),
                             strlen(TELEMETRY_HMAC_KEY),
                             body, len, mac);
    if (rc != 0) {
        out[0] = '\0';
        return;
    }
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = digits[mac[i] >> 4];
        out[i * 2 + 1] = digits[mac[i] & 0x0f];
    }
    out[64] = '\0';
}

static int httpPostOnce(const uint8_t *body, size_t len, const char *sig, String &response) {
    HTTPClient http;
    if (!http.begin(netClient, HTTPS_INGEST_URL)) return 0;
    http.setConnectTimeout(HTTPS_POST_TIMEOUT_MS);
    http.setTimeout(HTTPS_POST_TIMEOUT_MS);
    http.addHeader("Content-Type", RAW_CONTENT_TYPE);
    http.addHeader("x-htp-device", HTP_DEVICE_ID);
    http.addHeader("x-htp-signature", sig);
    int code = http.POST(const_cast<uint8_t *>(body), len);
    response = http.getString();
    // Leaves the TLS socket open in netClient when the server allows keep-alive.
    http.end();
    return code;
}

// Posts s_batch, stamping sent_ms and signing right before each attempt.
static int httpPostRaw(String &response) {
    response = "";
    if (!networkBearerUp() || !s_rawConfigOk) return 0;
    const uint8_t *body = (const uint8_t *)s_batch.body;
    size_t len = s_batch.len;
    char sig[65];
    rawStampSent();
    hmacHex(body, len, sig);
    if (sig[0] == '\0') return 0;

    // A kept-alive socket the server has since closed still reads as connected
    // until the first write fails. Retry once on a fresh connection instead of
    // waiting out a backoff; a duplicate delivery is deduped by seq.
    s_rawLastReused = netClient.connected();
    int code = httpPostOnce(body, len, sig, response);
    if (code < 0 && s_rawLastReused) {
        netClient.stop();
        s_rawLastReused = false;
        rawStampSent();
        hmacHex(body, len, sig);
        if (sig[0] == '\0') return 0;
        code = httpPostOnce(body, len, sig, response);
    }
#if !TELEMETRY_USE_WIFI
    if (code >= 0) {
        s_postFailStreak = 0;
    } else if (++s_postFailStreak >= POST_FAILS_BEFORE_PPP_RESET) {
        s_postFailStreak = 0;
        restartStalePpp();
    }
#endif
    return code;
}
#endif

static bool rawRetryable(int code) {
    return code <= 0 || code == 408 || code == 429 || code >= 500;
}

static void rawNoteFailure() {
    uint32_t span = s_rawBackoffMs / 5;
    uint32_t jitter = span == 0 ? 0 : (esp_random() % (2 * span + 1));
    uint32_t wait = s_rawBackoffMs - span + jitter;
    s_rawNextSendMs = millis() + wait;
    s_rawBackoffMs = std::min<uint32_t>(s_rawBackoffMs * 2, RAW_BACKOFF_MAX_MS);
    Serial.printf("[raw] retry in %lu ms\n", (unsigned long)wait);
}

static void rawDiscardBatch() {
    s_rawCarryDropped += s_batch.frames;
    s_rawDroppedTotal += s_batch.frames;
    s_batch.len = 0;
}

static void rawSendPending() {
    if (s_batch.len == 0 || !networkBearerUp()) return;
    if ((int32_t)(millis() - s_rawNextSendMs) < 0) return;

    String response;
    uint32_t t0 = millis();
    int code = httpPostRaw(response);
    uint32_t reqMs = millis() - t0;
    s_rawStats.reqMsSum += reqMs;
    s_rawStats.reqMsMax = std::max(s_rawStats.reqMsMax, reqMs);

    if (code >= 200 && code < 300) {
        bool dup = response.indexOf("dup") >= 0 && response.indexOf("\"dup\":[]") < 0 &&
                   response.indexOf("\"dup\":false") < 0;
        if (s_rawWaitingSession) {
            Serial.println("[raw] session is running; posting");
            s_rawWaitingSession = false;
        }
        s_rawStats.postsOk++;
        s_rawStats.framesSent += s_batch.frames;
        s_rawStats.bytesSent += s_batch.len;
        if (s_rawLastReused) s_rawStats.reused++;
        // age = how old the batch's first frame was when the server had it.
        Serial.printf("[raw] seq=%llu %lu frames (%u B, dropped %lu) %d%s in %lu ms (%s) "
                      "age=%lld ms queued=%u\n",
                      (unsigned long long)s_batch.seq, (unsigned long)s_batch.frames,
                      (unsigned)s_batch.len, (unsigned long)s_batch.dropped, code,
                      dup ? " dup" : "", (unsigned long)reqMs,
                      s_rawLastReused ? "reused" : "new conn",
                      (long long)(rawNowMs() - s_batch.oldestMs),
                      (unsigned)uxQueueMessagesWaiting(g_canQueue));
        s_batch.len = 0;
        s_rawBackoffMs = RAW_BACKOFF_INITIAL_MS;
        s_rawNextSendMs = 0;
        return;
    }
    s_rawStats.postsFailed++;
    if (code == 409) {
        // Nobody has pressed Start for this device. Hold the batch: it goes
        // into the session once one is started.
        if (!s_rawWaitingSession) {
            Serial.printf("[raw] no running session for device %s; waiting for Start session "
                          "on the dashboard (retry every %u ms)\n",
                          HTP_DEVICE_ID, (unsigned)RAW_NO_SESSION_RETRY_MS);
            s_rawWaitingSession = true;
        }
        s_rawNextSendMs = millis() + RAW_NO_SESSION_RETRY_MS;
        s_rawBackoffMs = RAW_BACKOFF_INITIAL_MS;
        return;
    }
    if (rawRetryable(code)) {
        Serial.printf("[raw] POST failed HTTP %d after %lu ms, resending seq=%llu\n", code,
                      (unsigned long)reqMs, (unsigned long long)s_batch.seq);
        rawNoteFailure();
        return;
    }
    s_rawRejected++;
    Serial.printf("[raw] POST rejected HTTP %d, dropping seq=%llu (%lu frames)%s\n", code,
                  (unsigned long long)s_batch.seq, (unsigned long)s_batch.frames,
                  code == 401 ? ": signature refused, check TELEMETRY_HMAC_KEY" : "");
    if (response.length() > 0) Serial.println(response.substring(0, 160));
    rawDiscardBatch();
    s_rawBackoffMs = RAW_BACKOFF_INITIAL_MS;
    s_rawNextSendMs = 0;
}

static void rawLogStats() {
    uint32_t now = millis();
    uint32_t span = now - s_rawStatsStartMs;
    if (span < RAW_STATS_INTERVAL_MS) return;
    const RawStats &s = s_rawStats;
    uint32_t posts = s.postsOk + s.postsFailed;
    Serial.printf("[raw] stats %lus: posts %lu ok/%lu failed, %lu frames (%.1f/s), %.1f kB/s, "
                  "req avg %lu max %lu ms, busy %lu%%, reused %lu/%lu, queued %u, pending %s, "
                  "dropped %lu, rejected %lu%s\n",
                  (unsigned long)(span / 1000), (unsigned long)s.postsOk,
                  (unsigned long)s.postsFailed, (unsigned long)s.framesSent,
                  s.framesSent * 1000.0f / span, s.bytesSent / (float)span,
                  (unsigned long)(posts ? s.reqMsSum / posts : 0), (unsigned long)s.reqMsMax,
                  (unsigned long)(s.reqMsSum * 100ULL / span), (unsigned long)s.reused,
                  (unsigned long)s.postsOk, (unsigned)uxQueueMessagesWaiting(g_canQueue),
                  s_batch.len ? "yes" : "no", (unsigned long)s_rawDroppedTotal,
                  (unsigned long)s_rawRejected, s_rawWaitingSession ? ", waiting for session" : "");
    s_rawStats = {};
    s_rawStatsStartMs = now;
}

static void rawMaintain() {
    rawLogConfigOnce();
    if (!s_rawConfigOk) return;
    if (!g_timeSync.isSynced()) {
        static uint32_t last = 0;
        if (millis() - last > 5000) {
            last = millis();
            Serial.printf("[raw] waiting for SNTP before the first batch (%u frames queued)\n",
                          g_canQueue ? (unsigned)uxQueueMessagesWaiting(g_canQueue) : 0u);
        }
        return;
    }
    if (s_rawStatsStartMs == 0) s_rawStatsStartMs = millis();
    if (s_batch.len == 0 && millis() - s_rawLastBuildMs >= BATCH_WINDOW_MS) {
        s_rawLastBuildMs = millis();
        rawBuildBatch();
    }
    rawSendPending();
    rawLogStats();
}


// ---- Task ------------------------------------------------------------
static void netTask(void *) {
    rawLogConfigOnce();

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
#if !(TELEMETRY_USE_WIFI && WIFI_AP_MODE)
    // Encrypted, certificate not checked. See HTTPS_INGEST_URL in telemetry_config.h.
    netClient.setInsecure();
#endif

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

#if TELEMETRY_STRESS_TEST
        {
            static bool warned = false;
            if (!warned) {
                warned = true;
                Serial.println("[stress] TELEMETRY_STRESS_TEST is set, but the HTTPS path "
                               "does not POST filler. Leave it 0.");
            }
        }
#endif
        rawMaintain();

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void netTaskStart() {
    s_rawConfigOk = rawConfigReady() && rawBufferInit();
    // TLS handshakes run in mbedTLS on this task's stack. HTTPClient adds another
    // buffer on top of the handshake, so this is larger than the MQTT task was.
    xTaskCreatePinnedToCore(netTask, "net_task", 24576, nullptr, tskIDLE_PRIORITY + 2,
                             nullptr, 1);
}
