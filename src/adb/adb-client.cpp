#include "adb-client.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <mutex>

#include <util/base.h>

extern char **environ;

namespace adb {

// ---------------------------------------------------------------------------
// Process spawning

namespace {

struct ChildProcess {
	pid_t pid = -1;
	int outPipe[2] = {-1, -1}; // child stdout -> parent
	int errPipe[2] = {-1, -1}; // child stderr -> parent

	~ChildProcess() { cleanup(); }

	void cleanup()
	{
		for (int *p : {outPipe, errPipe}) {
			if (p[0] != -1)
				close(p[0]);
			if (p[1] != -1)
				close(p[1]);
			p[0] = p[1] = -1;
		}
	}
};

} // namespace

int run(const std::string &adb, const std::vector<std::string> &args, std::string *stdoutOut, std::string *stderrOut,
	long timeoutMs)
{
	if (adb.empty())
		return -1;

	std::vector<char *> argv;
	argv.reserve(args.size() + 2);
	argv.push_back(const_cast<char *>(adb.c_str()));
	for (const auto &a : args)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);

	ChildProcess child;
	if (pipe(child.outPipe) != 0 || pipe(child.errPipe) != 0)
		return -1;

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, child.outPipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, child.errPipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, child.outPipe[0]);
	posix_spawn_file_actions_addclose(&actions, child.errPipe[0]);
	posix_spawn_file_actions_addclose(&actions, child.outPipe[1]);
	posix_spawn_file_actions_addclose(&actions, child.errPipe[1]);

	// Never let SIGPIPE from a dead adb peer kill OBS
	sigset_t mask, oldmask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &mask, &oldmask);

	pid_t pid = -1;
	int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);

	posix_spawn_file_actions_destroy(&actions);

	if (rc != 0) {
		pthread_sigmask(SIG_SETMASK, &oldmask, nullptr);
		blog(LOG_WARNING, "[obs-android-adb] posix_spawn failed for %s: %s", adb.c_str(), strerror(rc));
		return -1;
	}

	// Parent: close write ends
	close(child.outPipe[1]);
	child.outPipe[1] = -1;
	close(child.errPipe[1]);
	child.errPipe[1] = -1;

	std::string outStr, errStr;
	// Interleave reading both pipes so a chatty child cannot deadlock on a full pipe.
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 60000);
	bool outOpen = true, errOpen = true;
	char buf[4096];
	while (outOpen || errOpen) {
		struct pollfd pfds[2];
		int n = 0;
		int outIdx = -1, errIdx = -1;
		if (outOpen) {
			outIdx = n;
			pfds[n].fd = child.outPipe[0];
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			n++;
		}
		if (errOpen) {
			errIdx = n;
			pfds[n].fd = child.errPipe[0];
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			n++;
		}
		int remaining = timeoutMs > 0
			? (int)std::chrono::duration_cast<std::chrono::milliseconds>(
				  deadline - std::chrono::steady_clock::now())
				  .count()
			: -1;
		if (remaining < 0)
			remaining = 500;
		int r = poll(pfds, (nfds_t)n, remaining);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (timeoutMs > 0 && r == 0 && std::chrono::steady_clock::now() >= deadline) {
			kill(pid, SIGKILL);
			break;
		}
		if (outIdx >= 0 && (pfds[outIdx].revents & (POLLIN | POLLHUP))) {
			ssize_t got = read(child.outPipe[0], buf, sizeof(buf));
			if (got > 0)
				outStr.append(buf, (size_t)got);
			else
				outOpen = false;
		}
		if (errIdx >= 0 && (pfds[errIdx].revents & (POLLIN | POLLHUP))) {
			ssize_t got = read(child.errPipe[0], buf, sizeof(buf));
			if (got > 0)
				errStr.append(buf, (size_t)got);
			else
				errOpen = false;
		}
	}

	int status = 0;
	waitpid(pid, &status, 0);
	pthread_sigmask(SIG_SETMASK, &oldmask, nullptr);

	if (stdoutOut)
		*stdoutOut = std::move(outStr);
	if (stderrOut)
		*stderrOut = std::move(errStr);

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

int spawn(const std::string &adb, const std::vector<std::string> &args, pid_t *pidOut)
{
	if (adb.empty())
		return -1;

	std::vector<char *> argv;
	argv.reserve(args.size() + 2);
	argv.push_back(const_cast<char *>(adb.c_str()));
	for (const auto &a : args)
		argv.push_back(const_cast<char *>(a.c_str()));
	argv.push_back(nullptr);

	int outPipe[2] = {-1, -1};
	if (pipe(outPipe) != 0)
		return -1;

	int devnull = open("/dev/null", O_WRONLY);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, devnull >= 0 ? devnull : outPipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, outPipe[0]);
	posix_spawn_file_actions_addclose(&actions, outPipe[1]);

	// Never let SIGPIPE from a dead adb peer kill OBS
	sigset_t mask, oldmask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	pthread_sigmask(SIG_BLOCK, &mask, &oldmask);

	pid_t pid = -1;
	int rc = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);

	posix_spawn_file_actions_destroy(&actions);
	pthread_sigmask(SIG_SETMASK, &oldmask, nullptr);
	if (devnull >= 0)
		close(devnull);
	close(outPipe[1]);

	if (rc != 0) {
		close(outPipe[0]);
		blog(LOG_WARNING, "[obs-android-adb] posix_spawn failed for %s: %s", adb.c_str(), strerror(rc));
		return -1;
	}

	if (pidOut)
		*pidOut = pid;
	return outPipe[0];
}

// ---------------------------------------------------------------------------
// adb discovery

static bool isExecutableFile(const std::string &path)
{
	struct stat st;
	return !stat(path.c_str(), &st) && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

static std::string findInPath(const char *name)
{
	const char *pathEnv = getenv("PATH");
	if (!pathEnv)
		return {};
	std::string path = pathEnv;
	size_t pos = 0;
	while (pos != std::string::npos) {
		size_t colon = path.find(':', pos);
		std::string dir = path.substr(pos, colon == std::string::npos ? colon : colon - pos);
		pos = colon == std::string::npos ? colon : colon + 1;
		if (dir.empty())
			continue;
		std::string candidate = dir + "/" + name;
		if (isExecutableFile(candidate))
			return candidate;
	}
	return {};
}

std::string findAdb(const std::string &overridePath)
{
	if (!overridePath.empty() && isExecutableFile(overridePath))
		return overridePath;

	// Bundled copy (optional)
	const char *dataPath = getenv("OBS_ANDROID_ADB_BUNDLE_DIR");
	if (dataPath) {
		std::string bundled = std::string(dataPath) + "/bin/adb";
		if (isExecutableFile(bundled))
			return bundled;
	}

	static const char *candidates[] = {
		"/opt/homebrew/bin/adb",
		"/usr/local/bin/adb",
		"/usr/bin/adb",
	};
	for (const char *c : candidates) {
		if (isExecutableFile(c))
			return c;
	}
	return findInPath("adb");
}

// ---------------------------------------------------------------------------
// Device list parsing

static void trimInPlace(std::string &s)
{
	size_t b = s.find_first_not_of(" \t\r");
	size_t e = s.find_last_not_of(" \t\r");
	if (b == std::string::npos)
		s.clear();
	else
		s = s.substr(b, e - b + 1);
}

static bool isKnownState(const std::string &s)
{
	return s == "device" || s == "unauthorized" || s == "offline" || s == "bootloader" || s == "host"
		|| s == "recovery" || s == "rescue" || s == "sideload" || s == "authorizing" || s == "connecting"
		|| s == "detached";
}

static DeviceState stateFrom(const std::string &s)
{
	if (s == "device")
		return DeviceState::Ok;
	if (s == "unauthorized")
		return DeviceState::Unauthorized;
	if (s == "offline")
		return DeviceState::Offline;
	return DeviceState::Other;
}

std::vector<Device> parseDevicesOutput(const std::string &out)
{
	std::vector<Device> devices;
	size_t pos = 0;
	while (pos <= out.size()) {
		size_t eol = out.find('\n', pos);
		std::string line = out.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
		trimInPlace(line);
		pos = eol == std::string::npos ? out.size() + 1 : eol + 1;

		if (line.empty() || line[0] == '*' || line.rfind("List of devices", 0) == 0
			|| line.rfind("adb server", 0) == 0)
			continue;

		// Line: "<serial><sep><state> [key:value ...]" where <sep> is a run of
		// spaces and/or tabs (adb switched from a single tab to space padding
		// in recent versions).
		size_t sep = line.find_first_of(" \t");
		if (sep == std::string::npos)
			continue;

		Device dev;
		dev.serial = line.substr(0, sep);
		size_t restStart = line.find_first_not_of(" \t", sep);
		if (restStart == std::string::npos)
			continue;
		std::string rest = line.substr(restStart);

		// First whitespace-delimited token of `rest` is the state.
		size_t sp = rest.find(' ');
		dev.stateRaw = rest.substr(0, sp == std::string::npos ? std::string::npos : sp);
		if (!isKnownState(dev.stateRaw))
			continue;
		dev.state = stateFrom(dev.stateRaw);

		// Extract "model:" property
		size_t m = rest.find("model:");
		if (m != std::string::npos) {
			size_t start = m + 6;
			size_t end = rest.find(' ', start);
			dev.model = rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
		}

		devices.push_back(std::move(dev));
	}
	return devices;
}

bool listDevices(const std::string &adb, std::vector<Device> &devices, std::string *error)
{
	std::string out, err;
	int rc = run(adb, {"devices", "-l"}, &out, &err, 15000);
	if (rc != 0) {
		if (error)
			*error = err.empty() ? ("adb exited with status " + std::to_string(rc)) : err;
		return false;
	}
	devices = parseDevicesOutput(out);
	return true;
}

std::string getprop(const std::string &adb, const std::string &serial, const std::string &prop)
{
	std::string out;
	int rc = run(adb, {"-s", serial, "shell", "getprop", prop}, &out, nullptr, 10000);
	if (rc != 0)
		return {};
	// strip \r\n
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
		out.pop_back();
	return out;
}

} // namespace adb
