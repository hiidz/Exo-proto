#include "DeviceResolver.h"

#include <algorithm>

namespace
{
	std::string SerialFromInstance(const std::string& instance)
	{
		constexpr size_t prefixLen = 4;   // "adb-"
		if (instance.rfind("adb-", 0) != 0)
			return instance;

		const auto lastDash = instance.rfind('-');
		if (lastDash == std::string::npos || lastDash <= prefixLen)
			return instance;

		return instance.substr(prefixLen, lastDash - prefixLen);
	}

	std::vector<DeviceCandidate> CandidatesForSerial(const std::vector<DeviceCandidate>& all, const std::string& serial)
	{
		std::vector<DeviceCandidate> out;
		for (const auto& c : all)
			if (c.serial == serial)
				out.push_back(c);
		return out;
	}

	DeviceCandidate AutoSelectCandidate(const std::vector<DeviceCandidate>& candidates)
	{
		std::vector<DeviceCandidate> usbFirst;

		for (const auto& c : candidates)
			if (!c.wireless)
				usbFirst.push_back(c);

		if (usbFirst.empty())
			usbFirst = candidates;

		std::sort(usbFirst.begin(), usbFirst.end(),
			[](const DeviceCandidate& a, const DeviceCandidate& b)
			{
				return a.serial < b.serial;
			});

		return usbFirst.front();
	}
}

std::vector<DeviceCandidate> ListDeviceCandidates(const AdbConfig& base, DWORD timeoutMs)
{
	const AdbClient probe(base);
	std::vector<DeviceCandidate> candidates;

	// Wireless first. ResolveDevice's tiebreak does not depend on this order,
	// but the tray renders the Device menu in it.
	for (const auto& service : probe.ListMdnsServices(timeoutMs))
	{
		if (service.Connectable())
			candidates.push_back({ service.endpoint, SerialFromInstance(service.instance), service.instance, true });
	}

	for (const auto& device : probe.ListDevices(timeoutMs))
	{
		if (!device.Online())
			continue;

		// ip:port is a wireless device something already `adb connect`ed to.
		// No stable serial of its own, and if it's advertising it's already
		// listed above -- nothing to add either way.
		if (device.serial.find(':') != std::string::npos)
			continue;

		candidates.push_back({ device.serial, device.serial, device.serial, false });
	}

	return candidates;
}

bool ResolveDevice(const AdbConfig& base, AdbConfig& resolved, std::string& error, const LogFn& log, DeviceCandidate* chosenOut)
{
	const auto candidates = ListDeviceCandidates(base);
	DeviceCandidate chosen;

	if (base.serial.empty())
	{
		if (candidates.empty())
		{
			error = "no phone found (turn on Wireless debugging)";
			return false;
		}

		// No saved preference. Deterministic for this attempt only, so two
		// phones present never becomes an error and never becomes a coin flip.
		// chosen = AutoSelectCandidate(candidates);
	}
	else
	{
		// Pinned. serial alone is ambiguous once a device has both transports
		// up, so wireless has to match too.
		const auto forDevice = CandidatesForSerial(candidates, base.serial);

		std::vector<DeviceCandidate> matched;
		for (const auto& c : forDevice)
			if (c.wireless == base.wireless)
				matched.push_back(c);

		if (matched.empty())
		{
			error = "device " + base.serial + " not found";
			return false;
		}
		chosen = matched.front();
	}

	resolved = base;
	resolved.serial = chosen.target;
	resolved.wireless = chosen.wireless;

	if (chosen.wireless && !AdbClient(base).Connect(chosen.target))
	{
		error = "could not connect to " + chosen.target;
		return false;
	}

	if (!AdbClient(resolved).IsDeviceOnline())
	{
		error = chosen.label + " is not authorised (pair this PC on the phone)";
		return false;
	}

	if (log)
		log(std::string("device: ") + (chosen.wireless ? "wireless " : "usb ") + chosen.label);

	if (chosenOut)
		*chosenOut = chosen;

	return true;
}