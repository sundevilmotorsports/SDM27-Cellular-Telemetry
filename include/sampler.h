// 10 Hz sampler: builds each 1-second HTP window of channel set 1 from the
// latest channel values and hands it to the uplink (netTaskPushWindow).
//
// Ticks on wall-clock 100 ms boundaries once SNTP has synced, so a sample's
// timestamp is when it was taken. Each tick takes one snapshot of every
// channel; a group at rate_hz uses the ticks that fall on its sample times
// (decimation, not averaging). Values come from htpSynth() when
// HTP_SYNTH_MODE=1, or from the CAN map (can_map.h) otherwise, where a
// channel with no fresh CAN value is NaN (sent as a dropout).
//
// Runs in its own task so a POST blocking the net task for seconds does not
// stall sampling; the uplink queue absorbs the backlog.

#pragma once

void samplerStart();
