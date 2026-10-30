#include "can_handler.h"
#include "can_map.h"
#include "can_pins.h"
#include "telemetry_config.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <algorithm>
#include <cstring>

#if TELEMETRY_USE_SIMULATED_CAN
#include "bench_ecu_pattern.h"
#elif CAN_USE_MCP2518FD
#include "mcp2518fd.h"
#else
#include <driver/twai.h>
#endif

// Every received frame updates the CAN map in place; there is no frame queue.
// The sampler reads the latest values at 10 Hz, so a frame costs one table
// scan here and nothing on the uplink.
static void pushFrame(const CanFrame &frame) {
    canMapApply(frame);
}

#if TELEMETRY_USE_SIMULATED_CAN

// Sends the bench ECU pattern (include/bench_ecu_pattern.h, the same frames
// the C5 generator sends with 'e') every BENCH_ECU_PERIOD_MS, plus a counter
// frame on SIM_CAN_FAST_ID, through the same path as received frames. Decoded
// only with CAN_MAP_BENCH=1; the car table ignores these ids.
static void canSimTask(void *) {
    uint32_t step = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        BenchEcuFrame ecu[2];
        benchEcuFrames(step, ecu);
        for (const BenchEcuFrame &e : ecu) {
            CanFrame f{};
            f.id = e.id;
            f.fd = e.fd;
            f.brs = e.fd;
            f.dlc = e.len;
            memcpy(f.data, e.data, e.len);
            f.capture_us = esp_timer_get_time();
            pushFrame(f);
        }
        CanFrame c{};
        c.id = SIM_CAN_FAST_ID;
        c.dlc = 8;
        c.data[0] = (uint8_t)step;
        c.data[1] = (uint8_t)(step >> 8);
        c.capture_us = esp_timer_get_time();
        pushFrame(c);
        step++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(BENCH_ECU_PERIOD_MS));
    }
}

void canHandlerStart() {
    canMapInit();
    xTaskCreatePinnedToCore(canSimTask, "can_sim", 4096, nullptr,
                             configMAX_PRIORITIES - 2, nullptr, 0);
}

#elif CAN_USE_MCP2518FD

static TaskHandle_t s_canTask = nullptr;

static void IRAM_ATTR mcpIntIsr() {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_canTask, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static const Mcp2518fdPins kMcpPins = {MCP2518_SCK_PIN, MCP2518_MOSI_PIN, MCP2518_MISO_PIN,
                                       MCP2518_CS_PIN};

static void mcpBeginUntilOk() {
    const char *err;
    while ((err = mcp2518fdBegin(kMcpPins, CAN_BITRATE_KBPS * 1000UL,
                                 CAN_FD_DATA_BITRATE_KBPS * 1000UL)) != nullptr) {
        Serial.printf("[can] MCP2518FD init failed: %s -- retrying in 2s\n", err);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void canMcpTask(void *) {
    mcpBeginUntilOk();
    s_canTask = xTaskGetCurrentTaskHandle();
    pinMode(MCP2518_INT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(MCP2518_INT_PIN), mcpIntIsr, FALLING);
    Serial.printf("[can] MCP2518FD started, %d kbit/s nominal, %d kbit/s FD data\n",
                  CAN_BITRATE_KBPS, CAN_FD_DATA_BITRATE_KBPS);

    uint32_t rxCount = 0, reinits = 0;
    int64_t lastStatusUs = esp_timer_get_time();
    int64_t lastModeCheckUs = lastStatusUs;

    for (;;) {
        // A dip on the board's 5V rail (seen on the bench during modem
        // transmit bursts, with VCC fed from the S3's USB VBUS) resets the
        // controller into configuration mode: silent on the bus, so every
        // other node gets ACK errors, until it is configured again.
        if (esp_timer_get_time() - lastModeCheckUs >= 1000000) {
            lastModeCheckUs = esp_timer_get_time();
            if (!mcp2518fdRunning()) {
                reinits++;
                Serial.printf("[can] MCP2518FD left normal mode (reset/brownout on its 5V?) -- "
                              "reinitializing (#%lu)\n", (unsigned long)reinits);
                mcpBeginUntilOk();
            }
        }

        // Woken by INT's falling edge. INT stays low while the FIFO holds
        // anything, so a missed edge would stall reception -- the timeout
        // polls as a fallback.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        // Each frame carries the controller's own end-of-frame timestamp
        // (1 MHz). Pair that clock with esp_timer once per drain and convert,
        // so capture_us reflects when the frame finished on the wire, not
        // how long it sat in the controller's FIFO before this SPI read.
        // Signed difference: frames landing mid-drain are newer than the anchor.
        uint32_t tbcAnchor = mcp2518fdTimeBase();
        int64_t usAnchor = esp_timer_get_time();

        Mcp2518fdFrame m;
        while (mcp2518fdRead(m)) {
            CanFrame f{};
            f.id = m.id;
            f.extended = m.extended;
            f.fd = m.fd;
            f.brs = m.brs;
            f.dlc = m.len;
            memcpy(f.data, m.data, m.len);
            f.capture_us = usAnchor - (int32_t)(tbcAnchor - m.timestamp);
            pushFrame(f);
            rxCount++;
        }

        if (esp_timer_get_time() - lastStatusUs >= 5000000) {
            lastStatusUs = esp_timer_get_time();
            Mcp2518fdStatus st;
            mcp2518fdGetStatus(st);
            Serial.printf("[can] rx=%lu state=%s rec=%u tec=%u fifo_overflows=%lu reinits=%lu",
                          (unsigned long)rxCount,
                          st.busOff ? "bus-off" : st.rxPassive ? "error-passive"
                                                 : st.rxWarning ? "warning" : "ok",
                          st.rec, st.tec, (unsigned long)st.rxOverflows, (unsigned long)reinits);
            // CiBDIAG1 flag bits 16-29: nominal-phase then data-phase errors.
            static const char *const kErr[14] = {"bit0", "bit1", "ack", "form", "stuff", "crc", "",
                                                 "txbo", "fd-bit0", "fd-bit1", "", "fd-form",
                                                 "fd-stuff", "fd-crc"};
            bool any = false;
            for (int i = 0; i < 14; i++) {
                if ((st.errorFlags & (1u << (16 + i))) && kErr[i][0]) {
                    Serial.printf("%s%s", any ? "," : " errors=", kErr[i]);
                    any = true;
                }
            }
            Serial.println();
        }
    }
}

void canHandlerStart() {
    canMapInit();
    xTaskCreatePinnedToCore(canMcpTask, "can_mcp", 4096, nullptr,
                             configMAX_PRIORITIES - 2, nullptr, 0);
}

#else // built-in TWAI

#if CAN_BITRATE_KBPS == 500
#define CAN_TIMING_CONFIG TWAI_TIMING_CONFIG_500KBITS()
#elif CAN_BITRATE_KBPS == 250
#define CAN_TIMING_CONFIG TWAI_TIMING_CONFIG_250KBITS()
#elif CAN_BITRATE_KBPS == 125
#define CAN_TIMING_CONFIG TWAI_TIMING_CONFIG_125KBITS()
#elif CAN_BITRATE_KBPS == 1000
#define CAN_TIMING_CONFIG TWAI_TIMING_CONFIG_1MBITS()
#else
#error "Unsupported CAN_BITRATE_KBPS -- add a TWAI_TIMING_CONFIG_*KBITS() mapping"
#endif

static void canTwaiTask(void *) {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = CAN_TIMING_CONFIG;
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
        Serial.println("[can] TWAI driver install failed");
        vTaskDelete(nullptr);
        return;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("[can] TWAI start failed");
        vTaskDelete(nullptr);
        return;
    }
    Serial.printf("[can] TWAI started, TX=%d RX=%d, %d kbit/s\n", CAN_TX_PIN, CAN_RX_PIN,
                  CAN_BITRATE_KBPS);

    uint32_t rxCount = 0;
    int64_t lastStatusUs = esp_timer_get_time();

    for (;;) {
        // Periodic bus health line, so a silent bench can be split into
        // "nothing on the wire" (rx=0, no errors) vs "frames arriving but
        // malformed" (bus errors climbing, e.g. bitrate mismatch).
        if (esp_timer_get_time() - lastStatusUs >= 5000000) {
            lastStatusUs = esp_timer_get_time();
            twai_status_info_t st;
            if (twai_get_status_info(&st) == ESP_OK) {
                static const char *const kState[] = {"stopped", "running", "bus-off", "recovering"};
                Serial.printf("[can] rx=%lu state=%s rec=%lu bus_errors=%lu rx_missed=%lu rx_overrun=%lu\n",
                              (unsigned long)rxCount, st.state <= 3 ? kState[st.state] : "?",
                              (unsigned long)st.rx_error_counter, (unsigned long)st.bus_error_count,
                              (unsigned long)st.rx_missed_count, (unsigned long)st.rx_overrun_count);
            }
        }

        twai_message_t msg;
        // Block up to 100ms for a frame; this keeps the task off the run
        // queue between frames without ever blocking on network state.
        if (twai_receive(&msg, pdMS_TO_TICKS(100)) != ESP_OK) {
            continue;
        }
        rxCount++;
        // Timestamp captured immediately after the frame leaves the driver,
        // before it touches the queue.
        int64_t capture_us = esp_timer_get_time();

        CanFrame f{};
        f.id = msg.identifier;
        f.extended = msg.extd;
        f.dlc = msg.data_length_code > 8 ? 8 : msg.data_length_code;
        if (!msg.rtr) {
            memcpy(f.data, msg.data, f.dlc);
        }
        f.capture_us = capture_us;
        pushFrame(f);
    }
}

void canHandlerStart() {
    canMapInit();
    xTaskCreatePinnedToCore(canTwaiTask, "can_twai", 4096, nullptr,
                             configMAX_PRIORITIES - 2, nullptr, 0);
}

#endif
