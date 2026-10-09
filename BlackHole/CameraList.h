#pragma once

#include "ScrcpySession.h"

#include <string>
#include <vector>

struct CameraInfo
{
	// Exactly what camera_id= wants. Not always a small integer -- some devices
	// expose physical camera ids that are not 0/1, which is the whole reason
	// this is enumerated rather than assumed.
	std::string id;

	std::string facing;   // "back", "front", "external"
	std::string size;     // largest declared size, e.g. "4032x3024"
};

// Runs the scrcpy server's list-only mode on `adb`'s device and parses what it
// prints. That mode returns before opening any socket and never opens the
// camera, so this is safe to call while a session is streaming.
//
// Returns empty and fills `error` on any failure. Blocking, and can take a few
// seconds: a jar push plus a JVM start on the phone.
std::vector<CameraInfo> ListCameras(const AdbClient& adb, const ScrcpyOptions& options, std::string& error, DWORD timeoutMs = 20000);