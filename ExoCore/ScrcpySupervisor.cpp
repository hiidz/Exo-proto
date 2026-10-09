#include "ScrcpySupervisor.h"
#include "DeviceResolver.h"

#include <algorithm>

const char* ScrcpySupervisor::StateName(State state)
{
	switch (state)
	{
	case State::Stopped:          return "stopped";
	case State::WaitingForDevice: return "waiting for device";
	case State::Starting:         return "starting";
	case State::Running:          return "running";
	case State::Failed:           return "failed";
	}
	return "unknown";
}

void ScrcpySupervisor::Start(AdbConfig adb, ScrcpyOptions options, LogFn log)
{
	if (_running.exchange(true))
		return;

	_log = std::move(log);
	_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetStatus(State::Starting, "starting");

	_thread = std::thread(&ScrcpySupervisor::ThreadProc, this, std::move(adb), std::move(options));
}

void ScrcpySupervisor::Stop()
{
	if (!_running.exchange(false))
		return;

	if (_stopEvent)
		SetEvent(_stopEvent);

	if (_thread.joinable())
		_thread.join();

	if (_stopEvent)
	{
		CloseHandle(_stopEvent);
		_stopEvent = nullptr;
	}

	SetStatus(State::Stopped, "stopped");
}

ScrcpySupervisor::Status ScrcpySupervisor::GetStatus() const
{
	std::lock_guard<std::mutex> lock(_statusLock);
	return _status;
}

void ScrcpySupervisor::SetStatus(State state, const std::string& detail)
{
	{
		std::lock_guard<std::mutex> lock(_statusLock);

		if (state == State::Running && _status.state != State::Running)
			_status.restarts++;

		_status.state = state;
		_status.detail = detail;

		if (state == State::Stopped)
		{
			_status.activeSerial.clear();
			_status.activeWireless = false;
			_status.hasActiveDevice = false;
		}
	}

	if (_log)
		_log(std::string(StateName(state)) + ": " + detail);
}

bool ScrcpySupervisor::Sleep(DWORD ms) const
{
	if (!_stopEvent)
		return false;

	return WaitForSingleObject(_stopEvent, ms) == WAIT_TIMEOUT;
}

void ScrcpySupervisor::SetActiveDevice(
	const std::string& serial, bool wireless)
{
	std::lock_guard<std::mutex> lock(_statusLock);

	_status.activeSerial = serial;
	_status.activeWireless = wireless;
	_status.hasActiveDevice = true;
}

void ScrcpySupervisor::ThreadProc(AdbConfig adb, ScrcpyOptions options)
{
	DWORD backoff = kMinBackoffMs;

	if (_log)
		_log("adb: " + AdbClient(adb).ClientVersion());

	while (_running.load(std::memory_order_relaxed))
	{
		// Resolved every attempt, never cached. Android repicks the wireless
		// debugging port on every toggle, so a resolution from launch time is
		// stale the moment the phone's toggle is touched. Re-resolving here is
		// what makes "turn wireless debugging back on and it just reconnects"
		// work without restarting the tray app.
		AdbConfig resolved;
		std::string resolveError;
		DeviceCandidate chosen;

		if (!ResolveDevice(adb, resolved, resolveError, _log, &chosen))
		{
			SetStatus(State::WaitingForDevice, resolveError);

			if (!Sleep(backoff))
				break;

			backoff = min(backoff * 2, kMaxBackoffMs);
			continue;
		}

		SetActiveDevice(chosen.serial, chosen.wireless);

		SetStatus(State::Starting, "opening session");

		const AdbClient client(resolved);
		ScrcpySession session(client, options, _log);

		if (!session.Open())
		{
			const std::string reason = session.LastError();
			const bool deviceMissing = reason.find("not online") != std::string::npos;
			SetStatus(deviceMissing ? State::WaitingForDevice : State::Failed, reason);

			if (!Sleep(backoff))
				break;

			backoff = min(backoff * 2, kMaxBackoffMs);
			continue;
		}

		SetStatus(State::Running, "streaming");
		const ULONGLONG startedAt = GetTickCount64();

		// Waiting on the process handle, not polling adb. In the steady state
		// this layer issues no adb commands at all.
		while (_running.load(std::memory_order_relaxed))
		{
			if (session.WaitForExit(kAlivePollMs))
				break;
		}

		if (!_running.load(std::memory_order_relaxed))
			break;

		const ULONGLONG ranFor = GetTickCount64() - startedAt;
		if (ranFor >= kHealthyRuntimeMs)
			backoff = kMinBackoffMs;

		std::string reason = session.LastError();
		if (reason.empty())
			reason = "server exited after " + std::to_string(ranFor / 1000) + "s";

		SetStatus(State::Starting, "restarting: " + reason);

		if (!Sleep(backoff))
			break;

		backoff = min(backoff * 2, kMaxBackoffMs);
	}

	SetStatus(State::Stopped, "stopped");
}