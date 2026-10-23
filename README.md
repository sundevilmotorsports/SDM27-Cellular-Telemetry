# ESP32 CAN-to-Cellular Telemetry PoC

Proof-of-concept firmware proving a hardware-to-backend data path: real (or
simulated) vehicle CAN / CAN FD frames -> ESP32-S3 -> LTE (SIM7670G) -> MQTT broker,
with per-frame microsecond-accurate timestamps preserved end-to-end.

This is a PoC, not a production telemetry stack. It favors clarity and
correctness over completeness -- see [Known Limitations](#known-limitations).

## Hardware

- **Board:** LILYGO T-SIM7670G-S3 -- the **plain** variant (product page
  [t-sim-7670g-s3](https://www.lilygo.cc/products/t-sim-7670g-s3)), *not* the
  "T-SIM7670G-S3-Standard" edition. These have very different pinouts and are
  not interchangeable; if you have the Standard edition (it has a camera
  connector, this board doesn't), every pin in this README is wrong for it.
  - ESP32-S3-WROOM-1, 16MB flash, **8MB Octal PSRAM**
  - Modem: SIM7670G LTE Cat-1, on UART1 (not USB), 115200 baud
- **CAN controller + transceiver (default, `CAN_USE_MCP2518FD=1`):** Soldered
  "CAN Transceiver MCP2518 board" -- Microchip MCP2518FD SPI CAN FD
  controller + ATA6561 transceiver, 40 MHz crystal. Receives both classic CAN
  and CAN FD. External wiring required, see below.
- **Alternative, classic CAN only (`CAN_USE_MCP2518FD=0`):** TI SN65HVD232D
  (or Adafruit CAN Pal) wired to the ESP32-S3's built-in TWAI controller.
  TWAI is not FD-tolerant -- it error-frames any CAN FD frame -- so never put
  it on a bus carrying CAN FD.
- **Bench test rig:** an ESP32-C5 + Adafruit CAN Pal generating random
  classic/FD frames, plus an end-to-end MQTT verifier -- see
  [bench/README.md](bench/README.md).

### Pin reference

Sourced from `utilities.h` in
[Xinyuan-LilyGO/LilyGo-Modem-Series](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series)
(`LILYGO_T_SIM7670G_S3` block) -- these are fixed by the PCB, not a firmware
choice. CAN pins match LilyGo's own official reference wiring for this exact
board (`examples/SimHatCanBusRecv`).

| Function | GPIO | Notes |
|---|---|---|
| Modem TX (ESP32->modem RXD) | 11 | |
| Modem RX (ESP32<-modem TXD) | 10 | |
| Modem PWRKEY | 18 | |
| Modem DTR | 9 | |
| Modem RESET | 17 | active LOW |
| Modem RING | 3 | strapping pin, fixed by LilyGo's PCB, input only |
| Board LED | 12 | active LOW |
| Battery ADC | 4 | |
| Solar ADC | 5 | |
| **MCP2518 SCK** | **16** | -> board SCK |
| **MCP2518 SDI (MOSI)** | **15** | -> board SDI |
| **MCP2518 SDO (MISO)** | **7** | <- board SDO |
| **MCP2518 NCS** | **1** | -> board NCS |
| **MCP2518 INT** | **2** | <- board INT (active low) |
| CAN TX (TWAI mode only) | 1 | -> SN65HVD232D pin 1 (D) |
| CAN RX (TWAI mode only) | 2 | <- SN65HVD232D pin 4 (R) |
| USB D-/D+ | 19/20 | native USB, used for programming |

**Reserved, do not repurpose:** GPIO 0/3/45/46 (strapping), 19/20 (USB),
26-32 (SPI flash), 33-37 (Octal PSRAM -- this board's module has 8MB OPI
PSRAM, which needs these on top of the flash pins other ESP32-S3 boards get
away without).

### CAN wiring: MCP2518 board (default)

The board needs **5V on VCC**: it feeds the ATA6561 transceiver (4.5-5.5V)
and an onboard LDO that makes 3.3V for the MCP2518FD, whose SPI/INT pins
are therefore 3.3V logic -- no level shifting. This board's header has no
"5V" pin: use **VIN**, which is VBUS (USB-C's 5V, live only with USB-C
plugged in). On 3V3, SPI still works off the LDO but the ATA6561 stays off
the bus, so the node silently receives nothing. The MCP board's two VCC pins
are tied together: wire one, and never 3V3 to the other. The SPI pins are the ones
LilyGo routes to its SimShield header; GPIO1/2 are free because TWAI isn't
used in this mode. INT wakes the CAN task as frames arrive (with a 10 ms
polling fallback).

```
 ESP32-S3 (T-SIM7670G-S3)          Soldered MCP2518 board           CAN bus
 ┌───────────────────┐         ┌──────────────────────┐
 │   GPIO16 ──────────┼────────►│ SCK                  │
 │   GPIO15 ──────────┼────────►│ SDI                  │
 │   GPIO7  ◄─────────┼─────────┤ SDO        CANH ─────┼──────────► CAN-H
 │   GPIO1  ──────────┼────────►│ NCS        CANL ─────┼──────────► CAN-L
 │   GPIO2  ◄─────────┼─────────┤ INT                  │
 │   VIN    ──────────┼────────►│ VCC (5V)             │
 │   GND    ──────────┼────────►│ GND                  │
 └───────────────────┘         │ INT0/INT1/CLKO: NC   │
                                └──────────────────────┘
```

- **Termination:** the board's 120Ω terminator is enabled by solder-bridging
  **JP1** (open from the factory). Bridge it only when this node sits at a
  physical end of the bus (e.g. on a bench) -- a vehicle bus is already
  terminated at both ends.
- Bitrates are in `include/can_pins.h`: `CAN_BITRATE_KBPS` (arbitration /
  classic, default 500) and `CAN_FD_DATA_BITRATE_KBPS` (FD data phase,
  default 2000). Both must match the other nodes.

### CAN transceiver wiring (SN65HVD232D, TWAI mode)

The SN65HVD232D is a single-3.3V-supply device (VCC = 3.0-3.6V) with logic
levels that match the ESP32-S3's GPIO directly -- **no level shifting
needed**. It has no Rs/slope-control or standby pin (unlike the SN65HVD230);
pins 5 and 8 are simply not connected.

```
 ESP32-S3 (T-SIM7670G-S3)          SN65HVD232D                Vehicle CAN bus
 ┌───────────────────┐         ┌───────────────────┐
 │                    │         │  1 D   ●    NC  8 │
 │   GPIO1 (CAN_TX) ──┼────────►│  (TXD)      (NC)  │
 │                    │         │                    │
 │   GPIO2 (CAN_RX) ◄─┼─────────┤  4 R        CANL 6 ├──────────► CAN-L
 │                    │         │  (RXD)             │
 │        3V3 ────────┼────────►│  3 VCC      CANH 7 ├──────────► CAN-H
 │                    │         │  (3.3V)            │
 │        GND ────────┼────────►│  2 GND       NC  5 │
 │                    │         │                    │
 └───────────────────┘         └───────────────────┘
```

- **VCC connects to the board's 3V3 rail, never 5V** -- the SN65HVD232D is
  not a 5V-tolerant part like MCP2551/TJA1050.
- **Termination:** a real vehicle CAN bus already has 120Ω termination at
  both physical ends (e.g. inside the ECU and gateway). Don't add another
  120Ω resistor when tapping into an existing bus. Only add one across
  CANH/CANL on your own breadboard if you're bench-testing this node in a
  loopback with no other node present.
- **This module has no onboard CAN transceiver** -- the wiring above is
  external, on a breadboard/protoboard, not part of the LilyGo PCB.

## Bench wiring reference (all pins)

| From | Pin | To | Pin |
|---|---|---|---|
| T-SIM7670G-S3 | GPIO16 | MCP2518 board | SCK |
| T-SIM7670G-S3 | GPIO15 | MCP2518 board | SDI |
| T-SIM7670G-S3 | GPIO7 | MCP2518 board | SDO |
| T-SIM7670G-S3 | GPIO1 | MCP2518 board | NCS |
| T-SIM7670G-S3 | GPIO2 | MCP2518 board | INT |
| T-SIM7670G-S3 | VIN (= USB VBUS 5V) | MCP2518 board | one VCC pin only (never 3V3) |
| T-SIM7670G-S3 | GND | MCP2518 board | GND |
| ESP32-C5-DevKitC-1 | GPIO4 | CAN Pal | TX |
| ESP32-C5-DevKitC-1 | GPIO15 | CAN Pal | RX |
| ESP32-C5-DevKitC-1 | 3V3 | CAN Pal | Vcc |
| ESP32-C5-DevKitC-1 | GND | CAN Pal | GND and SLNT |
| CAN Pal | H / L / GND | MCP2518 board | CANH / CANL / GND |

Termination: MCP2518 JP1 solder-bridged and CAN Pal switch ON, giving ~60 Ω
across CANH/CANL. Bus: 500 kbit/s nominal, 2 Mbit/s FD data. USB ports on
the Mac: S3 = `/dev/cu.usbmodem101`, C5 = `/dev/cu.usbmodem1101` (native
USB on both). Test procedure, results and open issues: [bench/README.md](bench/README.md).

## Architecture

Two FreeRTOS tasks + one shared queue, exactly matching the CAN task's output
regardless of simulated or real CAN source:

```
┌─────────────────────┐        ┌──────────────────┐        ┌─────────────────────┐
│  CAN task (core 0)  │        │  Shared queue     │        │  Net task (core 1)  │
│  can_handler.cpp     │──push─►│  CanFrame[512]    │──pull─►│  net_task.cpp       │
│                      │        │  (drop-oldest      │        │                      │
│  - real: MCP2518FD   │        │   on overflow)      │        │  - modem power-on    │
│  - sim: synthetic    │        │                      │        │  - LTE + MQTT connect│
│    generator          │        └──────────────────┘        │  - batch + serialize │
│  - capture_us at the  │                                     │  - publish, backoff  │
│    instant of RX      │                                     │  - LittleFS spool    │
│  - never blocks        │                                     │    on publish fail   │
└─────────────────────┘                                     └─────────────────────┘
```

- **CAN task** captures `esp_timer_get_time()` (monotonic, microsecond) the
  instant a frame leaves its source, before it touches the queue -- so
  queueing or network jitter never pollutes inter-frame timing. With the
  MCP2518FD, that instant is the controller's own 1 µs end-of-frame
  timestamp, mapped onto `esp_timer` -- so time a frame spends waiting in the
  controller's FIFO for the SPI read doesn't shift it. If the queue is full,
  it drops the *oldest* queued frame and increments a counter; it never
  blocks.
- **Time sync** (`time_sync.cpp`) is separate from frame capture entirely: it
  tracks an offset between the monotonic clock and true epoch time, synced
  from SNTP (over PPP or WiFi) at boot and every 5 minutes. Each
  frame's `epoch_ms` is computed by applying that offset to *its own*
  `capture_us` at serialization time -- a resync only shifts the offset for
  frames serialized afterward, it never touches an already-captured
  timestamp.
- **Net task** owns the modem, MQTT connection, batching, and all
  reconnect/retry logic. It rechecks modem network registration separately
  from the MQTT-level connection (a modem can lose LTE registration while
  MQTT still thinks it's connected).

## MQTT payload format

JSON, documented in `telemetry_format.h`. One published message per batch:

```json
{
  "device_id": "esp32-sim7670g-poc-01",
  "seq": 42,
  "dropped_frames": 0,
  "frames": [
    { "can_id": 256, "extd": false, "dlc": 8, "data": "0102030405060708", "ts_ms": 1735500000123 },
    { "can_id": 291, "extd": false, "dlc": 12, "data": "0102030405060708090a0b0c", "ts_ms": 1735500000125, "fd": true, "brs": true }
  ]
}
```

`dlc` is the payload length in **bytes** (0-8 classic, up to 64 for CAN
FD), not the 4-bit DLC code from the wire. CAN FD frames add `"fd": true`
and `"brs"` (bit rate switch); classic frames omit both, so they cost no
extra uplink bytes.

`ts_ms` is Unix epoch milliseconds, per-frame, at full resolution -- batching
never averages or collapses timestamps. `dropped_frames` reports the number
of frames dropped since the *previous* successfully-built batch, so gaps are
visible server-side even under sustained overload.

JSON is used here for PoC clarity and easy debugging (readable in any MQTT
client). For production, a binary format (CBOR or protobuf) would
meaningfully cut per-frame overhead on a Cat-1 link -- see below.

## Configuration

Non-sensitive settings (transport mode, batching, CAN sim rates) go directly
in `include/telemetry_config.h`. Credentials (WiFi/APN/HTTPS ingest) don't -- copy
`.env.example` to `.env` (git-ignored) and put real values there instead;
`load_env.py` injects `.env` at build time as compiler defines, which
override the placeholders in `telemetry_config.h`. No `.env` -> the
placeholders are used as-is, so a fresh clone still builds.

Edit `include/telemetry_config.h`:

| Setting | What it controls |
|---|---|
| `TELEMETRY_USE_SIMULATED_CAN` | `1` = synthetic frames (no CAN hardware needed), `0` = real CAN (MCP2518FD or TWAI, per `CAN_USE_MCP2518FD` in `include/can_pins.h`) |
| `TELEMETRY_USE_WIFI` | `1` = transmit over the ESP32-S3's onboard WiFi radio instead of the cellular modem (bench testing without a SIM); `0` = cellular (default, real deployment path) |
| `WIFI_AP_MODE` | Only used when `TELEMETRY_USE_WIFI=1`. `1` = the ESP32-S3 hosts its own network (`WIFI_AP_SSID`/`_PASSWORD`) for your laptop/phone to join directly -- no existing WiFi needed, but no internet uplink either, so batches are spooled and not POSTed, and NTP time sync is skipped. `0` = join an existing network instead (the settings below). |
| `WIFI_SSID`, `WIFI_ENTERPRISE`, `WIFI_PASSWORD` / `WIFI_EAP_*` | Only used when `TELEMETRY_USE_WIFI=1` and `WIFI_AP_MODE=0`. Set `WIFI_ENTERPRISE=1` for WPA2/WPA3-Enterprise networks (802.1X, e.g. eduroam or a corporate network) that ask for a username **and** password to join directly -- PEAP/MSCHAPv2 only, not EAP-TLS. Leave it `0` for an ordinary single-password network. |
| `TELEMETRY_APN` / `_USER` / `_PASS` | Your carrier's APN credentials (cellular mode only) |
| `HTTPS_INGEST_URL` | Full POST URL. For Supabase: `https://<project-ref>.supabase.co/rest/v1/<table>`. Put it in `.env`, not in the header. |
| `HTTPS_API_KEY` | Sent as the `apikey` header. For Supabase this is `SUPABASE_PUBLISHABLE_KEY` (`sb_publishable_...`), or the legacy anon JWT. Never the secret / `service_role` key. Put it in `.env`. |
| `TELEMETRY_DEVICE_ID` | Identifies this device in every batch |
| `CAN_QUEUE_DEPTH`, `BATCH_WINDOW_MS`, `BATCH_MAX_FRAMES`, `BATCH_MAX_JSON_BYTES` | Batching/backpressure tuning. Every `BATCH_WINDOW_MS` the net task POSTs as many batches as it takes to empty the queue, each capped by `BATCH_MAX_FRAMES` and by `BATCH_MAX_JSON_BYTES` of JSON |
| `TELEMETRY_STRESS_TEST` | Leave `0`. The old MQTT filler burst is not POSTed to the ingest URL. |
| `MODEM_USB_BENCH_MODE` | `1` = power the modem on and stop -- no registration/MQTT/stress test, UART1 stays silent so a PC can drive the modem directly over its own USB port (see [USB direct-to-modem bench](#usb-direct-to-modem-bench-bypassing-the-esp32-modem-uart)); `0` = normal operation (default). Cellular only. |
| `SMS_TEST_ENABLED` | `1` = send one test SMS after the modem registers on the network, to `SMS_TEST_NUMBER` (see `include/sms_config.h`); `0` = off (default). Cellular only -- build error under `TELEMETRY_USE_WIFI=1` |

CAN hardware and bitrates are in `include/can_pins.h`: `CAN_USE_MCP2518FD`
(default `1`), `CAN_BITRATE_KBPS` (default 500 kbit/s) and
`CAN_FD_DATA_BITRATE_KBPS` (default 2000 kbit/s, MCP2518FD only) -- **must
match the bus you're connecting to**; body/comfort buses on some vehicles
run at 125 kbit/s or slower, while OBD-II diagnostic CAN is standard at
500 kbit/s.

## Building

```
pio run                # build
pio run -t upload       # flash (board connected via its ESP-USB port)
pio device monitor      # serial console at 115200
```

Start with `TELEMETRY_USE_SIMULATED_CAN=1` (the shipped default) to prove out
cellular connect -> batch -> MQTT publish -> broker receipt with no CAN
hardware attached. Only flip to `0` once that path is demonstrated working.

## Bench tools (no hardware required)

These let you exercise the full batch -> MQTT -> broker -> subscriber path with
`TELEMETRY_USE_SIMULATED_CAN=1` and `TELEMETRY_USE_WIFI=1`/`WIFI_AP_MODE=1`
(the shipped `.env.example` defaults), before any CAN wiring or SIM card is
involved.

1. **Broker** -- run Mosquitto with the repo's config, which opens both the
   plain MQTT port the device publishes to and a WebSocket port for the
   browser dashboard:
   ```
   mosquitto -c mosquitto.conf -d
   ```
   If your laptop is the SoftAP client (joined the ESP32's `esp32-telemetry`
   network), it's reachable at `192.168.4.2` -- the default `MQTT_BROKER_HOST`
   in this mode.
2. **Raw listener** -- `pip3 install -r requirements.txt`, then:
   ```
   python3 listener.py --ap          # points at the SoftAP laptop broker
   python3 listener.py --simulate    # no broker/device needed; publishes mock frames to itself
   ```
   Prints every message's hex dump/decoded text as it arrives -- useful for
   confirming raw bytes are flowing before trusting the dashboard's parsed
   view.
3. **Web dashboard** -- open `web/dashboard.html` directly in a browser (no
   server needed), leave the broker host/port at their SoftAP defaults
   (`192.168.4.2:9001`) or point them at your own broker's WebSocket
   listener, and click Connect. Renders each batch's `frames[]` as a live
   table plus running batch/seq/dropped-frame counters.

## Demonstration checklist (real CAN hardware)

1. Set `TELEMETRY_USE_SIMULATED_CAN` to `0`, set `MQTT_BROKER_HOST` to your
   own broker (not the public HiveMQ default), and set your APN.
2. Wire the MCP2518 board (or, in TWAI mode, the SN65HVD232D) per the
   diagrams above; connect CANH/CANL to the vehicle bus (e.g. OBD-II pins 6
   and 14).
3. Subscribe to `MQTT_TOPIC_TELEMETRY` on your broker (e.g.
   `mosquitto_sub -h <host> -t esp32_cellular_telemetry/batch -v`) before
   powering the board, so you catch the first batches.
4. Power the board. Watch the serial monitor for: modem AT response ->
   network registration -> GPRS/PDP context up -> initial time sync ->
   MQTT connected.
5. Confirm frames arrive with plausible `ts_ms` values (compare against
   wall-clock time) and that `can_id`/`data` match known frames from your
   vehicle (e.g. cross-check against a known wheel-speed or RPM ID from the
   vehicle's DBC file, if you have one).
6. To exercise drop-reporting: briefly disconnect the antenna or block the
   MQTT broker, let the queue fill, then restore connectivity and confirm
   the next batch's `dropped_frames` is nonzero and spooled batches (from
   LittleFS) replay in order.

## Uplink stress test

Set `TELEMETRY_STRESS_TEST=1` in `include/telemetry_config.h` (or via `.env`)
to run a one-time throughput burst: once MQTT connects, the device publishes
fixed filler payloads back-to-back on `MQTT_TOPIC_TELEMETRY/stress` for
`STRESS_TEST_DURATION_S` (default 60s), through the same TLS -> modem ->
cellular path real batches use, and logs achieved bits/sec against
`STRESS_TARGET_BPS` (default 5 Mbps, the SIM7670G's Cat-1 uplink spec
ceiling). It runs exactly once per boot, then the device resumes normal
telemetry publishing.

Watch it land with the listener:

```
python3 listener.py --stress -b
```

**Read this before running it on cellular:** it is designed to try to move
tens of megabytes over `STRESS_TEST_DURATION_S`, and every byte it manages to
push is real billed data on a metered SIM -- there is no dry-run mode. It is
also very unlikely to get anywhere near the 5 Mbps target -- see below for
what the real ceiling actually is and why.

**What the publish is actually sent as:** the modem is a PPP bearer only
(Arduino core 3.x `PPP` library, CMUX mode), so TCP runs in the ESP32's lwIP
stack and TLS in mbedTLS. `rawMqttPublish()` in `net_task.cpp` builds the
whole MQTT PUBLISH packet and hands it to the socket in one `write()` -- there
are no per-chunk `AT+CCHSEND`/`AT+CIPSEND` round-trips any more.

**Measured on this hardware before the switch to PPP** (T-Mobile
`fast.t-mobile.com`, HiveMQ Cloud broker over TLS, modem-side TLS via
`AT+CCHSEND`), holding everything else fixed and only changing chunk size:

| Chunk size | Sustained throughput |
|---|---|
| 255 bytes (the old `mqtt.publish()`-chunked path) | ~50.5 Kbps |
| 1024 bytes (`rawMqttPublish`) | ~67.9 Kbps |
| 1400 bytes (`rawMqttPublish`, current default) | ~72.0 Kbps |

Each of those chunks blocked on the modem confirming the send before the
next could start, so throughput was bounded by chunk size / per-chunk round
trip, not by the UART. Under PPP the ceiling is the UART itself:
`MODEM_TARGET_BAUDRATE` / 10 bytes/s (~740 Kbps at 921600), less a few
percent of PPP/TCP/TLS framing. A result well under 5 Mbps is still the
expected finding, not a bug in the test.

## USB direct-to-modem bench (bypassing the ESP32<->modem UART)

This board has a **second** connector for the SIM7670G's own USB interface:
a microUSB port next to a small boot button, right on the LilyGo PCB,
separate from the ESP32-S3's Type-C port. It's normally used for flashing
modem firmware, but the modem also answers ordinary AT commands over it in
normal (non-flash) operation -- which means a PC can talk to the modem
directly, with the ESP32/UART1 out of the picture entirely.

**Measured result: this was NOT faster.** At a matched 1024-byte chunk size,
`usb_modem_bench.py` over the modem's own USB measured ~40.6 Kbps, slower
than the same chunk size sent from the firmware over the "slow" 115200-baud
UART (~67.9 Kbps -- see the table in [Uplink stress
test](#uplink-stress-test)). The UART was never actually saturated at these
rates (67.9-72 Kbps is well under its ~92 Kbps raw capacity), so replacing
it with USB had nothing to win: the real bottleneck was the
`uint8_t`-capped chunk size, not which physical link carried the AT
commands. Kept below because it's still a legitimate way to test the modem
in isolation (e.g. to rule the ESP32 firmware in or out as a suspect), just
not a throughput win on its own.

**Before connecting anything:** don't hold the boot button while plugging
this port in unless you're deliberately entering firmware-flash mode --
LilyGo's own issue tracker has reports of bricked modems from a mismatched
firmware tool in that mode. If the modem doesn't answer plain `AT`, unplug
and replug without touching the button before assuming something's wrong.

1. Set `MODEM_USB_BENCH_MODE=1` in `include/telemetry_config.h` (or `.env`)
   and reflash over the ESP32's **Type-C** port as usual. This makes the
   firmware power the modem on and then stop -- no registration, no MQTT, no
   stress test, and critically, no more AT traffic on UART1. **This step
   matters**: if the old firmware is still running normally, it's already
   driving the modem over UART1 (registering, publishing MQTT, etc.) at the
   same time your PC would be issuing AT commands over USB -- two masters on
   one modem will produce confusing failures, not a clean measurement.
2. Plug the SIM7670G's microUSB port into your computer (normal mode, boot
   button untouched).
3. `pip3 install -r requirements.txt`, then:
   ```
   python3 usb_modem_bench.py --probe                          # finds the AT port
   python3 usb_modem_bench.py --port <the port above> --duration 20
   ```
   This drives the AT socket command sequence the firmware used for MQTT
   publish before it switched to PPP (`AT+CGDCONT`/`+NETOPEN`/`+CIPOPEN`/`+CIPSEND`, or
   `+CSSLCFG`/`+CCHSTART`/`+CCHOPEN`/`+CCHSEND` when `MQTT_USE_TLS=1`) --
   same modem, same cellular link, same `MQTT_BROKER_HOST`/`_PORT` -- just
   issued straight from the PC instead of relayed through UART1, in chunks
   up to `--chunk-size` (default 1024 bytes).
4. Compare the printed avg bps against the firmware's own stress test
   result. Before the PPP switch they came out close (USB slightly slower,
   if anything) -- see the measured numbers above.

**This also spends real billed data on a metered SIM, same as the firmware
stress test** -- there is no dry-run mode here either.

## Throughput and bottlenecks

Measured on the bench, 2026-10-22: MCP2518FD at 500 kbit/s arbitration /
2 Mbit/s FD data, LTE uplink, MQTT over TLS. Details are in
[bench/README.md](bench/README.md#bench-results-2026-10-22).

| Stage | Frames/s | Throughput | Limit |
|---|---|---|---|
| CAN bus into the S3 | ~4,700 max (2,000 sustained, 0 errors) | ~525 kbit/s of CAN payload | Bus bit rates (bus saturated) |
| S3 to modem to broker | **605** | 553 kbit/s of JSON = **68 kbit/s of CAN payload** | ESP32-to-modem UART (921,600 baud = 737 kbit/s raw) |

The uplink pipe is full, but mostly with JSON overhead: a frame with ~14
bytes of data costs ~115 bytes of JSON (hex doubles every byte, and field
names and a 13-digit timestamp repeat per frame). Carrying the bus's full
525 kbit/s of payload as JSON would need ~4.2 Mbit/s of uplink, about 6x
the UART. The bottlenecks, in order:

1. **JSON encoding (~8x the raw payload).** This is the biggest lever. A
   compact layout (one batch base timestamp plus per-frame ms deltas,
   positional arrays, base64 data) measures ~2.5-3x; see
   [the proposal below](#proposed-compact-timestamp-delta-json-not-implemented-yet). A binary format
   (CBOR/protobuf) is ~1.5-2x.
2. **ESP32-to-modem UART, 737 kbit/s raw.** PPP/TCP/TLS framing leaves ~75%
   for payload. Raising `MODEM_TARGET_BAUDRATE` lifts it, but the board has
   no RTS/CTS, so byte loss at higher rates needs testing.
3. **Default batching caps delivery at ~34 frames/s** (`CAN_QUEUE_DEPTH`
   512 per `BATCH_WINDOW_MS` 15 s), deliberately, to bound cellular data
   cost. Both are `.env`-overridable. The 605 frames/s figure used 4096 /
   1000 ms.
4. **Offline spooling** stalls the CPUs during each LittleFS write, which
   drops a few frames at ~2,000 frames/s (the MCP2518's 26-frame FIFO lasts
   13 ms). There's no flash writing while online.

Not bottlenecks: the CAN bus at realistic loads (real vehicles typically
run at 20-50% bus load), the LTE link itself (the SIM7670G is Cat-1,
~5 Mbit/s up), and the MQTT broker (HiveMQ took 553 kbit/s with zero bad
frames). Switching MQTT to HTTPS doesn't remove any of the above, and
per-request headers and round trips add cost unless the TLS connection is
reused across POSTs.

## Proposed: compact timestamp-delta JSON (not implemented yet)

The biggest uplink lever is the per-frame JSON (see above). Sending the
timestamp once per batch (`t0`) and a small per-frame ms offset, with frames
as positional arrays, keeps the payload JSON (so MQTT, HTTPS/Supabase and
`jsonb` all still work) while cutting its size ~2.5-3x:

```json
{"device_id":"esp32-sim7670g-poc-01","seq":42,"dropped_frames":0,"t0":1761184500123,
 "f":[[291,6,"a1b2c3d4e5f6a7b8c9d0e1f2",0],[1793,0,"0102030405060708",2]]}
```

Each frame is `[can_id, flags, data, ms after t0]`, where `flags` is a
bitmask: 1 = extended ID, 2 = CAN FD, 4 = bit rate switch. Measured on
60-frame batches:

| Layout | Bench mix (~14 B payload) | Classic car frames (8 B) |
|---|---|---|
| Current format | 117 B/frame (7.5x payload) | 85 B/frame (10.6x) |
| `t0` + offset, short keys | 63 B (4.0x) | 41 B (5.1x) |
| `t0` + offset, arrays (above) | 48 B (3.1x) | 31 B (3.9x) |
| Arrays + base64 `data` | 39 B (2.5x) | 27 B (3.4x) |

On the same uplink that's roughly 2.4-3x more frames/s: ~1,450-1,800 frames/s
instead of the measured 605.

Trade-offs and work needed:
- Harder to read by eye. `web/dashboard.html`, `bench/verify_frames.py`
  and the Supabase table on the HTTPS branch all have to understand it. Plan
  to put it behind a config switch, so the current format keeps working
  until they're updated.
- If Supabase stores one row per frame, the server (or a trigger in the
  database) has to expand each batch. One row per batch with the frames as
  `jsonb` works directly.
- Millisecond offsets drop the microsecond capture precision. Microsecond
  offsets cost about 3 more characters per frame.
- Beyond this, a binary format (CBOR/protobuf, ~1.5-2x payload) is the next
  step, at the cost of JSON's readability and direct `jsonb` storage.

## Known limitations

- **Timestamp accuracy** is bounded by SNTP over the cellular link --
  typically within tens of milliseconds of true UTC, not GPS-disciplined
  precision. Inter-frame
  *relative* timing (frame-to-frame within a short window) is far more
  accurate, since it comes entirely from the monotonic `esp_timer_get_time()`
  capture and is unaffected by sync jitter.
- **No onboard CAN transceiver** -- this board requires the external wiring
  described above; it is not a plug-and-play CAN device.
- **Best-effort delivery, not guaranteed.** Batches that fail to publish are
  spooled to LittleFS and replayed on reconnect, capped at
  `SPOOL_MAX_FILES` (oldest dropped beyond that). This is not a durable,
  unbounded store-and-forward system -- a very long outage will still lose
  the oldest spooled data.
- **JSON bandwidth overhead.** The current per-frame JSON measures ~7.5-10x
  the raw CAN payload (~115 bytes for a ~14-byte frame), which makes it the
  main uplink bottleneck (see [Throughput and bottlenecks](#throughput-and-bottlenecks)).
  A compact JSON layout or a binary format (CBOR/protobuf) cuts that 3-5x.
- **No production security hardening, OTA, or fleet management.** MQTT
  credentials are broker auth only (no TLS configured by default, no
  device-cert-based mTLS); this is explicitly out of scope for the PoC.
- **CAN filter accepts everything** (MCP2518FD filter 0 with an all-zero
  mask, or `TWAI_FILTER_CONFIG_ACCEPT_ALL()` in TWAI mode).
  Fine for a PoC observing all bus traffic; a production node would filter
  to the IDs it actually needs.
- **Uplink is capped by the ESP32<->modem UART**, not the SIM7670G's Cat-1
  spec (~5 Mbps): PPP moves every byte over it, so the ceiling is
  `MODEM_TARGET_BAUDRATE` / 10 bytes/s. The board has no RTS/CTS routed to
  the modem, so very high baud rates may lose bytes (TCP retransmits them,
  at a throughput cost).
- **WiFi mode (`TELEMETRY_USE_WIFI=1`) is a bench-testing convenience**, not
  the deployment target -- it lets you prove the batch/MQTT/spool pipeline
  without a SIM card. It supports WPA2/WPA3-Personal and -Enterprise
  (PEAP/MSCHAPv2 only); it cannot join a captive-portal network (one with a
  browser login page), since the ESP32 has no browser to complete that flow.
