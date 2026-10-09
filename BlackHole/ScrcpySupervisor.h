#pragma once

#include "ScrcpySession.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

// Keeps one ScrcpySession alive. The only stateful policy in the whole layer:
// when to retry, how fast, and what to tell the user.
//
// Deliberately has no idea that SuperDisplay exists. If the adb server goes away
// underneath us the session dies, this notices, and it retries with backoff. That
// is the same handling a cable being knocked out gets, which is the point.
class ScrcpySupervisor
{
public:
	enum class State
	{
		Stopped,
		WaitingForDevice,
		Starting,
		Running,
		Failed,
	};

	struct Status
	{
		State state = State::Stopped;
		std::string detail;
		uint32_t restarts = 0;

		// Device actually selected by the current/most recent successful
		// resolution. This is runtime state, not the persisted ini preference.
		std::string activeSerial;
		bool activeWireless = false;
		bool hasActiveDevice = false;
	};

	ScrcpySupervisor() = default;
	~ScrcpySupervisor() { Stop(); }

	ScrcpySupervisor(const ScrcpySupervisor&) = delete;
	ScrcpySupervisor& operator=(const ScrcpySupervisor&) = delete;

	// Returns immediately. The worker retries until Stop().
	void Start(AdbConfig adb, ScrcpyOptions options, LogFn log = nullptr);
	void Stop();

	Status GetStatus() const;
	static const char* StateName(State state);

private:
	void ThreadProc(AdbConfig adb, ScrcpyOptions options);
	void SetStatus(State state, const std::string& detail);
	void SetActiveDevice(const std::string& serial, bool wireless);
	bool Sleep(DWORD ms) const;

	// Backoff bounds. Retrying faster than this achieves nothing: a device that
	// is not there will still not be there in 200ms, and each attempt costs
	// several adb invocations.
	static constexpr DWORD kMinBackoffMs = 500;
	static constexpr DWORD kMaxBackoffMs = 8000;

	// A session that lasted this long counts as healthy, so backoff resets.
	// Without this, one bad night ratchets the delay to the cap and stays there.
	static constexpr DWORD kHealthyRuntimeMs = 15000;

	// How often the worker wakes to check a live session. Purely a handle wait,
	// no adb calls, so this can be short without cost.
	static constexpr DWORD kAlivePollMs = 500;

	std::thread _thread;
	std::atomic<bool> _running{ false };
	HANDLE _stopEvent = nullptr;

	mutable std::mutex _statusLock;
	Status _status;
	LogFn _log;
};