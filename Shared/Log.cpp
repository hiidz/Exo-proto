#include "Log.h"

#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "advapi32.lib")

namespace
{
	constexpr wchar_t kDir[] = L"C:\\Windows\\Temp\\Exo\\logs";
	constexpr ULONGLONG kMaxBytes = 4ull * 1024 * 1024;

	// Users get Modify (0x1301BF), not Full: the folder lives in a directory any
	// user can already write to, so this is not a new capability, but Full would
	// also hand over WRITE_DAC. Protected (D:P) so nothing is inherited from
	// Windows\Temp. The grant is the point -- the subfolder is owned by whichever
	// process creates it first, and if that is the Frame Server running as
	// LOCAL SERVICE, the interactive user cannot otherwise read the DLL's log.
	constexpr wchar_t kSddl[] = L"D:P(A;OICI;0x1301BF;;;BU)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";

	enum : int { kDetailUnknown = 0, kDetailOff = 1, kDetailOn = 2 };

	SRWLOCK g_lock = SRWLOCK_INIT;
	HANDLE g_file = INVALID_HANDLE_VALUE;
	ULONGLONG g_written = 0;
	bool g_openTried = false;
	bool g_pathsResolved = false;
	wchar_t g_path[MAX_PATH] = {};
	wchar_t g_iniPath[MAX_PATH] = {};

	// Relaxed throughout: a torn read is impossible for an int, and the worst
	// case for a race is two threads both resolving the same value from the
	// same file. Ordering buys nothing here.
	std::atomic<int> g_detail{ kDetailUnknown };

	void CreateDirs()
	{
		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
		const bool haveSd = ConvertStringSecurityDescriptorToSecurityDescriptorW(
			kSddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr) != FALSE;

		CreateDirectoryW(L"C:\\Windows\\Temp\\Exo", haveSd ? &sa : nullptr);
		CreateDirectoryW(kDir, haveSd ? &sa : nullptr);

		if (haveSd)
			LocalFree(sa.lpSecurityDescriptor);
	}

	// Derives both paths from the module containing this code: the log name, so
	// the DLL and the tray land in separate files with no configuration and no
	// Init() call anyone can forget, and the ini, which sits beside the binary.
	//
	// Duplicates VCamIniPathForThisModule() from VCamVideoSettings.h on purpose.
	// A dependency from the logger to the video settings header is the wrong
	// direction, and this is six lines.
	//
	// Caller holds the lock.
	void ResolvePaths()
	{
		if (g_pathsResolved)
			return;
		g_pathsResolved = true;

		wchar_t module[MAX_PATH] = L"unknown";
		HMODULE self = nullptr;
		if (GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&ResolvePaths), &self))
		{
			GetModuleFileNameW(self, module, MAX_PATH);
		}

		wchar_t* name = wcsrchr(module, L'\\');
		if (name)
		{
			// Split in place: `module` becomes the directory, `name` the leaf.
			*name = 0;
			name++;
			swprintf_s(g_iniPath, L"%s\\vcam.ini", module);
		}
		else
		{
			name = module;
			wcscpy_s(g_iniPath, L"vcam.ini");
		}

		if (wchar_t* dot = wcsrchr(name, L'.'))
			*dot = 0;

		swprintf_s(g_path, L"%s\\%s.log", kDir, name);
	}

	// Caller holds the lock, and must have called ResolvePaths first.
	void ResolveDetail()
	{
		if (g_detail.load(std::memory_order_relaxed) != kDetailUnknown)
			return;

		const bool on = GetPrivateProfileIntW(L"log", L"detail", 0, g_iniPath) != 0;
		g_detail.store(on ? kDetailOn : kDetailOff, std::memory_order_relaxed);
	}

	HANDLE OpenFile(const wchar_t* path)
	{
		// FILE_SHARE_READ so the file can be tailed while we hold it open;
		// FILE_SHARE_DELETE so rollover can rename it out from under a reader.
		return CreateFileW(path, FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
			OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	void WriteHeader()
	{
		SYSTEMTIME st{};
		GetLocalTime(&st);

		// The detail state goes in the header so a sparse log is self-explaining:
		// "detail=off" is the difference between nothing happening and nothing
		// being recorded, and that question otherwise costs a round trip.
		char header[512];
		const int n = _snprintf_s(header, _TRUNCATE,
			"\r\n---- %04u-%02u-%02u %02u:%02u:%02u  pid %lu  %ls  detail=%s ----\r\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			GetCurrentProcessId(), g_path,
			g_detail.load(std::memory_order_relaxed) == kDetailOn ? "on" : "off");

		if (n > 0)
		{
			DWORD wrote = 0;
			WriteFile(g_file, header, static_cast<DWORD>(n), &wrote, nullptr);
			g_written += wrote;
		}
	}

	// Lazy, so there is nothing to call at startup. A DLL must not touch the file
	// system from DllMain under the loader lock, which rules out eager open here.
	//
	// Caller holds the lock.
	bool EnsureOpen()
	{
		if (g_file != INVALID_HANDLE_VALUE)
			return true;

		// One attempt per process. Without this a machine that cannot host the
		// folder at all retries CreateDirectory on every single log line.
		if (g_openTried)
			return false;
		g_openTried = true;

		ResolvePaths();
		ResolveDetail();
		CreateDirs();

		g_file = OpenFile(g_path);
		if (g_file == INVALID_HANDLE_VALUE)
		{
			// Almost always another instance of the same module already holding
			// it. Fall back to a pid-suffixed name rather than losing the log.
			wchar_t alt[MAX_PATH];
			swprintf_s(alt, L"%.*s-%lu.log",
				static_cast<int>(wcslen(g_path) - 4), g_path, GetCurrentProcessId());
			wcscpy_s(g_path, alt);
			g_file = OpenFile(g_path);
		}

		if (g_file == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER size{};
		GetFileSizeEx(g_file, &size);
		g_written = static_cast<ULONGLONG>(size.QuadPart);

		WriteHeader();
		return true;
	}

	// One generation of history. The tray runs for days; truncate-on-start would
	// throw away the session that is usually the interesting one.
	void RollIfNeeded()
	{
		if (g_written < kMaxBytes)
			return;

		wchar_t old[MAX_PATH];
		swprintf_s(old, L"%.*s.old.log",
			static_cast<int>(wcslen(g_path) - 4), g_path);

		CloseHandle(g_file);
		g_file = INVALID_HANDLE_VALUE;
		g_written = 0;

		MoveFileExW(g_path, old, MOVEFILE_REPLACE_EXISTING);

		g_file = OpenFile(g_path);
		if (g_file != INVALID_HANDLE_VALUE)
			WriteHeader();
	}

	void Emit(const char* utf8, int length)
	{
		AcquireSRWLockExclusive(&g_lock);

		if (EnsureOpen())
		{
			RollIfNeeded();

			if (g_file != INVALID_HANDLE_VALUE)
			{
				SYSTEMTIME st{};
				GetLocalTime(&st);

				char stamp[24];
				const int s = _snprintf_s(stamp, _TRUNCATE, "%02u:%02u:%02u.%03u  ",
					st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

				DWORD wrote = 0;
				if (s > 0 && WriteFile(g_file, stamp, static_cast<DWORD>(s), &wrote, nullptr))
					g_written += wrote;

				if (WriteFile(g_file, utf8, static_cast<DWORD>(length), &wrote, nullptr))
					g_written += wrote;

				if (WriteFile(g_file, "\r\n", 2, &wrote, nullptr))
					g_written += wrote;

				// Flushed by hand rather than buffered: the questions this log is
				// kept to answer are the ones asked after something died, and a
				// buffered tail is exactly the part that would be missing.
				FlushFileBuffers(g_file);
			}
		}

		ReleaseSRWLockExclusive(&g_lock);
	}
}

namespace Log
{
	// Resolves itself rather than depending on EnsureOpen having run. The first
	// call into this module is often a LOG_DETAIL from CBaseAttributes during
	// Activator::Initialize; if that read an unresolved flag it would skip, the
	// file would never open, and detail would stay off for the life of the
	// process -- losing exactly the early attribute negotiation it exists for.
	//
	// SRWLOCK is not recursive, and this releases before Line() acquires.
	bool IsDetail()
	{
		const int state = g_detail.load(std::memory_order_relaxed);
		if (state != kDetailUnknown)
			return state == kDetailOn;

		AcquireSRWLockExclusive(&g_lock);
		ResolvePaths();
		ResolveDetail();
		const bool on = g_detail.load(std::memory_order_relaxed) == kDetailOn;
		ReleaseSRWLockExclusive(&g_lock);

		return on;
	}

	void LineV(const char* fmt, va_list args)
	{
		char text[2048];
		const int n = _vsnprintf_s(text, _TRUNCATE, fmt, args);
		Emit(text, n < 0 ? static_cast<int>(strlen(text)) : n);
	}

	void LineV(const wchar_t* fmt, va_list args)
	{
		wchar_t wide[2048];
		const int n = _vsnwprintf_s(wide, _TRUNCATE, fmt, args);
		const int count = n < 0 ? static_cast<int>(wcslen(wide)) : n;

		char utf8[4096];
		const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, count,
			utf8, sizeof(utf8), nullptr, nullptr);
		if (bytes > 0)
			Emit(utf8, bytes);
	}

	void Line(const char* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		LineV(fmt, args);
		va_end(args);
	}

	void Line(const wchar_t* fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		LineV(fmt, args);
		va_end(args);
	}

	void Close()
	{
		AcquireSRWLockExclusive(&g_lock);
		if (g_file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(g_file);
			g_file = INVALID_HANDLE_VALUE;
		}
		ReleaseSRWLockExclusive(&g_lock);
	}
}