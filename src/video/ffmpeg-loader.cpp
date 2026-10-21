// Runtime loading of the bundled FFmpeg DLLs (Windows).
//
// The plugin DLL must not import avcodec/avutil at load time: Windows does
// not search the plugin's own directory for dependencies (only the OBS
// executable's directory and the system paths), so import-time dependencies
// would make obs-android-adb.dll fail to load. Instead the symbols are
// resolved here, on first use, from the bundled DLLs' absolute paths.

#include "ffmpeg-loader.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include "ffmpeg-syms.hpp"

namespace ff {

AVPacket *(*av_packet_alloc_p)(void);
void (*av_packet_free_p)(AVPacket **);
AVFrame *(*av_frame_alloc_p)(void);
void (*av_frame_free_p)(AVFrame **);
void (*av_frame_unref_p)(AVFrame *);
void (*av_log_set_level_p)(int);
int (*av_strerror_p)(int, char *, size_t);
const AVPixFmtDescriptor *(*av_pix_fmt_desc_get_p)(enum AVPixelFormat);
void *(*av_memdup_p)(const void *, size_t);
const AVCodec *(*avcodec_find_decoder_p)(enum AVCodecID);
AVCodecContext *(*avcodec_alloc_context3_p)(const AVCodec *);
int (*avcodec_open2_p)(AVCodecContext *, const AVCodec *, AVDictionary **);
void (*avcodec_free_context_p)(AVCodecContext **);
int (*avcodec_send_packet_p)(AVCodecContext *, const AVPacket *);
int (*avcodec_receive_frame_p)(AVCodecContext *, AVFrame *);
void (*avcodec_flush_buffers_p)(AVCodecContext *);

static std::wstring toWide(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

static HMODULE tryLoad(const std::string &dir, const wchar_t *file)
{
	std::wstring path = toWide(dir);
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	path += file;
	// LOAD_WITH_ALTERED_SEARCH_PATH makes the DLL's own directory the first
	// search location for ITS dependencies (avcodec -> avutil/swresample).
	return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool load(std::string *err)
{
	static std::once_flag once;
	static bool ok = false;
	static std::string reason;

	std::call_once(once, [] {
		// Search order: explicit env override, the plugin data dir (set by
		// plugin.cpp via obs_module_file), the "ffmpeg" subfolder next to
		// this module (plugin DLL or test executable), the module's own
		// folder, and finally the default search path (OBS dir / system).
		std::vector<std::string> dirs;
		if (const char *env = getenv("OBS_ANDROID_FFMPEG_DIR"))
			dirs.push_back(env);
		if (const char *env = getenv("OBS_ANDROID_ADB_BUNDLE_DIR"))
			dirs.push_back(std::string(env) + "\\ffmpeg");
		{
			HMODULE self = nullptr;
			if (GetModuleHandleExW(
				    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				    (LPCWSTR)&load, &self)) {
				wchar_t buf[MAX_PATH] = {};
				if (GetModuleFileNameW(self, buf, MAX_PATH)) {
					std::string m(buf, buf + wcslen(buf));
					size_t slash = m.find_last_of("\\/");
					if (slash != std::string::npos) {
						std::string d = m.substr(0, slash);
						dirs.push_back(d + "\\ffmpeg");
						dirs.push_back(d);
					}
				}
			}
		}

		HMODULE avutil = nullptr, avcodec = nullptr;
		for (auto &d : dirs)
			if ((avutil = tryLoad(d, L"avutil-60.dll")) != nullptr)
				break;
		if (!avutil)
			avutil = LoadLibraryW(L"avutil-60.dll");
		if (!avutil) {
			reason = "avutil-60.dll not found";
			return;
		}
		for (auto &d : dirs)
			if ((avcodec = tryLoad(d, L"avcodec-62.dll")) != nullptr)
				break;
		if (!avcodec)
			avcodec = LoadLibraryW(L"avcodec-62.dll");
		if (!avcodec) {
			reason = "avcodec-62.dll not found";
			return;
		}

		bool missing = false;
		auto sym = [&](HMODULE m, const char *name, void **out) {
			*out = (void *)GetProcAddress(m, name);
			if (!*out) {
				reason = std::string("missing symbol ") + name;
				missing = true;
			}
		};
#define FF_RESOLVE(m, fn) sym(m, #fn, (void **)&fn##_p)
		// FFmpeg 8 moved the AVPacket API from libavutil to libavcodec.
		FF_RESOLVE(avcodec, av_packet_alloc);
		FF_RESOLVE(avcodec, av_packet_free);
		FF_RESOLVE(avutil, av_frame_alloc);
		FF_RESOLVE(avutil, av_frame_free);
		FF_RESOLVE(avutil, av_frame_unref);
		FF_RESOLVE(avutil, av_log_set_level);
		FF_RESOLVE(avutil, av_strerror);
		FF_RESOLVE(avutil, av_pix_fmt_desc_get);
		FF_RESOLVE(avutil, av_memdup);
		FF_RESOLVE(avcodec, avcodec_find_decoder);
		FF_RESOLVE(avcodec, avcodec_alloc_context3);
		FF_RESOLVE(avcodec, avcodec_open2);
		FF_RESOLVE(avcodec, avcodec_free_context);
		FF_RESOLVE(avcodec, avcodec_send_packet);
		FF_RESOLVE(avcodec, avcodec_receive_frame);
		FF_RESOLVE(avcodec, avcodec_flush_buffers);
#undef FF_RESOLVE
		ok = !missing;
	});
	if (err)
		*err = reason;
	return ok;
}

} // namespace ff

#else // non-Windows: libavcodec is linked normally

namespace ff {
bool load(std::string *)
{
	return true;
}
} // namespace ff

#endif
