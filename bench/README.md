# Breadboard CAN FD bench

A two-node CAN FD bus on a breadboard for testing the telemetry firmware with
real classic and CAN FD frames instead of `TELEMETRY_USE_SIMULATED_CAN=1`:

```
 Node A: traffic generator                        Node B: telemetry node (this repo)
 ┌───────────────┐ TX/RX ┌──────────────┐          ┌──────────────────┐ SPI ┌───────────────┐
 │ ESP32-C5      │──────►│ Adafruit     │          │ Soldered MCP2518 │◄───►│ LILYGO        │
 │ DevKitC-1     │◄──────│ CAN Pal      │          │ MCP2518FD ctrl + │ INT │ T-SIM7670G-S3 │──► LTE ──► MQTT
 │ TWAI-FD ctrl  │       │ TJA1051T/3   │          │ ATA6561 xcvr     │────►│               │
 └───────────────┘       └──────┬───────┘          └────────┬─────────┘     └───────────────┘
                           CANH │ CANL │ GND ═══════════ CANH │ CANL │ GND
                       120Ω (switch ON)                120Ω (JP1 bridged)
```

- **Node A** -- the C5's built-in TWAI controller supports CAN FD; the CAN
  Pal is only its transceiver. Firmware: [`can_tx_c5/`](can_tx_c5).
- **Node B** -- the MCP2518 board is both the CAN FD controller and the
  transceiver; the S3 talks to it over SPI. Firmware: the main project in
  the repo root, with `CAN_USE_MCP2518FD=1` (the default).
- Both controllers default to ISO CAN FD, and both transceivers are rated to
  5 Mbit/s. The bus runs 500 kbit/s arbitration / 2 Mbit/s data.

## Wiring

### Node A: ESP32-C5-DevKitC-1 → Adafruit CAN Pal

| ESP32-C5 pin | CAN Pal pin | Notes |
|---|---|---|
| 3V3 | Vcc | Match the C5's 3.3V logic. The CAN Pal's charge pump makes the transceiver's 5V. |
| GND | GND | |
| GPIO23 | TX | TX to TX: the labels are from the MCU's side |
| GPIO24 | RX | |
| GND | SLNT | High = silent mode (can't transmit or ACK). There's no pull-down on the board, so tie it low. |

GPIO23/24 are on header J3 and aren't strapping or USB pins. To use others,
change `PIN_CAN_TX`/`PIN_CAN_RX` in [`can_tx_c5/src/main.cpp`](can_tx_c5/src/main.cpp).

### Node B: T-SIM7670G-S3 → Soldered MCP2518 board

| S3 pin | MCP2518 board pin | Notes |
|---|---|---|
| 5V | VCC | **Must be 5V**: feeds the ATA6561 and the board's 3.3V LDO. SPI/INT are 3.3V logic. |
| GND | GND | |
| GPIO16 | SCK | |
| GPIO15 | SDI | S3 MOSI |
| GPIO7 | SDO | S3 MISO |
| GPIO1 | NCS | |
| GPIO2 | INT | Active low; wakes the S3's CAN task |
| -- | INT0, INT1, CLKO | Not connected |

The pins are defined in [`include/can_pins.h`](../include/can_pins.h)
(`MCP2518_*_PIN`).

### The bus

Between the two transceivers there are only three wires: **CANH↔CANH,
CANL↔CANL, GND↔GND** (the CAN Pal's terminal block middle pin is GND).

Termination is one 120Ω at each end, and no other resistors:
- CAN Pal: termination switch **ON** (the factory default).
- MCP2518 board: **JP1 solder-bridged** (it's open from the factory).

With everything unpowered, CANH to CANL should measure **about 60Ω**. 120Ω
means one terminator is missing; about 40Ω means there's an extra one.

## Running the test

1. **Telemetry node (S3).** In the repo-root `.env`, add
   `TELEMETRY_USE_SIMULATED_CAN=0`. Then flash from the repo root:
   `pio run -t upload && pio device monitor`. Look for:
   ```
   [mcp] nominal 500000 bit/s (80 TQ, sp 80%), data 2000000 bit/s (20 TQ, sp 80%), RX FIFO 26 x 64B
   [can] MCP2518FD started, 500 kbit/s nominal, 2000 kbit/s FD data
   [can] rx=0 state=ok rec=0 tec=0 fifo_overflows=0
   ```
2. **Generator (C5).** Run `cd bench/can_tx_c5 && pio run -t upload &&
   pio device monitor`. Use the C5's native **USB** USB-C port: the build
   sets `ARDUINO_USB_CDC_ON_BOOT=1`, so it both flashes and prints there.
   With both boards plugged in, pass `--upload-port`/`-p` (check which
   `/dev/cu.usbmodem*` is which with `pio device list`). Look for:
   ```
   [can] TWAI-FD started, TX=23 RX=24. Commands: + - p b v
   [status] queued=20 sent_ok=20 seq=20 rate=10Hz state=ok tec=0 rec=0 ack_errors=0 bus_errors=0 no_slot=0
   ```
   On the S3, `rx=` should climb at the same rate.
3. **End-to-end check.** On a machine that can reach the broker, run
   `python3 bench/verify_frames.py` (add `--ap` for the SoftAP bench
   broker). It prints a summary every 10 s and a final one on Ctrl+C.
   Example output:
   ```
   [summary] batches=6 frames=100 crc_ok=100 bad=0
     std=74 ext=26 classic=31 fd=69 (brs=55)
     lengths 2:9 3:8 4:10 5:7 6:9 7:8 8:9 12:6 16:5 20:6 24:4 32:6 48:6 64:7
     lost(seq gaps)=0 device_dropped_frames=0 out_of_order_batches=0 s3_restarts=0
     latency capture->broker: median=7900ms max=15600ms (last 100)
     verdict: PASS
   ```
   Latency is mostly the wait for the next batch window: up to
   `BATCH_WINDOW_MS` (15 s), about half that on average.

Each generated frame carries a rolling sequence byte and a CRC-8 over its ID,
the extended/FD/BRS flags, the length and the data. So `crc_ok` proves every
one of those survived bus → MCP2518FD → JSON → MQTT unchanged, and sequence
gaps count frames that never arrived. `python3 bench/verify_frames.py
--self-test` checks the verifier's own logic with no hardware or broker.

Generator serial commands: `+`/`-` double/halve the rate (1-2000 Hz), `p`
pause/resume, `b` burst 200 frames back-to-back, `v` log every frame.

**Data cost:** the generator's mix averages about 115 bytes of JSON per
frame on the uplink (a 64-byte FD frame is about 230 bytes), so 10 Hz is
about 4 MB/hr. Do long or
high-rate runs with `TELEMETRY_USE_WIFI=1`, not on a metered SIM.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| S3: `MCP2518FD init failed: SPI readback failed` | SCK/SDI/SDO/NCS miswired, or the board's VCC isn't 5V |
| C5: `ack_errors` climbing, `sent_ok=0`, `no_slot` climbing | Nobody is ACKing. Check that the S3 printed `MCP2518FD started`, CAN Pal SLNT is on GND, and CANH/CANL aren't swapped |
| C5 `bus_errors` and/or S3 `errors=fd-crc,fd-stuff,...` | Only the 2 Mbit/s data phase is failing. Suspect wiring/termination (measure the 60Ω), or a bitrate mismatch between `can_pins.h` and `can_tx_c5/platformio.ini`. Try `-DBENCH_CAN_FD_DATA_BITRATE=1000000` and `CAN_FD_DATA_BITRATE_KBPS 1000` |
| S3: `errors=crc,stuff,form` (no `fd-` prefix) | The arbitration phase is failing: nominal bitrate mismatch or bad wiring |
| S3: `fifo_overflows` > 0 | The S3's CAN task fell behind the controller's 26-frame FIFO |
| S3 `rx` climbs, but the verifier sees nothing | Network/MQTT side. Check with `python3 listener.py -b` |
| Verifier: `lost` > 0 and roughly equal to `device_dropped_frames` | The S3's 512-frame queue overflowed because the uplink couldn't keep up. Lower the generator rate |
| Verifier: any `bad` frame | A real bug in the pipeline. Keep the printed frame JSON |

## Why not Soldered's Arduino library

The S3 talks to the MCP2518FD through a small register-level driver
([`src/mcp2518fd.cpp`](../src/mcp2518fd.cpp)) instead. Soldered's library has
two timing problems:
- `begin()` defaults to a 20 MHz clock, but the board has a 40 MHz crystal,
  so the bus runs at twice the requested rate unless you pass
  `MCP2518FD_40MHz`.
- Its copy of the bit-timing code (v1.1.1) leaves out the `-1` that
  `CiNBTCFG.TSEG1/TSEG2` need, which upstream Seeed has. So "500 kbit/s"
  actually runs at 40 MHz / 82 TQ ≈ 488 kbit/s. Two Soldered boards never
  notice that 2.4% error; any other controller does.
