#include "CameraList.h"

#include <sstream>

namespace
{
	// "    --camera-id=0    (back, 4032x3024, fps=[15, 24, 30])"
	//
	// Anchored on the flag rather than the line shape, because the id is the
	// only part that has to be exact -- the parenthesised blurb is decoration
	// and its contents have shifted between scrcpy versions.
	bool ParseCameraLine(const std::string& line, CameraInfo& out)
	{
		constexpr char kMarker[] = "--camera-id=";
		const auto marker = line.find(kMarker);
		if (marker == std::string::npos)
			return false;

		const auto idStart = marker + sizeof(kMarker) - 1;
		const auto idEnd = line.find_first_of(" \t\r(", idStart);

		out.id = Trim(line.substr(idStart,
			idEnd == std::string::npos ? std::string::npos : idEnd - idStart));

		if (out.id.empty())
			return false;

		const auto open = line.find('(', idStart);
		const auto close = line.rfind(')');
		if (open == std::string::npos || close == std::string::npos || close <= open)
			return true;

		std::istringstream fields(line.substr(open + 1, close - open - 1));
		std::string token;

		if (std::getline(fields, token, ','))
			out.facing = Trim(token);
		if (std::getline(fields, token, ','))
			out.size = Trim(token);

		return true;
	}

	std::string FailureReason(const proc::RunResult& result)
	{
		if (!result.launched)
			return "could not run adb";

		if (result.exitCode == WAIT_TIMEOUT)
			return "timed out listing cameras";

		// The server puts its real complaint on a line tagged ERROR. Without
		// one, the last thing it said is the best guess available -- the same
		// reasoning as ScrcpySession's _lastError.
		std::istringstream lines(result.output);
		std::string line, lastNonEmpty, errorLine;

		while (std::getline(lines, line))
		{
			const std::string trimmed = Trim(line);
			if (trimmed.empty())
				continue;

			lastNonEmpty = trimmed;
			if (errorLine.empty() && trimmed.find("ERROR") != std::string::npos)
				errorLine = trimmed;
		}

		if (!errorLine.empty())
			return errorLine;

		return lastNonEmpty.empty() ? "the phone reported no cameras" : lastNonEmpty;
	}
}

std::vector<CameraInfo> ListCameras(const AdbClient& adb, const ScrcpyOptions& options,
	std::string& error, DWORD timeoutMs)
{
	std::vector<CameraInfo> cameras;
	error.clear();

	if (!PushScrcpyServer(adb, options, error))
		return cameras;

	std::wostringstream args;
	args << L"shell CLASSPATH=" << options.remoteJarPath
		<< L" /system/bin/app_process / com.genymobile.scrcpy.Server "
		<< options.scrcpyVersion
		<< L" list_cameras=true"
		// In list mode the server deletes its own jar unless cleanup is off,
		// which would silently undo the push above for the next session.
		<< L" cleanup=false";

	const auto result = adb.Run(args.str(), timeoutMs);

	std::istringstream lines(result.output);
	std::string line;

	while (std::getline(lines, line))
	{
		CameraInfo camera;
		if (ParseCameraLine(line, camera))
			cameras.push_back(std::move(camera));
	}

	if (cameras.empty())
		error = FailureReason(result);

	return cameras;
}