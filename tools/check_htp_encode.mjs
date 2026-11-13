// Encode a known window with tools/htp.mjs and with the firmware encoder
// (tools/htp_encode_host.cpp). Exit non-zero if the bytes differ.
//
//   c++ -std=c++17 -I include -o /tmp/htp_encode_host tools/htp_encode_host.cpp src/htp_frame.cpp
//   node tools/check_htp_encode.mjs /tmp/htp_encode_host

import { spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { encodeFrame } from "./htp.mjs";

const hostBin = process.argv[2];
if (!hostBin) {
  console.error("usage: node tools/check_htp_encode.mjs <htp_encode_host>");
  process.exit(2);
}

const set = JSON.parse(readFileSync(new URL("../docs/channel-set-1.json", import.meta.url), "utf8"));
const sessionId = "0f8fad5b-d9cb-469f-a165-70867728950e";

function f32(v) {
  const b = Buffer.alloc(4);
  b.writeFloatLE(v);
  return b.readFloatLE(0);
}

function sampleFor(ch, i) {
  const scale = f32(ch.scale ?? 1);
  const offset = f32(ch.offset ?? 0);
  if (i === 3) return Number.NaN;
  if (i === 4) return 1e6;
  if (i === 5) return -1e6;
  if (ch.enc === "f32") {
    if (ch.id === "gps.lat_ref") return 33.42;
    if (ch.id === "gps.lon_ref") return -111.93;
    return (i - 2) * 0.00125;
  }
  // Includes a positive and a negative halfway case (Math.round, not lround).
  if (i === 1) return f32(offset + 1.5 * scale);
  if (i === 2) return f32(offset + -1.5 * scale);
  if (ch.id === "engine.lambda") return f32(0.85 + i * 0.01);
  return f32((i - 1) * scale * 10);
}

function putU16(buf, v) {
  buf.push(v & 0xff, (v >> 8) & 0xff);
}
function putU32(buf, v) {
  buf.push(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >> 24) & 0xff);
}
function putU64(buf, v) {
  let x = BigInt(v);
  for (let i = 0; i < 8; i++) {
    buf.push(Number(x & 0xffn));
    x >>= 8n;
  }
}
function putF32(buf, v) {
  const b = Buffer.alloc(4);
  b.writeFloatLE(v);
  for (const byte of b) buf.push(byte);
}

let failed = 0;
const dir = mkdtempSync(join(tmpdir(), "htp-"));

for (const key of ["0", "1", "2"]) {
  const group = {
    rate_hz: set.definition.groups[key].rate_hz,
    channels: set.definition.groups[key].channels.map((ch) => ({
      ...ch,
      scale: ch.scale == null ? undefined : f32(ch.scale),
      offset: ch.offset == null ? undefined : f32(ch.offset),
    })),
  };
  const windowCount = 1;
  const seq = 7;
  const sendMs = 1700000000123n;
  const tStartUs = 1700000000000000n;
  const samples = {};
  const flat = [];
  for (const ch of group.channels) {
    const vals = [];
    for (let i = 0; i < group.rate_hz; i++) {
      const v = sampleFor(ch, i);
      vals.push(v);
      flat.push(v);
    }
    samples[ch.id] = vals;
  }

  const expected = encodeFrame({
    sessionId,
    channelSetId: set.id,
    groupKey: Number(key),
    firstSeq: seq,
    sendTimestampMs: sendMs,
    windows: [{ tStartUs, samples }],
    group,
  });

  const req = [];
  req.push(Number(key), windowCount);
  putU16(req, set.id);
  putU32(req, seq);
  putU64(req, sendMs);
  const hex = sessionId.replace(/-/g, "");
  for (let i = 0; i < 16; i++) req.push(parseInt(hex.slice(i * 2, i * 2 + 2), 16));
  putU64(req, tStartUs);
  for (const v of flat) putF32(req, v);

  const reqPath = join(dir, `req-${key}.bin`);
  const outPath = join(dir, `frame-${key}.bin`);
  writeFileSync(reqPath, Buffer.from(req));
  const run = spawnSync(hostBin, [reqPath, outPath], { encoding: "utf8" });
  if (run.status !== 0) {
    console.error(`group ${key}: host encoder failed`, run.stderr);
    failed++;
    continue;
  }
  const actual = readFileSync(outPath);
  if (Buffer.compare(actual, Buffer.from(expected)) !== 0) {
    console.error(`group ${key}: byte mismatch host=${actual.length} js=${expected.length}`);
    const n = Math.max(actual.length, expected.length);
    for (let i = 0; i < n; i++) {
      const a = i < actual.length ? actual[i] : -1;
      const e = i < expected.length ? expected[i] : -1;
      if (a !== e) {
        console.error(`  offset ${i}: host=0x${a.toString(16)} js=0x${e.toString(16)}`);
        if (i > 48) break;
      }
    }
    failed++;
    continue;
  }
  console.log(`group ${key}: ${actual.length} bytes match htp.mjs`);
}

rmSync(dir, { recursive: true, force: true });
if (failed) {
  console.error(`${failed} group(s) failed`);
  process.exit(1);
}
console.log("firmware encoder matches htp.mjs encodeFrame");
