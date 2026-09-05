#include "can_handler.h"
#include "can_pins.h"
#include "telemetry_config.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <atomic>
#include <algorithm>
#include <cstring>

#if !TELEMETRY_USE_SIMULATED_CAN
#include <driver/twai.h>
#endif

QueueHandle_t g_canQueue = nullptr;
static std::atomic<uint32_t> s_droppedFrames{0};

uint32_t canHandlerTakeDroppedCount() {
    return s_droppedFrames.exchange(0);
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
    g_canQueue = xQueueCreate(CAN_QUEUE_DEPTH, sizeof(CanFrame));
    xTaskCreatePinnedToCore(canSimTask, "can_sim", 4096, nullptr,
                             configMAX_PRIORITIES - 2, nullptr, 0);
}

#else // real TWAI

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

    for (;;) {
        twai_message_t msg;
        // Block up to 100ms for a frame; this keeps the task off the run
        // queue between frames without ever blocking on network state.
        if (twai_receive(&msg, pdMS_TO_TICKS(100)) != ESP_OK) {
            continue;
        }
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
    g_canQueue = xQueueCreate(CAN_QUEUE_DEPTH, sizeof(CanFrame));
    xTaskCreatePinnedToCore(canTwaiTask, "can_twai", 4096, nullptr,
                             configMAX_PRIORITIES - 2, nullptr, 0);
}

#endif
