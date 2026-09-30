#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "protocol.hpp"

namespace scrcpy {

struct ServerParams {
	std::string serial;
	std::string adbPath;
	std::string serverJarPath; // local path of the bundled scrcpy-server
	std::string serverVersion = SCRCPY_SERVER_VERSION; // defined via CMake

	std::string videoCodec = "h264"; // "h264" | "h265"
	uint32_t videoBitRate = 20000000;
	uint32_t maxFps = 60;
	uint32_t maxSize = 0; // 0 = native resolution
	bool audio = true;
	bool screenOff = false; // requires the control channel
	bool stayAwake = false;

	std::string dumpPath; // optional: dump raw video elementary stream
};

struct ServerCallbacks {
	// Called once when the video session starts (after device name + codec id
	// + initial session meta have been read).
	std::function<void(const std::string &deviceName, VideoCodecId codec, uint32_t width, uint32_t height)>
		onSessionStart;
	// Resolution/orientation change mid-stream.
	std::function<void(uint32_t width, uint32_t height)> onSessionChanged;
	// Video codec configuration data (Annex-B parameter sets).
	std::function<void(const uint8_t *data, size_t len)> onVideoConfig;
	// Encoded video packet (Annex-B). ptsUs == 0 for config packets is not
	// passed here (they go to onVideoConfig).
	std::function<void(const uint8_t *data, size_t len, uint64_t ptsUs, bool keyFrame)> onVideoPacket;

	std::function<void()> onAudioStarted;
	// reason is human-readable; stream continues video-only.
	std::function<void(const std::string &reason)> onAudioDisabled;
	// Raw PCM packet (S16LE interleaved stereo 48 kHz).
	std::function<void(const uint8_t *pcm, size_t len, uint64_t ptsUs)> onAudioPacket;

	// Stream ended (socket EOF, transport error or fatal server error).
	std::function<void(const std::string &error)> onEnded;
};

// One streaming session against one device. All blocking work happens on
// internal threads; start() blocks for the bring-up (~1-5 s).
class Server {
public:
	Server(ServerParams params, ServerCallbacks callbacks);
	~Server();

	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	// Bring up the session. Returns false with a human-readable error.
	bool start(std::string *error);
	// Tear down (idempotent, thread-safe). Unblocks all reader threads.
	void stop();

private:
	struct Socket {
		int fd = -1;

		bool valid() const { return fd >= 0; }
		void closeFd();
		void shutdownBoth();
	};

	bool pushServer(std::string *error);
	bool setupForward(std::string *error);
	void removeForward();
	bool spawnServerProcess(std::string *error);
	bool readFully(int fd, uint8_t *buf, size_t len);

	void videoThreadFunc();
	void audioThreadFunc();
	void controlThreadFunc();
	void stderrDrainFunc();

	void endSession(const std::string &error);

	ServerParams params_;
	ServerCallbacks cbs_;

	uint32_t scid_ = 0;
	uint16_t localPort_ = 0;
	Socket videoSock_;
	Socket audioSock_;
	Socket controlSock_;

	// adb shell process running the scrcpy server
	pid_t childPid_ = -1;
	int childStdoutFd_ = -1;
	int childStderrFd_ = -1;
	std::string childError_;
	std::mutex childMutex_;

	std::atomic<bool> stopping_{false};
	std::atomic<bool> ended_{false};
	std::string deviceName_;
	std::thread videoThread_;
	std::thread audioThread_;
	std::thread controlThread_;
	std::thread stderrThread_;

	FILE *dumpFile_ = nullptr;
};

} // namespace scrcpy
