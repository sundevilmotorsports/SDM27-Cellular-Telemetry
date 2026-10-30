"""Subscribe to the live broadcast and measure what a dashboard would see.

Joins Realtime Broadcast topic telemetry:live:{HTP_SESSION_ID} with the
publishable key (the same access a browser has) and prints one line per
message: group, seq, arrival interval for that group, and age = arrival time
minus the sample's t_us. Out-of-order messages (t_us not newer than the last
one for that group, e.g. backlog replay after an outage) are flagged. A
summary per group prints on exit.

Reads HTTPS_INGEST_URL, HTP_SESSION_ID and HTTPS_API_KEY (publishable key)
from .env. Needs `websockets` (requirements.txt).

    .venv/bin/python scripts/watch_live.py [--seconds N]

Age mixes the board's clock with this machine's; both are NTP-synced, so it is
good to tens of ms. Synth windows are stamped with the second they describe,
and the broadcast carries the last sample of a window (t_start + 0.9 s at
10 Hz), so a fresh window arrives with age >= its upload time.
"""

import argparse
import asyncio
import json
import os
import ssl
import statistics
import sys
import time
from urllib.parse import urlparse

import certifi
import websockets

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from post_synth_htp import load_env, need  # noqa: E402


def channel_groups():
    with open(os.path.join(ROOT, "docs", "channel-set-1.json")) as f:
        groups = json.load(f)["definition"]["groups"]
    return {ch["id"]: int(key) for key, g in groups.items() for ch in g["channels"]}


class Group:
    def __init__(self):
        self.count = 0
        self.last_arrival = None
        self.last_t_us = None
        self.intervals = []
        self.ages = []
        self.stale = 0


async def watch(ws_url, topic, key, seconds, group_of):
    groups = {}
    ctx = ssl.create_default_context(cafile=certifi.where())
    deadline = time.time() + seconds if seconds else None
    async with websockets.connect(ws_url, ssl=ctx) as ws:
        await ws.send(json.dumps({
            "topic": f"realtime:{topic}",
            "event": "phx_join",
            "payload": {"config": {"broadcast": {"self": False, "ack": False},
                                   "presence": {"key": ""},
                                   "postgres_changes": [],
                                   "private": False}},
            "ref": "1",
            "join_ref": "1",
        }))
        ref = 1
        next_heartbeat = time.time() + 25
        while deadline is None or time.time() < deadline:
            timeout = min(next_heartbeat, deadline or next_heartbeat) - time.time()
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=max(timeout, 0.01))
            except asyncio.TimeoutError:
                if time.time() >= next_heartbeat:
                    ref += 1
                    await ws.send(json.dumps({"topic": "phoenix", "event": "heartbeat",
                                              "payload": {}, "ref": str(ref)}))
                    next_heartbeat = time.time() + 25
                continue
            arrival = time.time()
            msg = json.loads(raw)
            event = msg.get("event")
            if event == "phx_reply" and msg.get("ref") == "1":
                status = msg["payload"].get("status")
                print(f"joined {topic}: {status}")
                if status != "ok":
                    print(json.dumps(msg["payload"]))
                    return groups
                continue
            if event in ("phx_error", "phx_close", "system"):
                print(f"{event}: {json.dumps(msg.get('payload'))}")
                continue
            if event != "broadcast":
                continue
            live = msg["payload"].get("payload", {})
            values = live.get("values", {})
            gkeys = {group_of.get(ch, -1) for ch in values}
            gkey = gkeys.pop() if len(gkeys) == 1 else -1
            g = groups.setdefault(gkey, Group())
            t_us = live.get("t_us", 0)
            age_ms = arrival * 1000 - t_us / 1000
            interval = "" if g.last_arrival is None else f"{(arrival - g.last_arrival) * 1000:6.0f}"
            note = ""
            if g.last_t_us is not None and t_us <= g.last_t_us:
                g.stale += 1
                note = "  OUT OF ORDER (older than last shown)"
            else:
                g.last_t_us = t_us
                g.ages.append(age_ms)
            if g.last_arrival is not None:
                g.intervals.append(arrival - g.last_arrival)
            g.last_arrival = arrival
            g.count += 1
            sample = next(iter(values.items()), ("", ""))
            print(f"g{gkey} seq={live.get('seq')} every={interval:>6} ms age={age_ms:7.0f} ms "
                  f"{len(values)} ch ({sample[0]}={sample[1]}){note}")
    return groups


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (default: until Ctrl-C)")
    args = ap.parse_args()

    env = load_env(os.path.join(ROOT, ".env"))
    host = urlparse(need(env, "HTTPS_INGEST_URL")).netloc
    session = need(env, "HTP_SESSION_ID")
    key = env.get("SUPABASE_PUBLISHABLE_KEY") or need(env, "HTTPS_API_KEY")
    ws_url = f"wss://{host}/realtime/v1/websocket?apikey={key}&vsn=1.0.0"
    topic = f"telemetry:live:{session}"

    groups = {}
    try:
        groups = asyncio.run(watch(ws_url, topic, key, args.seconds, channel_groups()))
    except KeyboardInterrupt:
        pass
    print("\nsummary")
    if not groups:
        print("  no broadcasts received")
    for gkey, g in sorted(groups.items()):
        rate = 1 / statistics.mean(g.intervals) if g.intervals else 0
        ages = sorted(g.ages)
        p50 = ages[len(ages) // 2] if ages else 0
        print(f"  g{gkey}: {g.count} msgs, {rate:.2f}/s, age p50 {p50:.0f} ms "
              f"max {max(ages, default=0):.0f} ms, out of order {g.stale}")


if __name__ == "__main__":
    main()
