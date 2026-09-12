// SMS (text message) configuration. Kept separate from telemetry_config.h
// since it's a standalone diagnostic feature, not part of the telemetry
// uplink -- see net_task.cpp's runSmsTest().
//
// Cellular only. SMS goes out over the SIM7670G's own network registration
// (AT+CMGS), no APN/PDP context needed -- but there's no modem at all in
// TELEMETRY_USE_WIFI=1 mode, so SMS_TEST_ENABLED=1 there is a build error
// (see the check in net_task.cpp).
//
// SMS_TEST_NUMBER is wrapped in an #ifndef guard, same convention as
// telemetry_config.h: copy .env.example to .env and set the real number
// there instead of editing this file.

#pragma once

// 1 = send one test SMS after the modem registers on the cellular network,
//     to confirm the SIM/plan can actually send texts. Runs once per boot.
// 0 = off (default) -- no SMS is ever sent.
#ifndef SMS_TEST_ENABLED
#define SMS_TEST_ENABLED 0
#endif

// Destination number in E.164 format (e.g. "+15551234567"). Left empty on
// purpose so a test send can't fire to nobody by accident -- runSmsTest()
// skips with a log line if this is still blank when SMS_TEST_ENABLED=1.
#ifndef SMS_TEST_NUMBER
#define SMS_TEST_NUMBER ""
#endif

#ifndef SMS_TEST_MESSAGE
#define SMS_TEST_MESSAGE "Whats up Evan. This is from SDM27 Telemetry"
#endif
