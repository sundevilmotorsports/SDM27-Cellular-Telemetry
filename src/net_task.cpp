#include "net_task.h"
#include "board_pins.h"
#include "telemetry_config.h"
#include "sms_config.h"
#include "time_sync.h"
#include "htp_frame.h"

#include <Arduino.h>
#include <ArduinoJson.h>
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
// Limitations. There is also no route to HTTPS_INGEST_URL, so batches are
// spooled and not POSTed until the board joins a network that can reach it.
static bool s_apStarted = false;

static void wifiConnectBlocking() {
    if (s_apStarted) return;
    Serial.printf("[net] starting WiFi AP '%s'...\n", WIFI_AP_SSID);
    WiFi.mode(WIFI_AP);
    s_apStarted = WiFi.softAP(WIFI_AP_SSID, strlen(WIFI_AP_PASSWORD) ? WIFI_AP_PASSWORD : nullptr);
    if (s_apStarted) {
        Serial.printf("[net] AP up, IP: %s -- no route to the ingest host from here; "
                      "batches will spool until the board has internet\n",
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

// ---- HTP/1 POST ------------------------------------------------------------
// One RAM ring of encoded windows per group; the rings are the retry protocol.
// seq is the window's unix second, so a reboot under the same HTP_SESSION_ID
// never reuses a seq (the server would ack it as a dup and keep the old data).
// Each POST takes up to HTP_POST_MAX_WINDOWS consecutive windows from the group
// whose oldest window is oldest, so a backlog drains in fewer requests.
// Old LittleFS JSON batches are not replayed: they are not HTP and would 400.

static_assert(HTP_POST_MAX_WINDOWS >= 1 && HTP_POST_MAX_WINDOWS <= HTP_MAX_WINDOWS,
              "HTP_POST_MAX_WINDOWS must be 1..8");

struct HtpQueuedWindow {
    uint32_t seq;
    uint64_t tStartUs;
};

struct HtpGroupQueue {
    HtpQueuedWindow meta[HTP_RETRY_QUEUE_MAX];
    uint8_t *bytes;     // window sections only (no header), windowBytes each
    size_t windowBytes;
    int head;
    int count;
};

// Reset every HTP_STATS_INTERVAL_MS by htpLogStats().
struct HtpStats {
    uint32_t postsOk;
    uint32_t postsFailed;
    uint32_t windowsAcked;
    uint32_t reqMsSum;
    uint32_t reqMsMax;
    uint32_t reused;
};

static HtpGroupQueue s_htpQ[HTP_GROUP_COUNT];
static uint8_t s_htpPost[HTP_MAX_POST_BYTES];
static HtpStats s_htpStats = {};
static uint32_t s_htpStatsStartMs = 0;
static uint32_t s_htpBackoffMs = HTP_BACKOFF_INITIAL_MS;
static uint32_t s_htpNextSendMs = 0;
static uint32_t s_htpDroppedOldest = 0;
static uint32_t s_htpPermanentErrors = 0;
static int64_t s_htpLastWindowSec = -1;
static int64_t s_htpFirstWindowSec = -1;
static bool s_htpLastReused = false;
static uint8_t s_htpSession[16];
static bool s_htpSessionOk = false;
static bool s_htpConfigOk = false;

static bool htpConfigReady() {
    return HTP_CHANNEL_SET_ID == 1 && strlen(HTTPS_INGEST_URL) > 0 &&
           strlen(TELEMETRY_HMAC_KEY) > 0 && s_htpSessionOk;
}

static void htpLogConfigOnce() {
    static bool logged = false;
    if (logged) return;
    logged = true;
    if (!strlen(HTTPS_INGEST_URL) || !strlen(TELEMETRY_HMAC_KEY) || !strlen(HTP_SESSION_ID)) {
        Serial.println("[htp] HTTPS_INGEST_URL, TELEMETRY_HMAC_KEY, or HTP_SESSION_ID is empty; "
                       "no posts until .env is set and the board is reflashed");
        return;
    }
    if (!s_htpSessionOk) {
        Serial.println("[htp] HTP_SESSION_ID is not a UUID; no posts");
        return;
    }
    if (HTP_CHANNEL_SET_ID != 1) {
        Serial.println("[htp] HTP_CHANNEL_SET_ID must be 1 until another set is frozen; no posts");
        return;
    }
    if (strstr(HTTPS_INGEST_URL, "/rest/") != nullptr) {
        Serial.println("[htp] URL looks like PostgREST, not telemetry-ingest; posts will be rejected");
    }
    Serial.printf("[htp] ingest %s\n", HTTPS_INGEST_URL);
    Serial.printf("[htp] device=%s session=%s set=%u\n",
                  HTP_DEVICE_ID, HTP_SESSION_ID, (unsigned)HTP_CHANNEL_SET_ID);
}

static bool htpQueuesInit() {
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        HtpGroupQueue &q = s_htpQ[g];
        q.windowBytes = htpWindowBytes(&kHtpGroups[g]);
        size_t n = q.windowBytes * HTP_RETRY_QUEUE_MAX;
        q.bytes = psramFound() ? (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : nullptr;
        if (!q.bytes) q.bytes = (uint8_t *)malloc(n);
        if (!q.bytes) {
            Serial.printf("[htp] no memory for the group %u queue (%u bytes)\n",
                          kHtpGroups[g].key, (unsigned)n);
            return false;
        }
    }
    return true;
}

static int htpSlot(const HtpGroupQueue &q, int offset) {
    return (q.head + offset) % HTP_RETRY_QUEUE_MAX;
}

static void htpDequeue(HtpGroupQueue &q, int n) {
    n = std::min(n, q.count);
    q.head = htpSlot(q, n);
    q.count -= n;
}

static void htpEnqueue(int g, uint32_t seq, uint64_t tStartUs, const uint8_t *frame, size_t len) {
    HtpGroupQueue &q = s_htpQ[g];
    if (!q.bytes || len != HTP_HEADER_BYTES + q.windowBytes) return;
    if (q.count == HTP_RETRY_QUEUE_MAX) {
        htpDequeue(q, 1);
        s_htpDroppedOldest++;
    }
    int slot = htpSlot(q, q.count);
    q.meta[slot] = {seq, tStartUs};
    memcpy(q.bytes + (size_t)slot * q.windowBytes, frame + HTP_HEADER_BYTES, q.windowBytes);
    q.count++;
}

// Group with the oldest queued window, or -1 when every queue is empty.
static int htpOldestGroup() {
    int best = -1;
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        const HtpGroupQueue &q = s_htpQ[g];
        if (q.count == 0) continue;
        if (best < 0 || q.meta[q.head].tStartUs < s_htpQ[best].meta[s_htpQ[best].head].tStartUs) {
            best = g;
        }
    }
    return best;
}

static int64_t htpNowMs() {
    return g_timeSync.epochMsFor(esp_timer_get_time());
}

// Age of the oldest queued window's last sample, in ms. 0 when nothing is queued.
static int64_t htpOldestAgeMs() {
    int g = htpOldestGroup();
    if (g < 0) return 0;
    const HtpGroupQueue &q = s_htpQ[g];
    return htpNowMs() - (int64_t)(q.meta[q.head].tStartUs / 1000ULL) - 1000;
}

// Copies consecutive seqs from the head of group g into s_htpPost under one
// header. send_timestamp_ms is the time of this attempt, not of encoding, so a
// retried frame does not report its queue wait as network latency.
static uint8_t htpBuildPost(int g, size_t *len) {
    const HtpGroupQueue &q = s_htpQ[g];
    uint32_t first = q.meta[q.head].seq;
    uint8_t n = 0;
    size_t off = HTP_HEADER_BYTES;
    while (n < q.count && n < HTP_POST_MAX_WINDOWS) {
        int slot = htpSlot(q, n);
        if (q.meta[slot].seq != first + n) break;
        memcpy(s_htpPost + off, q.bytes + (size_t)slot * q.windowBytes, q.windowBytes);
        off += q.windowBytes;
        n++;
    }
    htpWriteHeader(s_htpPost, s_htpSession, HTP_CHANNEL_SET_ID, kHtpGroups[g].key, n, first,
                   (uint64_t)htpNowMs());
    *len = off;
    return n;
}

#if TELEMETRY_USE_WIFI && WIFI_AP_MODE
static int httpPostHtp(const uint8_t *body, size_t len, String &response) {
    (void)body;
    (void)len;
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
    http.addHeader("Content-Type", "application/x-htp");
    http.addHeader("x-htp-device", HTP_DEVICE_ID);
    http.addHeader("x-htp-signature", sig);
    int code = http.POST(const_cast<uint8_t *>(body), len);
    response = http.getString();
    // Leaves the TLS socket open in netClient when the server allows keep-alive.
    http.end();
    return code;
}

static int httpPostHtp(const uint8_t *body, size_t len, String &response) {
    response = "";
    if (!networkBearerUp() || !s_htpConfigOk) return 0;
    char sig[65];
    hmacHex(body, len, sig);
    if (sig[0] == '\0') return 0;

    // A kept-alive socket the server has since closed still reads as connected
    // until the first write fails. Retry once on a fresh connection instead of
    // waiting out a backoff; a duplicate delivery is deduped by seq.
    s_htpLastReused = netClient.connected();
    int code = httpPostOnce(body, len, sig, response);
    if (code < 0 && s_htpLastReused) {
        netClient.stop();
        s_htpLastReused = false;
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

// How many seqs of [first, first+n) the ack lists in acked or dup; -1 = not JSON.
// serverMs is the function's own processing time (server_send - server_recv).
static int ackedCount(const String &body, uint32_t first, uint8_t n, int32_t *serverMs) {
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return -1;
    uint32_t seen = 0;
    for (const char *key : {"acked", "dup"}) {
        JsonArray arr = doc[key].as<JsonArray>();
        for (JsonVariant v : arr) {
            uint32_t d = v.as<uint32_t>() - first;
            if (d < n) seen |= 1u << d;
        }
    }
    *serverMs = (int32_t)(doc["server_send_ms"].as<int64_t>() - doc["server_recv_ms"].as<int64_t>());
    return __builtin_popcount(seen);
}

static bool htpRetryable(int code) {
    return code <= 0 || code == 408 || code == 429 || code >= 500;
}

static uint32_t htpBackoffDelay() {
    uint32_t span = s_htpBackoffMs / 5;
    uint32_t jitter = span == 0 ? 0 : (esp_random() % (2 * span + 1));
    return s_htpBackoffMs - span + jitter;
}

static void htpNoteFailure() {
    uint32_t wait = htpBackoffDelay();
    s_htpNextSendMs = millis() + wait;
    if (s_htpBackoffMs < HTP_BACKOFF_MAX_MS) {
        s_htpBackoffMs = std::min<uint32_t>(s_htpBackoffMs * 2, HTP_BACKOFF_MAX_MS);
    }
    Serial.printf("[htp] retry in %lu ms\n", (unsigned long)wait);
}

static void htpSendNext() {
    if (!s_htpConfigOk || !networkBearerUp()) return;
    if ((int32_t)(millis() - s_htpNextSendMs) < 0) return;
    int g = htpOldestGroup();
    if (g < 0) return;

    HtpGroupQueue &q = s_htpQ[g];
    size_t len = 0;
    uint8_t n = htpBuildPost(g, &len);
    uint32_t first = q.meta[q.head].seq;
    // End of the newest window in this POST: what the dashboard would show.
    int64_t newestEndMs = (int64_t)(q.meta[htpSlot(q, n - 1)].tStartUs / 1000ULL) + 1000;

    String response;
    uint32_t t0 = millis();
    int code = httpPostHtp(s_htpPost, len, response);
    uint32_t reqMs = millis() - t0;
    s_htpStats.reqMsSum += reqMs;
    s_htpStats.reqMsMax = std::max(s_htpStats.reqMsMax, reqMs);

    if (code == 200) {
        int32_t serverMs = 0;
        int listed = ackedCount(response, first, n, &serverMs);
        if (listed == n) {
            htpDequeue(q, n);
            s_htpStats.postsOk++;
            s_htpStats.windowsAcked += n;
            if (s_htpLastReused) s_htpStats.reused++;
            // age = how old the newest sample in this POST was when its ack came back.
            Serial.printf("[htp] g%u seq=%lu+%u (%u B) 200 in %lu ms (server %ld ms, %s) "
                          "age=%lld ms queued=%d/%d/%d\n",
                          kHtpGroups[g].key, (unsigned long)first, n, (unsigned)len,
                          (unsigned long)reqMs, (long)serverMs, s_htpLastReused ? "reused" : "new conn",
                          (long long)(htpNowMs() - newestEndMs),
                          s_htpQ[0].count, s_htpQ[1].count, s_htpQ[2].count);
            s_htpBackoffMs = HTP_BACKOFF_INITIAL_MS;
            s_htpNextSendMs = 0;
            return;
        }
        s_htpStats.postsFailed++;
        if (listed < 0) {
            Serial.println("[htp] HTTP 200 but ack was not JSON; will retry the same frame");
            htpNoteFailure();
            return;
        }
        s_htpPermanentErrors += n;
        Serial.printf("[htp] HTTP 200 acked %d of %u windows from seq=%lu; dropping them "
                      "(permanent=%lu)\n",
                      listed, n, (unsigned long)first, (unsigned long)s_htpPermanentErrors);
        htpDequeue(q, n);
        s_htpNextSendMs = 0;
        return;
    }
    s_htpStats.postsFailed++;
    if (htpRetryable(code)) {
        Serial.printf("[htp] POST failed HTTP %d after %lu ms, resending g%u seq=%lu+%u\n",
                      code, (unsigned long)reqMs, kHtpGroups[g].key, (unsigned long)first, n);
        // The function does not create sessions. Keep retrying: once the row
        // exists the queued windows go through.
        if (response.indexOf("session_id_fkey") >= 0) {
            Serial.printf("[htp] session %s is not a telemetry.sessions row; create it "
                          "(service role) or change HTP_SESSION_ID\n", HTP_SESSION_ID);
        }
        htpNoteFailure();
        return;
    }
    s_htpPermanentErrors += n;
    Serial.printf("[htp] POST rejected HTTP %d, dropping g%u seq=%lu+%u (permanent=%lu)\n",
                  code, kHtpGroups[g].key, (unsigned long)first, n,
                  (unsigned long)s_htpPermanentErrors);
    if (response.length() > 0) {
        Serial.println(response.substring(0, 160));
    }
    htpDequeue(q, n);
    s_htpBackoffMs = HTP_BACKOFF_INITIAL_MS;
    s_htpNextSendMs = 0;
}

// busy = share of the interval spent inside POSTs. Near 100% the uplink cannot
// keep up and queued/oldest grow; that is the number to watch before tuning.
static void htpLogStats() {
    uint32_t now = millis();
    uint32_t span = now - s_htpStatsStartMs;
    if (span < HTP_STATS_INTERVAL_MS) return;
    const HtpStats &s = s_htpStats;
    uint32_t posts = s.postsOk + s.postsFailed;
    Serial.printf("[htp] stats %lus: posts %lu ok/%lu failed, %lu windows (%.1f/s), "
                  "req avg %lu max %lu ms, busy %lu%%, reused %lu/%lu, queued %d/%d/%d, "
                  "oldest %lld ms, dropped %lu, rejected %lu\n",
                  (unsigned long)(span / 1000), (unsigned long)s.postsOk,
                  (unsigned long)s.postsFailed, (unsigned long)s.windowsAcked,
                  s.windowsAcked * 1000.0f / span,
                  (unsigned long)(posts ? s.reqMsSum / posts : 0), (unsigned long)s.reqMsMax,
                  (unsigned long)(s.reqMsSum * 100ULL / span), (unsigned long)s.reused,
                  (unsigned long)s.postsOk, s_htpQ[0].count, s_htpQ[1].count, s_htpQ[2].count,
                  (long long)htpOldestAgeMs(), (unsigned long)s_htpDroppedOldest,
                  (unsigned long)s_htpPermanentErrors);
    s_htpStats = {};
    s_htpStatsStartMs = now;
}

// Synthetic samples for one completed second. The whole window is generated at
// once after the second ends: its timestamps say when the samples would have
// been taken, not when anything was measured.
static void htpEmitWindow(int64_t winSec) {
    uint64_t tStartUs = (uint64_t)winSec * 1000000ULL;
    float relS = (float)(winSec - s_htpFirstWindowSec);
    float samples[22 * 10];
    uint8_t frame[HTP_MAX_FRAME_BYTES];
    for (int g = 0; g < HTP_GROUP_COUNT; g++) {
        const HtpGroup *group = &kHtpGroups[g];
        if ((size_t)group->channel_count * group->rate_hz > sizeof(samples) / sizeof(samples[0])) {
            Serial.printf("[htp] group %u does not fit the sample buffer\n", group->key);
            continue;
        }
        size_t n = 0;
        for (uint8_t c = 0; c < group->channel_count; c++) {
            for (uint16_t i = 0; i < group->rate_hz; i++) {
                float t = relS + (float)i / (float)group->rate_hz;
                samples[n++] = htpSynth(group->channels[c].id, t);
            }
        }
        uint32_t seq = (uint32_t)winSec;
        // send_timestamp_ms is rewritten by htpBuildPost().
        size_t len = htpEncodeFrame(frame, sizeof(frame), s_htpSession, HTP_CHANNEL_SET_ID,
                                    group, seq, 0, 1, &tStartUs, samples);
        if (len == 0) {
            Serial.printf("[htp] encode failed group=%u seq=%lu\n", group->key, (unsigned long)seq);
            continue;
        }
        htpEnqueue(g, seq, tStartUs, frame, len);
    }
}

static void htpEmitWindows() {
#if !HTP_SYNTH_MODE
    return;
#else
    if (!g_timeSync.isSynced()) return;
    int64_t done = htpNowMs() / 1000 - 1; // newest completed second
    if (s_htpLastWindowSec < 0) {
        s_htpFirstWindowSec = done;
        s_htpLastWindowSec = done - 1;
    }
    // A slow POST blocks this task for several seconds. Emit every second it
    // covered (bounded by the queue) so the backlog is real, not skipped.
    if (done - s_htpLastWindowSec > HTP_RETRY_QUEUE_MAX) {
        s_htpLastWindowSec = done - HTP_RETRY_QUEUE_MAX;
    }
    while (s_htpLastWindowSec < done) {
        htpEmitWindow(++s_htpLastWindowSec);
    }
#endif
}

static void htpMaintain() {
    htpLogConfigOnce();
    if (!s_htpConfigOk) return;
#if !(HTP_SYNTH_MODE)
    static bool warned = false;
    if (!warned) {
        warned = true;
        Serial.println("[htp] HTP_SYNTH_MODE=0; CAN is not mapped onto channel set 1, so nothing is posted");
    }
    return;
#endif
    if (!g_timeSync.isSynced()) {
        static uint32_t last = 0;
        if (millis() - last > 5000) {
            last = millis();
            Serial.println("[htp] waiting for SNTP before the first window");
        }
        return;
    }
    if (s_htpStatsStartMs == 0) s_htpStatsStartMs = millis();
    htpEmitWindows();
    htpSendNext();
    htpLogStats();
    if (s_htpDroppedOldest && (s_htpDroppedOldest % 10) == 0) {
        static uint32_t reported = 0;
        if (reported != s_htpDroppedOldest) {
            reported = s_htpDroppedOldest;
            Serial.printf("[htp] dropped oldest windows: %lu\n", (unsigned long)s_htpDroppedOldest);
        }
    }
}


// ---- Task ------------------------------------------------------------
static void netTask(void *) {
    s_htpSessionOk = htpParseUuid(HTP_SESSION_ID, s_htpSession);
    s_htpConfigOk = htpConfigReady() && htpQueuesInit();
    htpLogConfigOnce();

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
        htpMaintain();

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void netTaskStart() {
    // TLS handshakes run in mbedTLS on this task's stack. HTTPClient adds another
    // buffer on top of the handshake, so this is larger than the MQTT task was.
    xTaskCreatePinnedToCore(netTask, "net_task", 24576, nullptr, tskIDLE_PRIORITY + 2,
                             nullptr, 1);
}
