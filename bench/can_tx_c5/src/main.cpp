// Bench CAN FD traffic generator for the cellular telemetry node.
//
// The ESP32-C5's own TWAI-FD controller drives an Adafruit CAN Pal and puts
// random classic and CAN FD frames on the bus. The main T-SIM7670G-S3 node
// receives them through its Soldered MCP2518 board and publishes them over
// MQTT; bench/verify_frames.py then checks every frame that reaches the broker.
//
// Every frame is self-checking, so the receiving end can verify it without
// any shared state:
//   id   : random 11-bit (75%) or 29-bit (25%)
//   type : CAN FD 70% (bit rate switch on 80% of those), classic 30%
//   len  : classic 2..8; FD 2..8, 12, 16, 20, 24, 32, 48 or 64
//   data[0]         : rolling 8-bit sequence counter (gaps = lost frames)
//   data[1..len-2]  : random bytes
//   data[len-1]     : CRC-8 (poly 0x07, init 0) over id (4 bytes LE), ext,
//                     fd, brs, len (1 byte each), data[0..len-2]
//
// Serial commands (115200): + / - double/halve rate, p pause/resume,
// b burst 200 frames back-to-back, v toggle per-frame logging.

#include <Arduino.h>
#include <esp_random.h>
#include <esp_twai.h>
#include <esp_twai_onchip.h>
#include <atomic>

// ---- Pins (ESP32-C5-DevKitC-1, as wired on the bench) ----------------------
// TWAI goes through the GPIO matrix, so any free pin works. Neither is a C5
// strapping pin (2, 3, 7, 25, 26, 27, 28) or USB (13, 14). GPIO15 only
// exists on module variants without in-package PSRAM (it's PSRAM CS on others).
#define PIN_CAN_TX 4   // -> CAN Pal TX
#define PIN_CAN_RX 15  // <- CAN Pal RX

#ifndef BENCH_CAN_BITRATE
#define BENCH_CAN_BITRATE 500000
#endif
#ifndef BENCH_CAN_FD_DATA_BITRATE
#define BENCH_CAN_FD_DATA_BITRATE 2000000
#endif
#ifndef BENCH_TX_RATE_HZ
#define BENCH_TX_RATE_HZ 10
#endif

// The driver queues a *pointer* to each frame (esp_twai_onchip.c), so a
// frame and its payload must stay untouched until on_tx_done. Frames
// complete in submission order, so a ring of slots guarded by a counting
// semaphore (one count per free slot) is enough.
#define TX_POOL 8

struct TxSlot {
    twai_frame_t frame;
    uint8_t data[64];
};

static twai_node_handle_t s_node;
static TxSlot s_pool[TX_POOL];
static uint32_t s_submitted = 0;
static SemaphoreHandle_t s_freeSlots;
static std::atomic<uint32_t> s_txOk{0}, s_txFailed{0}, s_ackErrors{0}, s_busErrors{0};
static std::atomic<bool> s_busOff{false};

static uint8_t s_seq = 0;
static uint32_t s_queued = 0, s_noSlot = 0, s_busOffSkips = 0;
static uint32_t s_rateHz = BENCH_TX_RATE_HZ;
static bool s_paused = false, s_verbose = false;

// All callbacks run in ISR context.
static bool onTxDone(twai_node_handle_t, const twai_tx_done_event_data_t *e, void *) {
    (e->is_tx_success ? s_txOk : s_txFailed)++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_freeSlots, &woken);
    return woken == pdTRUE;
}

static bool onError(twai_node_handle_t, const twai_error_event_data_t *e, void *) {
    if (e->err_flags.ack_err) s_ackErrors++;
    if (e->err_flags.bit_err || e->err_flags.form_err || e->err_flags.stuff_err) s_busErrors++;
    return false;
}

static bool onStateChange(twai_node_handle_t, const twai_state_change_event_data_t *e, void *) {
    s_busOff = e->new_sta == TWAI_ERROR_BUS_OFF;
    return false;
}

static uint8_t crc8(const uint8_t *p, size_t n, uint8_t crc = 0) {
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static uint8_t frameCrc(uint32_t id, bool ext, bool fd, bool brs, uint8_t len, const uint8_t *data) {
    uint8_t hdr[8] = {(uint8_t)id, (uint8_t)(id >> 8), (uint8_t)(id >> 16), (uint8_t)(id >> 24),
                      (uint8_t)ext, (uint8_t)fd, (uint8_t)brs, len};
    return crc8(data, len - 1, crc8(hdr, sizeof hdr));
}

// waitMs = how long to wait for a free slot; 0 for the paced loop.
static bool sendRandomFrame(uint32_t waitMs) {
    if (s_busOff) {  // the driver rejects transmits until printStatus() recovers the node
        s_busOffSkips++;
        return false;
    }
    if (xSemaphoreTake(s_freeSlots, pdMS_TO_TICKS(waitMs)) != pdTRUE) {
        s_noSlot++;  // all TX_POOL frames still in flight: almost always "nobody is ACKing"
        return false;
    }

    static const uint8_t kFdLens[] = {2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    bool ext = (esp_random() & 3) == 0;
    uint32_t id = ext ? (esp_random() & 0x1FFFFFFF) : (esp_random() & 0x7FF);
    bool fd = esp_random() % 10 < 7;
    bool brs = fd && esp_random() % 10 < 8;
    uint8_t len = fd ? kFdLens[esp_random() % sizeof kFdLens] : 2 + esp_random() % 7;

    TxSlot &slot = s_pool[s_submitted % TX_POOL];
    slot.data[0] = s_seq;
    for (int i = 1; i < len - 1; i++) slot.data[i] = (uint8_t)esp_random();
    slot.data[len - 1] = frameCrc(id, ext, fd, brs, len, slot.data);

    slot.frame = {};
    slot.frame.header.id = id;
    slot.frame.header.ide = ext;
    slot.frame.header.fdf = fd;
    slot.frame.header.brs = brs;
    slot.frame.header.dlc = twaifd_len2dlc(len);
    slot.frame.buffer = slot.data;
    slot.frame.buffer_len = len;

    esp_err_t err = twai_node_transmit(s_node, &slot.frame, 0);
    if (err != ESP_OK) {
        xSemaphoreGive(s_freeSlots);
        Serial.printf("[tx] transmit failed: %s\n", esp_err_to_name(err));
        return false;
    }
    s_submitted++;
    s_seq++;
    s_queued++;
    if (s_verbose) {
        Serial.printf("[tx] %s%s %s 0x%0*lX len=%u data=", fd ? "fd" : "classic", brs ? "+brs" : "",
                      ext ? "ext" : "std", ext ? 8 : 3, (unsigned long)id, len);
        for (int i = 0; i < len; i++) Serial.printf("%02x", slot.data[i]);
        Serial.println();
    }
    return true;
}

static void printStatus() {
    twai_node_status_t st;
    twai_node_record_t rec;
    if (twai_node_get_info(s_node, &st, &rec) != ESP_OK) return;
    static const char *const kState[] = {"ok", "warning", "error-passive", "bus-off"};
    Serial.printf("[status] queued=%lu sent_ok=%lu seq=%u rate=%luHz%s state=%s tec=%u rec=%u "
                  "ack_errors=%lu bus_errors=%lu no_slot=%lu bus_off_skips=%lu\n",
                  (unsigned long)s_queued, (unsigned long)s_txOk.load(), s_seq,
                  (unsigned long)s_rateHz, s_paused ? " (paused)" : "",
                  st.state <= TWAI_ERROR_BUS_OFF ? kState[st.state] : "?", st.tx_error_count,
                  st.rx_error_count, (unsigned long)s_ackErrors.load(),
                  (unsigned long)s_busErrors.load(), (unsigned long)s_noSlot,
                  (unsigned long)s_busOffSkips);
    if (s_ackErrors.load() && s_txOk.load() == 0) {
        Serial.println("         ^ no ACKs: is the S3 running with TELEMETRY_USE_SIMULATED_CAN=0 and its "
                       "MCP2518FD initialized ([can] MCP2518FD started)? Also check CAN Pal SLNT is tied "
                       "to GND and CANH/CANL aren't swapped.");
    }
    if (s_busErrors.load() && s_txOk.load() == 0 && s_ackErrors.load() == 0) {
        Serial.printf("         ^ bit errors with no ACK errors: the C5 never sees its own bits on RX. "
                      "Check the CAN Pal has power (Vcc=3V3, GND), SLNT is tied to GND, and its "
                      "TX/RX go to GPIO%d/GPIO%d.\n", PIN_CAN_TX, PIN_CAN_RX);
    }
    if (st.state == TWAI_ERROR_BUS_OFF) {
        Serial.println("[status] bus-off, recovering");
        twai_node_recover(s_node);
    }
}

static esp_err_t startNode(uint16_t sspPermill) {
    twai_onchip_node_config_t cfg = {};
    cfg.io_cfg.tx = (gpio_num_t)PIN_CAN_TX;
    cfg.io_cfg.rx = (gpio_num_t)PIN_CAN_RX;
    cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    // 80% sample points, matching the MCP2518FD side (include/can_pins.h).
    // The secondary sample point drives transmitter delay compensation in
    // the fast data phase.
    cfg.bit_timing = {BENCH_CAN_BITRATE, 800, 0};
    cfg.data_timing = {BENCH_CAN_FD_DATA_BITRATE, 800, sspPermill};
    cfg.fail_retry_cnt = -1;  // retransmit until ACKed, like a normal CAN node
    cfg.tx_queue_depth = TX_POOL;

    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) return err;
    twai_event_callbacks_t cbs = {};
    cbs.on_tx_done = onTxDone;
    cbs.on_error = onError;
    cbs.on_state_change = onStateChange;
    if ((err = twai_node_register_event_callbacks(s_node, &cbs, nullptr)) != ESP_OK) return err;
    return twai_node_enable(s_node);
}

void setup() {
    Serial.begin(115200);
    // Drop output instead of blocking when nothing is reading the USB port,
    // so an unattended generator keeps pacing frames.
    Serial.setTxTimeoutMs(0);
    delay(1500);
    Serial.printf("\n[bench] CAN FD traffic generator: %d bit/s nominal, %d bit/s data, %lu Hz\n",
                  BENCH_CAN_BITRATE, BENCH_CAN_FD_DATA_BITRATE, (unsigned long)s_rateHz);

    s_freeSlots = xSemaphoreCreateCounting(TX_POOL, TX_POOL);
    esp_err_t err = startNode(800);
    if (err != ESP_OK) {
        Serial.printf("[can] start with transmitter delay compensation failed (%s), retrying without\n",
                      esp_err_to_name(err));
        if (s_node) twai_node_delete(s_node);
        s_node = nullptr;
        err = startNode(0);
    }
    while (err != ESP_OK) {
        Serial.printf("[can] TWAI-FD start failed: %s\n", esp_err_to_name(err));
        delay(2000);
    }
    Serial.printf("[can] TWAI-FD started, TX=%d RX=%d. Commands: + - p b v\n", PIN_CAN_TX, PIN_CAN_RX);

    // On the bench the generator twice stopped transmitting and printing
    // after minutes of unacknowledged retries (the S3 side was dark). Cause
    // not yet found; until it is, reboot if loop() stalls for 5 s.
    enableLoopWDT();
}

void loop() {
    static uint32_t nextTxUs = micros();
    static uint32_t lastStatusMs = 0;

    while (Serial.available()) {
        switch (Serial.read()) {
        case '+': s_rateHz = min<uint32_t>(s_rateHz * 2, 2000); break;
        case '-': s_rateHz = max<uint32_t>(s_rateHz / 2, 1); break;
        case 'p': s_paused = !s_paused; break;
        case 'v': s_verbose = !s_verbose; break;
        case 'b': {
            // As fast as the bus drains the TX pool; a 20 ms wait for a free
            // slot is ~50 frame times, so a timeout still means "no ACK".
            uint32_t ok = 0;
            for (int i = 0; i < 200; i++) ok += sendRandomFrame(20);
            Serial.printf("[burst] queued %lu/200\n", (unsigned long)ok);
            break;
        }
        default: continue;
        }
        Serial.printf("[cmd] rate=%luHz paused=%d verbose=%d\n", (unsigned long)s_rateHz, s_paused, s_verbose);
        nextTxUs = micros();
    }

    if (!s_paused && (int32_t)(micros() - nextTxUs) >= 0) {
        sendRandomFrame(0);
        nextTxUs += 1000000UL / s_rateHz;
    }

    if (millis() - lastStatusMs >= 2000) {
        lastStatusMs = millis();
        printStatus();
    }
}
