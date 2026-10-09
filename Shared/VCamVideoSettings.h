#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

// The [video] section of vcam.ini, and the only settings the two processes have
// to agree on: the tray uses them to tell scrcpy what to capture, the capture
// DLL uses them to build the format it offers Windows. If they disagree, every
// frame takes CopyDecodedNV12's scaling path instead of its memcpy path --
// silently, and at a quality loss, because ScaleNV12 is nearest neighbour.
// Hence one reader, one set of defaults, one set of clamps, here.
//
// bitrate is deliberately absent. It only ever reaches the phone's encoder, so
// the DLL has no opinion on it and it stays in VCamConfig.
struct VCamVideoSettings
{
	uint32_t width = 1920;
	uint32_t height = 1080;
	uint32_t fps = 30;
};

namespace vcamvideo
{
	// Out of range falls back; in range rounds down to even, because NV12's
	// half-resolution chroma cannot represent an odd dimension. A bad value
	// should degrade to something workable rather than leave the camera dead.
	inline uint32_t ClampEven(uint32_t value, uint32_t low, uint32_t high, uint32_t fallback)
	{
		return (value < low || value > high) ? fallback : (value & ~1u);
	}

	// Returns the fallback for a missing key and 0 for a non-numeric one, both
	// of which the range checks above turn back into the fallback.
	inline uint32_t ReadUInt(const std::wstring& ini, const wchar_t* key, uint32_t fallback)
	{
		return (uint32_t)GetPrivateProfileIntW(L"video", key, (INT)fallback, ini.c_str());
	}
}

// Never fails. A missing file, a missing key and a nonsense value all give the
// defaults.
inline VCamVideoSettings ReadVCamVideoSettings(const std::wstring& iniPath)
{
	const VCamVideoSettings defaults;
	VCamVideoSettings out;

	out.width = vcamvideo::ClampEven(
		vcamvideo::ReadUInt(iniPath, L"width", defaults.width), 160, 4096, defaults.width);
	out.height = vcamvideo::ClampEven(
		vcamvideo::ReadUInt(iniPath, L"height", defaults.height), 120, 4096, defaults.height);

	const uint32_t fps = vcamvideo::ReadUInt(iniPath, L"fps", defaults.fps);
	out.fps = (fps >= 1 && fps <= 120) ? fps : defaults.fps;

	return out;
}

// Resolves vcam.ini next to the CALLING module -- the exe for the tray, the DLL
// for the capture source. That makes co-location a deployment requirement: if
// ExoCamSource.dll and vcam.ini end up in different folders the DLL quietly
// uses defaults while the tray uses the file, which is the exact divergence this
// header exists to prevent. MediaStream::Initialize traces the path it read for
// that reason.
//
// Resolved via GetModuleHandleEx off the address of a function in this header
// rather than an HINSTANCE saved by DllMain, so it behaves identically from a
// DLL and an EXE and depends on nothing the host project has to remember to do.
inline std::wstring VCamIniPathForThisModule()
{
	HMODULE module = nullptr;
	GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)&VCamIniPathForThisModule,
		&module);

	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(module, path, MAX_PATH);

	const std::wstring full(path);
	const auto slash = full.find_last_of(L'\\');
	return (slash == std::wstring::npos ? std::wstring(L".") : full.substr(0, slash)) + L"\\vcam.ini";
}