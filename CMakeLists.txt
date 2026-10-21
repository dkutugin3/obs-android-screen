cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

project(obs-android-adb
  VERSION 2.0.0
  DESCRIPTION "Android Screen (ADB) source plugin for OBS Studio — scrcpy protocol over USB"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
set(SCRCPY_SERVER_VERSION "4.1" CACHE STRING "Bundled scrcpy-server version")
set(OBS_STUDIO_HEADERS_VERSION "32.2.2" CACHE STRING "obs-studio version used for headers")
set(OBS_PLUGIN_INSTALL_DIR "$ENV{HOME}/Library/Application Support/obs-studio/plugins"
  CACHE PATH "Directory OBS loads user plugins from")

set(BUNDLE_NAME "obs-android-adb")
set(BUNDLE_ID "com.obs-android-adb")
set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/${BUNDLE_NAME}.plugin")
set(BUNDLE_CONTENTS "${BUNDLE_DIR}/Contents")

# ---------------------------------------------------------------------------
# libobs headers: download the obs-studio source tarball and use its libobs
# headers. The plugin itself is linked with -undefined dynamic_lookup, so no
# libobs binary is needed at build time; symbols resolve from OBS.app at load.
# ---------------------------------------------------------------------------
set(_deps_dir "${CMAKE_BINARY_DIR}/deps")
set(_obs_root "${_deps_dir}/obs-studio-${OBS_STUDIO_HEADERS_VERSION}")
set(_obs_tar "${_deps_dir}/obs-studio-${OBS_STUDIO_HEADERS_VERSION}.tar.gz")
if(NOT EXISTS "${_obs_root}/libobs/obs.h")
  if(NOT EXISTS "${_obs_tar}")
    message(STATUS "Downloading obs-studio ${OBS_STUDIO_HEADERS_VERSION} sources (for libobs headers)")
    file(DOWNLOAD
      "https://github.com/obsproject/obs-studio/archive/refs/tags/${OBS_STUDIO_HEADERS_VERSION}.tar.gz"
      "${_obs_tar}"
      SHOW_PROGRESS
      STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
      list(GET _dl_status 1 _dl_err)
      file(REMOVE "${_obs_tar}")
      message(FATAL_ERROR "Failed to download obs-studio sources: ${_dl_err}")
    endif()
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_obs_tar}" DESTINATION "${_deps_dir}")
endif()

if(NOT EXISTS "${_obs_root}/libobs/obs.h")
  message(FATAL_ERROR "libobs headers not found in ${_obs_root}")
endif()

# libobs headers include <simde/x86/sse2.h> on non-x86 platforms; the simde
# submodule is not part of the obs-studio release tarball, so fetch it there.
set(_simde_root "${_deps_dir}/simde-include")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  set(_simde_root "") # not needed
endif()
if(_simde_root AND NOT EXISTS "${_simde_root}/simde/x86/sse2.h")
  set(_simde_tar "${_deps_dir}/simde.tar.xz")
  if(NOT EXISTS "${_simde_tar}")
    message(STATUS "Downloading simde headers (required by libobs headers on arm64)")
    file(DOWNLOAD
      "https://github.com/simd-everywhere/simde/releases/download/v0.8.2/simde-amalgamated-0.8.2.tar.xz"
      "${_simde_tar}"
      SHOW_PROGRESS
      STATUS _simde_status)
    list(GET _simde_status 0 _simde_code)
    if(NOT _simde_code EQUAL 0)
      list(GET _simde_status 1 _simde_err)
      file(REMOVE "${_simde_tar}")
      message(FATAL_ERROR "Failed to download simde: ${_simde_err}")
    endif()
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_simde_tar}" DESTINATION "${_deps_dir}")
  # the archive extracts the simde directory CONTENTS into a versioned dir
  file(GLOB _simde_dirs "${_deps_dir}/simde-amalgamated-*")
  if(_simde_dirs)
    list(GET _simde_dirs 0 _simde_dir)
    file(MAKE_DIRECTORY "${_simde_root}")
    file(RENAME "${_simde_dir}" "${_simde_root}/simde")
  endif()
endif()
if(_simde_root AND NOT EXISTS "${_simde_root}/simde/x86/sse2.h")
  message(FATAL_ERROR "simde headers not found in ${_simde_root}")
endif()

# obs-config.h includes "obsconfig.h", generated upstream from obsconfig.h.in
# by the obs-studio build. Generate an equivalent one in the build tree.
set(_generated_dir "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated_dir}")
set(OBS_DATA_PATH "/")
set(OBS_PLUGIN_PATH "/")
set(OBS_PLUGIN_DESTINATION "/")
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
configure_file("${_obs_root}/libobs/obsconfig.h.in" "${_generated_dir}/obsconfig.h" @ONLY)

# ---------------------------------------------------------------------------
# scrcpy-server binary bundled with the plugin
# ---------------------------------------------------------------------------
set(SCRCPY_SERVER_FILE "scrcpy-server-${SCRCPY_SERVER_VERSION}")
set(SCRCPY_SERVER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/scrcpy-server/${SCRCPY_SERVER_FILE}")
if(NOT EXISTS "${SCRCPY_SERVER_PATH}")
  message(FATAL_ERROR
    "scrcpy-server not found: ${SCRCPY_SERVER_PATH}\n"
    "Run: ./scripts/fetch-scrcpy-server.sh")
endif()

# ---------------------------------------------------------------------------
# Bundled adb (platform-tools): makes the plugin dependency-free for end
# users. Google ships adb as a universal (x86_64+arm64) Mach-O binary under
# Apache-2.0. Set BUNDLE_ADB=OFF to rely on a system-installed adb instead.
# ---------------------------------------------------------------------------
if(WIN32)
  set(_default_pt_url "https://dl.google.com/android/repository/platform-tools-latest-windows.zip")
else()
  set(_default_pt_url "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip")
endif()
set(ANDROID_PLATFORM_TOOLS_URL "${_default_pt_url}"
  CACHE STRING "platform-tools zip to take the bundled adb from")
option(BUNDLE_ADB "Bundle the adb binary inside the plugin" ON)
if(WIN32)
  set(_adb_src "${_deps_dir}/platform-tools/adb.exe")
else()
  set(_adb_src "${_deps_dir}/platform-tools/adb")
endif()
if(BUNDLE_ADB AND NOT EXISTS "${_adb_src}")
  set(_adb_zip "${_deps_dir}/platform-tools.zip")
  if(NOT EXISTS "${_adb_zip}")
    message(STATUS "Downloading Android platform-tools (bundled adb)")
    file(DOWNLOAD "${ANDROID_PLATFORM_TOOLS_URL}" "${_adb_zip}" SHOW_PROGRESS STATUS _adb_status)
    list(GET _adb_status 0 _adb_code)
    if(NOT _adb_code EQUAL 0)
      list(GET _adb_status 1 _adb_err)
      file(REMOVE "${_adb_zip}")
      message(FATAL_ERROR "Failed to download platform-tools: ${_adb_err}")
    endif()
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_adb_zip}" DESTINATION "${_deps_dir}")
endif()
if(BUNDLE_ADB AND NOT EXISTS "${_adb_src}")
  message(FATAL_ERROR "adb not found after extracting platform-tools")
endif()

# ---------------------------------------------------------------------------
# Windows: libavcodec (LGPL shared build) for software H.264/HEVC decode
# ---------------------------------------------------------------------------
set(FFMPEG_WIN_URL "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-10-20-13-08/ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-shared-8.1.zip"
  CACHE STRING "FFmpeg win64 LGPL shared build used for the Windows decoder")
set(_ff_root "")
if(WIN32)
  file(GLOB _ff_dir_hint "${_deps_dir}/ffmpeg-*-win64-lgpl-shared*")
  if(_ff_dir_hint)
    list(GET _ff_dir_hint 0 _ff_root)
  endif()
  if(NOT EXISTS "${_ff_root}/include/libavcodec/avcodec.h")
    set(_ff_zip "${_deps_dir}/ffmpeg-win64-lgpl-shared.zip")
    if(NOT EXISTS "${_ff_zip}")
      message(STATUS "Downloading FFmpeg (win64 LGPL shared)")
      file(DOWNLOAD "${FFMPEG_WIN_URL}" "${_ff_zip}" SHOW_PROGRESS STATUS _ff_status)
      list(GET _ff_status 0 _ff_code)
      if(NOT _ff_code EQUAL 0)
        list(GET _ff_status 1 _ff_err)
        file(REMOVE "${_ff_zip}")
        message(FATAL_ERROR "Failed to download FFmpeg: ${_ff_err}")
      endif()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_ff_zip}" DESTINATION "${_deps_dir}")
    file(GLOB _ff_dir_hint "${_deps_dir}/ffmpeg-*-win64-lgpl-shared*")
    list(GET _ff_dir_hint 0 _ff_root)
  endif()
  if(NOT EXISTS "${_ff_root}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg headers not found in ${_ff_root}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
set(_decoder_src src/video/decoder-vt.cpp)
if(WIN32)
  set(_decoder_src src/video/decoder-ffmpeg.cpp)
endif()

add_library(${BUNDLE_NAME} MODULE
  src/plugin.cpp
  src/android-source.cpp
  src/device-monitor.cpp
  src/placeholder.cpp
  src/adb/adb-client.cpp
  src/scrcpy/server.cpp
  src/scrcpy/protocol.cpp
  ${_decoder_src}
)

set_target_properties(${BUNDLE_NAME} PROPERTIES
  OUTPUT_NAME ${BUNDLE_NAME}
  PREFIX ""
  SUFFIX ".so"
  MACOSX_RPATH FALSE)

target_include_directories(${BUNDLE_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_obs_root}/libobs"
  "${_generated_dir}")
if(_simde_root)
  target_include_directories(${BUNDLE_NAME} PRIVATE "${_simde_root}")
endif()
if(WIN32)
  target_include_directories(${BUNDLE_NAME} PRIVATE "${_ff_root}/include")
endif()

target_compile_definitions(${BUNDLE_NAME} PRIVATE
  PLUGIN_VERSION="${PROJECT_VERSION}"
  SCRCPY_SERVER_VERSION="${SCRCPY_SERVER_VERSION}"
  SCRCPY_SERVER_FILE="${SCRCPY_SERVER_FILE}")

if(MSVC)
  target_compile_options(${BUNDLE_NAME} PRIVATE /W4 /permissive-)
else()
  target_compile_options(${BUNDLE_NAME} PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    $<$<CONFIG:Release>:-O2>)
  if(MINGW)
    # Bundle the MinGW runtime DLLs next to the plugin (found in the
    # toolchain's bin directory).
    get_filename_component(_cxx_real "${CMAKE_CXX_COMPILER}" REALPATH)
    get_filename_component(_cxx_dir "${_cxx_real}" DIRECTORY)
    get_filename_component(_mingw_root "${_cxx_dir}/.." ABSOLUTE)
    file(GLOB _mingw_rt_dlls
      "${_cxx_dir}/libgcc_s_seh-1.dll"
      "${_cxx_dir}/libstdc++-6.dll"
      "${_cxx_dir}/libwinpthread-1.dll"
      "${_mingw_root}/x86_64-w64-mingw32/bin/libgcc_s_seh-1.dll"
      "${_mingw_root}/x86_64-w64-mingw32/bin/libstdc++-6.dll"
      "${_mingw_root}/x86_64-w64-mingw32/bin/libwinpthread-1.dll"
      "${_mingw_root}/x86_64-w64-mingw32/lib/libgcc_s_seh-1.dll"
      "${_mingw_root}/x86_64-w64-mingw32/lib/libstdc++-6.dll"
      "${_mingw_root}/x86_64-w64-mingw32/lib/libwinpthread-1.dll")
    set(MINGW_RUNTIME_DLLS ${_mingw_rt_dlls})
    message(STATUS "mingw runtime dlls: ${MINGW_RUNTIME_DLLS}")
  endif()
endif()

if(WIN32)
  # libobs import libraries. On Windows the OBS API is split across
  # obs.dll (obs_* API) and obs64.exe (blog/bmalloc/base — the process
  # executable re-exports them); plugins import from both.
  # See third_party/libobs-win/README.md.
  set(_libobs_lib "${CMAKE_BINARY_DIR}/libobs.lib")
  set(_obsbase_lib "${CMAKE_BINARY_DIR}/obsbase.lib")
  if(NOT EXISTS "${_libobs_lib}" OR NOT EXISTS "${_obsbase_lib}")
    if(MINGW)
      find_program(DLLTOOL x86_64-w64-mingw32-dlltool dlltool REQUIRED)
      execute_process(COMMAND ${DLLTOOL} -d "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libobs-win/libobs.def"
        -l "${_libobs_lib}" -m i386:x86-64 RESULT_VARIABLE _rc1)
      execute_process(COMMAND ${DLLTOOL} -d "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libobs-win/obs64.def"
        -l "${_obsbase_lib}" -m i386:x86-64 RESULT_VARIABLE _rc2)
    else()
      find_program(LIB_TOOL lib REQUIRED)
      execute_process(COMMAND ${LIB_TOOL} /machine:x64 /def:"${CMAKE_CURRENT_SOURCE_DIR}/third_party/libobs-win/libobs.def"
        /out:"${_libobs_lib}" RESULT_VARIABLE _rc1)
      execute_process(COMMAND ${LIB_TOOL} /machine:x64 /def:"${CMAKE_CURRENT_SOURCE_DIR}/third_party/libobs-win/obs64.def"
        /out:"${_obsbase_lib}" RESULT_VARIABLE _rc2)
    endif()
    if(NOT _rc1 EQUAL 0 OR NOT _rc2 EQUAL 0)
      message(FATAL_ERROR "could not generate the libobs import libraries from the .def files")
    endif()
  endif()
  target_link_libraries(${BUNDLE_NAME} PRIVATE
    "${_ff_root}/lib/avcodec.lib"
    "${_ff_root}/lib/avutil.lib"
    "${_libobs_lib}"
    "${_obsbase_lib}"
    ws2_32 gdi32)
else()
  # libobs symbols resolve from OBS.app when the plugin is loaded
  target_link_options(${BUNDLE_NAME} PRIVATE
    "-Wl,-undefined,dynamic_lookup")

  find_library(CF CoreFoundation REQUIRED)
  find_library(CM CoreMedia REQUIRED)
  find_library(CV CoreVideo REQUIRED)
  find_library(VT VideoToolbox REQUIRED)
  find_library(CT CoreText REQUIRED)
  find_library(CG CoreGraphics REQUIRED)
  target_link_libraries(${BUNDLE_NAME} PRIVATE ${CF} ${CM} ${CV} ${VT} ${CT} ${CG})
endif()

# ---------------------------------------------------------------------------
# Offline test harness (no device required)
# ---------------------------------------------------------------------------
option(ENABLE_TESTS "Build the offline test harness" ON)
if(ENABLE_TESTS)
  add_executable(obs-android-adb-tests
    tests/test-main.cpp
    src/adb/adb-client.cpp
    src/scrcpy/protocol.cpp
    src/placeholder.cpp
    ${_decoder_src})
  target_include_directories(obs-android-adb-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${_obs_root}/libobs"
    "${_generated_dir}")
  if(_simde_root)
    target_include_directories(obs-android-adb-tests PRIVATE "${_simde_root}")
  endif()
  if(WIN32)
    target_include_directories(obs-android-adb-tests PRIVATE "${_ff_root}/include")
    target_link_libraries(obs-android-adb-tests PRIVATE
      "${_ff_root}/lib/avcodec.lib" "${_ff_root}/lib/avutil.lib" ws2_32 gdi32)
    if(MINGW)
      # Let the test executable run from the build tree: its DLL
      # dependencies must sit next to it.
      set(_tests_deps ${MINGW_RUNTIME_DLLS}
        "${_ff_root}/bin/avcodec-62.dll"
        "${_ff_root}/bin/avutil-60.dll"
        "${_ff_root}/bin/swresample-6.dll")
      add_custom_command(TARGET obs-android-adb-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_tests_deps} $<TARGET_FILE_DIR:obs-android-adb-tests>)
    endif()
  else()
    target_compile_options(obs-android-adb-tests PRIVATE -Wno-unused-parameter)
    target_link_libraries(obs-android-adb-tests PRIVATE ${CF} ${CM} ${CV} ${VT} ${CT} ${CG})
  endif()
endif()

# ---------------------------------------------------------------------------
# Assemble the plugin bundle / distribution layout
# ---------------------------------------------------------------------------
configure_file(cmake/Info.plist.in "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)

set(_bundle_adb_commands)
if(BUNDLE_ADB AND NOT WIN32)
  list(APPEND _bundle_adb_commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_adb_src}" "${BUNDLE_CONTENTS}/Resources/adb/adb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/adb/NOTICE.txt"
      "${BUNDLE_CONTENTS}/Resources/adb/NOTICE.txt")
endif()

if(WIN32)
  # OBS Windows layout: <obs>/obs-plugins/64bit/<plugin>.dll + data/obs-plugins/<plugin>/...
  set(_win_root "${CMAKE_BINARY_DIR}/win-dist")
  set(_win_dll_dir "${_win_root}/obs-plugins/64bit")
  set(_win_data_dir "${_win_root}/data/obs-plugins/obs-android-adb")
  foreach(_rt ${MINGW_RUNTIME_DLLS})
    list(APPEND _mingw_rt_copy COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_rt}" "${_win_dll_dir}")
  endforeach()
  add_custom_target(obs-android-adb-bundle
    ALL
    DEPENDS ${BUNDLE_NAME}
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_win_root}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_win_dll_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${BUNDLE_NAME}>" "${_win_dll_dir}/${BUNDLE_NAME}.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ff_root}/bin/avcodec-62.dll" "${_win_dll_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ff_root}/bin/avutil-60.dll" "${_win_dll_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ff_root}/bin/swresample-6.dll" "${_win_dll_dir}"
    ${_mingw_rt_copy}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_win_data_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCRCPY_SERVER_PATH}"
      "${_win_data_dir}/${SCRCPY_SERVER_FILE}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/locale"
      "${_win_data_dir}/locale"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/adb/NOTICE.txt"
      "${_win_data_dir}/adb-NOTICE.txt"
    VERBATIM)
  # data-dir-relative adb bundle for obs_module_file("adb")
  set(_win_adb_dir "${_win_data_dir}/adb")
  add_custom_command(TARGET obs-android-adb-bundle POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_win_adb_dir}")
  if(BUNDLE_ADB)
    add_custom_command(TARGET obs-android-adb-bundle POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_adb_src}" "${_win_adb_dir}/adb.exe"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_deps_dir}/platform-tools/AdbWinApi.dll" "${_win_adb_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_deps_dir}/platform-tools/AdbWinUsbApi.dll" "${_win_adb_dir}")
  endif()
else()
  # macOS bundle
  add_custom_target(obs-android-adb-bundle
    ALL
    DEPENDS ${BUNDLE_NAME}
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_CONTENTS}/MacOS"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_CONTENTS}/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${BUNDLE_NAME}>" "${BUNDLE_CONTENTS}/MacOS/${BUNDLE_NAME}.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/Info.plist" "${BUNDLE_CONTENTS}/Info.plist"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SCRCPY_SERVER_PATH}" "${BUNDLE_CONTENTS}/Resources/${SCRCPY_SERVER_FILE}"
    ${_bundle_adb_commands}
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data" "${BUNDLE_CONTENTS}/Resources"
    COMMAND codesign --force --sign - --timestamp=none "${BUNDLE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "Bundle: ${BUNDLE_DIR}"
    VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Install into the OBS user plugins directory
# ---------------------------------------------------------------------------
install(CODE "
  set(DEST \"${OBS_PLUGIN_INSTALL_DIR}\")
  message(STATUS \"Installing ${BUNDLE_NAME}.plugin to \${DEST}\")
  file(MAKE_DIRECTORY \"\${DEST}\")
  file(COPY \"${BUNDLE_DIR}\" DESTINATION \"\${DEST}\")
  execute_process(COMMAND codesign --force --sign - --timestamp=none \"\${DEST}/${BUNDLE_NAME}.plugin\")
")
