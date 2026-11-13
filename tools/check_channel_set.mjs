// Host-side check that docs/channel-set-1.json matches the frozen header
// sizes and that tools/htp.mjs can encode one known window of each group.
//
//   node tools/check_channel_set.mjs

import { readFileSync } from "node:fs";
import { encodeFrame, frameBytes, synthWindow } from "./htp.mjs";

const doc = JSON.parse(readFileSync(new URL("../docs/channel-set-1.json", import.meta.url), "utf8"));
const groups = doc.definition.groups;

const expected = {
  "0": { rate_hz: 10, channels: 22, windowBytes: 448 },
  "1": { rate_hz: 10, channels: 5, windowBytes: 208 },
  "2": { rate_hz: 1, channels: 11, windowBytes: 30 },
};

const sessionId = "0f8fad5b-d9cb-469f-a165-70867728950e";
let failed = 0;

for (const [key, exp] of Object.entries(expected)) {
  const group = groups[key];
  if (!group) {
    console.error(`missing group ${key}`);
    failed++;
    continue;
  }
  if (group.rate_hz !== exp.rate_hz || group.channels.length !== exp.channels) {
    console.error(`group ${key} layout mismatch`, group.rate_hz, group.channels.length);
    failed++;
    continue;
  }
  const bytes = frameBytes(group, 1);
  if (bytes !== 36 + exp.windowBytes) {
    console.error(`group ${key} frame size ${bytes}, expected ${36 + exp.windowBytes}`);
    failed++;
    continue;
  }
  const window = synthWindow(group, 1_700_000_000_000_000n, 1.0);
  const frame = encodeFrame({
    sessionId,
    channelSetId: doc.id,
    groupKey: Number(key),
    firstSeq: 0,
    sendTimestampMs: 1_700_000_000_000,
    windows: [window],
    group,
  });
  if (frame.length !== bytes || frame[0] !== 0x54 || frame[1] !== 0x48 || frame[2] !== 1) {
    console.error(`group ${key} encoded header mismatch, len=${frame.length}`);
    failed++;
    continue;
  }
  console.log(`group ${key}: ${group.channels.length} ch @ ${group.rate_hz} Hz, frame=${frame.length} B, magic ok`);
}

if (doc.id !== 1 || doc.name !== "SDM26-cell-v1") {
  console.error("set id/name mismatch");
  failed++;
}

if (failed) {
  console.error(`${failed} check(s) failed`);
  process.exit(1);
}
console.log("channel set 1 matches htp.mjs encodeFrame");
