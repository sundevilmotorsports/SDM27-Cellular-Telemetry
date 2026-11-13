#!/bin/sh
# Host test of the raw CAN batch uplink in src/net_task.cpp: batching, the
# 409 no-session hold, retries, drops, the byte/frame caps, seq across a
# reboot. Compiles the "Raw CAN batch POST" section of net_task.cpp and the
# real frame serializer against stubs.h (fake clock, queue, server). The
# batches the fake server stored are then decoded with the logger's own
# config.json rules (check_bench_decode.py) and checked against the bench ECU
# pattern. Needs ArduinoJson from a prior `pio run`.
#
#   sh tools/raw_uplink_host/run.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
JSON=$(find "$ROOT/.pio/libdeps" -maxdepth 3 -type d -path "*ArduinoJson/src" | head -1)
[ -n "$JSON" ] || { echo "ArduinoJson not found; run pio run first"; exit 1; }
awk '/^\/\/ ---- Raw CAN batch POST/{on=1} /^\/\/ ---- Task ---/{on=0} on' \
    "$ROOT/src/net_task.cpp" > "$OUT/section.inc"
c++ -std=c++17 -O1 -Wall -Wno-unused-function \
    -I"$OUT" -I"$ROOT/tools/raw_uplink_host" -I"$ROOT/tools/raw_uplink_host/fake" -I"$ROOT/include" -I"$JSON" \
    "$ROOT/tools/raw_uplink_host/test.cpp" "$ROOT/src/telemetry_format.cpp" -o "$OUT/test"
"$OUT/test" "$OUT/stored.jsonl" > "$OUT/log" || { cat "$OUT/log"; exit 1; }
grep -v '^\[raw\] seq=' "$OUT/log" || true
python3 "$ROOT/tools/raw_uplink_host/check_bench_decode.py" "$OUT/stored.jsonl" "$ROOT/tools/logger_config/config.json"
