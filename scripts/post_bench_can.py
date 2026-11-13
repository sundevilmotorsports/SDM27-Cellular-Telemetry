#!/usr/bin/env python3
"""Post raw CAN batches of the bench ECU pattern from this Mac, signed exactly
like the board (src/net_task.cpp): Content-Type application/x-sdm-can-batch+json,
HMAC-SHA256 of the raw body in x-htp-signature, device id in x-htp-device.

Use it to check the dashboard end to end without the board: press Start
session for the device on the dashboard, then run

  .venv/bin/python scripts/post_bench_can.py --device bench-mac --seconds 30

One batch per second, 10 pattern steps each (include/bench_ecu_pattern.h,
logger config ids). seq is epoch ms, like the board. A 409 means no session is
running for the device; the script waits and resends the same batch.

Reads .env (HTTPS_INGEST_URL, TELEMETRY_HMAC_KEY, HTP_DEVICE_ID). Does not
print the key. Writes real rows to the project; mind the data you leave there.
"""

import argparse
import hashlib
import hmac
import json
import subprocess
import time

from post_synth_htp import load_env, need, ROOT


def be16(x):
    x &= 0xFFFF
    return [x >> 8, x & 0xFF]


def le16(x):
    x &= 0xFFFF
    return [x & 0xFF, x >> 8]


def bench_frames(i):
    """Same frames as benchEcuFrames() in include/bench_ecu_pattern.h."""
    mux = (0, 1, 4)[i % 3]
    e = [0] * 8
    e[0] = mux
    if mux == 0:
        e[1:3] = be16(1000 + (i * 37) % 11000)
        e[3] = 60 + (i // 10) % 40 - 50
        e[4] = 70 + (i // 10) % 40 - 50
        e[5:7] = be16(200 + i % 300)
    elif mux == 1:
        e[1] = 85 + i % 30
        e[2] = i % 101
        e[3] = 1 + (i // 50) % 6
        e[4:6] = be16((i * 7) % 3000)
    else:
        e[1] = 1
        e[6] = 100 - (i // 100) % 101
    lat = ((i % 400) - 200) * 10
    imu = le16(lat) + le16(-lat) + le16(1000) + [0, 0]
    out = [(0x3E8, False, e), (0x361, False, imu)]
    if i % 10 == 0:
        out.append((0x5F1, True, [(i + k) & 0xFF for k in range(16)]))
    return out


def batch(device, seq, first_step, t0_ms):
    frames = []
    for i in range(first_step, first_step + 10):
        for can_id, fd, data in bench_frames(i):
            f = {"can_id": can_id, "extd": False, "dlc": len(data), "data": bytes(data).hex(),
                 "ts_ms": t0_ms + (i - first_step) * 100}
            if fd:
                f["fd"], f["brs"] = True, True
            frames.append(f)
    return {"device_id": device, "seq": seq, "dropped_frames": 0, "frames": frames}


def post(url, device, key, body):
    sig = hmac.new(key, body, hashlib.sha256).hexdigest()
    r = subprocess.run(
        ["curl", "-sS", "-o", "-", "-w", "\n%{http_code} %{time_total}", "-X", "POST", url,
         "-H", "Content-Type: application/x-sdm-can-batch+json",
         "-H", f"x-htp-device: {device}", "-H", f"x-htp-signature: {sig}", "--data-binary", "@-"],
        input=body, capture_output=True, check=False)
    text, _, tail = r.stdout.decode().rpartition("\n")
    code, _, secs = tail.partition(" ")
    return int(code or 0), text, float(secs or 0)


def main():
    env = load_env(f"{ROOT}/.env")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default=env.get("HTP_DEVICE_ID") or "bench-mac")
    ap.add_argument("--seconds", type=int, default=10)
    args = ap.parse_args()
    url = need(env, "HTTPS_INGEST_URL")
    key = need(env, "TELEMETRY_HMAC_KEY").encode()

    step = 0
    for _ in range(args.seconds):
        start = time.time()
        now_ms = int(start * 1000)
        body = json.dumps(batch(args.device, now_ms, step, now_ms - 1000), separators=(",", ":")).encode()
        while True:
            code, text, secs = post(url, args.device, key, body)
            print(f"seq={now_ms} {len(body)} B -> {code} in {secs * 1000:.0f} ms {text[:120]}")
            if code != 409:
                break
            print("  no running session: press Start session for", args.device, "on the dashboard")
            time.sleep(2)
        step += 10
        time.sleep(max(0.0, 1.0 - (time.time() - start)))


if __name__ == "__main__":
    main()
