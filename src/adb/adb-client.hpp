#pragma once

#include <string>
#include <vector>

namespace adb {

enum class DeviceState { Unknown, Ok, Unauthorized, Offline, Other };

struct Device {
	std::string serial;
	DeviceState state = DeviceState::Unknown;
	std::string stateRaw;
	std::string model;   // from "model:" field of `adb devices -l` (e.g. SM-S931B)
	std::string marketName; // ro.product.marketname (e.g. "Galaxy S26"), fetched lazily

	bool usable() const { return state == DeviceState::Ok; }
};

// Locate the adb executable.
// Order: explicit override, bundled copy, Homebrew, system, PATH.
// Returns empty string when not found.
std::string findAdb(const std::string &overridePath = "");

// Run an adb command, capture stdout/stderr. Returns process exit status
// (<0 on spawn failure). `timeoutMs` <= 0 means no timeout.
int run(const std::string &adb, const std::vector<std::string> &args, std::string *stdoutOut = nullptr,
	std::string *stderrOut = nullptr, long timeoutMs = 15000);

// Spawn a long-lived adb command with its stdout piped back to the caller
// (stderr goes to /dev/null). Returns the read end of the pipe, or -1 on
// failure. The caller owns the pipe fd and must reap the child.
int spawn(const std::string &adb, const std::vector<std::string> &args, pid_t *pidOut);

// Parse `adb devices -l` output.
std::vector<Device> parseDevicesOutput(const std::string &out);

// Enumerate devices. Returns false if the adb command itself failed.
bool listDevices(const std::string &adb, std::vector<Device> &devices, std::string *error = nullptr);

// Read a system property from a device (empty string on failure).
std::string getprop(const std::string &adb, const std::string &serial, const std::string &prop);

} // namespace adb
