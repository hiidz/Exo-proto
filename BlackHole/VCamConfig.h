#pragma once

#include "ScrcpySession.h"

#include <string>

// Everything the tray needs from vcam.ini. The stream port is deliberately not
// here -- see kVCamStreamPort in Shared/VCamPort.h for why it is compile-time.
struct VCamConfig
{
	AdbConfig adb;
	ScrcpyOptions scrcpy;

	// Absolute path to the ini that was read, for logging.
	std::wstring iniPath;

	// True if the file existed. False means defaults are in play.
	bool loaded = false;

	// Reads vcam.ini from the executable's directory. Never fails: on any
	// problem you get defaults and a populated `problem` string.
	static VCamConfig Load(std::string& problem);

	// Writes the current values out as a commented starter ini, for first run.
	bool WriteTemplate() const;

	// Writes [adb] serial= and wireless= back, leaving the rest of the file --
	// including comments -- alone.
	bool SavePinned(const std::string& serial, bool wireless) const;

	// Writes [scrcpy] camera_id= back. Kept even when the device changes: it is
	// a preference, not a claim that the id exists on whatever is plugged in now.
	bool SaveCameraId(const std::wstring& cameraId) const;

	// Writes [scrcpy] capture_orientation= back.
	bool SaveOrientation(const std::wstring& orientation) const;
};