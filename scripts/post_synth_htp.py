"""Post firmware-synth HTP/1, the same bytes the board would send.

Uses tools/htp_emit_sample (src/htp_frame.cpp) for the body, then signs it the
same way src/net_task.cpp does: HMAC-SHA256 of the raw body, x-htp-device,
no apikey and no bearer token.

Reads .env. Does not print TELEMETRY_HMAC_KEY. Does not use the service role.
"""

import argparse
import hashlib
import hmac
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_env(path):
    values = {}
    if not os.path.isfile(path):
        sys.exit(f"no {path}")
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            value = value.strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
                value = value[1:-1]
            values[key.strip()] = value
    return values


def need(env, key):
    value = env.get(key, "")
    if not value:
        sys.exit(f"{key} is empty in .env")
    return value


def post(url, device, key, path):
    """POST one frame. Returns (http status, ack body, seconds)."""
    with open(path, "rb") as f:
        body = f.read()
    sig = hmac.new(key, body, hashlib.sha256).hexdigest()
    # curl uses the macOS trust store. python.org's ssl bundle does not.
    res = subprocess.run(
        [
            "curl", "-sS", "-m", "20",
            "-o", "-",
            "-w", "\n%{http_code} %{time_total}",
            "-X", "POST",
            url,
            "-H", "Content-Type: application/x-htp",
            "-H", f"x-htp-device: {device}",
            "-H", f"x-htp-signature: {sig}",
            "--data-binary", f"@{path}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if res.returncode != 0:
        return 0, res.stderr.strip(), 0.0
    ack, _, tail = res.stdout.rpartition("\n")
    code, _, secs = tail.partition(" ")
    return int(code), ack.strip(), float(secs)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--windows", type=int, default=1,
                    help="windows per frame, 1..8 (default 1)")
    ap.add_argument("--seconds", type=int, default=1,
                    help="repeat once per second this many times, like the board (default 1)")
    args = ap.parse_args()
    if not 1 <= args.windows <= 8:
        sys.exit("--windows must be 1..8")

    env = load_env(os.path.join(ROOT, ".env"))
    url = need(env, "HTTPS_INGEST_URL")
    device = need(env, "HTP_DEVICE_ID")
    session = need(env, "HTP_SESSION_ID")
    channel_set = env.get("HTP_CHANNEL_SET_ID", "1")
    key = need(env, "TELEMETRY_HMAC_KEY").encode()
    if "/rest/" in url:
        sys.exit("HTTPS_INGEST_URL still looks like PostgREST, not telemetry-ingest")
    if not url.endswith("/functions/v1/telemetry-ingest"):
        sys.exit("HTTPS_INGEST_URL must end with /functions/v1/telemetry-ingest")

    out_dir = os.path.join(ROOT, "tools", ".htp_sample")
    os.makedirs(out_dir, exist_ok=True)
    binary = os.path.join(ROOT, "tools", "htp_emit_sample")
    if not os.path.isfile(binary):
        sys.exit("missing tools/htp_emit_sample; compile it first")

    print(f"POST {url}")
    print(f"device={device} session={session} set={channel_set}")
    failed = 0
    for i in range(args.seconds):
        if i:
            time.sleep(1.0 - (time.time() % 1.0))
        now_ms = str(int(time.time() * 1000))
        emit = subprocess.run(
            [binary, session, channel_set, now_ms, out_dir, str(args.windows)],
            check=False,
            capture_output=True,
            text=True,
        )
        if emit.returncode != 0:
            sys.stderr.write(emit.stderr)
            sys.exit(emit.returncode)
        for line, name in zip(emit.stdout.splitlines(),
                              ("group-0.bin", "group-1.bin", "group-2.bin")):
            code, ack, secs = post(url, device, key, os.path.join(out_dir, name))
            print(f"{line}: HTTP {code} in {secs * 1000:.0f} ms {ack}")
            if code != 200:
                failed += 1
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
