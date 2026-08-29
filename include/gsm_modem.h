// TinyGSM requires TINY_GSM_MODEM_XXXX to be #defined before the FIRST
// #include of <TinyGsmClient.h> in each translation unit (it's a
// single-header-with-config-macro library, not a normal class hierarchy you
// select at link time). Centralizing that here means every .cpp that needs
// TinyGsm just includes this header instead of TinyGsmClient.h directly, so
// the modem type can never accidentally go undefined in one TU.
//
// TINY_GSM_MODEM_SIM7670G maps to TinyGsmClientSIM7672.h (SIM7670G is
// SIM7672-compatible; see lewisxhe/TinyGSM-fork's TinyGsmClient.h dispatch).

#pragma once

#define TINY_GSM_MODEM_SIM7670G
#include <TinyGsmClient.h>
