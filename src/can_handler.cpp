#include "can_handler.h"
#include "can_pins.h"
#include "telemetry_config.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <atomic>
#include <algorithm>
#include <cstring>

#include <freertos/idf_additions.h>

#if TELEMETRY_USE_SIMULATED_CAN
#elif CAN_USE_MCP2518FD
#include "mcp2518fd.h"
#else
#include <driver/twai.h>
#endif

QueueHandle_t g_canQueue = nullptr;
static std::atomic<uint32_t> s_droppedFrames{0};

uint32_t canHandlerTakeDroppedCount() {
    return s_droppedFrames.exchange(0);
}

// CanFrame has room for a 64-byte CAN FD payload (~80 bytes), so the queue is
// ~40 KB: keep it in PSRAM when the board has it, leaving internal RAM to
// WiFi/PPP/TLS. Only tasks touch the queue (never an ISR), which is what
// makes PSRAM backing safe.
static void createQueue() {
    if (psramFound()) {
        g_canQueue = xQueueCreateWithCaps(CAN_QUEUE_DEPTH, sizeof(CanFrame), MALLOC_CAP_SPIRAM);
    }
    if (!g_canQueue) {
        g_canQueue = xQueueCreate(CAN_QUEUE_DEPTH, sizeof(CanFrame));
    }
}

// Pushes a captured frame onto the shared queue without blocking. If full,
// drops the oldest queued frame first (never the CAN task itself).
//
// The evict-then-retry is a loop rather than a straight-line
// send/receive/send because the net task drains this same queue from the
// other core (net_task.cpp's drainAndPublish), truly in parallel -- this
// task's higher priority does not serialize the two. A slot can therefore
// free up between a failed send and the eviction below, and evicting
// unconditionally in that case would discard a live frame, and report a
// drop, that overflow never actually forced. Re-testing the send each pass
// evicts only when the queue is still genuinely full.
//
// Terminates in at most a couple of passes: this is the only producer, so
// once a slot is freed (or the consumer has emptied the queue outright)
// nothing can refill it ahead of the retry.
static void pushFrame(const CanFrame &frame) {
    while (xQueueSend(g_canQueue, &frame, 0) != pdTRUE) {
        CanFrame discard;
        if (xQueueReceive(g_canQueue, &discard, 0) == pdTRUE) {
            s_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

#if TELEMETRY_USE_SIMULATED_CAN

// Generates two synthetic streams (a fast one and a slow one) so the
// timestamp/batching path can be exercised without CAN hardware. Uses a
// single task with a tight scheduling loop rather than two timers, so both
// streams share the same capture-timestamp code path as the real driver.
static void canSimTask(void *) {
    const int64_t fastPeriodUs = 1000000LL / SIM_CAN_FAST_RATE_HZ;
    const int64_t slowPeriodUs = 1000000LL / SIM_CAN_SLOW_RATE_HZ;
    int64_t nextFast = esp_timer_get_time();
    int64_t nextSlow = nextFast;
    uint32_t counter = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();

        if (now >= nextFast) {
            CanFrame f{};
            f.id = SIM_CAN_FAST_ID;
            f.extended = false;
            f.dlc = 8;
            f.data[0] = (uint8_t)(counter & 0xFF);
            f.data[1] = (uint8_t)((counter >> 8) & 0xFF);
            memset(&f.data[2], 0, 6);
            f.capture_us = esp_timer_get_time(); // captured at push time, not at nextFast
            pushFrame(f);
            nextFast += fastPeriodUs;
        }

        if (now >= nextSlow) {
            CanFrame f{};
            f.id = SIM_CAN_SLOW_ID;
            f.extended = false;
            f.dlc = 4;
            f.data[0] = 0xAA;
            f.data[1] = (uint8_t)(counter & 0xFF);
            memset(&f.data[2], 0, 2);
            f.capture_us = esp_timer_get_time();
            pushFrame(f);
            nextSlow += slowPeriodUs;
        }

        counter++;
        int64_t sleepUs = std::min(nextFast, nextSlow) - esp_timer_get_time();
        if (sleepUs > 0) {
            vTaskDelay(pdMS_TO_TICKS(std::max<int64_t>(1, sleepUs / 1000)));
        }
    }
}

void canHandlerStart() {
    createQueue();
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

static void canMcpTask(void *) {
    const Mcp2518fdPins pins = {MCP2518_SCK_PIN, MCP2518_MOSI_PIN, MCP2518_MISO_PIN, MCP2518_CS_PIN};
    const char *err;
    while ((err = mcp2518fdBegin(pins, CAN_BITRATE_KBPS * 1000UL,
                                 CAN_FD_DATA_BITRATE_KBPS * 1000UL)) != nullptr) {
        Serial.printf("[can] MCP2518FD init failed: %s -- retrying in 2s\n", err);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    s_canTask = xTaskGetCurrentTaskHandle();
    pinMode(MCP2518_INT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(MCP2518_INT_PIN), mcpIntIsr, FALLING);
    Serial.printf("[can] MCP2518FD started, %d kbit/s nominal, %d kbit/s FD data\n",
                  CAN_BITRATE_KBPS, CAN_FD_DATA_BITRATE_KBPS);

    uint32_t rxCount = 0;
    int64_t lastStatusUs = esp_timer_get_time();

    for (;;) {
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
            Serial.printf("[can] rx=%lu state=%s rec=%u tec=%u fifo_overflows=%lu",
                          (unsigned long)rxCount,
                          st.busOff ? "bus-off" : st.rxPassive ? "error-passive"
                                                 : st.rxWarning ? "warning" : "ok",
                          st.rec, st.tec, (unsigned long)st.rxOverflows);
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
    createQueue();
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
    createQueue();
    xTaskCreatePinnedToCore(canTwaiTask, "can_twai", 4096, nullptr,
                             configMAX_PRIORITIES - 2, nullptr, 0);
}

#endif
