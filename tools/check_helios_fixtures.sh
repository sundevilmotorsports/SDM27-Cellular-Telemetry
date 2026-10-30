#!/bin/sh
# Byte-compare src/htp_frame.cpp against the Helios golden HTP/1 fixtures
# (tools/helios_fixtures, from crates/helios-htp). Covers multi-window frames
# and null samples (i16fp sentinel, f32 NaN).
#
#   sh tools/check_helios_fixtures.sh
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
c++ -std=c++17 -O2 -I"$ROOT/include" "$ROOT/tools/htp_fixture_encode.cpp" "$ROOT/src/htp_frame.cpp" -o "$OUT/enc"
python3 - "$ROOT/tools/helios_fixtures" "$OUT/enc" <<'PY'
import glob, json, os, subprocess, sys
fx, enc = sys.argv[1], sys.argv[2]
cs = json.load(open(f"{fx}/channel_set_1.json"))
bad = 0
for path in sorted(glob.glob(f"{fx}/htp1/*.json")):
    j = json.load(open(path))
    chans = [c["id"] for c in cs["groups"][str(j["group_key"])]["channels"]]
    toks = [j["session_id"], j["group_key"], len(j["windows"]), j["seq"], j["send_timestamp_ms"]]
    toks += [w["t_start_us"] for w in j["windows"]]
    for w in j["windows"]:
        for c in chans:
            toks += ["nan" if v is None else repr(v) for v in w["samples"][c]]
    r = subprocess.run([enc], input=" ".join(map(str, toks)).encode(), capture_output=True)
    want = open(path[:-5] + ".htp", "rb").read()
    name = os.path.basename(path)[:-5]
    if r.returncode == 0 and r.stdout == want:
        print(f"ok {name} ({len(want)} bytes)")
    else:
        bad += 1
        at = next((i for i, (a, b) in enumerate(zip(r.stdout, want)) if a != b), None)
        print(f"MISMATCH {name}: rc={r.returncode}, {len(r.stdout)} vs {len(want)} bytes, first diff at {at}")
print("firmware encoder matches the Helios fixtures" if not bad else f"{bad} fixture(s) differ")
sys.exit(1 if bad else 0)
PY
