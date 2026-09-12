# ESP32 CAN-to-Cellular Telemetry PoC

Proof-of-concept firmware proving a hardware-to-backend data path: real (or
simulated) vehicle CAN frames -> ESP32-S3 -> LTE (SIM7670G) -> MQTT broker,
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
- **CAN transceiver:** TI SN65HVD232D, single 3.3V supply, wired directly to
  the ESP32-S3's built-in TWAI controller (no onboard transceiver on this
  board -- external wiring required, see below).

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
| **CAN TX (TWAI)** | **1** | -> SN65HVD232D pin 1 (D) |
| **CAN RX (TWAI)** | **2** | <- SN65HVD232D pin 4 (R) |
| USB D-/D+ | 19/20 | native USB, used for programming |

**Reserved, do not repurpose:** GPIO 0/3/45/46 (strapping), 19/20 (USB),
26-32 (SPI flash), 33-37 (Octal PSRAM -- this board's module has 8MB OPI
PSRAM, which needs these on top of the flash pins other ESP32-S3 boards get
away without).

### CAN transceiver wiring (SN65HVD232D)

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

## Architecture

Two FreeRTOS tasks + one shared queue, exactly matching the CAN task's output
regardless of simulated or real CAN source:

```
┌─────────────────────┐        ┌──────────────────┐        ┌─────────────────────┐
│  CAN task (core 0)  │        │  Shared queue     │        │  Net task (core 1)  │
│  can_handler.cpp     │──push─►│  CanFrame[512]    │──pull─►│  net_task.cpp       │
│                      │        │  (drop-oldest      │        │                      │
│  - real: TWAI driver │        │   on overflow)      │        │  - modem power-on    │
│  - sim: synthetic    │        │                      │        │  - LTE + MQTT connect│
│    generator          │        └──────────────────┘        │  - batch + serialize │
│  - capture_us at the  │                                     │  - publish, backoff  │
│    instant of RX      │                                     │  - LittleFS spool    │
│  - never blocks        │                                     │    on publish fail   │
└─────────────────────┘                                     └─────────────────────┘
```

- **CAN task** captures `esp_timer_get_time()` (monotonic, microsecond) the
  instant a frame leaves its source, before it touches the queue -- so
  queueing or network jitter never pollutes inter-frame timing. If the queue
  is full, it drops the *oldest* queued frame and increments a counter; it
  never blocks.
- **Time sync** (`time_sync.cpp`) is separate from frame capture entirely: it
  tracks an offset between the monotonic clock and true epoch time, synced
  from the modem's network time (`AT+CCLK`) at boot and every 5 minutes. Each
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
    { "can_id": 256, "extd": false, "dlc": 8, "data": "0102030405060708", "ts_ms": 1735500000123 }
  ]
}
```

`ts_ms` is Unix epoch milliseconds, per-frame, at full resolution -- batching
never averages or collapses timestamps. `dropped_frames` reports the number
of frames dropped since the *previous* successfully-built batch, so gaps are
visible server-side even under sustained overload.

JSON is used here for PoC clarity and easy debugging (readable in any MQTT
client). For production, a binary format (CBOR or protobuf) would
meaningfully cut per-frame overhead on a Cat-1 link -- see below.

## Configuration

Non-sensitive settings (transport mode, batching, CAN sim rates) go directly
in `include/telemetry_config.h`. Credentials (WiFi/APN/MQTT) don't -- copy
`.env.example` to `.env` (git-ignored) and put real values there instead;
`load_env.py` injects `.env` at build time as compiler defines, which
override the placeholders in `telemetry_config.h`. No `.env` -> the
placeholders are used as-is, so a fresh clone still builds.

Edit `include/telemetry_config.h`:

| Setting | What it controls |
|---|---|
| `TELEMETRY_USE_SIMULATED_CAN` | `1` = synthetic frames (no CAN hardware needed), `0` = real TWAI driver |
| `TELEMETRY_USE_WIFI` | `1` = transmit over the ESP32-S3's onboard WiFi radio instead of the cellular modem (bench testing without a SIM); `0` = cellular (default, real deployment path) |
| `WIFI_AP_MODE` | Only used when `TELEMETRY_USE_WIFI=1`. `1` = the ESP32-S3 hosts its own network (`WIFI_AP_SSID`/`_PASSWORD`) for your laptop/phone to join directly -- no existing WiFi needed, but no internet uplink either, so point `MQTT_BROKER_HOST` at a broker running on whatever device joins this AP, and note NTP time sync is skipped in this mode. `0` = join an existing network instead (the settings below). |
| `WIFI_SSID`, `WIFI_ENTERPRISE`, `WIFI_PASSWORD` / `WIFI_EAP_*` | Only used when `TELEMETRY_USE_WIFI=1` and `WIFI_AP_MODE=0`. Set `WIFI_ENTERPRISE=1` for WPA2/WPA3-Enterprise networks (802.1X, e.g. eduroam or a corporate network) that ask for a username **and** password to join directly -- PEAP/MSCHAPv2 only, not EAP-TLS. Leave it `0` for an ordinary single-password network. |
| `TELEMETRY_APN` / `_USER` / `_PASS` | Your carrier's APN credentials (cellular mode only) |
| `MQTT_BROKER_HOST` / `_PORT` | Your broker. Ships pointed at the public HiveMQ broker for an initial connectivity smoke test only -- **do not** use it for real vehicle data |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | Broker auth, if required |
| `MQTT_TOPIC_TELEMETRY` | Publish topic |
| `TELEMETRY_DEVICE_ID` | Identifies this device in every batch |
| `CAN_QUEUE_DEPTH`, `BATCH_WINDOW_MS`, `BATCH_MAX_FRAMES` | Batching/backpressure tuning |
| `TELEMETRY_STRESS_TEST` | `1` = run a one-time, time-bounded uplink throughput burst after MQTT connects (see [Uplink stress test](#uplink-stress-test)); `0` = normal operation (default) |
| `MODEM_USB_BENCH_MODE` | `1` = power the modem on and stop -- no registration/MQTT/stress test, UART1 stays silent so a PC can drive the modem directly over its own USB port (see [USB direct-to-modem bench](#usb-direct-to-modem-bench-bypassing-the-esp32-modem-uart)); `0` = normal operation (default). Cellular only. |
| `SMS_TEST_ENABLED` | `1` = send one test SMS after the modem registers on the network, to `SMS_TEST_NUMBER` (see `include/sms_config.h`); `0` = off (default). Cellular only -- build error under `TELEMETRY_USE_WIFI=1` |

CAN bitrate is in `include/can_pins.h` (`CAN_BITRATE_KBPS`, default 500
kbit/s) -- **must match the vehicle bus you're connecting to**; body/comfort
buses on some vehicles run at 125 kbit/s or slower, while OBD-II diagnostic
CAN is standard at 500 kbit/s.

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
2. Wire the SN65HVD232D per the diagram above; connect CANH/CANL to the
   vehicle bus (e.g. OBD-II pins 6 and 14).
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

**What the publish is actually sent as:** it does NOT go through
`mqtt.publish()`. `PubSubClient`'s own writer chunks every write to
`MQTT_MAX_TRANSFER_SIZE` (255 bytes, capped by a `uint8_t` inside
`PubSubClient` itself -- see the comment in `platformio.ini`; going over 255
there truncates to 0 and hangs). That cap is fine for real telemetry batches,
but it silently caps how big a single `AT+CCHSEND`/`AT+CIPSEND` chunk can be,
which turned out to matter a lot for throughput (see measurements below). So
the stress test instead builds the MQTT PUBLISH packet by hand
(`rawMqttPublish()` in `net_task.cpp`) and writes it to the same already-open
socket in `STRESS_RAW_CHUNK_BYTES`-sized pieces -- default 1400 bytes,
just under the ~1460-1500 byte single-send cap SIMCom's AT command manual
documents for CCHSEND/CIPSEND. Raise `STRESS_RAW_CHUNK_BYTES` further at your
own risk -- past that cap the modem fails the send outright, it doesn't
chunk for you.

**Measured on this hardware** (T-Mobile `fast.t-mobile.com`, HiveMQ Cloud
broker over TLS), holding everything else fixed and only changing chunk size:

| Chunk size | Sustained throughput |
|---|---|
| 255 bytes (the old `mqtt.publish()`-chunked path) | ~50.5 Kbps |
| 1024 bytes (`rawMqttPublish`) | ~67.9 Kbps |
| 1400 bytes (`rawMqttPublish`, current default) | ~72.0 Kbps |

So the `uint8_t` chunk cap was real and was costing a genuine ~30%+ of
achievable throughput -- but the gain flattens out well short of the 92 Kbps
raw-UART figure and nowhere near the 5 Mbps Cat-1 spec. That flattening
points at the modem's own AT-command/TLS processing plus the actual cellular
RF link as the real remaining ceiling on this SIM/carrier, not the
ESP32<->modem UART (115200 baud, `MODEM_BAUDRATE` in `include/board_pins.h`)
-- raising the baud rate is unlikely to move this number much further. A
result well under 5 Mbps is the expected finding, not a bug in the test.

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
   This drives the same AT command sequence `net_task.cpp` uses for MQTT
   publish (`AT+CGDCONT`/`+NETOPEN`/`+CIPOPEN`/`+CIPSEND`, or
   `+CSSLCFG`/`+CCHSTART`/`+CCHOPEN`/`+CCHSEND` when `MQTT_USE_TLS=1`) --
   same modem, same cellular link, same `MQTT_BROKER_HOST`/`_PORT` -- just
   issued straight from the PC instead of relayed through UART1, and in
   chunks up to `--chunk-size` (default 1024 bytes, well above this
   firmware's 255-byte `MQTT_MAX_TRANSFER_SIZE` workaround) instead of the
   firmware's 255-byte cap.
4. Compare the printed avg bps against the firmware's own stress test result
   at the same `--chunk-size`/`STRESS_RAW_CHUNK_BYTES`. On this hardware they
   came out close (USB slightly slower, if anything) -- see the measured
   numbers above.

**This also spends real billed data on a metered SIM, same as the firmware
stress test** -- there is no dry-run mode here either.

## Known limitations

- **Timestamp accuracy** is bounded by the modem's network-time
  (`AT+CCLK`) sync granularity and by the ESP32<->modem AT-command
  round-trip latency at sync time -- typically within a few hundred
  milliseconds of true UTC, not GPS-disciplined precision. Inter-frame
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
- **JSON bandwidth overhead.** At full detail, JSON costs roughly 2-3x the
  bytes of a packed binary format per frame. Fine for a PoC on a Cat-1 link
  at moderate frame rates; a production version should switch to CBOR or
  protobuf.
- **No production security hardening, OTA, or fleet management.** MQTT
  credentials are broker auth only (no TLS configured by default, no
  device-cert-based mTLS); this is explicitly out of scope for the PoC.
- **CAN filter accepts everything** (`TWAI_FILTER_CONFIG_ACCEPT_ALL()`).
  Fine for a PoC observing all bus traffic; a production node would filter
  to the IDs it actually needs.
- **~70 Kbps measured ceiling on the cellular uplink** on this hardware/SIM,
  regardless of the SIM7670G's own Cat-1 spec (~5 Mbps). It is NOT the
  ESP32<->modem UART (115200 baud, ~92 Kbps raw) -- see [Uplink stress
  test](#uplink-stress-test) for the measurements: fixing an artificial
  255-byte AT+CCHSEND chunk cap recovered most of the gap to the UART's raw
  capacity, and going straight to the modem over its own USB port (bypassing
  the UART entirely) didn't beat it either. What's left points at the
  modem's own AT/TLS processing and the real cellular RF link on this
  carrier/SIM, not any one link in the ESP32's own pipeline.
- **WiFi mode (`TELEMETRY_USE_WIFI=1`) is a bench-testing convenience**, not
  the deployment target -- it lets you prove the batch/MQTT/spool pipeline
  without a SIM card. It supports WPA2/WPA3-Personal and -Enterprise
  (PEAP/MSCHAPv2 only); it cannot join a captive-portal network (one with a
  browser login page), since the ESP32 has no browser to complete that flow.
