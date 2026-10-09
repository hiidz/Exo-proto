#pragma once

#include "AdbClient.h"
#include "VCamVideoSettings.h"

#include <cstdint>
#include <string>

struct ScrcpyOptions
{
	std::wstring serverJarPath;                                          // local scrcpy-server.jar
	std::wstring remoteJarPath = L"/data/local/tmp/vcam-scrcpy-server.jar";

	// Must match the jar exactly. The server refuses to start otherwise, and the
	// error only appears on stderr, which is why the session pumps it.
	std::wstring scrcpyVersion;

	uint16_t localPort = 0;                                              // kVCamStreamPort
	uint32_t width = VCamVideoSettings{}.width;
	uint32_t height = VCamVideoSettings{}.height;
	uint32_t fps = VCamVideoSettings{}.fps;
	uint32_t bitRateBps = 8000000;

	// Empty means "let the server pick", which selects the first camera. A set
	// value is the user's explicit preference and is remembered across launches
	// and device switches -- an id that does not exist on the current device
	// makes the server refuse to start, and that message is what surfaces.
	std::wstring cameraId;

	// 0, 90, 180, 270, or flip0/flip90/flip180/flip270. Empty means unrotated.
	// A property of how the phone is physically mounted rather than of the phone
	// itself, so it is universal: it survives launches, device switches and
	// camera switches alike.
	std::wstring captureOrientation;
};

// Pushes the server jar to the device. Shared with camera enumeration, which
// needs the same jar in place before it can run the server's list-only mode.
bool PushScrcpyServer(const AdbClient& adb, const ScrcpyOptions& options, std::string& error);

// One bring-up and tear-down of the phone-side video server, as an RAII unit.
// Owns exactly two pieces of external state, and releases both in the
// destructor: the adb port forward, and the server process.
//
// Not thread safe. ScrcpySupervisor owns it from a single thread.
//
// Holds the AdbClient by reference and does not extend its lifetime: the client
// must outlive the session. ScrcpySupervisor::ThreadProc satisfies this by
// declaring the client first in the same scope.
class ScrcpySession
{
public:
	ScrcpySession(const AdbClient& adb, ScrcpyOptions options, LogFn log);
	~ScrcpySession();

	ScrcpySession(const ScrcpySession&) = delete;
	ScrcpySession& operator=(const ScrcpySession&) = delete;

	// Full bring-up. False leaves nothing behind: partial state is unwound.
	bool Open();
	void Close();

	bool IsAlive() const { return _server.IsAlive(); }
	bool WaitForExit(DWORD timeoutMs) const { return _server.WaitForExit(timeoutMs); }

	// Last line the server printed before dying, or our own failure reason.
	// This is what the tray tooltip should show.
	const std::string& LastError() const { return _lastError; }

private:
	bool PushServerJar();
	bool AddForward();
	void RemoveForward();
	bool LaunchServer();
	void KillOrphanServers();
	std::wstring SocketName() const;
	void Log(const std::string& line) const;

	const AdbClient& _adb;
	ScrcpyOptions _options;
	LogFn _log;

	// scrcpy names its abstract socket scrcpy_<scid>. A fixed name means a
	// server orphaned by an earlier crash is still listening, and we connect to
	// it and get a stream that never produces frames. A fresh id per session
	// makes that impossible.
	uint32_t _scid = 0;

	bool _forwardActive = false;
	proc::ChildProcess _server;
	std::string _lastError;
};