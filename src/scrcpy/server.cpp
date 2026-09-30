#include "server.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "adb/adb-client.hpp"
#include <util/base.h>

extern char **environ;

namespace scrcpy {

static constexpr const char *DEVICE_SERVER_PATH = "/data/local/tmp/scrcpy-server.jar";
static constexpr size_t DEVICE_NAME_LENGTH = 64;

// ---------------------------------------------------------------------------
// Helpers

static uint16_t pickFreePort()
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return 0;
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	uint16_t port = 0;
	if (bind(fd, (sockaddr *)&addr, sizeof(addr)) == 0) {
		socklen_t len = sizeof(addr);
		if (getsockname(fd, (sockaddr *)&addr, &len) == 0)
			port = ntohs(addr.sin_port);
	}
	close(fd);
	return port;
}

static int connectTo(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if (connect(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

static std::string socketNameForScid(uint32_t scid)
{
	char buf[32];
	snprintf(buf, sizeof(buf), "scrcpy_%08x", scid);
	return buf;
}

void Server::Socket::closeFd()
{
	if (fd >= 0) {
		::close(fd);
		fd = -1;
	}
}

void Server::Socket::shutdownBoth()
{
	if (fd >= 0)
		::shutdown(fd, SHUT_RDWR);
}

bool Server::readFully(int fd, uint8_t *buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		ssize_t r = recv(fd, buf + got, len - got, 0);
		if (r == 0)
			return false; // EOF
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		got += (size_t)r;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Lifecycle

Server::Server(ServerParams params, ServerCallbacks callbacks)
	: params_(std::move(params)),
	  cbs_(std::move(callbacks))
{
}

Server::~Server()
{
	stop();
	if (dumpFile_) {
		fclose(dumpFile_);
		dumpFile_ = nullptr;
	}
}

bool Server::pushServer(std::string *error)
{
	std::string out, err;
	int rc = adb::run(params_.adbPath,
		{"-s", params_.serial, "push", params_.serverJarPath, DEVICE_SERVER_PATH}, &out, &err, 30000);
	if (rc != 0) {
		*error = "adb push failed: " + (err.empty() ? out : err);
		return false;
	}
	return true;
}

bool Server::setupForward(std::string *error)
{
	localPort_ = pickFreePort();
	if (localPort_ == 0) {
		*error = "could not allocate a local TCP port";
		return false;
	}
	std::string out, err;
	int rc = adb::run(params_.adbPath,
		{"-s", params_.serial, "forward", "tcp:" + std::to_string(localPort_),
			"localabstract:" + socketNameForScid(scid_)},
		&out, &err, 10000);
	if (rc != 0) {
		*error = "adb forward failed: " + (err.empty() ? out : err);
		return false;
	}
	return true;
}

void Server::removeForward()
{
	if (localPort_ == 0)
		return;
	std::string out, err;
	adb::run(params_.adbPath,
		{"-s", params_.serial, "forward", "--remove", "tcp:" + std::to_string(localPort_)}, &out, &err, 5000);
	localPort_ = 0;
}

bool Server::spawnServerProcess(std::string *error)
{
	// CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server <ver> k=v ...
	std::vector<std::string> shellArgs = {"shell", "-T",
		"CLASSPATH=" + std::string(DEVICE_SERVER_PATH),
		"app_process",
		"/",
		"com.genymobile.scrcpy.Server",
		params_.serverVersion,

		"scid=" + [&] {
			char b[16];
			snprintf(b, sizeof(b), "%08x", scid_);
			return std::string(b);
		}(),
		"log_level=info",
		"video=true",
		std::string("audio=") + (params_.audio ? "true" : "false"),
		"video_codec=" + params_.videoCodec,
		"audio_codec=raw",
		"audio_source=output",
		"video_bit_rate=" + std::to_string(params_.videoBitRate),
		"max_fps=" + std::to_string(params_.maxFps),
		"max_size=" + std::to_string(params_.maxSize),
		"tunnel_forward=true",
		std::string("control=") + (params_.screenOff ? "true" : "false"),
		"clipboard_autosync=false",
		std::string("stay_awake=") + (params_.stayAwake ? "true" : "false"),
		"send_device_meta=true",
		"send_frame_meta=true",
		"send_dummy_byte=true",
		"send_stream_meta=true"};

	std::vector<std::string> fullArgs = {"-s", params_.serial};
	fullArgs.insert(fullArgs.end(), shellArgs.begin(), shellArgs.end());

	int outPipe[2], errPipe[2];
	if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
		*error = "pipe() failed";
		return false;
	}

	std::vector<char *> argv;
	argv.push_back(const_cast<char *>(params_.adbPath.c_str()));
	for (auto &a : fullArgs)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, outPipe[0]);
	posix_spawn_file_actions_addclose(&actions, errPipe[0]);
	posix_spawn_file_actions_addclose(&actions, outPipe[1]);
	posix_spawn_file_actions_addclose(&actions, errPipe[1]);

	pid_t pid = -1;
	int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	close(outPipe[1]);
	close(errPipe[1]);
	if (rc != 0) {
		close(outPipe[0]);
		close(errPipe[0]);
		*error = std::string("could not execute adb: ") + strerror(rc);
		return false;
	}

	childPid_ = pid;
	childStdoutFd_ = outPipe[0];
	childStderrFd_ = errPipe[0];

	int fl = fcntl(childStdoutFd_, F_GETFL, 0);
	fcntl(childStdoutFd_, F_SETFL, fl | O_NONBLOCK);
	fl = fcntl(childStderrFd_, F_GETFL, 0);
	fcntl(childStderrFd_, F_SETFL, fl | O_NONBLOCK);

	stderrThread_ = std::thread(&Server::stderrDrainFunc, this);
	return true;
}

bool Server::start(std::string *error)
{
	scid_ = arc4random() & 0x7fffffff;

	if (!params_.dumpPath.empty()) {
		dumpFile_ = fopen(params_.dumpPath.c_str(), "wb");
		if (dumpFile_)
			blog(LOG_INFO, "[obs-android-adb] dumping elementary stream to %s", params_.dumpPath.c_str());
	}

	blog(LOG_INFO, "[obs-android-adb] starting scrcpy server %s on %s (codec=%s bitrate=%u max_fps=%u max_size=%u audio=%s)",
		params_.serverVersion.c_str(), params_.serial.c_str(), params_.videoCodec.c_str(), params_.videoBitRate,
		params_.maxFps, params_.maxSize, params_.audio ? "on" : "off");

	if (!pushServer(error))
		return false;
	if (!setupForward(error))
		return false;
	if (!spawnServerProcess(error)) {
		removeForward();
		return false;
	}

	// Connect the video socket and read the dummy byte with retries: the adb
	// forward accepts TCP locally before the server is listening on the
	// device, so a successful connect() says nothing (scrcpy retries the
	// connect+read pair the same way).
	bool ok = false;
	uint8_t dummy = 0;
	for (int i = 0; i < 80 && !stopping_; i++) {
		int fd = connectTo(localPort_);
		if (fd >= 0) {
			struct timeval tv{};
			tv.tv_sec = 1;
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			ssize_t r = recv(fd, &dummy, 1, 0);
			if (r == 1) {
				struct timeval zero{};
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof(zero));
				videoSock_.fd = fd;
				ok = true;
				break;
			}
			// EOF (tunnel refused) or timeout: the server is not ready yet.
			close(fd);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	if (!ok) {
		*error = "could not connect to the scrcpy server on the device";
		std::lock_guard<std::mutex> lock(childMutex_);
		if (!childError_.empty())
			*error += ": " + childError_;
		removeForward();
		return false;
	}

	// IMPORTANT: connect the remaining sockets BEFORE reading any further
	// data. The server accepts video, then audio, then control sockets, and
	// only after all accepts it writes the device metadata — reading the name
	// earlier deadlocks both sides.
	if (params_.audio) {
		int fd = connectTo(localPort_);
		if (fd < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			fd = connectTo(localPort_);
		}
		if (fd < 0) {
			*error = "could not connect the audio socket";
			removeForward();
			return false;
		}
		audioSock_.fd = fd;
	}

	if (params_.screenOff) {
		int fd = connectTo(localPort_);
		if (fd < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			fd = connectTo(localPort_);
		}
		if (fd < 0) {
			*error = "could not connect the control socket";
			removeForward();
			return false;
		}
		controlSock_.fd = fd;
	}

	// Device name (64 bytes, NUL padded) — the server sends it once every
	// enabled socket has been accepted.
	uint8_t nameBuf[DEVICE_NAME_LENGTH];
	if (!readFully(videoSock_.fd, nameBuf, DEVICE_NAME_LENGTH)) {
		*error = "could not read device metadata";
		removeForward();
		return false;
	}
	nameBuf[DEVICE_NAME_LENGTH - 1] = 0;
	std::string deviceName((const char *)nameBuf);

	// The forward is only needed to establish new connections.
	removeForward();

	if (params_.screenOff && controlSock_.valid()) {
		uint8_t msg[2] = {CONTROL_MSG_TYPE_SET_DISPLAY_POWER, 0};
		ssize_t r = send(controlSock_.fd, msg, 2, 0);
		if (r != 2)
			blog(LOG_WARNING, "[obs-android-adb] could not send screen-off message");
	}

	videoThread_ = std::thread(&Server::videoThreadFunc, this);
	if (audioSock_.valid())
		audioThread_ = std::thread(&Server::audioThreadFunc, this);
	if (controlSock_.valid())
		controlThread_ = std::thread(&Server::controlThreadFunc, this);

	deviceName_ = deviceName;
	blog(LOG_INFO, "[obs-android-adb] connected: %s", deviceName.c_str());
	return true;
}

void Server::stop()
{
	if (stopping_.exchange(true))
		return;

	videoSock_.shutdownBoth();
	audioSock_.shutdownBoth();
	controlSock_.shutdownBoth();

	if (videoThread_.joinable())
		videoThread_.join();
	if (audioThread_.joinable())
		audioThread_.join();
	if (controlThread_.joinable())
		controlThread_.join();
	if (stderrThread_.joinable())
		stderrThread_.join();

	videoSock_.closeFd();
	audioSock_.closeFd();
	controlSock_.closeFd();

	if (childPid_ > 0) {
		kill(childPid_, SIGTERM);
		int status = 0;
		// small grace period, then hard kill
		for (int i = 0; i < 20; i++) {
			if (waitpid(childPid_, &status, WNOHANG) == childPid_) {
				childPid_ = -1;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		if (childPid_ > 0) {
			kill(childPid_, SIGKILL);
			waitpid(childPid_, &status, 0);
			childPid_ = -1;
		}
	}
	if (childStdoutFd_ >= 0) {
		close(childStdoutFd_);
		childStdoutFd_ = -1;
	}
	if (childStderrFd_ >= 0) {
		close(childStderrFd_);
		childStderrFd_ = -1;
	}
}

// ---------------------------------------------------------------------------
// Reader threads

void Server::endSession(const std::string &error)
{
	bool expected = false;
	if (!ended_.compare_exchange_strong(expected, true))
		return;
	if (cbs_.onEnded)
		cbs_.onEnded(error);
}

void Server::videoThreadFunc()
{
	uint8_t codecBuf[4];
	if (!readFully(videoSock_.fd, codecBuf, 4)) {
		endSession("video stream closed before codec id");
		return;
	}
	uint32_t codecId = read32be(codecBuf);
	VideoCodecId codec = (VideoCodecId)codecId;
	switch (codecId) {
	case (uint32_t)VideoCodecId::H264:
	case (uint32_t)VideoCodecId::H265:
	case (uint32_t)VideoCodecId::AV1:
		break;
	default:
		endSession("device disabled the video stream (unsupported codec id)");
		return;
	}

	// Initial session header
	uint8_t header[12];
	if (!readFully(videoSock_.fd, header, 12) || !(header[0] & 0x80)) {
		endSession("expected video session header");
		return;
	}
	uint32_t w = read32be(header + 4);
	uint32_t h = read32be(header + 8);
	blog(LOG_INFO, "[obs-android-adb] video: codec=%s %ux%u", videoCodecName(codec), w, h);
	if (cbs_.onSessionStart)
		cbs_.onSessionStart(deviceName_, codec, w, h);

	std::vector<uint8_t> payload;
	payload.reserve(512 * 1024);

	for (;;) {
		if (!readFully(videoSock_.fd, header, 12))
			break; // EOF / error

		if (header[0] & 0x80) {
			// session change (rotation, resolution)
			uint32_t nw = read32be(header + 4);
			uint32_t nh = read32be(header + 8);
			blog(LOG_INFO, "[obs-android-adb] video session changed: %ux%u", nw, nh);
			if (cbs_.onSessionChanged)
				cbs_.onSessionChanged(nw, nh);
			continue;
		}

		PacketHeader ph;
		ph.ptsAndFlags = read64be(header);
		ph.size = read32be(header + 8);
		if (ph.size == 0 || ph.size > 16 * 1024 * 1024) {
			endSession("invalid video packet size");
			return;
		}

		payload.resize(ph.size);
		if (!readFully(videoSock_.fd, payload.data(), ph.size))
			break;

		if (dumpFile_)
			fwrite(payload.data(), 1, ph.size, dumpFile_);

		if (ph.isConfig()) {
			if (cbs_.onVideoConfig)
				cbs_.onVideoConfig(payload.data(), ph.size);
		} else {
			if (cbs_.onVideoPacket)
				cbs_.onVideoPacket(payload.data(), ph.size, ph.ptsUs(), ph.isKeyFrame());
		}
	}

	if (stopping_)
		return;
	endSession("");
}

void Server::audioThreadFunc()
{
	uint8_t codecBuf[4];
	if (!readFully(audioSock_.fd, codecBuf, 4)) {
		if (cbs_.onAudioDisabled)
			cbs_.onAudioDisabled("audio stream closed before codec id");
		return;
	}
	uint32_t codecId = read32be(codecBuf);
	if (codecId == 0) {
		blog(LOG_WARNING, "[obs-android-adb] audio disabled by the device (Android < 11 or capture refused)");
		if (cbs_.onAudioDisabled)
			cbs_.onAudioDisabled("audio capture is not available on this device");
		return;
	}
	if (codecId == 1) {
		blog(LOG_WARNING, "[obs-android-adb] audio configuration error on the device");
		if (cbs_.onAudioDisabled)
			cbs_.onAudioDisabled("audio configuration error on the device");
		return;
	}
	if (codecId != (uint32_t)AudioCodecId::Raw) {
		if (cbs_.onAudioDisabled)
			cbs_.onAudioDisabled("unexpected audio codec id");
		return;
	}

	blog(LOG_INFO, "[obs-android-adb] audio capture started (raw PCM 48 kHz stereo)");
	if (cbs_.onAudioStarted)
		cbs_.onAudioStarted();

	FILE *audioDump = nullptr;
	if (const char *audioDumpPath = getenv("OBS_ANDROID_ADB_DUMP_AUDIO"))
		audioDump = fopen(audioDumpPath, "wb");

	uint8_t header[12];
	std::vector<uint8_t> payload;
	for (;;) {
		if (!readFully(audioSock_.fd, header, 12))
			break;
		if (header[0] & 0x80) {
			continue; // audio streams should not send sessions; ignore
		}
		PacketHeader ph;
		ph.ptsAndFlags = read64be(header);
		ph.size = read32be(header + 8);
		if (ph.size == 0 || ph.size > 1024 * 1024)
			break;

		payload.resize(ph.size);
		if (!readFully(audioSock_.fd, payload.data(), ph.size))
			break;

		if (ph.isConfig())
			continue; // raw audio has no config packets

		if (audioDump)
			fwrite(payload.data(), 1, ph.size, audioDump);
		if (cbs_.onAudioPacket)
			cbs_.onAudioPacket(payload.data(), ph.size, ph.ptsUs());
	}

	if (audioDump) {
		fclose(audioDump);
		audioDump = nullptr;
	}
	// Audio EOF alone does not end the session (video keeps running); the
	// video thread or the transport watchdog reports disconnects.
}

void Server::controlThreadFunc()
{
	// Drain device messages (clipboard events etc.). We never send control
	// messages after the initial screen-off request.
	uint8_t sink[4096];
	for (;;) {
		ssize_t r = recv(controlSock_.fd, sink, sizeof(sink), 0);
		if (r <= 0) {
			if (r < 0 && errno == EINTR)
				continue;
			break;
		}
	}
}

void Server::stderrDrainFunc()
{
	char buf[2048];
	bool stdoutOpen = childStdoutFd_ >= 0;
	bool stderrOpen = childStderrFd_ >= 0;
	std::string pending;

	while (stdoutOpen || stderrOpen) {
		struct pollfd pfds[2];
		int n = 0;
		int outIdx = -1, errIdx = -1;
		if (stdoutOpen) {
			outIdx = n;
			pfds[n].fd = childStdoutFd_;
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			n++;
		}
		if (stderrOpen) {
			errIdx = n;
			pfds[n].fd = childStderrFd_;
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			n++;
		}
		int r = poll(pfds, (nfds_t)n, 1000);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			continue;
		auto slurp = [&](int idx, int fd, bool &open) {
			if (idx >= 0 && (pfds[idx].revents & (POLLIN | POLLHUP))) {
				ssize_t got = read(fd, buf, sizeof(buf));
				if (got > 0) {
					pending.append(buf, (size_t)got);
					size_t lineStart = 0;
					size_t nl;
					while ((nl = pending.find('\n', lineStart)) != std::string::npos) {
						std::string line = pending.substr(lineStart, nl - lineStart);
						lineStart = nl + 1;
						if (!line.empty() && line.back() == '\r')
							line.pop_back();
						if (line.empty() || line.find("DEBUG") != std::string::npos)
							continue;
						int level = line.find("ERROR") != std::string::npos
							? LOG_ERROR
							: (line.find("WARN") != std::string::npos ? LOG_WARNING : LOG_INFO);
						blog(level, "%s", line.c_str());
						std::lock_guard<std::mutex> lock(childMutex_);
						childError_ = line;
					}
					pending.erase(0, lineStart);
				} else {
					open = false;
				}
			}
		};
		slurp(outIdx, childStdoutFd_, stdoutOpen);
		slurp(errIdx, childStderrFd_, stderrOpen);
	}
}

} // namespace scrcpy
