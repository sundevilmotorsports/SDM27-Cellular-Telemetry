#!/usr/bin/env python3
"""Decode stored raw CAN batches the way the car logger does and check the
bench ECU pattern came through with the right values.

Usage: check_bench_decode.py stored.jsonl logger_config.json

stored.jsonl is one batch body per line (written by test.cpp's fake server).
The decode follows sundevilmotorsports/logger src/can.rs, which the dashboard's
decodeCanFrame.ts also reproduces: device by (can_id, extended); Fixed or
Muxed signals; bytes start..start+len; integer width len if 1/2/4 else 8
(right-padded); big or little endian; optional sign; scale * (raw - offset)
in f32; no scale = raw bytes. Expected values mirror benchEcuValues() in
include/bench_ecu_pattern.h.
"""

import json
import struct
import sys

ENGINE, IMU, FD = 0x3E8, 0x361, 0x5F1


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def raw_int(b, sig):
    width = sig["len"] if sig["len"] in (1, 2, 4) else 8
    buf = bytes(b[:width]).ljust(width, b"\0")
    v = int.from_bytes(buf, "big" if sig.get("big_endian") else "little")
    if sig.get("signed") or width == 8:
        if v >= 1 << (8 * width - 1):
            v -= 1 << (8 * width)
    return v


def decode(frame, devices):
    """{signal name: value} for a frame, or None if no device matches."""
    dev = devices.get((frame["can_id"], frame["extd"]))
    if dev is None:
        return None
    data = bytes.fromhex(frame["data"])
    sigs = dev["signals"]
    if "Fixed" in sigs:
        signals = sigs["Fixed"]
    else:
        mux = sigs["Muxed"]
        if mux["byte"] >= len(data):
            return {}
        group = next((g for g in mux["groups"] if g["type_val"] == data[mux["byte"]]), None)
        signals = group["signals"] if group else []
    out = {}
    for s in signals:
        b = data[s["start"]:s["start"] + s["len"]]
        if len(b) < s["len"]:
            continue
        if s.get("scale") is None:
            out[s["name"]] = int.from_bytes(b, "little")  # dashboard's numeric view of raw bytes
        else:
            out[s["name"]] = f32(f32(s["scale"]) * f32(f32(raw_int(b, s)) - f32(s.get("offset", 0.0))))
    return out


def expected(i):
    mux = (0, 1, 4)[i % 3]
    lat = ((i % 400) - 200) * 10
    return {
        "mux": mux,
        0: {"r_engine": 1000 + (i * 37) % 11000, "t_eng_coolant": 60 + (i // 10) % 40,
            "t_oil": 70 + (i // 10) % 40, "p_oil": 200 + i % 300},
        1: {"Lamb_1": f32(f32(0.01) * f32(85 + i % 30)), "%_TPS": i % 101,
            "n-Gear": 1 + (i // 50) % 6, "v_trans_out": (i * 7) % 3000},
        4: {"in_gear": 1, "%_fuel_left": 100 - (i // 100) % 101},
        "imu": {"a_Lat": lat, "a_Long": -lat, "a_Vert": 1000},
    }


def main():
    stored, config = sys.argv[1], sys.argv[2]
    devices = {(d["id"], d["extended"]): d for d in json.load(open(config))["can_devices"]}
    frames = [f for line in open(stored) if line.strip() for f in json.loads(line)["frames"]]
    bench = [f for f in frames if f["can_id"] in (ENGINE, IMU, FD)]
    if not bench:
        sys.exit("no bench frames stored")
    t0 = min(f["ts_ms"] for f in bench)
    fails, checked = 0, {ENGINE: 0, IMU: 0, FD: 0}
    for f in bench:
        i, rem = divmod(f["ts_ms"] - t0, 100)
        if rem:
            print(f"FAIL: ts_ms {f['ts_ms']} is not on a 100 ms step")
            fails += 1
            continue
        exp = expected(i)
        got = decode(f, devices)
        if f["can_id"] == FD:
            ok = got is None and f.get("fd") is True and f.get("brs") is True and f["dlc"] == 16
            want = "unknown id, FD+BRS, 16 bytes"
        else:
            want = exp["imu"] if f["can_id"] == IMU else exp[exp["mux"]]
            ok = got is not None and all(got.get(k) == v for k, v in want.items())
        checked[f["can_id"]] += 1
        if not ok:
            fails += 1
            if fails <= 5:
                print(f"FAIL: step {i} id 0x{f['can_id']:X} data {f['data']}: got {got}, want {want}")
    print(f"decode: {checked[ENGINE]} engine, {checked[IMU]} imu, {checked[FD]} fd frames checked "
          f"against the logger config, {fails} wrong")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
