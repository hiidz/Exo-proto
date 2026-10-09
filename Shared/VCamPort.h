#pragma once

#include <cstdint>

// Both halves of the pipeline must agree on this number: ScrcpySession passes
// it to `adb forward tcp:<port>`, StreamClient connects to it on 127.0.0.1. A
// mismatch has no symptom beyond the DLL never connecting.
//
// Compile-time rather than an ini key: it is a loopback port between two halves
// of one product, and one device means one stream, so there is nothing to vary.
// Making it configurable on one side only would break the pairing silently.
inline constexpr uint16_t kVCamStreamPort = 1234;