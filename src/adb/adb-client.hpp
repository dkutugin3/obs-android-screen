#pragma once

#include <cstdint>
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

// Platform handles for a long-lived child process (see spawn()).
#ifdef _WIN32
using PipeHandle = void *; // HANDLE
using ProcHandle = void *; // HANDLE
inline constexpr PipeHandle kInvalidPipe = nullptr;
inline constexpr ProcHandle kInvalidProc = nullptr;
#else
using PipeHandle = int;
using ProcHandle = int; // pid_t
#define ADB_INVALID_PIPE (-1)
#define ADB_INVALID_PROC (-1)
inline constexpr PipeHandle kInvalidPipe = ADB_INVALID_PIPE;
inline constexpr ProcHandle kInvalidProc = ADB_INVALID_PROC;
#endif

// Locate the adb executable.
// Order: explicit override, bundled copy, platform-typical installs, PATH.
// Returns empty string when not found.
std::string findAdb(const std::string &overridePath = "");

// Run an adb command, capture stdout/stderr. Returns process exit status
// (<0 on spawn failure). `timeoutMs` <= 0 means no timeout.
int run(const std::string &adb, const std::vector<std::string> &args, std::string *stdoutOut = nullptr,
	std::string *stderrOut = nullptr, long timeoutMs = 15000);

// Spawn a long-lived adb command with its stdout piped back to the caller
// (stderr is discarded unless `stderrPipe` is given). `ok` (may be null)
// reports spawn success. The caller owns the pipes and must reap the child
// via terminate/reapChild.
PipeHandle spawn(const std::string &adb, const std::vector<std::string> &args, ProcHandle *procOut, bool *ok,
	PipeHandle *stderrPipe = nullptr);

// Blocking read from a spawn() pipe: returns the number of bytes read
// (0 = orderly end of stream, -1 = error).
long readPipe(PipeHandle pipe, uint8_t *buf, size_t len);
void closePipe(PipeHandle pipe);
void terminateChild(ProcHandle proc); // ask the child to die
void reapChild(ProcHandle proc);      // block until it is gone, release the handle

// Parse `adb devices -l` output.
std::vector<Device> parseDevicesOutput(const std::string &out);

// Enumerate devices. Returns false if the adb command itself failed.
bool listDevices(const std::string &adb, std::vector<Device> &devices, std::string *error = nullptr);

// Read a system property from a device (empty string on failure).
std::string getprop(const std::string &adb, const std::string &serial, const std::string &prop);

} // namespace adb
