#!/usr/bin/env python3
"""Convert the car logger's SD-card config.json into include/can_map_car.h.

The logger (sundevilmotorsports/logger, src/can.rs) describes every CAN device
it decodes: id, extended, fd, bus, and signals that are either Fixed or Muxed
on a selector byte. Each signal is {name, start, len, signed, big_endian,
scale, offset}, decoded as scale * (raw - offset). This script keeps the
signals whose name matches a channel set 1 channel and writes them as a
kCanSignals[] table, so the board decodes exactly what the logger records.

A signal name matches a channel when it equals the channel id or one of its
`aliases:` in docs/channels.yaml (case-insensitive). Use --map NAME=CHANNEL
for anything else. Signals without a scale are logged as raw bytes by the
logger and have no number to send, so they are skipped.

  python3 tools/logger_config_to_can_map.py config.json --bus Engine > include/can_map_car.h
  python3 tools/logger_config_to_can_map.py config.json --map "Oil P=engine.oil_pressure" ...

Unmatched signals are listed on stderr and in a comment in the output.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def channel_set_ids(path):
    doc = json.loads(Path(path).read_text())
    groups = doc["definition"]["groups"]
    return [c["id"] for g in groups.values() for c in g["channels"]]


def channel_aliases(path):
    """{lowercase alias: channel id} from channels.yaml (id + aliases only)."""
    out = {}
    current = None
    collecting = None
    for line in Path(path).read_text().splitlines():
        m = re.match(r"\s*- id:\s*(\S+)", line)
        if m:
            current = m.group(1).strip("\"'")
            out[current.lower()] = current
            continue
        if collecting is not None:
            collecting += " " + line
        else:
            m = re.match(r"\s*aliases:\s*(.*)", line)
            if not m or current is None:
                continue
            collecting = m.group(1)
        if collecting.count("[") and collecting.count("]") >= collecting.count("["):
            body = collecting[collecting.index("[") + 1 : collecting.rindex("]")]
            for item in re.findall(r'"((?:[^"\\]|\\.)*)"|\'([^\']*)\'|([^,\s][^,]*)', body):
                name = next(x for x in item if x).strip()
                if name and not name.startswith("#"):
                    out.setdefault(name.lower(), current)
            collecting = None
    return out


def c_float(x):
    s = repr(float(x))
    return (s if ("." in s or "e" in s) else s + ".0") + "f"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config", help="logger config.json")
    ap.add_argument("--bus", default="all", help="Module, Engine, or all (default)")
    ap.add_argument("--map", action="append", default=[], metavar="NAME=CHANNEL",
                    help="map a logger signal name to a channel id (repeatable)")
    ap.add_argument("--channels", default=ROOT / "docs/channels.yaml")
    ap.add_argument("--channel-set", default=ROOT / "docs/channel-set-1.json")
    args = ap.parse_args()

    ids = set(channel_set_ids(args.channel_set))
    names = {k: v for k, v in channel_aliases(args.channels).items() if v in ids}
    for m in args.map:
        name, _, ch = m.partition("=")
        if ch not in ids:
            sys.exit(f"--map {m}: {ch} is not a channel set 1 channel")
        names[name.strip().lower()] = ch

    cfg = json.loads(Path(args.config).read_text())
    rows, skipped, unmatched = [], [], []
    for dev in cfg.get("can_devices", []):
        bus = dev.get("bus", "Module")
        if args.bus != "all" and bus.lower() != args.bus.lower():
            continue
        sig = dev["signals"]
        if "Fixed" in sig:
            parts = [(-1, 0, s) for s in sig["Fixed"]]
        else:
            mux = sig["Muxed"]
            parts = [(mux["byte"], g["type_val"], s) for g in mux["groups"] for s in g["signals"]]
        for mux_byte, mux_val, s in parts:
            where = f"0x{dev['id']:X} {s['name']}"
            ch = names.get(s["name"].strip().lower())
            if ch is None:
                unmatched.append(where)
                continue
            if s.get("scale") is None:
                skipped.append(f"{where}: no scale (logged as raw bytes)")
                continue
            if not 1 <= s["len"] <= 8:
                skipped.append(f"{where}: len {s['len']} is over 8 bytes")
                continue
            if s.get("big_endian") and s["len"] not in (1, 2, 4, 8):
                print(f"warning: {where}: {s['len']}-byte big-endian; the logger pads it to 8 "
                      "bytes before decoding, so its values differ from the board's", file=sys.stderr)
            rows.append((dev, mux_byte, mux_val, s, ch, bus))

    out = [
        f"// CAN signals on the car -> channel set 1. Generated from {Path(args.config).name}",
        f"// by tools/logger_config_to_can_map.py (bus: {args.bus}). Do not edit by hand.",
        "// Columns: id, extended, muxByte (-1 = none), muxValue, start, len, signed,",
        "// bigEndian, scale, offset, channel. value = scale * (raw - offset).",
    ]
    if unmatched:
        out.append("//")
        out.append("// Logger signals with no channel (add --map NAME=CHANNEL to include one):")
        out += [f"//   {u}" for u in unmatched]
    out += ["", "#pragma once", "", "static const CanSignal kCanSignals[] = {"]
    for dev, mb, mv, s, ch, bus in rows:
        out.append(
            f"    {{0x{dev['id']:X}, {str(bool(dev['extended'])).lower()}, {mb}, {mv}, {s['start']}, "
            f"{s['len']}, {str(bool(s.get('signed', False))).lower()}, "
            f"{str(bool(s.get('big_endian', False))).lower()}, {c_float(s['scale'])}, "
            f"{c_float(s.get('offset', 0.0))}, \"{ch}\"}}, // {bus}: {s['name']}"
        )
    out += ["    {0, false, -1, 0, 0, 0, false, false, 0.0f, 0.0f, nullptr},", "};", ""]
    print("\n".join(out))

    covered = sorted({r[4] for r in rows})
    print(f"{len(rows)} signals -> {len(covered)} channels: {', '.join(covered) or 'none'}", file=sys.stderr)
    for s in skipped:
        print(f"skipped {s}", file=sys.stderr)
    if unmatched:
        print(f"{len(unmatched)} signals had no channel: {', '.join(unmatched)}", file=sys.stderr)


if __name__ == "__main__":
    main()
