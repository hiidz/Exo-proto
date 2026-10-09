#pragma once

// The product's only logging sink: one UTF-8 text file per process under
// C:\Windows\Temp\Exo\logs, named after the module that writes it.
//
// No <windows.h> here. StreamClient.h includes this header before <winsock2.h>,
// and pulling in windows.h would drag the old winsock.h in ahead of it.
//
// Both char and wchar_t overloads exist because the two halves of the product
// speak different string types natively -- BlackHole is std::string, the Media
// Foundation layer is wide (GUID_ToStringW and friends). One sink, two front
// doors: the conversion happens once here rather than at every call site.

#include <cstdarg>

namespace Log
{
	void Line(const char* fmt, ...);
	void Line(const wchar_t* fmt, ...);

	// For callers that are themselves variadic and already hold a va_list.
	void LineV(const char* fmt, va_list args);
	void LineV(const wchar_t* fmt, va_list args);

	// Backs LOG_DETAIL. Resolved once from [log] detail in vcam.ini, then a
	// relaxed atomic load per call. Public because a caller doing expensive
	// work purely to build a detail line (StreamClient::LogPacket scans every
	// NAL in the packet) should check this and bail before doing it, rather
	// than relying on the macro's argument evaluation.
	bool IsDetail();

	// Optional. The file closes with the process anyway; this exists so the DLL
	// can release the handle at DLL_PROCESS_DETACH rather than at teardown.
	void Close();
}

// High-frequency diagnostics -- per-packet dumps, per-property COM attribute
// tracing. Off by default, enabled by `detail=1` under `[log]` in vcam.ini.
//
// Arguments are not evaluated when it's off, which is what makes it safe to
// wrap calls like GUID_ToStringW(x).c_str() that allocate on every use.
#define LOG_DETAIL(...)                  \
	do {                                 \
		if (::Log::IsDetail())           \
			::Log::Line(__VA_ARGS__);    \
	} while (0)