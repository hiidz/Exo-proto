#pragma once

#include "Proc.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AdbDevice
{
	std::string serial;
	std::string state;   // "device", "offline", "unauthorized", ...

	bool Online() const { return state == "device"; }
};

struct AdbMdnsService
{
	std::string instance;   // adb-0123456789ABCDEF-Ab3xYz
	std::string service;    // _adb-tls-connect._tcp
	std::string endpoint;   // 192.168.1.50:37251

	// _adb-tls-pairing._tcp is the one-time pairing service and is not something
	// we can connect to; only the connect service is usable here.
	bool Connectable() const { return service.find("_adb-tls-connect") != std::string::npos; }
};

using LogFn = std::function<void(const std::string&)>;

struct AdbConfig
{
	std::wstring adbPath;

	// Pinned device serial. Blank means auto-detect, which picks
	// deterministically among whatever is reachable and never errors just
	// because several devices are present. See DeviceResolver.
	std::string serial;

	// Which transport the pin refers to. Meaningless when serial is blank. A
	// device reachable both ways has two candidates sharing a serial; this is
	// what tells them apart.
	bool wireless = true;

	uint16_t serverPort = 5037;
};

// Thin wrapper over the adb client binary. Knows nothing about scrcpy.
//
// One rule this class enforces by construction: we never issue `kill-server`.
// The server is shared infrastructure and killing it drops every live session
// on the machine, ours included.
class AdbClient
{
public:
	explicit AdbClient(AdbConfig config) : _config(std::move(config)) {}

	const AdbConfig& Config() const { return _config; }

	// `adb -P <port> -s <serial> <args>`
	proc::RunResult Run(const std::wstring& args, DWORD timeoutMs = 10000) const;

	// `adb -P <port> <args>`, for commands that predate device selection.
	proc::RunResult RunGlobal(const std::wstring& args, DWORD timeoutMs = 10000) const;

	// Full command line for a long-running child the caller supervises itself.
	// Exists so the port and serial flags cannot be forgotten on the one command
	// that does not go through Run().
	std::wstring BuildCommandLine(const std::wstring& args, bool withDevice = true) const;

	bool StartServer() const;
	bool IsDeviceOnline() const;
	std::string ClientVersion() const;

	// Every attached device and its state, as `adb devices` reports it. A wireless
	// device only appears here once something has run `adb connect` on it.
	std::vector<AdbDevice> ListDevices(DWORD timeoutMs = 10000) const;

	// Wireless devices advertising themselves on the local network. This is how a
	// phone is found before it has ever been connected, and the only way to learn
	// the port Android picked this time round.
	std::vector<AdbMdnsService> ListMdnsServices(DWORD timeoutMs = 10000) const;

	// `adb connect <endpoint>`. Harmless when already connected.
	bool Connect(const std::string& endpoint) const;

private:
	AdbConfig _config;
};

std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
std::string Trim(const std::string& s);