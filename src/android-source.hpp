#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <obs.h>

#include "video/decoder.hpp"

#include "scrcpy/protocol.hpp"
#include "scrcpy/server.hpp"
#include "video/decoder.hpp"

struct obs_source_frame;
struct obs_source_audio;

class AndroidSource {
public:
	AndroidSource(obs_source_t *source, obs_data_t *settings);
	~AndroidSource();

	void update(obs_data_t *settings);
	void activate();
	void deactivate();
	void show();
	void hide();

	obs_properties_t *properties();
	uint32_t width() const { return curWidth_; }
	uint32_t height() const { return curHeight_; }

private:
	struct Settings {
		std::string serial;
		std::string videoCodec = "h264";
		std::string resolution = "1920x1080";
		uint32_t fps = 60;
		uint32_t bitrateMbps = 20;
		bool audio = true;
		bool screenOff = false;
		bool stayAwake = false;
		bool autoReconnect = true;
		std::string adbPath;
	};

	enum class Status {
		Inactive,
		Connecting,
		Streaming,
		Reconnecting,
		WaitingForDevice,
		Unauthorized,
		AdbMissing,
		Error,
	};

	void applySettingsLocked(obs_data_t *settings);
	void setStatus(Status s, const std::string &text);
	void sessionLoop();
	void startSession();      // spawns the loop thread
	void stopSessionThread(); // joins it
	void runOneSessionAttempt();
	void showPlaceholder(const std::string &line1, const std::string &line2);

	// scrcpy server callbacks (invoked on server threads)
	void onSessionStart(const std::string &deviceName, scrcpy::VideoCodecId codec, uint32_t w, uint32_t h);
	void onSessionChanged(uint32_t w, uint32_t h);
	void onVideoConfig(const uint8_t *data, size_t len);
	void onVideoPacket(const uint8_t *data, size_t len, uint64_t ptsUs, bool keyFrame);
	void onAudioPacket(const uint8_t *pcm, size_t len, uint64_t ptsUs);
	void onAudioDisabled(const std::string &reason);
	void onEnded(const std::string &error);

	// Decoder output callback (VideoToolbox worker thread on macOS, the
	// demux thread on Windows/libavcodec)
	void onDecodedFrame(const video::DecodedFrame &f, uint64_t ptsNs, bool keyFrame);
	void emitAudio(const uint8_t *pcm, size_t len, uint64_t ptsNs);
	void allocateFrames(uint32_t w, uint32_t h);
	uint64_t mapTimestamp(uint64_t ptsNs, bool isAudio);

	obs_source_t *source_;

	std::mutex settingsMutex_;
	Settings settings_;

	std::atomic<bool> active_{false};
	std::atomic<bool> stopping_{false};
	std::atomic<bool> restartRequested_{false};
	std::atomic<bool> sessionEnded_{false};
	std::atomic<bool> devicesDirty_{false};

	std::atomic<Status> status_{Status::Inactive};
	std::string statusText_;
	std::mutex statusMutex_;

	std::thread sessionThread_;
	std::unique_ptr<scrcpy::Server> server_; // owned by the session thread

	// video output state (touched by VT callback thread + session thread)
	std::mutex frameMutex_;
	obs_source_frame frames_[4];
	int frameIdx_ = 0;
	uint32_t curWidth_ = 0;
	uint32_t curHeight_ = 0;
	std::unique_ptr<video::Decoder> decoder_;
	scrcpy::VideoCodecId videoCodecId_ = scrcpy::VideoCodecId::H264;
	std::string deviceName_;

	// clock mapping: host_ns = device_ns + clockOffsetNs (set on first video frame)
	bool clockBaseSet_ = false;
	int64_t clockOffsetNs_ = 0;

	// audio arriving before the clock base is established waits briefly
	std::vector<std::pair<std::vector<uint8_t>, uint64_t>> audioHoldback_;
};
