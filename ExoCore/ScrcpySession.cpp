#include "ScrcpySession.h"

#include <random>
#include <sstream>

namespace
{
	uint32_t MakeScid()
	{
		std::random_device rd;
		// scrcpy requires the id to fit in 31 bits.
		return (uint32_t)(rd() & 0x7FFFFFFFu);
	}

	std::wstring Hex8(uint32_t v)
	{
		wchar_t buf[16];
		swprintf_s(buf, L"%08x", v);
		return buf;
	}
}

ScrcpySession::ScrcpySession(const AdbClient& adb, ScrcpyOptions options, LogFn log)
	: _adb(adb), _options(std::move(options)), _log(std::move(log))
{
}

ScrcpySession::~ScrcpySession()
{
	Close();
}

void ScrcpySession::Log(const std::string& line) const
{
	if (_log)
		_log(line);
}

std::wstring ScrcpySession::SocketName() const
{
	return L"scrcpy_" + Hex8(_scid);
}

bool ScrcpySession::Open()
{
	_lastError.clear();
	_scid = MakeScid();

	if (!_adb.StartServer())
	{
		_lastError = "could not start the adb server";
		return false;
	}

	// Connecting and choosing the device is ResolveDevice's job; by the time a
	// session is constructed the serial is known good. This check only catches
	// the device going away between resolution and here.
	if (!_adb.IsDeviceOnline())
	{
		_lastError = "device " + _adb.Config().serial + " is not online";
		return false;
	}

	// An orphan from a crashed session holds the camera and would make this one
	// fail with an error that looks like a hardware problem.
	KillOrphanServers();

	if (!PushServerJar() || !AddForward())
	{
		Close();
		return false;
	}

	if (!LaunchServer())
	{
		Close();
		return false;
	}

	Log("session open on port " + std::to_string(_options.localPort)
		+ " via " + Narrow(SocketName()));
	return true;
}

void ScrcpySession::Close()
{
	_server.Terminate();
	RemoveForward();
}

void ScrcpySession::KillOrphanServers()
{
	// Best effort. pkill is absent on some Android builds, and there is usually
	// nothing to kill, so failure here is not an error.
	_adb.Run(L"shell pkill -f com.genymobile.scrcpy.Server", 5000);
}

bool PushScrcpyServer(const AdbClient& adb, const ScrcpyOptions& options, std::string& error)
{
	std::wostringstream args;
	args << L"push \"" << options.serverJarPath << L"\" " << options.remoteJarPath;

	// 30s: a wireless push on a weak 2.4GHz link is meaningfully slower than USB2.
	const auto result = adb.Run(args.str(), 30000);
	if (!result.Ok())
	{
		error = "push failed: " + Trim(result.output);
		return false;
	}

	return true;
}

bool ScrcpySession::PushServerJar()
{
	return PushScrcpyServer(_adb, _options, _lastError);
}

bool ScrcpySession::AddForward()
{
	std::wostringstream args;
	args << L"forward tcp:" << _options.localPort << L" localabstract:" << SocketName();

	const auto result = _adb.Run(args.str(), 10000);
	if (!result.Ok())
	{
		_lastError = "forward failed: " + Trim(result.output);
		return false;
	}

	_forwardActive = true;
	return true;
}

void ScrcpySession::RemoveForward()
{
	if (!_forwardActive)
		return;

	// Leaked forwards accumulate in the server and the next session either binds
	// a stale mapping or fails outright, so this runs even on the failure paths.
	std::wostringstream args;
	args << L"forward --remove tcp:" << _options.localPort;
	_adb.Run(args.str(), 5000);

	_forwardActive = false;
}

bool ScrcpySession::LaunchServer()
{
	std::wostringstream args;
	args << L"shell CLASSPATH=" << _options.remoteJarPath
		<< L" /system/bin/app_process / com.genymobile.scrcpy.Server "
		<< _options.scrcpyVersion
		<< L" scid=" << Hex8(_scid)
		<< L" tunnel_forward=true"
		<< L" audio=false"
		<< L" control=false"
		<< L" cleanup=false"
		<< L" video_source=camera";

	// Both omitted rather than sent empty -- the server's own defaults are
	// "first camera" and "unrotated", which is exactly what unset should mean.
	if (!_options.cameraId.empty())
		args << L" camera_id=" << _options.cameraId;

	if (!_options.captureOrientation.empty())
		args << L" capture_orientation=" << _options.captureOrientation;

	args << L" camera_size=" << _options.width << L"x" << _options.height
		<< L" camera_fps=" << _options.fps
		<< L" video_bit_rate=" << _options.bitRateBps;

	const std::wstring cmdLine = _adb.BuildCommandLine(args.str(), true);

	const bool started = _server.Start(cmdLine, [this](const std::string& line)
		{
			// Everything the server says is worth keeping. The last line before
			// death is almost always the actual reason.
			_lastError = line;
			Log("server: " + line);
		});

	if (!started)
	{
		_lastError = "could not launch adb shell for the scrcpy server";
		return false;
	}

	// A server that is going to reject its arguments does so immediately. Catching
	// it here turns "the camera is blank" into a specific message at startup.
	if (_server.WaitForExit(1500))
	{
		if (_lastError.empty())
			_lastError = "scrcpy server exited immediately, code "
			+ std::to_string(_server.ExitCode());
		return false;
	}

	return true;
}