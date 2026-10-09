#include "AdbClient.h"

#include <sstream>

std::wstring Widen(const std::string& s)
{
	if (s.empty())
		return {};

	const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring out((size_t)len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len);
	return out;
}

std::string Narrow(const std::wstring& s)
{
	if (s.empty())
		return {};

	const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
	std::string out((size_t)len, '\0');
	WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), len, nullptr, nullptr);
	return out;
}

std::string Trim(const std::string& s)
{
	const auto first = s.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};

	const auto last = s.find_last_not_of(" \t\r\n");
	return s.substr(first, last - first + 1);
}

std::wstring AdbClient::BuildCommandLine(const std::wstring& args, bool withDevice) const
{
	std::wostringstream ss;
	ss << L'"' << _config.adbPath << L'"'
		<< L" -P " << _config.serverPort;

	if (withDevice && !_config.serial.empty())
		ss << L" -s " << Widen(_config.serial);

	ss << L' ' << args;
	return ss.str();
}

proc::RunResult AdbClient::Run(const std::wstring& args, DWORD timeoutMs) const
{
	return proc::RunAndCapture(BuildCommandLine(args, true), timeoutMs);
}

proc::RunResult AdbClient::RunGlobal(const std::wstring& args, DWORD timeoutMs) const
{
	return proc::RunAndCapture(BuildCommandLine(args, false), timeoutMs);
}

bool AdbClient::StartServer() const
{
	// Explicit, so that starting the server is a step we can see fail rather than
	// a side effect of whichever command happened to run first.
	return RunGlobal(L"start-server", 20000).Ok();
}

bool AdbClient::IsDeviceOnline() const
{
	const auto result = Run(L"get-state", 5000);
	return result.Ok() && Trim(result.output) == "device";
}

std::string AdbClient::ClientVersion() const
{
	const auto result = RunGlobal(L"version", 5000);
	if (!result.Ok())
		return {};

	// First line is "Android Debug Bridge version 1.0.41".
	const auto eol = result.output.find('\n');
	return Trim(eol == std::string::npos ? result.output : result.output.substr(0, eol));
}

std::vector<AdbDevice> AdbClient::ListDevices(DWORD timeoutMs) const{
	std::vector<AdbDevice> devices;

	const auto result = RunGlobal(L"devices", timeoutMs);
	if (!result.Ok())
		return devices;

	std::istringstream lines(result.output);
	std::string line;

	while (std::getline(lines, line))
	{
		// Skip the "List of devices attached" banner and any blank lines.
		const auto tab = line.find('\t');
		if (tab == std::string::npos)
			continue;

		AdbDevice device;
		device.serial = Trim(line.substr(0, tab));
		device.state = Trim(line.substr(tab + 1));

		if (!device.serial.empty())
			devices.push_back(std::move(device));
	}

	return devices;
}

std::vector<AdbMdnsService> AdbClient::ListMdnsServices(DWORD timeoutMs) const
{
	std::vector<AdbMdnsService> services;

	const auto result = RunGlobal(L"mdns services", timeoutMs);
	if (!result.Ok())
		return services;

	std::istringstream lines(result.output);
	std::string line;

	while (std::getline(lines, line))
	{
		// Three whitespace-separated fields. Splitting on whitespace rather than
		// tabs specifically, because the separator has varied between adb builds.
		std::istringstream fields(line);
		AdbMdnsService service;

		if (!(fields >> service.instance >> service.service >> service.endpoint))
			continue;   // the "List of discovered mdns services" banner, or blank

		if (service.endpoint.find(':') == std::string::npos)
			continue;

		services.push_back(std::move(service));
	}

	return services;
}

bool AdbClient::Connect(const std::string& endpoint) const
{
	const auto result = RunGlobal(L"connect " + Widen(endpoint), 10000);
	if (!result.Ok())
		return false;

	// adb connect reports failure through its output, not its exit code: a
	// refused connection still exits zero.
	return result.output.find("connected to") != std::string::npos;
}