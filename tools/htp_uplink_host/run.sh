#!/bin/sh
# Host test of the HTP uplink queue in src/net_task.cpp: batching, retries,
# drops, stale-socket retry, ack parsing. Compiles the "HTP/1 POST" section of
# net_task.cpp against stubs.h (fake clock, HTTP server, socket) and the real
# encoder. Needs ArduinoJson from a prior `pio run`.
#
#   sh tools/htp_uplink_host/run.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
JSON=$(find "$ROOT/.pio/libdeps" -maxdepth 3 -type d -path "*ArduinoJson/src" | head -1)
[ -n "$JSON" ] || { echo "ArduinoJson not found; run pio run first"; exit 1; }
awk '/^\/\/ ---- HTP\/1 POST/{on=1} /^\/\/ ---- Task ---/{on=0} on' \
    "$ROOT/src/net_task.cpp" > "$OUT/section.inc"
c++ -std=c++17 -O1 -Wall -Wno-unused-function \
    -I"$OUT" -I"$ROOT/tools/htp_uplink_host" -I"$ROOT/include" -I"$JSON" \
    "$ROOT/tools/htp_uplink_host/test.cpp" "$ROOT/src/htp_frame.cpp" -o "$OUT/test"
"$OUT/test" | grep -v '^\[htp\] g'
