#include "device-monitor.hpp"

#include <chrono>

#include <util/base.h>

static std::string makeDisplayName(const adb::Device &dev, const std::string &marketName)
{
	std::string base;
	if (!marketName.empty())
		base = marketName;
	else if (!dev.model.empty())
		base = dev.model;
	else
		base = "Android device";

	if (dev.state != adb::DeviceState::Ok)
		base += " [" + dev.stateRaw + "]";

	return base + " (" + dev.serial + ")";
}

static bool sameDevices(const std::vector<DeviceMonitor::DeviceInfo> &a,
	const std::vector<DeviceMonitor::DeviceInfo> &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i].serial != b[i].serial || a[i].stateRaw != b[i].stateRaw
			|| a[i].displayName != b[i].displayName)
			return false;
	}
	return true;
}

DeviceMonitor &DeviceMonitor::instance()
{
	static DeviceMonitor monitor;
	return monitor;
}

DeviceMonitor::~DeviceMonitor()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	killTracker(); // wakes the blocked read() in runTracker
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void DeviceMonitor::addObserver(const void *owner, Observer cb)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		observers_[owner] = std::move(cb);
		ensureThread();
	}
	cv_.notify_all();
}

void DeviceMonitor::removeObserver(const void *owner)
{
	bool noneLeft = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		observers_.erase(owner);
		noneLeft = observers_.empty() && !pollRequested_.load();
	}
	if (noneLeft)
		killTracker();
}

std::vector<DeviceMonitor::DeviceInfo> DeviceMonitor::snapshot(bool refreshNow)
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		ensureThread();
	}
	if (refreshNow) {
		pollRequested_ = true;
		cv_.notify_all();
		std::unique_lock<std::mutex> lock(mutex_);
		if (!firstBlockSeen_)
			cv_.wait_for(lock, std::chrono::milliseconds(1500),
				[this] { return firstBlockSeen_ || stop_.load(); });
		return devices_; // unique_lock still held; that is the read lock
	}
	std::lock_guard<std::mutex> lock(mutex_);
	return devices_;
}

std::string DeviceMonitor::adbPath() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return adbPath_;
}

void DeviceMonitor::ensureThread()
{
	if (threadRunning_)
		return;
	threadRunning_ = true;
	stop_ = false;
	thread_ = std::thread(&DeviceMonitor::threadFunc, this);
}

void DeviceMonitor::threadFunc()
{
	blog(LOG_INFO, "[obs-android-adb] device monitor thread started");
	while (!stop_) {
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (observers_.empty() && !pollRequested_) {
				cv_.wait(lock, [this] { return stop_ || !observers_.empty() || pollRequested_; });
				if (stop_)
					break;
			}
		}

		runTracker();
		if (stop_)
			break;

		// The tracker ended on its own (adb server restarted or died) or is no
		// longer wanted. Pause before respawning so a broken adb cannot turn
		// into a fork bomb.
		std::unique_lock<std::mutex> lock(mutex_);
		cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_ || pollRequested_; });
	}
	blog(LOG_INFO, "[obs-android-adb] device monitor thread stopped");
}

void DeviceMonitor::runTracker()
{
	std::string adb = adb::findAdb();
	if (adb.empty()) {
		bool warn = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			warn = !adbFailed_;
			adbFailed_ = true;
			adbPath_.clear();
			devices_.clear();
		}
		if (warn)
			blog(LOG_WARNING, "[obs-android-adb] adb not found");
		return;
	}

	adb::ProcHandle proc = adb::kInvalidProc;
	adb::PipeHandle pipe = adb::kInvalidPipe;
	bool broken = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		broken = trackerBroken_;
	}
	if (!broken) {
		bool ok = false;
		pipe = adb::spawn(adb, {"track-devices", "-l"}, &proc, &ok);
		if (!ok)
			pipe = adb::kInvalidPipe;
	}
	if (pipe == adb::kInvalidPipe) {
		// Long-lived tracking unavailable; degrade to one-shot polls.
		pollOnce(adb);
		pollRequested_ = false;
		notifyObservers();
		return;
	}

	{
		std::lock_guard<std::mutex> lock(mutex_);
		adbFailed_ = false;
		adbPath_ = adb;
	}
	trackerProc_ = (uintptr_t)proc;
	blog(LOG_INFO, "[obs-android-adb] using adb: %s", adb.c_str());
	blog(LOG_INFO, "[obs-android-adb] tracking device events (adb track-devices)");

	// `adb track-devices` output is a sequence of messages, each framed as a
	// 4-hex-digit length prefix followed by that many bytes containing the
	// complete current device list (empty list = zero length). One message
	// arrives for every device connect/disconnect/state change.
	std::string buf;
	uint8_t chunk[4096];
	bool done = false;
	bool framingError = false;
	while (!done && !stop_) {
		long n = adb::readPipe(pipe, chunk, sizeof(chunk));
		if (n < 0)
			break;
		if (n == 0)
			break;

		buf.append((const char *)chunk, (size_t)n);
		while (!done && buf.size() >= 4) {
			size_t len = 0;
			bool valid = true;
			for (int i = 0; i < 4; i++) {
				char c = buf[i];
				len <<= 4;
				if (c >= '0' && c <= '9')
					len |= (size_t)(c - '0');
				else if (c >= 'a' && c <= 'f')
					len |= (size_t)(c - 'a' + 10);
				else if (c >= 'A' && c <= 'F')
					len |= (size_t)(c - 'A' + 10);
				else {
					valid = false;
					break;
				}
			}
			if (!valid) {
				framingError = true;
				break;
			}
			if (len > 1u << 20) { // sanity: a device list can never be this big
				framingError = true;
				break;
			}
			if (buf.size() < 4 + len)
				break; // need more bytes for the full message

			std::string payload = buf.substr(4, len);
			buf.erase(0, 4 + len);
			commitDevices(adb::parseDevicesOutput(payload), adb);

			bool wanted = false;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				wanted = !stop_ && (!observers_.empty() || pollRequested_.load());
			}
			if (!wanted)
				done = true;
		}
		if (framingError) {
			blog(LOG_WARNING, "[obs-android-adb] unexpected adb track-devices framing, falling back to polling");
			std::lock_guard<std::mutex> lock(mutex_);
			trackerBroken_ = true;
			break;
		}
	}

	adb::closePipe(pipe);
	uintptr_t tracked = trackerProc_.exchange(0);
	if (tracked != 0) {
		adb::ProcHandle proc2 = (adb::ProcHandle)tracked;
		adb::terminateChild(proc2); // no-op if it already exited on its own
		adb::reapChild(proc2);
	}
	blog(LOG_INFO, "[obs-android-adb] device event stream ended");
}

void DeviceMonitor::pollOnce(const std::string &adb)
{
	std::vector<adb::Device> raw;
	std::string error;
	if (!adb::listDevices(adb, raw, &error)) {
		bool warn = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			warn = !adbFailed_;
			adbFailed_ = true;
			adbPath_ = adb;
		}
		if (warn)
			blog(LOG_WARNING, "[obs-android-adb] adb devices failed: %s", error.c_str());
		return; // keep previous list on transient failure
	}
	commitDevices(std::move(raw), adb);
}

void DeviceMonitor::commitDevices(std::vector<adb::Device> &&raw, const std::string &adb)
{
	std::vector<DeviceInfo> next;
	next.reserve(raw.size());
	for (auto &dev : raw) {
		DeviceInfo info;
		info.serial = dev.serial;
		info.state = dev.state;
		info.stateRaw = dev.stateRaw;

		std::string marketName;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			auto it = marketNameCache_.find(dev.serial);
			if (it != marketNameCache_.end())
				marketName = it->second;
		}
		if (dev.usable() && marketName.empty()) {
			std::string fetched = adb::getprop(adb, dev.serial, "ro.product.marketname");
			marketName = fetched.empty() ? dev.model : fetched; // non-empty marks "queried"
			{
				std::lock_guard<std::mutex> lock(mutex_);
				marketNameCache_[dev.serial] = marketName;
			}
			blog(LOG_INFO, "[obs-android-adb] device %s: model=%s marketname=%s", dev.serial.c_str(),
				dev.model.c_str(), marketName.c_str());
		}
		info.displayName = makeDisplayName(dev, marketName);
		next.push_back(std::move(info));
	}

	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		changed = !firstBlockSeen_ || !sameDevices(devices_, next);
		if (changed) {
			devices_ = std::move(next);
			firstBlockSeen_ = true;
		}
		pollRequested_ = false;
	}
	if (changed)
		notifyObservers();
}

void DeviceMonitor::notifyObservers()
{
	std::vector<Observer> toNotify;
	std::vector<DeviceInfo> current;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		current = devices_;
		for (auto &[owner, cb] : observers_)
			toNotify.push_back(cb);
	}
	for (auto &cb : toNotify) {
		if (cb)
			cb(current);
	}
}

void DeviceMonitor::killTracker()
{
	uintptr_t v = trackerProc_.exchange(0);
	if (v != 0)
		adb::terminateChild((adb::ProcHandle)v); // its death closes the pipe and wakes readPipe()
}
