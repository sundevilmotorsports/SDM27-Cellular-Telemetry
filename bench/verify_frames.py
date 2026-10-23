#!/usr/bin/env python3
"""
End-to-end checker for the bench CAN FD generator (bench/can_tx_c5).

Subscribes to the telemetry topic and verifies every frame the S3 published
against the self-checking format the C5 transmits:

  data[0]        rolling 8-bit sequence counter
  data[dlc-1]    CRC-8 (poly 0x07, init 0) over can_id (4 bytes LE),
                 extd, fd, brs, dlc (1 byte each), data[0..dlc-2]

(`dlc` in the JSON is the payload length in bytes, up to 64 for CAN FD.)
A CRC pass proves can_id, extd, the FD/BRS flags, the length and every data
byte survived CAN bus -> MCP2518FD -> queue -> JSON -> MQTT unchanged. Sequence gaps count
frames that never arrived; compare them against the batches' own
dropped_frames to tell S3 queue overflow apart from loss on the bus.

  python3 bench/verify_frames.py                 # broker from .env, like listener.py
  python3 bench/verify_frames.py --ap            # SoftAP bench broker (192.168.4.2)
  python3 bench/verify_frames.py --self-test     # no broker: checks this script's own logic
"""

import argparse
import json
import os
import random
import signal
import statistics
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from listener import load_env_file  # noqa: E402

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERROR: 'paho-mqtt' is not installed. Run: pip3 install -r requirements.txt", file=sys.stderr)
    sys.exit(1)


def crc8(data: bytes, crc: int = 0) -> int:
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


# Payload lengths a CAN FD frame can carry (DLC codes 0-15).
FD_LENGTHS = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64}


def frame_crc(can_id: int, extd: bool, fd: bool, brs: bool, dlc: int, data: bytes) -> int:
    hdr = can_id.to_bytes(4, "little") + bytes([int(extd), int(fd), int(brs), dlc])
    return crc8(data[: dlc - 1], crc8(hdr))


class Verifier:
    def __init__(self, show_failures: int = 10):
        self.show_failures = show_failures
        self.batches = 0
        self.frames = 0
        self.crc_ok = 0
        self.bad = 0
        self.lost = 0
        self.device_dropped = 0
        self.out_of_order_batches = 0
        self.restarts = 0
        self.std = 0
        self.ext = 0
        self.classic = 0
        self.fd = 0
        self.fd_brs = 0
        self.lengths = {}
        self.latencies_ms = []
        self.mqtt_bytes = 0      # JSON payload bytes received
        self.can_bytes = 0       # CAN data bytes in CRC-verified frames
        self.first_recv_ms = None
        self.last_recv_ms = None
        self.last_batch_seq = None
        self.last_frame_seq = None

    def _fail(self, why: str, f: dict):
        self.bad += 1
        if self.bad <= self.show_failures:
            print(f"  [FAIL] {why}: {json.dumps(f)}")
            if self.bad == self.show_failures:
                print("  [FAIL] (further failures counted but not printed)")

    def batch(self, msg: dict, recv_ms: int, nbytes: int = 0):
        frames = msg.get("frames")
        if not isinstance(frames, list):
            return  # stress-test or other non-batch message
        self.batches += 1
        self.mqtt_bytes += nbytes
        if self.first_recv_ms is None:
            self.first_recv_ms = recv_ms
        self.last_recv_ms = recv_ms
        self.device_dropped += int(msg.get("dropped_frames", 0))

        # Live batches arrive with increasing seq. A lower one is either a
        # spool replay (published late after an outage) or the S3 rebooting
        # (s_batchSeq restarts at 0). Replays still get CRC-checked, but are
        # kept out of gap accounting, which only makes sense in send order.
        bseq = msg.get("seq")
        in_order = True
        if self.last_batch_seq is not None and bseq is not None and bseq <= self.last_batch_seq:
            if bseq == 0:
                self.restarts += 1
                print(f"  [info] batch seq reset to 0 -- S3 restarted")
                self.last_frame_seq = None
            else:
                self.out_of_order_batches += 1
                in_order = False
        if in_order:
            self.last_batch_seq = bseq

        for f in frames:
            self.frame(f, recv_ms, in_order)

    def frame(self, f: dict, recv_ms: int, in_order: bool):
        self.frames += 1
        try:
            can_id, extd, dlc = int(f["can_id"]), bool(f["extd"]), int(f["dlc"])
            fd, brs = bool(f.get("fd", False)), bool(f.get("brs", False))
            data = bytes.fromhex(f["data"])
        except (KeyError, ValueError, TypeError):
            self._fail("malformed frame", f)
            return
        if len(data) != dlc:
            self._fail(f"dlc={dlc} but {len(data)} data bytes", f)
            return
        if dlc < 2:
            self._fail("dlc < 2 (not a bench frame -- other traffic on the bus?)", f)
            return
        if can_id > (0x1FFFFFFF if extd else 0x7FF):
            self._fail("can_id out of range for extd flag", f)
            return
        if (dlc not in FD_LENGTHS) if fd else (dlc > 8):
            self._fail(f"dlc={dlc} impossible for a {'CAN FD' if fd else 'classic'} frame", f)
            return
        if brs and not fd:
            self._fail("brs set on a classic frame", f)
            return
        if frame_crc(can_id, extd, fd, brs, dlc, data) != data[dlc - 1]:
            self._fail("CRC mismatch (corrupted, or not a bench frame)", f)
            return

        self.crc_ok += 1
        self.ext += extd
        self.std += not extd
        self.fd += fd
        self.fd_brs += brs
        self.classic += not fd
        self.lengths[dlc] = self.lengths.get(dlc, 0) + 1
        self.can_bytes += dlc
        if "ts_ms" in f:
            self.latencies_ms.append(recv_ms - int(f["ts_ms"]))

        if in_order:
            seq = data[0]
            if self.last_frame_seq is not None:
                # Mod-256: a gap of 256+ frames is undercounted, so keep the
                # batch's dropped_frames in view when losses are large.
                self.lost += (seq - self.last_frame_seq - 1) & 0xFF
            self.last_frame_seq = seq

    def summary(self) -> str:
        lines = [
            f"batches={self.batches} frames={self.frames} crc_ok={self.crc_ok} bad={self.bad}",
            f"std={self.std} ext={self.ext} classic={self.classic} fd={self.fd} (brs={self.fd_brs})",
            "lengths " + " ".join(f"{n}:{c}" for n, c in sorted(self.lengths.items())),
            f"lost(seq gaps)={self.lost} device_dropped_frames={self.device_dropped} "
            f"out_of_order_batches={self.out_of_order_batches} s3_restarts={self.restarts}",
        ]
        # Delivered throughput between the first and last batch to arrive.
        # Batches land in bursts once per window, so this needs a run several
        # windows long to mean much.
        if self.first_recv_ms is not None and self.last_recv_ms > self.first_recv_ms:
            span = (self.last_recv_ms - self.first_recv_ms) / 1000
            lines.append(
                f"throughput over {span:.0f}s: {self.crc_ok / span:.1f} frames/s, "
                f"CAN payload {self.can_bytes * 8 / span / 1000:.1f} kbit/s, "
                f"MQTT JSON {self.mqtt_bytes * 8 / span / 1000:.1f} kbit/s "
                f"({self.mqtt_bytes / 1e6:.2f} MB total)")
        lat = self.latencies_ms[-1000:]
        if lat:
            med = statistics.median(lat)
            if abs(med) > 86_400_000:
                lines.append("latency: n/a (S3 clock not synced -- normal in SoftAP mode)")
            else:
                lines.append(f"latency capture->broker: median={med:.0f}ms max={max(lat)}ms (last {len(lat)})")
        verdict = "PASS" if self.crc_ok and not self.bad else ("FAIL" if self.bad else "NO DATA")
        lines.append(f"verdict: {verdict}" + (" (with frame loss)" if self.lost and verdict == "PASS" else ""))
        return "\n  ".join(lines)


def self_test() -> int:
    """Feeds synthetic batches (what the S3 would publish) through Verifier."""
    rng = random.Random(1)
    seq = 0

    def make_frame():
        # Mirrors bench/can_tx_c5 generation and the S3's JSON (fd/brs keys
        # only on FD frames).
        nonlocal seq
        extd = rng.random() < 0.25
        can_id = rng.getrandbits(29) if extd else rng.getrandbits(11)
        fd = rng.random() < 0.7
        brs = fd and rng.random() < 0.8
        dlc = rng.choice([2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]) if fd else rng.randint(2, 8)
        data = bytearray([seq & 0xFF] + [rng.getrandbits(8) for _ in range(dlc - 2)])
        data.append(frame_crc(can_id, extd, fd, brs, dlc, bytes(data)))
        seq += 1
        f = {"can_id": can_id, "extd": extd, "dlc": dlc, "data": data.hex(), "ts_ms": 1000}
        if fd:
            f.update(fd=True, brs=brs)
        return f

    v = Verifier(show_failures=0)
    frames = [make_frame() for _ in range(600)]
    del frames[100:103]                                               # 3 frames lost on the way
    corrupted = bytearray.fromhex(frames[200]["data"])
    corrupted[1 if len(corrupted) > 2 else 0] ^= 0x01
    frames[200] = dict(frames[200], data=corrupted.hex())             # 1 flipped bit
    frames[300] = dict(frames[300], extd=not frames[300]["extd"])     # 1 wrong extd flag
    i = next(i for i in range(400, 600) if frames[i].get("brs"))
    frames[i] = dict(frames[i], brs=False)                            # 1 lost bit rate switch flag
    batches = [frames[i:i + 50] for i in range(0, len(frames), 50)]
    for n, b in enumerate(batches):
        v.batch({"seq": n, "dropped_frames": 0, "frames": b}, 1000)
    v.batch({"seq": 3, "dropped_frames": 0, "frames": batches[3]}, 1000)  # spool replay

    checks = [
        ("frames", v.frames, 597 + 50),
        ("bad", v.bad, 3),
        ("lost", v.lost, 3 + 3),  # 3 deleted + 3 bad frames skipped by seq tracking
        ("out_of_order_batches", v.out_of_order_batches, 1),
        ("crc_ok", v.crc_ok, 594 + 50),
        ("fd seen", v.fd > 0 and v.classic > 0 and v.fd_brs > 0, True),
    ]
    ok = True
    for name, got, want in checks:
        status = "ok" if got == want else "MISMATCH"
        ok &= got == want
        print(f"  {name:22s} got={got!s:>5} want={want!s:>5} {status}")
    print("self-test", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


def main():
    load_env_file()
    default_host = os.getenv("MQTT_HOST") or os.getenv("MQTT_BROKER_HOST", "localhost")
    default_port = int(os.getenv("MQTT_PORT") or os.getenv("MQTT_BROKER_PORT", "1883"))
    default_topic = os.getenv("MQTT_TOPIC") or os.getenv("MQTT_TOPIC_TELEMETRY", "esp32_cellular_telemetry/batch")

    p = argparse.ArgumentParser(description="Verify bench CAN frames end-to-end over MQTT.")
    p.add_argument("-H", "--host", default=default_host)
    p.add_argument("-p", "--port", type=int, default=default_port)
    p.add_argument("-t", "--topic", default=default_topic)
    p.add_argument("-u", "--username", default=os.getenv("MQTT_USER") or os.getenv("MQTT_USERNAME"))
    p.add_argument("-P", "--password", default=os.getenv("MQTT_PASSWORD"))
    p.add_argument("--tls", action="store_true", help="Connect over TLS (implied by port 8883)")
    p.add_argument("--insecure", action="store_true", help="Skip TLS certificate verification")
    p.add_argument("--ap", action="store_true", help="Use the SoftAP bench broker at 192.168.4.2")
    p.add_argument("--every", type=float, default=10.0, help="Seconds between summaries (default 10)")
    p.add_argument("--self-test", action="store_true", help="Check this script's logic offline and exit")
    args = p.parse_args()

    if args.self_test:
        sys.exit(self_test())
    if args.ap:
        args.host = "192.168.4.2"

    v = Verifier()
    last_summary = time.monotonic()

    def on_connect(client, userdata, flags, rc, properties=None):
        failed = rc.is_failure if hasattr(rc, "is_failure") else rc != 0
        if failed:
            print(f"[-] connect failed: {rc}")
            return
        client.subscribe(args.topic)
        print(f"[+] connected to {args.host}:{args.port}, verifying frames on '{args.topic}'")

    def on_message(client, userdata, msg):
        nonlocal last_summary
        try:
            parsed = json.loads(msg.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        v.batch(parsed, int(time.time() * 1000), len(msg.payload))
        if time.monotonic() - last_summary >= args.every:
            last_summary = time.monotonic()
            print(f"[summary] {v.summary()}")

    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"bench_verify_{os.getpid()}")
    except AttributeError:
        client = mqtt.Client(client_id=f"bench_verify_{os.getpid()}")
    if args.username:
        client.username_pw_set(args.username, args.password)
    if args.tls or args.port == 8883:
        try:
            import certifi
            client.tls_set(ca_certs=certifi.where())
        except ImportError:
            client.tls_set()
        if args.insecure:
            client.tls_insecure_set(True)
    client.on_connect = on_connect
    client.on_message = on_message

    def stop(*_):
        client.disconnect()
        print(f"\n[final] {v.summary()}")
        sys.exit(1 if v.bad else 0)

    signal.signal(signal.SIGINT, stop)
    client.connect(args.host, args.port, keepalive=60)
    client.loop_forever()


if __name__ == "__main__":
    main()
