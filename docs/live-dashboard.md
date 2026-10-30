# Live dashboard contract

What a browser dashboard gets from the current pipeline, and how to merge it.
Nothing here needs a Supabase change: it describes the broadcast that
`supabase/functions/telemetry-ingest` already publishes.

Verified 2026-10-29 on the team project (`rwbaahoitfipazbyzlkm`) with the
publishable key and `scripts/watch_live.py`, driven by
`scripts/post_synth_htp.py --seconds 8` from a host (not from the board).

## What the live feed is (and is not)

- One broadcast message per accepted POST. A POST carries 1..8 one-second
  windows of **one** channel group.
- Each message holds **the last sample of the last window only**. A 10 Hz
  channel shows up on the live feed at most once per POST, so about 1 Hz in
  steady state. Every sample is stored in `telemetry.staging_chunks`; use
  that for charts that need full resolution.
- The firmware currently sends **synthetic** data (`HTP_SYNTH_MODE=1`). CAN is
  not mapped onto channel set 1 yet. Synthetic windows are generated after the
  second they describe ends and stamped with that second, so `t_us` is a
  plausible capture time, not a measured one.

## Subscribing

| | |
|---|---|
| URL | `wss://<project-ref>.supabase.co/realtime/v1/websocket?apikey=<publishable key>&vsn=1.0.0` |
| Topic | `telemetry:live:{session_id}` (Phoenix topic `realtime:telemetry:live:{session_id}`) |
| Event | `live` |
| Channel type | public broadcast (`private: false`). The publishable key is enough. No RLS on `realtime.messages` is involved. |

supabase-js:

```js
const ch = supabase.channel(`telemetry:live:${sessionId}`)
  .on("broadcast", { event: "live" }, ({ payload }) => onLive(payload))
  .subscribe();
```

If Realtime authorization (private channels) is ever turned on for the
project, the function must send `"private": true` and the browser needs an
authenticated session plus a `realtime.messages` select policy. Not the case
today.

## Payload

```json
{
  "seq": 1793323977,
  "send_timestamp_ms": 1793323978012,
  "t_us": 1793323977900000,
  "values": { "engine.rpm": 8123.5, "engine.tps": 41.2, "...": 0 }
}
```

| Field | Meaning |
|---|---|
| `seq` | seq of the last window in the POST. Per `(session, group)`. The firmware uses the window's unix second, so it also tells you which second the values are from. |
| `t_us` | unix microseconds of the sample in `values`: `t_start + (rate_hz - 1) / rate_hz` s. 10 Hz groups: window start + 0.9 s. 1 Hz group: window start. |
| `send_timestamp_ms` | board's clock when the POST was attempted. Only for latency measurement. |
| `values` | channel id -> value, in the channel's units (below), already scaled. See "Dropouts" below. |

There is **no group key** in the payload. Each message contains exactly the
channels of one group, so the group is the group of any key in `values`
(table below).

## Merge rules

1. **Partial updates.** A message updates only its group's channels. Keep the
   last value per channel and merge; never clear channels that are absent.
2. **Ordering.** Keep `lastT[group]`. Apply a message only if
   `t_us > lastT[group]`; drop it otherwise. After an outage the board
   uploads its backlog oldest first, and every backlog POST is broadcast
   too, so older readings arrive after newer ones would have. Duplicates
   (a retried POST) have equal `t_us` and are dropped by the same rule.
3. **Staleness.** Show a channel group as stale when
   `Date.now() - t_us / 1000` exceeds about 3 s (10 Hz groups) or 5 s
   (group 2). Expect roughly 0.3-1.5 s of age in normal operation, plus the
   1 s window for group 2. During a backlog the age shows how far behind the
   board is.
4. **GPS.** Position is split into a reference and a delta, both f32:
   `lat = gps.lat_ref + gps.lat_d`, `lon = gps.lon_ref + gps.lon_d`. Add
   them as JS numbers.
5. **Session.** `session_id` is fixed per flash (`HTP_SESSION_ID`) and must be
   a row in `telemetry.sessions`. The dashboard needs it up front to pick the
   topic. See "Sessions" below for who creates it.
6. **Dropouts.** When the board has no value for an i16fp sample it sends
   raw `-32768`, which `wire-protocol.md` 3.3 says means null. The ingest
   function (as deployed, and as in the Helios dev kit, whose `frame_test.ts`
   asserts it) decodes it like any other raw value: `-32768 * scale + offset`.
   So a dropout shows up as that exact number per channel, e.g. -16384 for
   `engine.rpm`, -327.68 for `engine.water_temp`, -32.768 for `imu.lat_g`.
   Treat a value equal to `-32768 * scale + offset` (table below) as missing.
   Helios's newer Rust codec (`crates/helios-htp` on `feat/telemetry-live-path`)
   decodes the sentinel to null, so the deployed function is the outlier.
   The same value is what lands in staging. f32 channels have no sentinel.
   The synth never sends dropouts; real CAN data will once it is mapped.
   Values outside the i16fp range are clamped to `+-32767 * scale + offset`,
   e.g. `engine.rpm` tops out at 16383.5.

## Sessions

- A session is a row in `telemetry.sessions`. Postgres fills `id` with
  `gen_random_uuid()` when the row is inserted, so the UUID is generated
  server-side, but something has to insert the row: a service-role call.
- Today that is `npm run session` in the Helios telemetry dev kit (a
  placeholder). Its `CONTEXT.md` (2026-10-17) says the pit-side session tool
  and the Helios live UI do not exist yet, and how the car learns the id is
  undecided. Nothing in Helios creates sessions automatically yet.
- The ingest function never creates one. A frame for an id that is not a row
  fails with HTTP 500 (`staging_chunks_session_id_fkey`), checked against
  the team project 2026-10-29. The board keeps retrying it with backoff, so
  creating the row afterwards recovers the queued data (up to 32 s of it).
- For now the id is flashed into the board (`HTP_SESSION_ID` in `.env`), so
  one session spans every boot until it is changed and reflashed.
- Planned in Helios (spec `docs/superpowers/specs/2026-09-02-cellular-telemetry-fast-path.md`
  on `feat/telemetry-live-path`, section 3.4): a `telemetry-session` edge
  function, HMAC-signed like ingest, that the device calls at boot
  (`{"action":"open","device_id":...,"channel_set_id":1}` returns a new
  `session_id`; it first closes any session the same device left open).
  Not built yet: the branch stops at the protocol crate, and
  `/functions/v1/telemetry-session` returns 404 on the team project
  (2026-10-29). Once it exists the board should open a session per boot and
  can go back to seqs counting from 0.

## Channel set 1 (`SDM26-cell-v1`)

Units and names from `docs/channels.yaml`. Resolution is the i16fp scale
(smallest step the value can take).

| Group | Rate | Channel | Name | Units | Resolution |
|---|---|---|---|---|---|
| 0 | 10 Hz | `engine.rpm` | Engine RPM | rpm | 0.5 |
| 0 | 10 Hz | `engine.tps` | Throttle Position | % | 0.01 |
| 0 | 10 Hz | `engine.aps` | Accelerator Pedal | % | 0.01 |
| 0 | 10 Hz | `engine.map` | MAP | kPa | 0.01 |
| 0 | 10 Hz | `engine.lambda` | Lambda | lambda | 0.0001 |
| 0 | 10 Hz | `engine.gear` | Gear | | 1 |
| 0 | 10 Hz | `chassis.steering_angle` | Steering Angle | ° | 0.01 |
| 0 | 10 Hz | `imu.lat_g` | Lateral G | g | 0.001 |
| 0 | 10 Hz | `imu.long_g` | Longitudinal G | g | 0.001 |
| 0 | 10 Hz | `imu.vert_g` | Vertical G | g | 0.001 |
| 0 | 10 Hz | `imu.yaw_rate` | Yaw Rate | deg/s | 0.02 |
| 0 | 10 Hz | `brake.front_pressure` | Front Brake Pressure | kPa | 0.5 |
| 0 | 10 Hz | `brake.rear_pressure` | Rear Brake Pressure | kPa | 0.5 |
| 0 | 10 Hz | `suspension.travel_fl` | Suspension Travel FL | mm | 0.01 |
| 0 | 10 Hz | `suspension.travel_fr` | Suspension Travel FR | mm | 0.01 |
| 0 | 10 Hz | `suspension.travel_rl` | Suspension Travel RL | mm | 0.01 |
| 0 | 10 Hz | `suspension.travel_rr` | Suspension Travel RR | mm | 0.01 |
| 0 | 10 Hz | `drivetrain.wheel_speed_fl` | Wheel Speed FL | rpm | 0.125 |
| 0 | 10 Hz | `drivetrain.wheel_speed_fr` | Wheel Speed FR | rpm | 0.125 |
| 0 | 10 Hz | `drivetrain.wheel_speed_rl` | Wheel Speed RL | rpm | 0.125 |
| 0 | 10 Hz | `drivetrain.wheel_speed_rr` | Wheel Speed RR | rpm | 0.125 |
| 0 | 10 Hz | `drivetrain.vehicle_speed` | Vehicle Speed | km/h | 0.01 |
| 1 | 10 Hz | `gps.lat_ref` | Latitude reference | deg | f32 |
| 1 | 10 Hz | `gps.lat_d` | Latitude delta from ref | deg | f32 |
| 1 | 10 Hz | `gps.lon_ref` | Longitude reference | deg | f32 |
| 1 | 10 Hz | `gps.lon_d` | Longitude delta from ref | deg | f32 |
| 1 | 10 Hz | `gps.speed` | GPS Speed | m/s | f32 |
| 2 | 1 Hz | `engine.water_temp` | Water Temp | °C | 0.01 |
| 2 | 1 Hz | `engine.oil_temp` | Oil Temp | °C | 0.01 |
| 2 | 1 Hz | `engine.oil_pressure` | Oil Pressure | kPa | 0.05 |
| 2 | 1 Hz | `engine.fuel_pressure` | Fuel Pressure | kPa | 0.05 |
| 2 | 1 Hz | `engine.battery_voltage` | Battery Voltage | V | 0.001 |
| 2 | 1 Hz | `power.supply_current` | Supply Current | A | 0.01 |
| 2 | 1 Hz | `tire.surface_temp_fl` | Tire Surface Temp FL | °C | 0.01 |
| 2 | 1 Hz | `tire.surface_temp_fr` | Tire Surface Temp FR | °C | 0.01 |
| 2 | 1 Hz | `tire.surface_temp_rl` | Tire Surface Temp RL | °C | 0.01 |
| 2 | 1 Hz | `tire.surface_temp_rr` | Tire Surface Temp RR | °C | 0.01 |
| 2 | 1 Hz | `gps.fix_quality` | GPS Fix Quality | | 1 |

## Cadence and what limits it

The board posts each group separately: 3 POSTs per second in steady state,
one at a time, each waiting for its response. It keeps up while

    POSTs per second x average POST time < 1 s

The board prints this as `busy` in its `[htp] stats` line every 10 s, next to
the average and max request time, queue depth per group, and age of the oldest
queued window. When busy approaches 100%, the queues grow and each POST then
carries several windows (up to `HTP_POST_MAX_WINDOWS`, 8). That cuts the
request rate while it catches up, but the live feed still shows only the
newest sample of each POST.

Measured from a wired host (new TLS connection per request, 2026-10-29):
POSTs took 120-600 ms after the cold first one (about 1.4 s). Broadcast age
at arrival: about 250 ms (group 0), 450 ms (group 1), 1.5 s (group 2,
whose single sample is at the window start). The board over LTE is not
measured yet.

## If ~1 Hz is not enough

Helios already has an approved design for this (same spec, sections 2-3):
a separate `live_fast` stream. The device keeps one WebSocket to Realtime
and broadcasts every 100 ms on the same topic, event `live_fast`, payload
`{seq, t_us, t_send_ms, cs, v}` where `v` is base64 of one sample per
channel in channel-set order (86 bytes for set 1, i16fp sentinel = null).
Target is under 500 ms sensor to screen. It needs a device JWT and private
channel policies on `realtime.messages`, neither of which exists yet, so the
board cannot publish it today. Clients are meant to prefer `live_fast` and
fall back to `live`. Golden fixtures for the packing are in
`crates/helios-htp/fixtures/live/` on that branch.

Smaller steps that do not need Helios, in the order worth trying, after
measuring on the board:

1. Check `busy` and request time on LTE. If requests are slow, look at TLS
   reuse first (`reused x/y` in the stats line).
2. Put all three groups in one request. Needs a protocol and function change
   (HTP/1 frames are one group each).
3. Shorter live windows (100-200 ms) separate from the 1 s archival windows.
   Needs a format change, not just a timer.
4. Send the newest window ahead of the backlog so the live feed stays fresh
   during recovery. The ordering rule above already copes with that.
