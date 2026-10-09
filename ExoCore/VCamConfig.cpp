#include "VCamConfig.h"
#include "VCamPort.h"
#include "VCamVideoSettings.h"

#include <windows.h>
#include <fstream>

namespace
{
	std::wstring ExecutableDirectory()
	{
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(nullptr, path, MAX_PATH);

		std::wstring full(path);
		const auto slash = full.find_last_of(L'\\');
		return slash == std::wstring::npos ? L"." : full.substr(0, slash);
	}

	// GetPrivateProfileString resolves a bare filename against the Windows
	// directory, not the current one, so the path always has to be absolute.
	std::wstring ReadString(const std::wstring& ini, const wchar_t* section,
		const wchar_t* key, const std::wstring& fallback)
	{
		wchar_t buffer[1024]{};
		GetPrivateProfileStringW(section, key, fallback.c_str(), buffer, ARRAYSIZE(buffer), ini.c_str());
		return buffer;
	}

	uint32_t ReadUInt(const std::wstring& ini, const wchar_t* section,
		const wchar_t* key, uint32_t fallback)
	{
		return (uint32_t)GetPrivateProfileIntW(section, key, (INT)fallback, ini.c_str());
	}

	bool FileExists(const std::wstring& path)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool WriteKey(const std::wstring& ini, const wchar_t* section,
		const wchar_t* key, const std::wstring& value)
	{
		return WritePrivateProfileStringW(section, key, value.c_str(), ini.c_str()) != FALSE;
	}
}

VCamConfig VCamConfig::Load(std::string& problem)
{
	VCamConfig config;
	config.iniPath = ExecutableDirectory() + L"\\vcam.ini";
	config.loaded = FileExists(config.iniPath);

	config.adb.serial = Narrow(ReadString(config.iniPath, L"adb", L"serial", L""));
	config.adb.wireless = ReadUInt(config.iniPath, L"adb", L"wireless", 1) != 0;
	config.adb.serverPort = (uint16_t)ReadUInt(config.iniPath, L"adb", L"port", 5037);

	config.scrcpy.serverJarPath = ReadString(config.iniPath, L"scrcpy", L"server", L"scrcpy-server");
	config.scrcpy.scrcpyVersion = ReadString(config.iniPath, L"scrcpy", L"version", L"4.1");
	config.scrcpy.cameraId = ReadString(config.iniPath, L"scrcpy", L"camera_id", L"");
	config.scrcpy.captureOrientation = ReadString(config.iniPath, L"scrcpy", L"capture_orientation", L"");

	// Shared with the DLL at build time. Not configurable, on purpose.
	config.scrcpy.localPort = kVCamStreamPort;

	// Through the shared reader, not local ReadUInt calls: these three are the
	// values the DLL also reads, and identical clamping on both sides is the
	// whole point. bitrate stays local because the DLL never sees it.
	const VCamVideoSettings video = ReadVCamVideoSettings(config.iniPath);
	config.scrcpy.width = video.width;
	config.scrcpy.height = video.height;
	config.scrcpy.fps = video.fps;

	config.scrcpy.bitRateBps = ReadUInt(config.iniPath, L"video", L"bitrate", 8000000);

	// No path validation here. The caller decides where the tools actually live
	// and overwrites adbPath/serverJarPath right after this returns, so anything
	// checked at this point is a value nobody ends up using. Exo's
	// CheckFileExists covers the real paths, with a dialog rather than a log line.
	if (!config.loaded)
		problem = "no vcam.ini found, using defaults";

	return config;
}

bool VCamConfig::WriteTemplate() const
{
	std::wofstream file(iniPath);
	if (!file)
		return false;

	// Deliberately omits path= and server=. The tray overwrites both from the
	// bundled scrcpy folder immediately after Load() returns, so writing them
	// here would show two fields that look editable and silently are not.
	file << L"; Device selection. Leave serial blank to automatically choose a device on startup.\n"
		<< L"; With multiple devices, auto-detect prefers USB and then chooses\n"
		<< L"; alphabetically by serial. The automatic choice is not saved, so\n"
		<< L"; this decision is made again each time the tray starts.\n"
		<< L";\n"
		<< L"; To manually pin a specific device, put its stable serial here.\n"
		<< L"; Example: serial=0123456789ABCDEF\n"
		<< L"; A manually pinned serial is remembered across launches.\n"
		<< L";\n"
		<< L"; Use the stable device serial, not an ip:port. Android repicks the\n"
		<< L"; wireless port every time the toggle goes off and on; the serial\n"
		<< L"; does not change, and is matched against both transports.\n"
		<< L"[adb]\n"
		<< L"serial=" << Widen(adb.serial) << L"\n"
		<< L"wireless=" << (adb.wireless ? 1 : 0) << L"\n"
		<< L"port=" << adb.serverPort << L"\n"
		<< L"\n"
		<< L"; adb.exe and scrcpy-server come from the bundled scrcpy folder next to\n"
		<< L"; the exe. version must match that jar exactly or the server refuses to\n"
		<< L"; start, and says so only on stderr.\n"
		<< L";\n"
		<< L"; camera_id is written by the tray's Camera menu, which lists the ids the\n"
		<< L"; phone actually reports. Blank means the server picks the first camera.\n"
		<< L"; The value is kept when you switch devices, so switching back restores\n"
		<< L"; it -- but ids are not portable, and one that does not exist on the\n"
		<< L"; current phone makes the session fail rather than silently fall back.\n"
		<< L";\n"
		<< L"; capture_orientation describes how the phone is mounted, not the phone,\n"
		<< L"; so it applies to every device and camera. Values: 0, 90, 180, 270, or\n"
		<< L"; flip0/flip90/flip180/flip270. Blank means unrotated.\n"
		<< L"[scrcpy]\n"
		<< L"version=" << scrcpy.scrcpyVersion << L"\n"
		<< L"camera_id=" << scrcpy.cameraId << L"\n"
		<< L"capture_orientation=" << scrcpy.captureOrientation << L"\n"
		<< L"\n"
		<< L"; Video format. width, height and fps are read by BOTH the tray (to\n"
		<< L"; tell the phone what to capture) and the capture DLL (to build the\n"
		<< L"; format Windows sees), so a change needs the tray restarted AND the\n"
		<< L"; app that uses the camera reopened. An app already streaming keeps\n"
		<< L"; the old format until it lets go of the camera.\n"
		<< L";\n"
		<< L"; width/height must be a size the selected camera actually supports.\n"
		<< L"; scrcpy refuses an unsupported size outright rather than picking a\n"
		<< L"; near one, so a wrong value fails at startup with the server's own\n"
		<< L"; message. 1920x1080 and 1280x720 are safe on essentially any phone.\n"
		<< L";\n"
		<< L"; Before raising these: video calling rarely sends 1080p. Zoom needs a\n"
		<< L"; paid plan for 720p and a business plan for 1080p, Teams and Meet\n"
		<< L"; default to 720p, and all of them drop to 360p on a busy call.\n"
		<< L"; Capturing more than the app sends costs phone battery and wireless\n"
		<< L"; bandwidth for pixels that get discarded.\n"
		<< L";\n"
		<< L"; fps above 30 is wasted for the same reason: the DLL hands Windows\n"
		<< L"; whatever the newest decoded frame is when asked, so surplus frames\n"
		<< L"; are decoded and thrown away.\n"
		<< L";\n"
		<< L"; bitrate is the phone's H264 encoder target in bits per second, and\n"
		<< L"; is the one key here the DLL never reads. Lower it if wireless is\n"
		<< L"; unreliable.\n"
		<< L"[video]\n"
		<< L"width=" << scrcpy.width << L"\n"
		<< L"height=" << scrcpy.height << L"\n"
		<< L"fps=" << scrcpy.fps << L"\n"
		<< L"bitrate=" << scrcpy.bitRateBps << L"\n"
		<< L"\n"
		<< L"; Both processes write a log to C:\\Windows\\Temp\\Exo\\logs -- one file\n"
		<< L"; per process, named after the binary. Lifecycle events and failures\n"
		<< L"; are always recorded.\n"
		<< L";\n"
		<< L"; detail=1 adds per-packet and per-attribute tracing. It is verbose\n"
		<< L"; enough to be unreadable during normal use, and exists for format\n"
		<< L"; negotiation problems that only appear in one particular app.\n"
		<< L";\n"
		<< L"; Read once at startup by each process independently, so the tray\n"
		<< L"; needs restarting and the app using the camera needs to reopen it.\n"
		<< L"; The header line at the top of each log says which mode it is in.\n"
		<< L"[log]\n"
		<< L"detail=0\n";

	return true;
}

bool VCamConfig::SavePinned(const std::string& serial, bool wireless) const
{
	if (!WriteKey(iniPath, L"adb", L"serial", Widen(serial)))
		return false;

	WriteKey(iniPath, L"adb", L"wireless", wireless ? L"1" : L"0");
	WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());
	return true;
}

bool VCamConfig::SaveCameraId(const std::wstring& cameraId) const
{
	if (!WriteKey(iniPath, L"scrcpy", L"camera_id", cameraId))
		return false;

	WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());
	return true;
}

bool VCamConfig::SaveOrientation(const std::wstring& orientation) const
{
	if (!WriteKey(iniPath, L"scrcpy", L"capture_orientation", orientation))
		return false;

	WritePrivateProfileStringW(nullptr, nullptr, nullptr, iniPath.c_str());
	return true;
}