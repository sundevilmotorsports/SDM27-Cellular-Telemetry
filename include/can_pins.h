// CAN transceiver wiring for LILYGO T-SIM7670G-S3 (plain variant).
//
// GPIO1/GPIO2 are LilyGo's own officially validated CAN pins for this exact
// board -- see examples/SimHatCanBusRecv/SimHatCanBusRecv.ino in
// Xinyuan-LilyGO/LilyGo-Modem-Series, block `#elif defined(LILYGO_T_SIM7670G_S3)`.
// They are free general-purpose pins on the bare board (not used by the modem,
// USB, flash, or PSRAM); they double as an optional I2C header (BOARD_SDA_PIN/
// BOARD_SCL_PIN) for LilyGo's SimShield accessory, which this project does not use.
//
// Transceiver: TI SN65HVD232D (single 3.3V supply, D=pin1/TXD in, R=pin4/RXD out,
// VCC=pin3, GND=pin2, CANH=pin7, CANL=pin6, pins 5 & 8 = NC -- this part has no
// Rs/slope-control or standby pin, unlike the SN65HVD230). See README.md for the
// full wiring diagram.

#pragma once

#define CAN_TX_PIN   1   // ESP32 TWAI TX -> SN65HVD232D pin 1 (D)
#define CAN_RX_PIN   2   // ESP32 TWAI RX <- SN65HVD232D pin 4 (R)

// Default bitrate. Must match the vehicle bus you're connecting to -- 500 kbit/s
// is standard for OBD-II diagnostic CAN, but body/comfort buses on some vehicles
// run at 125 kbit/s or 33.3 kbit/s. Change TWAI_TIMING_CONFIG_500KBITS() in
// can_handler.cpp to match if needed.
#define CAN_BITRATE_KBPS 500
