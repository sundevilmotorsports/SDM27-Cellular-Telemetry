// Pin assignments for LILYGO T-SIM7670G-S3 (plain/non-Standard variant).
//
// Modem/board pins below are transcribed verbatim from the LILYGO_T_SIM7670G_S3
// block of utilities.h in Xinyuan-LilyGO/LilyGo-Modem-Series (examples/ATdebug/utilities.h).
// Do not change these without re-checking that file -- they are fixed by the PCB,
// not a firmware choice.
//
// This board's ESP32-S3-WROOM-1 module has 16MB flash + 8MB Octal PSRAM
// (confirmed via docs/en/esp32s3/sim7670g-s3/README.MD in the same repo), which
// additionally reserves GPIO33-37 for PSRAM on top of the ESP32-S3-WROOM-1
// datasheet's universal reservations. Reserved/unusable GPIOs on this board:
//   0, 3, 45, 46        -- strapping pins (3 is also MODEM_RING, wired by LilyGo)
//   19, 20               -- native USB D-/D+ (used for the ESP-USB programming port)
//   26-32                 -- SPI flash
//   33-37                 -- Octal PSRAM (this module has 8MB OPI PSRAM)
//   4, 5, 9, 10, 11, 12, 17, 18 -- modem/LED/ADC pins, see below

#pragma once

#define MODEM_BAUDRATE      115200
#define MODEM_DTR_PIN       9
#define MODEM_TX_PIN        11   // ESP32 UART1 TX -> modem RXD
#define MODEM_RX_PIN        10   // ESP32 UART1 RX <- modem TXD
#define MODEM_RING_PIN       3   // strapping pin GPIO3, fixed by LilyGo's PCB; input only
#define MODEM_RESET_PIN     17
#define MODEM_RESET_LEVEL   LOW

#define BOARD_PWRKEY_PIN    18
#define BOARD_LED_PIN       12
#define LED_ON               LOW

#define BOARD_BAT_ADC_PIN    4
#define BOARD_SOLAR_ADC_PIN  5

// SIM7670G power-on sequence timing, from LilyGo's ATdebug example / utilities.h
// (values are specific to TINY_GSM_MODEM_SIM7670G, not shared with other modems).
#define MODEM_POWERON_PULSE_WIDTH_MS   100
#define MODEM_POWEROFF_PULSE_WIDTH_MS  3000
#define MODEM_START_WAIT_MS            3000
#define MODEM_RESET_PULSE_WIDTH_MS     2600
