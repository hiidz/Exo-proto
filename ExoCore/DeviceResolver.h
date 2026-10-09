#pragma once

#include "AdbClient.h"

#include <string>
#include <vector>

struct DeviceCandidate
{
	// What `adb -s` addresses: ip:port for wireless, the serial for USB. Valid
	// for this discovery pass only -- Android repicks the wireless port on every
	// toggle, so this must never be persisted.
	std::string target;

	// The stable serial. For USB it is the label; for wireless it is extracted
	// from the mDNS instance name. This is the only part safe to write to ini.
	std::string serial;

	// What a person would recognise: the raw instance name or the USB serial.
	std::string label;

	bool wireless = false;
};

// Everything reachable: advertised wireless endpoints and online USB devices,
// wireless first so the tray's Device menu leads with them. Does not filter by
// config.serial and connects to nothing -- this is for showing a list.
//
// A device reachable both ways appears twice, once per transport. That is the
// point: serial alone does not identify a connection.
std::vector<DeviceCandidate> ListDeviceCandidates(const AdbConfig& base, DWORD timeoutMs = 10000);

// Picks one candidate, connects to it if wireless, and confirms it is
// authorised. Two cases: a pinned serial must match on serial AND transport or
// this fails; an empty serial auto-selects (USB first, then lowest serial) for
// this attempt only. Nothing chosen here is ever written back to the ini.
//
// Called once per session attempt and never cached: Android repicks the
// wireless debugging port on every toggle, so a stale resolution is worse than
// none. `resolved.serial` is the -s target for this pass, not a stable serial.
bool ResolveDevice(const AdbConfig& base, AdbConfig& resolved, std::string& error, const LogFn& log, DeviceCandidate* chosenOut = nullptr);