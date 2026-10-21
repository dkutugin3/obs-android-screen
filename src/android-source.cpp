#include "android-source.hpp"

#include <chrono>
#include <cstring>

#include <util/base.h>
#include <util/platform.h>

#include "device-monitor.hpp"
#include "placeholder.hpp"

// defined in plugin.cpp (obs_module_text is only usable there)
const char *aadText(const char *lookup);

#define LOGP "[obs-android-adb] "

static uint32_t resolutionToMaxSize(const std::string &resolution)
{
	if (resolution == "native")
		return 0;
	if (resolution == "1280x720")
		return 1280;
	if (resolution == "2560x1440")
		return 2560;
	return 1920; // "1920x1080" and any unknown value
}

AndroidSource::AndroidSource(obs_source_t *source, obs_data_t *settings)
	: source_(source)
{
	memset(frames_, 0, sizeof(frames_));
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		applySettingsLocked(settings);
	}
	// Keep the device monitor hot while this source exists; it drives
	// auto-reconnect and the device list.
	DeviceMonitor::instance().addObserver(this, [this](const std::vector<DeviceMonitor::DeviceInfo> &) {
		devicesDirty_ = true;
	});
}

AndroidSource::~AndroidSource()
{
	stopSessionThread();
	DeviceMonitor::instance().removeObserver(this);
	for (auto &f : frames_) {
		if (f.data[0])
			bfree(f.data[0]);
		if (f.data[1])
			bfree(f.data[1]);
	}
}

// ---------------------------------------------------------------------------
// Settings

void AndroidSource::applySettingsLocked(obs_data_t *settings)
{
	Settings s;
	s.serial = obs_data_get_string(settings, "serial");
	s.videoCodec = obs_data_get_string(settings, "video_codec");
	if (s.videoCodec != "h264" && s.videoCodec != "h265")
		s.videoCodec = "h264";
	s.resolution = obs_data_get_string(settings, "resolution");
	if (s.resolution.empty())
		s.resolution = "1920x1080";
	s.fps = (uint32_t)obs_data_get_int(settings, "fps");
	if (s.fps == 0)
		s.fps = 60;
	s.bitrateMbps = (uint32_t)obs_data_get_int(settings, "bitrate_mbps");
	if (s.bitrateMbps == 0)
		s.bitrateMbps = 20;
	s.audio = obs_data_get_bool(settings, "audio");
	s.screenOff = obs_data_get_bool(settings, "screen_off");
	s.stayAwake = obs_data_get_bool(settings, "stay_awake");
	s.autoReconnect = obs_data_get_bool(settings, "auto_reconnect");
	s.adbPath = obs_data_get_string(settings, "adb_path");
	settings_ = std::move(s);
}

void AndroidSource::update(obs_data_t *settings)
{
	Settings old;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		old = settings_;
		applySettingsLocked(settings);
	}
	const Settings &cur = settings_;

	bool needsRestart = old.serial != cur.serial || old.videoCodec != cur.videoCodec
		|| old.resolution != cur.resolution || old.fps != cur.fps || old.bitrateMbps != cur.bitrateMbps
		|| old.audio != cur.audio || old.screenOff != cur.screenOff || old.stayAwake != cur.stayAwake
		|| old.adbPath != cur.adbPath;

	if (needsRestart)
		restartRequested_ = true; // the session loop picks this up
}

void AndroidSource::activate()
{
	active_ = true;
	startSession();
}

void AndroidSource::deactivate()
{
	active_ = false; // the session loop tears the stream down
}

void AndroidSource::show() {}
void AndroidSource::hide() {}

void AndroidSource::startSession()
{
	if (sessionThread_.joinable())
		return;
	stopping_ = false;
	sessionThread_ = std::thread(&AndroidSource::sessionLoop, this);
}

void AndroidSource::stopSessionThread()
{
	stopping_ = true;
	if (sessionThread_.joinable())
		sessionThread_.join();
}

// ---------------------------------------------------------------------------
// Status

void AndroidSource::setStatus(Status s, const std::string &text)
{
	status_.store(s);
	{
		std::lock_guard<std::mutex> lock(statusMutex_);
		statusText_ = text;
	}
	// Surface the text in the properties pane (OBS_TEXT_INFO reads the
	// settings value).
	obs_data_t *settings = obs_source_get_settings(source_);
	obs_data_set_string(settings, "status", text.c_str());
	obs_data_release(settings);
}

void AndroidSource::showPlaceholder(const std::string &line1, const std::string &line2)
{
	uint32_t w, h;
	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		w = curWidth_ ? curWidth_ : 1280;
		h = curHeight_ ? curHeight_ : 720;
	}
	placeholder::Frame f = placeholder::render(line1, line2, w, h);
	if (!f.valid())
		return;

	std::lock_guard<std::mutex> lock(frameMutex_);
	allocateFrames(f.width, f.height);
	obs_source_frame &frame = frames_[frameIdx_];
	frameIdx_ = (frameIdx_ + 1) % 4;

	size_t ySize = (size_t)f.width * f.height;
	memcpy(frame.data[0], f.data.data(), ySize);
	memcpy(frame.data[1], f.data.data() + ySize, ySize / 2);
	frame.timestamp = os_gettime_ns();
	obs_source_output_video(source_, &frame);
}

// ---------------------------------------------------------------------------
// Session loop

void AndroidSource::sessionLoop()
{
	blog(LOG_INFO, LOGP "session thread started");

	int backoffMs = 1000;

	while (!stopping_) {
		Settings s;
		{
			std::lock_guard<std::mutex> lock(settingsMutex_);
			s = settings_;
		}

		if (s.serial.empty()) {
			if (active_ && status_.load() == Status::Inactive) {
				showPlaceholder("Android Screen (ADB)", "Select a device in the source properties.");
				setStatus(Status::Inactive, "Select a device.");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			backoffMs = 1000;
			continue;
		}
		if (!active_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			backoffMs = 1000;
			continue;
		}

		std::string adb = adb::findAdb(s.adbPath);
		if (adb.empty()) {
			if (status_.load() != Status::AdbMissing) {
				blog(LOG_ERROR, LOGP "adb was not found");
				showPlaceholder("ADB was not found",
					"Install Android Platform Tools: brew install --cask android-platform-tools");
				setStatus(Status::AdbMissing,
					"ADB was not found. Install Android Platform Tools, then click Refresh.");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(2000));
			continue;
		}

		auto devices = DeviceMonitor::instance().snapshot(false);
		const DeviceMonitor::DeviceInfo *dev = nullptr;
		for (auto &d : devices) {
			if (d.serial == s.serial) {
				dev = &d;
				break;
			}
		}
		if (!dev) {
			if (status_.load() != Status::WaitingForDevice) {
				blog(LOG_INFO, LOGP "device %s disconnected, waiting...", s.serial.c_str());
				showPlaceholder("Device disconnected", "Waiting for the device to reconnect...");
				setStatus(Status::WaitingForDevice, "Device is not connected.");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			backoffMs = 1000;
			continue;
		}
		if (dev->state == adb::DeviceState::Unauthorized) {
			if (status_.load() != Status::Unauthorized) {
				blog(LOG_WARNING, LOGP "device %s is not authorized", s.serial.c_str());
				showPlaceholder("Device is not authorized",
					"Unlock your phone and accept the USB debugging prompt.");
				setStatus(Status::Unauthorized,
					"Device is not authorized. Unlock your phone and accept the USB "
					"debugging prompt.");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1500));
			backoffMs = 1000;
			continue;
		}
		if (dev->state != adb::DeviceState::Ok) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			continue;
		}

		runOneSessionAttempt();

		if (stopping_)
			break;
		if (restartRequested_.exchange(false)) {
			backoffMs = 1000;
			continue; // restart immediately with the new settings
		}
		if (!active_)
			continue;

		if (!s.autoReconnect) {
			setStatus(Status::Error, "Stream ended. (auto reconnect disabled)");
			continue;
		}

		setStatus(Status::Reconnecting, "Reconnecting...");
		showPlaceholder("Device disconnected", "Reconnecting...");
		std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
		backoffMs = std::min(backoffMs * 2, 5000);
	}

	blog(LOG_INFO, LOGP "session thread stopped");
}

void AndroidSource::runOneSessionAttempt()
{
	Settings s;
	{
		std::lock_guard<std::mutex> lock(settingsMutex_);
		s = settings_;
	}

	scrcpy::ServerParams params;
	params.serial = s.serial;
	params.adbPath = adb::findAdb(s.adbPath);
	params.videoCodec = s.videoCodec;
	params.videoBitRate = s.bitrateMbps * 1000000;
	params.maxFps = s.fps;
	params.maxSize = resolutionToMaxSize(s.resolution);
	params.audio = s.audio;
	params.screenOff = s.screenOff;
	params.stayAwake = s.stayAwake;
	if (const char *dump = getenv("OBS_ANDROID_ADB_DUMP"))
		params.dumpPath = dump;
	if (const char *serverPath = getenv("OBS_ANDROID_ADB_SERVER_JAR"))
		params.serverJarPath = serverPath;

	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		clockBaseSet_ = false;
		audioHoldback_.clear();
	}
	sessionEnded_ = false;

	scrcpy::ServerCallbacks cbs;
	cbs.onSessionStart = [this](const std::string &name, scrcpy::VideoCodecId c, uint32_t w, uint32_t h) {
		onSessionStart(name, c, w, h);
	};
	cbs.onSessionChanged = [this](uint32_t w, uint32_t h) { onSessionChanged(w, h); };
	cbs.onVideoConfig = [this](const uint8_t *d, size_t l) { onVideoConfig(d, l); };
	cbs.onVideoPacket = [this](const uint8_t *d, size_t l, uint64_t p, bool k) { onVideoPacket(d, l, p, k); };
	cbs.onAudioStarted = [] {};
	cbs.onAudioDisabled = [this](const std::string &reason) { onAudioDisabled(reason); };
	cbs.onAudioPacket = [this](const uint8_t *d, size_t l, uint64_t p) { onAudioPacket(d, l, p); };
	cbs.onEnded = [this](const std::string &error) { onEnded(error); };

	server_ = std::make_unique<scrcpy::Server>(params, cbs);

	setStatus(Status::Connecting, "Connecting...");

	std::string error;
	if (!server_->start(&error)) {
		blog(LOG_ERROR, LOGP "session failed: %s", error.c_str());
		server_.reset();

		bool unauthorized = error.find("unauthorized") != std::string::npos
			|| error.find("offline") != std::string::npos;
		if (unauthorized) {
			showPlaceholder("Device is not authorized", "Unlock your phone and accept the USB debugging prompt.");
			setStatus(Status::Unauthorized,
				"Device is not authorized. Unlock your phone and accept the USB debugging prompt.");
		} else {
			showPlaceholder("Connection failed", error);
			setStatus(Status::Error, error);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
		return;
	}

	// Stream until the server ends, the source is deactivated, or settings change.
	while (!stopping_ && !restartRequested_ && !sessionEnded_ && active_) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	server_.reset(); // destructor stops and joins everything

	// The demux threads are joined now, so no decode() is in flight. flush()
	// must run WITHOUT frameMutex_ (the VT output callbacks need it).
	if (decoder_) {
		decoder_->flush();
		decoder_.reset();
	}
}

// ---------------------------------------------------------------------------
// Server callbacks

void AndroidSource::onSessionStart(const std::string &deviceName, scrcpy::VideoCodecId codec, uint32_t w, uint32_t h)
{
	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		videoCodecId_ = codec;
		if (!deviceName.empty())
			deviceName_ = deviceName;
		curWidth_ = w;
		curHeight_ = h;
	}
	// Runs on the video demux thread, which exclusively owns the decoder.
	// flush() waits for VT callbacks → must not hold frameMutex_ here.
	if (decoder_) {
		decoder_->flush();
		decoder_.reset(); // a fresh session starts with fresh parameter sets
	}
	blog(LOG_INFO, LOGP "streaming %s: %s %ux%u", deviceName_.c_str(), scrcpy::videoCodecName(codec), w, h);
	setStatus(Status::Streaming, "Streaming.");
}

void AndroidSource::onSessionChanged(uint32_t w, uint32_t h)
{
	blog(LOG_INFO, LOGP "session changed: %ux%u (orientation/resolution change)", w, h);
	std::lock_guard<std::mutex> lock(frameMutex_);
	curWidth_ = w;
	curHeight_ = h;
	// The decoder is rebuilt when the new config packet arrives.
}

// Both handlers run on the video demux thread, which is the exclusive owner
// of the decoder. frameMutex_ must NOT be held while calling into the
// decoder: VideoToolbox delivers output frames on its own threads via
// onDecodedFrame(), which needs frameMutex_ — holding it here could deadlock
// once VT's internal queue fills up.
void AndroidSource::onVideoConfig(const uint8_t *data, size_t len)
{
	if (!decoder_) {
		decoder_ = std::make_unique<video::Decoder>(
			[this](const video::DecodedFrame &f, uint64_t ptsNs, bool keyFrame) {
				onDecodedFrame(f, ptsNs, keyFrame);
			},
			videoCodecId_);
	}
	if (!decoder_->feedConfig(data, len)) {
		blog(LOG_ERROR, LOGP "decoder initialization failed (config data rejected)");
		setStatus(Status::Error, "Decoder initialization failed.");
	}
}

void AndroidSource::onVideoPacket(const uint8_t *data, size_t len, uint64_t ptsUs, bool keyFrame)
{
	if (decoder_)
		decoder_->decode(data, len, ptsUs * 1000, keyFrame);
}

void AndroidSource::onAudioPacket(const uint8_t *pcm, size_t len, uint64_t ptsUs)
{
	uint64_t ptsNs = ptsUs * 1000;
	{
		std::lock_guard<std::mutex> lock(frameMutex_);
		if (!clockBaseSet_) {
			// Hold a short burst of audio until the first video frame anchors
			// the shared clock.
			audioHoldback_.emplace_back(std::vector<uint8_t>(pcm, pcm + len), ptsNs);
			if (audioHoldback_.size() > 64)
				audioHoldback_.clear();
			return;
		}
	}
	emitAudio(pcm, len, ptsNs);
}

void AndroidSource::onAudioDisabled(const std::string &reason)
{
	blog(LOG_WARNING, LOGP "audio disabled: %s", reason.c_str());
	setStatus(Status::Streaming, "Streaming (audio disabled: " + reason + ")");
}

void AndroidSource::onEnded(const std::string &error)
{
	if (error.empty())
		blog(LOG_INFO, LOGP "stream ended");
	else
		blog(LOG_WARNING, LOGP "stream ended: %s", error.c_str());
	if (!error.empty())
		setStatus(Status::Reconnecting, error);
	sessionEnded_ = true;
}

// ---------------------------------------------------------------------------
// Decoded frames / audio → OBS

void AndroidSource::allocateFrames(uint32_t w, uint32_t h)
{
	if (frames_[0].width == w && frames_[0].height == h && frames_[0].data[0])
		return;

	size_t ySize = (size_t)w * h;
	for (auto &f : frames_) {
		if (f.data[0])
			bfree(f.data[0]);
		if (f.data[1])
			bfree(f.data[1]);
		memset(&f, 0, sizeof(f));
		f.data[0] = (uint8_t *)bmalloc(ySize);
		f.data[1] = (uint8_t *)bmalloc(ySize / 2);
		f.linesize[0] = w;
		f.linesize[1] = w;
		f.width = w;
		f.height = h;
		f.format = VIDEO_FORMAT_NV12;
		f.full_range = false;
		// BT.709 limited range, matching what HD device encoders signal.
		// The min/max range vectors MUST be filled as well: the libobs
		// conversion shader applies them whenever full_range == false, and
		// zeroed ranges (0/0 division) render as pure green.
		video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, f.color_matrix, f.color_range_min,
			f.color_range_max);
	}
}

uint64_t AndroidSource::mapTimestamp(uint64_t ptsNs, bool isAudio)
{
	(void)isAudio;
	// Both streams share the offset anchored by the first video frame, so
	// OBS sees the device-side capture-time relationship between A and V.
	int64_t ts = (int64_t)ptsNs + clockOffsetNs_;
	if (ts < 0)
		ts = 0;
	return (uint64_t)ts;
}

void AndroidSource::onDecodedFrame(const video::DecodedFrame &dec, uint64_t ptsNs, bool keyFrame)
{
	(void)keyFrame;

	const uint8_t *yBase = dec.y;
	const uint8_t *uvBase = dec.uv;
	size_t yStride = dec.yStride;
	size_t uvStride = dec.uvStride;
	uint32_t w = dec.width;
	uint32_t h = dec.height;

	if (yBase && uvBase) {
		std::lock_guard<std::mutex> lock(frameMutex_);
		if (!clockBaseSet_) {
			blog(LOG_INFO, LOGP "first decoded frame %ux%u -> OBS", w, h);
			clockBaseSet_ = true;
			clockOffsetNs_ = (int64_t)os_gettime_ns() - (int64_t)ptsNs;
			for (auto &held : audioHoldback_)
				emitAudio(held.first.data(), held.first.size(), held.second);
			audioHoldback_.clear();
		}

		allocateFrames(w, h);
		obs_source_frame &frame = frames_[frameIdx_];
		frameIdx_ = (frameIdx_ + 1) % 4;

		size_t ySize = (size_t)w * h;
		if (yStride == (size_t)w && uvStride == (size_t)w) {
			memcpy(frame.data[0], yBase, ySize);
			memcpy(frame.data[1], uvBase, ySize / 2);
		} else {
			for (uint32_t row = 0; row < h; row++)
				memcpy(frame.data[0] + (size_t)row * w, (uint8_t *)yBase + (size_t)row * yStride, w);
			for (uint32_t row = 0; row < h / 2; row++)
				memcpy(frame.data[1] + (size_t)row * w, (uint8_t *)uvBase + (size_t)row * uvStride, w);
		}
		frame.timestamp = mapTimestamp(ptsNs, false);

		// debug: dump the first frames exactly as handed to OBS
		static int dumpCount = getenv("OBS_ANDROID_ADB_DUMP_FRAMES") ? 5 : 0;
		if (dumpCount > 0) {
			char path[512];
			snprintf(path, sizeof(path), "%s/nv12-%ux%u-%d.yuv", getenv("OBS_ANDROID_ADB_DUMP_FRAMES"), w, h,
				5 - dumpCount);
			FILE *fp = fopen(path, "wb");
			if (fp) {
				fwrite(frame.data[0], 1, ySize, fp);
				fwrite(frame.data[1], 1, ySize / 2, fp);
				fclose(fp);
				blog(LOG_INFO, LOGP "dumped NV12 frame to %s", path);
			}
			dumpCount--;
		}

		obs_source_output_video(source_, &frame);
	}
}

void AndroidSource::emitAudio(const uint8_t *pcm, size_t len, uint64_t ptsNs)
{
	struct obs_source_audio audio = {};
	audio.data[0] = pcm;
	audio.frames = (uint32_t)(len / 4); // S16LE interleaved stereo
	audio.speakers = SPEAKERS_STEREO;
	audio.format = AUDIO_FORMAT_16BIT;
	audio.samples_per_sec = 48000;
	audio.timestamp = mapTimestamp(ptsNs, true);
	obs_source_output_audio(source_, &audio);
}

// ---------------------------------------------------------------------------
// Properties

static bool refreshDevicesClicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	(void)prop;
	(void)data;
	auto devices = DeviceMonitor::instance().snapshot(true);

	obs_property_t *list = obs_properties_get(props, "serial");
	obs_property_list_clear(list);
	if (devices.empty()) {
		obs_property_list_add_string(list, "No devices found — connect a phone and refresh", "");
	} else {
		for (auto &d : devices)
			obs_property_list_add_string(list, d.displayName.c_str(), d.serial.c_str());
	}
	return true;
}

obs_properties_t *AndroidSource::properties()
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *devices = obs_properties_add_list(
		props, "serial", aadText("Device"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(devices, "-- select a device --", "");
	for (auto &d : DeviceMonitor::instance().snapshot(false))
		obs_property_list_add_string(devices, d.displayName.c_str(), d.serial.c_str());

	obs_properties_add_button2(props, "refresh", aadText("RefreshDevices"), refreshDevicesClicked, nullptr);

	obs_property_t *codecs = obs_properties_add_list(
		props, "video_codec", aadText("Codec"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(codecs, "H.264", "h264");
	obs_property_list_add_string(codecs, "H.265 (HEVC)", "h265");

	obs_property_t *res = obs_properties_add_list(
		props, "resolution", aadText("Resolution"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(res, aadText("NativeResolution"), "native");
	obs_property_list_add_string(res, "1920×1080 (1080p)", "1920x1080");
	obs_property_list_add_string(res, "2560×1440 (1440p)", "2560x1440");
	obs_property_list_add_string(res, "1280×720 (720p)", "1280x720");

	obs_property_t *fps = obs_properties_add_list(
		props, "fps", aadText("FPS"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "165", 165);
	obs_property_list_add_int(fps, "144", 144);
	obs_property_list_add_int(fps, "120", 120);
	obs_property_list_add_int(fps, "90", 90);
	obs_property_list_add_int(fps, "60", 60);
	obs_property_list_add_int(fps, "30", 30);
	obs_property_list_add_int(fps, "15", 15);

	obs_property_t *br = obs_properties_add_list(
		props, "bitrate_mbps", aadText("Bitrate"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(br, "80 Mbps", 80);
	obs_property_list_add_int(br, "50 Mbps", 50);
	obs_property_list_add_int(br, "40 Mbps", 40);
	obs_property_list_add_int(br, "30 Mbps", 30);
	obs_property_list_add_int(br, "20 Mbps", 20);
	obs_property_list_add_int(br, "12 Mbps", 12);
	obs_property_list_add_int(br, "8 Mbps", 8);
	obs_property_list_add_int(br, "4 Mbps", 4);

	obs_properties_add_bool(props, "audio", aadText("CaptureAudio"));
	obs_properties_add_bool(props, "screen_off", aadText("ScreenOff"));
	obs_properties_add_bool(props, "stay_awake", aadText("StayAwake"));
	obs_properties_add_bool(props, "auto_reconnect", aadText("AutoReconnect"));

	obs_properties_add_text(props, "status", aadText("Status"), OBS_TEXT_INFO);

	obs_properties_t *advanced = obs_properties_create();
	obs_properties_add_path(advanced, "adb_path", aadText("AdbPath"), OBS_PATH_FILE, "adb", "");
	obs_properties_add_group(props, "advanced", aadText("Advanced"), OBS_GROUP_NORMAL, advanced);

	// Static version caption; the value comes from the source defaults.
	obs_properties_add_text(props, "version_info", NULL, OBS_TEXT_INFO);

	return props;
}
