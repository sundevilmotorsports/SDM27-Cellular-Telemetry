// CAN controller/transceiver wiring for LILYGO T-SIM7670G-S3 (plain variant).
//
// TWAI mode (CAN_USE_MCP2518FD=0): GPIO1/GPIO2 are LilyGo's own officially validated CAN pins for this exact
// board -- see examples/SimHatCanBusRecv/SimHatCanBusRecv.ino in
// Xinyuan-LilyGO/LilyGo-Modem-Series, block `#elif defined(LILYGO_T_SIM7670G_S3)`.
// They are free general-purpose pins on the bare board (not used by the modem,
// USB, flash, or PSRAM); they double as an optional I2C header (BOARD_SDA_PIN/
// BOARD_SCL_PIN) for LilyGo's SimShield accessory, which this project does not use.
//
// TWAI-mode transceiver: TI SN65HVD232D (single 3.3V supply, D=pin1/TXD in, R=pin4/RXD out,
// VCC=pin3, GND=pin2, CANH=pin7, CANL=pin6, pins 5 & 8 = NC -- this part has no
// Rs/slope-control or standby pin, unlike the SN65HVD230), or an Adafruit CAN
// Pal (TJA1051T/3) wired TX->TX, RX->RX with SLNT tied to GND. See README.md
// for the full wiring diagrams of both modes.

#pragma once

// ---- CAN controller selection --------------------------------------------
// 1 = Soldered "CAN Transceiver MCP2518 board" (MCP2518FD controller + ATA6561
//     transceiver) over SPI: receives classic CAN and CAN FD.
// 0 = the ESP32-S3's built-in TWAI controller + an external transceiver
//     (SN65HVD232D or Adafruit CAN Pal) on CAN_TX_PIN/CAN_RX_PIN: classic CAN
//     only. TWAI is not FD-tolerant -- it error-frames any CAN FD frame on
//     the bus, destroying it for every node -- so never use it on an FD bus.
#ifndef CAN_USE_MCP2518FD
#define CAN_USE_MCP2518FD 1
#endif

// MCP2518 board wiring (SPI on LilyGo's SimShield header pins; GPIO1/2 are
// free because TWAI isn't used in this mode). The board's VCC needs 5V --
// the S3 header's VIN (= USB VBUS), not 3V3: it feeds the ATA6561 and an
// onboard 3.3V LDO for the MCP2518FD, so the SPI and INT lines are 3.3V
// logic. On 3V3 the SPI side still works but the transceiver stays dark.
#define MCP2518_SCK_PIN   16  // -> board SCK
#define MCP2518_MOSI_PIN  15  // -> board SDI
#define MCP2518_MISO_PIN  7   // <- board SDO
#define MCP2518_CS_PIN    1   // -> board NCS
#define MCP2518_INT_PIN   2   // <- board INT (active low)

// CAN FD data-phase bitrate (after the bit rate switch). Arbitration and
// classic frames use CAN_BITRATE_KBPS below. Must match the transmitting
// nodes; the transceivers (ATA6561, TJA1051T/3) are rated to 5 Mbit/s, but
// breadboard wiring is happiest at 2 Mbit/s or less.
#define CAN_FD_DATA_BITRATE_KBPS 2000

// TWAI pins (CAN_USE_MCP2518FD=0 only)
#define CAN_TX_PIN   1   // ESP32 TWAI TX -> SN65HVD232D pin 1 (D)
#define CAN_RX_PIN   2   // ESP32 TWAI RX <- SN65HVD232D pin 4 (R)

// Nominal (arbitration / classic CAN) bitrate. Must match the bus you're
// connecting to -- 500 kbit/s is standard for OBD-II diagnostic CAN, but
// body/comfort buses on some vehicles run at 125 kbit/s or 33.3 kbit/s.
// TWAI supports 125/250/500/1000; the MCP2518FD any rate dividing 40 MHz
// between ~125k and 1M.
#define CAN_BITRATE_KBPS 500
