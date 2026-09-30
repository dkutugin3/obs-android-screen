#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>

#include "adb/adb-client.hpp"

// Process-wide monitor that keeps track of connected devices using a single
// long-lived `adb track-devices -l` process (event-driven: zero polling while
// idle), enriches entries with friendly names (ro.product.marketname) and
// notifies registered observers whenever the set of devices changes. Used by
// the source properties UI and by the auto-reconnect logic ("wait until the
// device reappears").
class DeviceMonitor {
public:
	struct DeviceInfo {
		std::string serial;
		adb::DeviceState state = adb::DeviceState::Unknown;
		std::string stateRaw;
		std::string displayName; // e.g. "Galaxy S26 (R3CW30ABCDEF)"
	};

	static DeviceMonitor &instance();

	// Register/unregister an observer. While at least one observer exists the
	// tracking thread (and the adb tracker process) runs; the callback fires
	// on the monitor thread, only when the device set actually changes.
	using Observer = std::function<void(const std::vector<DeviceInfo> &)>;
	void addObserver(const void *owner, Observer cb);
	void removeObserver(const void *owner);

	// Snapshot for UI. With refreshNow, waits (bounded) for fresh data from
	// the event stream (or a one-shot poll fallback).
	std::vector<DeviceInfo> snapshot(bool refreshNow = false);

	std::string adbPath() const;

private:
	DeviceMonitor() = default;
	~DeviceMonitor();
	DeviceMonitor(const DeviceMonitor &) = delete;
	DeviceMonitor &operator=(const DeviceMonitor &) = delete;

	void ensureThread();
	void threadFunc();
	void runTracker();
	void pollOnce(const std::string &adb);
	void commitDevices(std::vector<adb::Device> &&raw, const std::string &adb);
	void notifyObservers();
	void killTracker();

	mutable std::mutex mutex_;
	std::map<const void *, Observer> observers_;
	std::map<std::string, std::string> marketNameCache_;
	std::vector<DeviceInfo> devices_;
	std::string adbPath_;
	bool adbFailed_ = false;
	bool firstBlockSeen_ = false;
	bool trackerBroken_ = false; // track-devices framing unexpected; poll instead
	std::atomic<bool> pollRequested_{false};
	std::atomic<bool> stop_{false};
	std::thread thread_;
	std::condition_variable cv_;
	bool threadRunning_ = false;
	std::atomic<pid_t> trackerPid_{-1};
};
