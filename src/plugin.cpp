#include <obs-module.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "android-source.hpp"
#include "device-monitor.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-android-adb", "en-US")

#define LOGP "[obs-android-adb] "

// obs_module_text() is only usable in this translation unit (the module is
// declared here); expose it for the rest of the plugin.
const char *aadText(const char *lookup)
{
	return obs_module_text(lookup);
}

static const char *androidScreenGetName(void *typeData)
{
	UNUSED_PARAMETER(typeData);
	return "Android Screen (ADB)";
}

static void *androidScreenCreate(obs_data_t *settings, obs_source_t *source)
{
	return new AndroidSource(source, settings);
}

static void androidScreenDestroy(void *data)
{
	delete static_cast<AndroidSource *>(data);
}

static void androidScreenUpdate(void *data, obs_data_t *settings)
{
	static_cast<AndroidSource *>(data)->update(settings);
}

static void androidScreenActivate(void *data)
{
	static_cast<AndroidSource *>(data)->activate();
}

static void androidScreenDeactivate(void *data)
{
	static_cast<AndroidSource *>(data)->deactivate();
}

static void androidScreenShow(void *data)
{
	static_cast<AndroidSource *>(data)->show();
}

static void androidScreenHide(void *data)
{
	static_cast<AndroidSource *>(data)->hide();
}

static obs_properties_t *androidScreenProperties(void *data)
{
	return static_cast<AndroidSource *>(data)->properties();
}

static uint32_t androidScreenWidth(void *data)
{
	return static_cast<AndroidSource *>(data)->width();
}

static uint32_t androidScreenHeight(void *data)
{
	return static_cast<AndroidSource *>(data)->height();
}

static void androidScreenDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "serial", "");
	obs_data_set_default_string(settings, "video_codec", "h264");
	obs_data_set_default_string(settings, "resolution", "1920x1080");
	obs_data_set_default_int(settings, "fps", 60);
	obs_data_set_default_int(settings, "bitrate_mbps", 20);
	obs_data_set_default_bool(settings, "audio", true);
	obs_data_set_default_bool(settings, "screen_off", false);
	obs_data_set_default_bool(settings, "stay_awake", false);
	obs_data_set_default_bool(settings, "auto_reconnect", true);
	obs_data_set_default_string(settings, "adb_path", "");
	obs_data_set_default_string(settings, "status", "");
	// Static caption in the properties pane (not a user setting).
	obs_data_set_default_string(settings, "version_info",
		"obs-android-adb v" PLUGIN_VERSION " · scrcpy protocol v" SCRCPY_SERVER_VERSION);
}

static struct obs_source_info androidScreenSourceInfo = {
	.id = "android_adb_screen",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = androidScreenGetName,
	.create = androidScreenCreate,
	.destroy = androidScreenDestroy,
	.get_width = androidScreenWidth,
	.get_height = androidScreenHeight,
	.get_defaults = androidScreenDefaults,
	.get_properties = androidScreenProperties,
	.update = androidScreenUpdate,
	.activate = androidScreenActivate,
	.deactivate = androidScreenDeactivate,
	.show = androidScreenShow,
	.hide = androidScreenHide,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};

bool obs_module_load(void)
{
	// Resolve the bundled scrcpy-server path once; the sources pick it up
	// through the environment.
	char *serverPath = obs_module_file(SCRCPY_SERVER_FILE);
	if (serverPath) {
#ifdef _WIN32
		_putenv_s("OBS_ANDROID_ADB_SERVER_JAR", serverPath);
#else
		setenv("OBS_ANDROID_ADB_SERVER_JAR", serverPath, 0 /* do not overwrite */);
#endif
		blog(LOG_INFO, LOGP "scrcpy-server: %s", serverPath);
		bfree(serverPath);
	} else {
		blog(LOG_WARNING, LOGP "scrcpy-server not found in plugin data");
	}

	// Bundled adb (built with BUNDLE_ADB=ON): Resources/adb/adb. Makes the
	// plugin work out of the box without platform-tools installed.
	char *adbDir = obs_module_file("adb");
	if (adbDir) {
#ifdef _WIN32
		_putenv_s("OBS_ANDROID_ADB_BUNDLE_DIR", adbDir);
#else
		setenv("OBS_ANDROID_ADB_BUNDLE_DIR", adbDir, 0 /* do not overwrite */);
#endif
		bfree(adbDir);
	}

	obs_register_source(&androidScreenSourceInfo);
	blog(LOG_INFO, LOGP "plugin v" PLUGIN_VERSION " loaded (scrcpy protocol v" SCRCPY_SERVER_VERSION ")");
	return true;
}

void obs_module_unload(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
	blog(LOG_INFO, LOGP "plugin unloaded");
}

const char *obs_module_description(void)
{
	return "Mirror an Android screen (and system audio) into OBS over USB via ADB "
	       "using the scrcpy protocol";
}
