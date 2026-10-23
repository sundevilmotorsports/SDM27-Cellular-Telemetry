// Minimal receive-only driver for the Microchip MCP2518FD SPI CAN FD
// controller, as used on the Soldered "CAN Transceiver MCP2518 board"
// (MCP2518FD + ATA6561 transceiver, 40 MHz crystal).
//
// Runs the controller in its Normal CAN FD mode, which receives both classic
// and CAN FD frames (ISO CRC) into one RX FIFO, ACKs them, and never
// transmits anything else. Each received frame carries the controller's own
// 1 MHz end-of-frame timestamp, so the capture instant doesn't depend on how
// long the frame waited in the FIFO before the SPI read.
//
// Register-level rather than a library: Soldered's Arduino library (v1.1.1)
// writes CiNBTCFG.TSEG1/TSEG2 without the required -1, so "500 kbit/s"
// actually runs at ~488 kbit/s, and defaults to a 20 MHz clock where this
// board has a 40 MHz crystal. Register map: Microchip DS20006027.

#pragma once

#include <Arduino.h>

struct Mcp2518fdPins {
    int sck, mosi, miso, cs;
};

struct Mcp2518fdFrame {
    uint32_t id;
    bool extended;
    bool fd;
    bool brs;
    uint8_t len;          // payload bytes, 0-64
    uint8_t data[64];
    uint32_t timestamp;   // controller time base at end of frame, 1 tick = 1 us
};

struct Mcp2518fdStatus {
    uint8_t rec, tec;          // CiTREC error counters
    bool rxWarning, rxPassive, busOff;
    uint32_t errorFlags;       // CiBDIAG1 bits 16-29 since the last call (cleared on read)
    uint32_t rxOverflows;      // RX FIFO overflows since begin (frames lost in the controller)
};

// Resets and configures the controller. Returns nullptr on success, or a
// static description of what failed.
const char *mcp2518fdBegin(const Mcp2518fdPins &pins, uint32_t nominalBitrate,
                           uint32_t dataBitrate);

// Reads the oldest frame from the RX FIFO. Returns false if it is empty.
bool mcp2518fdRead(Mcp2518fdFrame &out);

// True while the controller is in Normal CAN FD mode. False after it has
// reset (e.g. a brownout on the board's 5V rail): it then comes back up in
// configuration mode, silent on the bus, and needs mcp2518fdBegin() again.
bool mcp2518fdRunning();

// Current value of the controller's 1 MHz time base (same clock as
// Mcp2518fdFrame::timestamp), for mapping timestamps onto esp_timer.
uint32_t mcp2518fdTimeBase();

void mcp2518fdGetStatus(Mcp2518fdStatus &out);
