#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Process helpers. Deliberately knows nothing about adb or scrcpy.
namespace proc
{
	struct RunResult
	{
		bool launched = false;          // did the executable itself start
		DWORD exitCode = MAXDWORD;
		std::string output;             // stdout and stderr merged; adb emits ASCII

		bool Ok() const { return launched && exitCode == 0; }
	};

	// Runs a command to completion with output captured. On timeout the child is
	// killed and the result reports launched == true with exitCode == WAIT_TIMEOUT.
	RunResult RunAndCapture(const std::wstring& commandLine, DWORD timeoutMs);

	// A long-running child whose output matters. scrcpy-server reports every
	// startup failure (version mismatch, missing jar, camera in use, bad
	// camera_size) on stderr and then exits. Without pumping that, every failure
	// mode is indistinguishable from the outside.
	//
	// The child runs inside a job object with KILL_ON_JOB_CLOSE, so killing it
	// takes the whole tree rather than orphaning adb.exe's children.
	class ChildProcess
	{
	public:
		using LineHandler = std::function<void(const std::string&)>;

		ChildProcess() = default;
		~ChildProcess() { Terminate(); }

		ChildProcess(const ChildProcess&) = delete;
		ChildProcess& operator=(const ChildProcess&) = delete;

		bool Start(const std::wstring& commandLine, LineHandler onLine);

		bool IsAlive() const;

		// True if the process exited within the timeout. This is how the
		// supervisor detects death: waiting on a handle, not polling adb.
		bool WaitForExit(DWORD timeoutMs) const;

		DWORD ExitCode() const;
		void Terminate();

	private:
		void PumpOutput();

		HANDLE _job = nullptr;
		HANDLE _process = nullptr;
		HANDLE _outRead = nullptr;
		std::thread _pump;
		LineHandler _onLine;
	};
}