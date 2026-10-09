#include "Proc.h"

#include <vector>

namespace
{
	// CreateProcessW may modify its command line buffer, so it can never be a
	// string literal or a c_str().
	std::vector<wchar_t> MutableCommandLine(const std::wstring& s)
	{
		std::vector<wchar_t> buf(s.begin(), s.end());
		buf.push_back(L'\0');
		return buf;
	}

	struct Pipe
	{
		HANDLE read = nullptr;
		HANDLE write = nullptr;

		~Pipe()
		{
			if (read) CloseHandle(read);
			if (write) CloseHandle(write);
		}

		bool Create()
		{
			SECURITY_ATTRIBUTES sa{};
			sa.nLength = sizeof(sa);
			sa.bInheritHandle = TRUE;

			if (!CreatePipe(&read, &write, &sa, 0))
				return false;

			// The read end must NOT be inherited, or the child holds a copy of it
			// and we never see EOF when the child exits.
			return SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0) != 0;
		}

		HANDLE ReleaseRead()
		{
			HANDLE h = read;
			read = nullptr;
			return h;
		}

		void CloseWrite()
		{
			if (write)
			{
				CloseHandle(write);
				write = nullptr;
			}
		}
	};

	void ReadToEof(HANDLE h, std::string& out)
	{
		char buffer[4096];
		DWORD read = 0;
		while (ReadFile(h, buffer, sizeof(buffer), &read, nullptr) && read > 0)
			out.append(buffer, read);
	}
}

namespace proc
{
	RunResult RunAndCapture(const std::wstring& commandLine, DWORD timeoutMs)
	{
		RunResult result;

		Pipe pipe;
		if (!pipe.Create())
			return result;

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = pipe.write;
		si.hStdError = pipe.write;
		si.hStdInput = nullptr;

		PROCESS_INFORMATION pi{};
		auto buf = MutableCommandLine(commandLine);

		if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
		{
			return result;
		}

		result.launched = true;

		// Our copy of the write end must go, or EOF never arrives.
		pipe.CloseWrite();

		HANDLE readEnd = pipe.ReleaseRead();
		std::string captured;
		std::thread reader([readEnd, &captured] { ReadToEof(readEnd, captured); });

		if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_TIMEOUT)
		{
			// Killing the child closes its handles, which unblocks the reader.
			TerminateProcess(pi.hProcess, WAIT_TIMEOUT);
			result.exitCode = WAIT_TIMEOUT;
			WaitForSingleObject(pi.hProcess, 1000);
		}
		else
		{
			GetExitCodeProcess(pi.hProcess, &result.exitCode);
		}

		reader.join();
		CloseHandle(readEnd);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);

		result.output = std::move(captured);
		return result;
	}

	bool ChildProcess::Start(const std::wstring& commandLine, LineHandler onLine)
	{
		Terminate();
		_onLine = std::move(onLine);

		_job = CreateJobObjectW(nullptr, nullptr);
		if (!_job)
			return false;

		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

		Pipe pipe;
		if (!pipe.Create())
		{
			Terminate();
			return false;
		}

		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		si.wShowWindow = SW_HIDE;
		si.hStdOutput = pipe.write;
		si.hStdError = pipe.write;
		si.hStdInput = nullptr;

		PROCESS_INFORMATION pi{};
		auto buf = MutableCommandLine(commandLine);

		// Suspended so the process is inside the job before it can spawn anything.
		if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
		{
			Terminate();
			return false;
		}

		AssignProcessToJobObject(_job, pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);

		_process = pi.hProcess;
		pipe.CloseWrite();
		_outRead = pipe.ReleaseRead();

		_pump = std::thread(&ChildProcess::PumpOutput, this);
		return true;
	}

	void ChildProcess::PumpOutput()
	{
		std::string pending;
		char buffer[4096];
		DWORD read = 0;

		while (ReadFile(_outRead, buffer, sizeof(buffer), &read, nullptr) && read > 0)
		{
			pending.append(buffer, read);

			size_t start = 0;
			for (size_t i = 0; i < pending.size(); i++)
			{
				if (pending[i] != '\n')
					continue;

				size_t end = i;
				if (end > start && pending[end - 1] == '\r')
					end--;

				if (end > start && _onLine)
					_onLine(pending.substr(start, end - start));

				start = i + 1;
			}

			pending.erase(0, start);
		}

		if (!pending.empty() && _onLine)
			_onLine(pending);
	}

	bool ChildProcess::IsAlive() const
	{
		return _process && WaitForSingleObject(_process, 0) == WAIT_TIMEOUT;
	}

	bool ChildProcess::WaitForExit(DWORD timeoutMs) const
	{
		if (!_process)
			return true;

		return WaitForSingleObject(_process, timeoutMs) != WAIT_TIMEOUT;
	}

	DWORD ChildProcess::ExitCode() const
	{
		DWORD code = MAXDWORD;
		if (_process)
			GetExitCodeProcess(_process, &code);
		return code;
	}

	void ChildProcess::Terminate()
	{
		// Closing the job kills the whole tree, which closes the child's pipe
		// handles, which ends the pump thread's blocking read.
		if (_job)
		{
			CloseHandle(_job);
			_job = nullptr;
		}

		if (_pump.joinable())
			_pump.join();

		if (_outRead)
		{
			CloseHandle(_outRead);
			_outRead = nullptr;
		}

		if (_process)
		{
			CloseHandle(_process);
			_process = nullptr;
		}

		_onLine = nullptr;
	}
}