// Reference HTP/1 encoder in JavaScript. Mirrors docs/wire-protocol.md
// section 3 byte for byte; firmware does the same thing with packed structs.
// scripts/simulate.py has the equivalent in Python (struct.pack).

export const HTP_MAGIC = 0x4854;
export const HTP_VERSION = 1;
export const HEADER_BYTES = 36;
export const I16_NULL = -32768; // sensor-dropout sentinel for i16fp

function uuidBytes(id) {
  const hex = id.replace(/-/g, "");
  if (hex.length !== 32) throw new Error(`bad uuid ${id}`);
  const out = new Uint8Array(16);
  for (let i = 0; i < 16; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

export function sampleBytes(ch) {
  return ch.enc === "f32" ? 4 : 2;
}

/** Exact body size the server will insist on. */
export function frameBytes(group, windowCount) {
  const rate = group.rate_hz;
  const perWindow = 8 + group.channels.reduce((n, ch) => n + rate * sampleBytes(ch), 0);
  return HEADER_BYTES + windowCount * perWindow;
}

/**
 * windows: [{ tStartUs: bigint, samples: { [channelId]: number[] } }]
 * Each samples[id] must hold exactly group.rate_hz values, channels in
 * registered order come from group.channels.
 */
export function encodeFrame({ sessionId, channelSetId, groupKey, firstSeq, sendTimestampMs, windows, group }) {
  if (windows.length < 1 || windows.length > 8) throw new Error("window_count must be 1..8");
  const rate = group.rate_hz;
  const buf = new ArrayBuffer(frameBytes(group, windows.length));
  const dv = new DataView(buf);
  const u8 = new Uint8Array(buf);
  dv.setUint16(0, HTP_MAGIC, true);
  dv.setUint8(2, HTP_VERSION);
  dv.setUint8(3, 0);
  u8.set(uuidBytes(sessionId), 4);
  dv.setUint16(20, channelSetId, true);
  dv.setUint8(22, groupKey);
  dv.setUint8(23, windows.length);
  dv.setUint32(24, firstSeq, true);
  dv.setBigUint64(28, BigInt(sendTimestampMs), true);
  let off = HEADER_BYTES;
  for (const w of windows) {
    dv.setBigUint64(off, BigInt(w.tStartUs), true);
    off += 8;
    for (const ch of group.channels) {
      const vals = w.samples[ch.id];
      if (!vals || vals.length !== rate) throw new Error(`channel ${ch.id}: need ${rate} samples, got ${vals?.length}`);
      for (const v of vals) {
        if (ch.enc === "f32") {
          dv.setFloat32(off, v, true);
          off += 4;
        } else {
          const scale = ch.scale ?? 1;
          const offset = ch.offset ?? 0;
          let raw = Number.isFinite(v) ? Math.round((v - offset) / scale) : I16_NULL;
          if (raw !== I16_NULL) raw = Math.max(-32767, Math.min(32767, raw));
          dv.setInt16(off, raw, true);
          off += 2;
        }
      }
    }
  }
  return new Uint8Array(buf);
}

/** Physically plausible fake signal for a channel id at time t (seconds). */
export function synth(id, t) {
  if (id.includes("rpm")) return 3000 + 9000 * ((t % 6) / 6);
  if (id === "gps.lat_ref") return 33.42;
  if (id === "gps.lon_ref") return -111.93;
  if (id === "gps.lat_d") return 0.002 * Math.cos((2 * Math.PI * (t % 90)) / 90);
  if (id === "gps.lon_d") return 0.002 * Math.sin((2 * Math.PI * (t % 90)) / 90);
  if (id.includes("fix_quality")) return 4;
  if (id.includes("speed")) return 20 + 15 * Math.sin((2 * Math.PI * t) / 30);
  if (id.includes("temp")) return 80 + 15 * (1 - Math.exp(-t / 120)) + 0.5 * Math.sin(t);
  if (id.includes("pressure")) return 300 + 150 * ((t % 6) / 6);
  if (id.includes("voltage")) return 13.8 + 0.1 * Math.sin(t / 5);
  if (id.includes("gear")) return 1 + Math.floor(t % 6);
  if (id.includes("lambda")) return 0.9 + 0.05 * Math.sin(t);
  if (id.includes("_g")) return 1.2 * Math.sin(t / 3);
  return 10 + Math.sin(t + id.length);
}

export function synthWindow(group, tStartUs, tRelS) {
  const rate = group.rate_hz;
  const samples = {};
  for (const ch of group.channels) {
    samples[ch.id] = Array.from({ length: rate }, (_, i) => synth(ch.id, tRelS + i / rate));
  }
  return { tStartUs, samples };
}
