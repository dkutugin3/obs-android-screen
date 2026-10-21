#include "adb-client.hpp"

#include <chrono>
#include <cstring>
#include <mutex>

#include <util/base.h>

#ifndef _WIN32
extern char **environ;
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace adb {

// ---------------------------------------------------------------------------
// Platform process layer

#ifdef _WIN32

namespace {

std::wstring toWide(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

// Build a Windows command line: every argument containing whitespace is
// double-quoted (backslash quoting rules do not matter for adb arguments).
std::wstring buildCommandLine(const std::string &adb, const std::vector<std::string> &args)
{
	std::string line;
	auto append = [&](const std::string &a) {
		if (!line.empty())
			line += ' ';
		bool quote = a.find_first_of(" \t") != std::string::npos;
		if (quote)
			line += '"';
		line += a;
		if (quote)
			line += '"';
	};
	append(adb);
	for (auto &a : args)
		append(a);
	return toWide(line);
}

struct WinPipes {
	HANDLE outRd = nullptr, outWr = nullptr;
	HANDLE errRd = nullptr, errWr = nullptr;
	~WinPipes()
	{
		for (HANDLE h : {outRd, outWr, errRd, errWr})
			if (h)
				CloseHandle(h);
	}
};

} // namespace

int run(const std::string &adb, const std::vector<std::string> &args, std::string *stdoutOut, std::string *stderrOut,
	long timeoutMs)
{
	if (adb.empty())
		return -1;

	WinPipes pipes;
	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	if (!CreatePipe(&pipes.outRd, &pipes.outWr, &sa, 0) || !CreatePipe(&pipes.errRd, &pipes.errWr, &sa, 0))
		return -1;
	SetHandleInformation(pipes.outRd, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(pipes.errRd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = pipes.outWr;
	si.hStdError = pipes.errWr;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

	PROCESS_INFORMATION pi{};
	std::wstring cmd = buildCommandLine(adb, args);
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		blog(LOG_WARNING, "[obs-android-adb] CreateProcess failed for %s (%lu)", adb.c_str(), GetLastError());
		return -1;
	}
	CloseHandle(pi.hThread);
	// The child owns its ends now.
	CloseHandle(pipes.outWr);
	pipes.outWr = nullptr;
	CloseHandle(pipes.errWr);
	pipes.errWr = nullptr;

	std::string outStr, errStr;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 60000);
	char buf[4096];
	bool outOpen = true, errOpen = true;

	while (outOpen || errOpen) {
		long remainingMs = timeoutMs > 0
			? (long)std::chrono::duration_cast<std::chrono::milliseconds>(
				  deadline - std::chrono::steady_clock::now())
				  .count()
			: 100;
		if (remainingMs < 1)
			remainingMs = 1;
		WaitForSingleObject(pi.hProcess, (DWORD)remainingMs);

		auto drain = [](HANDLE h, bool &open, std::string &out) {
			if (!open)
				return;
			DWORD avail = 0;
			if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
				open = false; // all write ends closed = end of stream
				return;
			}
			if (avail == 0)
				return;
			char b[4096];
			DWORD toRead = avail < sizeof(b) ? avail : (DWORD)sizeof(b);
			DWORD got = 0;
			if (ReadFile(h, b, toRead, &got, nullptr) && got > 0)
				out.append(b, (size_t)got);
			else
				open = false;
		};
		drain(pipes.outRd, outOpen, outStr);
		drain(pipes.errRd, errOpen, errStr);

		if (timeoutMs > 0 && std::chrono::steady_clock::now() >= deadline) {
			TerminateProcess(pi.hProcess, 1);
			break;
		}
	}

	DWORD exitCode = 0;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);

	if (stdoutOut)
		*stdoutOut = std::move(outStr);
	if (stderrOut)
		*stderrOut = std::move(errStr);
	return (int)exitCode;
}

PipeHandle spawn(const std::string &adb, const std::vector<std::string> &args, ProcHandle *procOut, bool *ok,
	PipeHandle *stderrPipe)
{
	if (ok)
		*ok = false;
	if (adb.empty())
		return nullptr;

	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE rd = nullptr, wr = nullptr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		blog(LOG_WARNING, "[obs-android-adb] CreatePipe failed (%lu)", GetLastError());
		return nullptr;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	HANDLE errRd = nullptr, errWr = nullptr;
	if (stderrPipe && CreatePipe(&errRd, &errWr, &sa, 0)) {
		SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);
	} else {
		errRd = nullptr;
		errWr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	}

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = errWr;

	PROCESS_INFORMATION pi{};
	std::wstring cmd = buildCommandLine(adb, args);
	if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
		blog(LOG_WARNING, "[obs-android-adb] CreateProcess failed for %s (%lu)", adb.c_str(), GetLastError());
		CloseHandle(errWr);
		if (errRd)
			CloseHandle(errRd);
		CloseHandle(rd);
		CloseHandle(wr);
		return nullptr;
	}
	CloseHandle(pi.hThread);
	CloseHandle(wr);
	CloseHandle(errWr);
	if (stderrPipe)
		*stderrPipe = errRd;
	else if (errRd)
		CloseHandle(errRd);
	if (procOut)
		*procOut = pi.hProcess;
	if (ok)
		*ok = true;
	return rd;
}

long readPipe(PipeHandle pipe, uint8_t *buf, size_t len)
{
	if (!pipe)
		return -1;
	DWORD got = 0;
	if (!ReadFile((HANDLE)pipe, buf, (DWORD)len, &got, nullptr))
		return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
	return (long)got;
}

void closePipe(PipeHandle pipe)
{
	if (pipe)
		CloseHandle((HANDLE)pipe);
}

void terminateChild(ProcHandle proc)
{
	if (proc)
		TerminateProcess((HANDLE)proc, 1);
}

void reapChild(ProcHandle proc)
{
	if (proc) {
		WaitForSingleObject((HANDLE)proc, 5000);
		CloseHandle((HANDLE)proc);
	}
}

#else // POSIX

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

PipeHandle spawn(const std::string &adb, const std::vector<std::string> &args, ProcHandle *procOut, bool *ok,
	PipeHandle *stderrPipe)
{
	if (ok)
		*ok = false;
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

	int errPipe[2] = {-1, -1};
	bool wantStderr = stderrPipe != nullptr;
	if (wantStderr && pipe(errPipe) != 0)
		wantStderr = false;

	int devnull = open("/dev/null", O_WRONLY);
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
	if (wantStderr)
		posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);
	else
		posix_spawn_file_actions_adddup2(&actions, devnull >= 0 ? devnull : outPipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, outPipe[0]);
	posix_spawn_file_actions_addclose(&actions, outPipe[1]);
	if (wantStderr)
		posix_spawn_file_actions_addclose(&actions, errPipe[0]);

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
	if (wantStderr)
		close(errPipe[1]);

	if (rc != 0) {
		close(outPipe[0]);
		if (wantStderr)
			close(errPipe[0]);
		blog(LOG_WARNING, "[obs-android-adb] posix_spawn failed for %s: %s", adb.c_str(), strerror(rc));
		return -1;
	}

	if (procOut)
		*procOut = pid;
	if (stderrPipe)
		*stderrPipe = wantStderr ? errPipe[0] : -1;
	if (ok)
		*ok = true;
	return outPipe[0];
}

long readPipe(PipeHandle pipe, uint8_t *buf, size_t len)
{
	while (true) {
		ssize_t got = read(pipe, buf, len);
		if (got < 0 && errno == EINTR)
			continue;
		return (long)got;
	}
}

void closePipe(PipeHandle pipe)
{
	close(pipe);
}

void terminateChild(ProcHandle proc)
{
	kill(proc, SIGTERM);
}

void reapChild(ProcHandle proc)
{
	waitpid(proc, nullptr, 0);
}

#endif // process layer

// ---------------------------------------------------------------------------
// adb discovery

#ifdef _WIN32

static bool isExecutableFile(const std::string &path)
{
	if (path.empty())
		return false;
	DWORD attr = GetFileAttributesA(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string findInPath(const char *name)
{
	char buf[MAX_PATH];
	if (SearchPathA(nullptr, name, ".exe", MAX_PATH, buf, nullptr))
		return std::string(buf);
	return {};
}

std::string findAdb(const std::string &overridePath)
{
	if (!overridePath.empty() && isExecutableFile(overridePath))
		return overridePath;

	// Bundled copy (data/adb/adb.exe) — preferred so the plugin works
	// without platform-tools.
	const char *dataPath = getenv("OBS_ANDROID_ADB_BUNDLE_DIR");
	if (dataPath) {
		std::string bundled = std::string(dataPath) + "\\adb.exe";
		if (isExecutableFile(bundled))
			return bundled;
	}

	// Typical Android SDK / Studio location.
	const char *localAppData = getenv("LOCALAPPDATA");
	if (localAppData) {
		std::string sdk = std::string(localAppData) + "\\Android\\Sdk\\platform-tools\\adb.exe";
		if (isExecutableFile(sdk))
			return sdk;
	}
	return findInPath("adb.exe");
}

#else

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

	// Bundled copy (Resources/adb/adb), preferred over system installs so the
	// plugin works without platform-tools. A copy downloaded inside an
	// archive may carry a quarantine xattr, which makes macOS kill the
	// process on exec — drop it so Gatekeeper never sees the file.
	const char *dataPath = getenv("OBS_ANDROID_ADB_BUNDLE_DIR");
	if (dataPath) {
		std::string bundled = std::string(dataPath) + "/adb";
		if (isExecutableFile(bundled)) {
			removexattr(bundled.c_str(), "com.apple.quarantine", 0);
			return bundled;
		}
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

#endif

// ---------------------------------------------------------------------------
// Device list parsing (pure string handling — shared)

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
