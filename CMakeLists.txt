cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

project(obs-android-adb
  VERSION 1.2.1
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
# submodule is not part of the obs-studio release tarball, so fetch it too.
set(_simde_root "${_deps_dir}/simde-include")
if(NOT EXISTS "${_simde_root}/simde/x86/sse2.h")
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
if(NOT EXISTS "${_simde_root}/simde/x86/sse2.h")
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
set(ANDROID_PLATFORM_TOOLS_URL "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip"
  CACHE STRING "platform-tools zip to take the bundled adb from")
option(BUNDLE_ADB "Bundle the adb binary inside the plugin" ON)
set(_adb_src "${_deps_dir}/platform-tools/adb")
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
# Plugin target
# ---------------------------------------------------------------------------
add_library(${BUNDLE_NAME} MODULE
  src/plugin.cpp
  src/android-source.cpp
  src/device-monitor.cpp
  src/placeholder.cpp
  src/adb/adb-client.cpp
  src/scrcpy/server.cpp
  src/scrcpy/protocol.cpp
  src/video/decoder.cpp
)

set_target_properties(${BUNDLE_NAME} PROPERTIES
  OUTPUT_NAME ${BUNDLE_NAME}
  PREFIX ""
  SUFFIX ".so"
  MACOSX_RPATH FALSE)

target_include_directories(${BUNDLE_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${_obs_root}/libobs"
  "${_simde_root}"
  "${_generated_dir}")

target_compile_definitions(${BUNDLE_NAME} PRIVATE
  PLUGIN_VERSION="${PROJECT_VERSION}"
  SCRCPY_SERVER_VERSION="${SCRCPY_SERVER_VERSION}"
  SCRCPY_SERVER_FILE="${SCRCPY_SERVER_FILE}")

target_compile_options(${BUNDLE_NAME} PRIVATE
  -Wall -Wextra
  -Wno-unused-parameter
  $<$<CONFIG:Release>:-O2>)

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
    src/video/decoder.cpp)
  target_include_directories(obs-android-adb-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${_obs_root}/libobs"
    "${_simde_root}"
    "${_generated_dir}")
  target_compile_options(obs-android-adb-tests PRIVATE -Wno-unused-parameter)
  target_link_libraries(obs-android-adb-tests PRIVATE ${CF} ${CM} ${CV} ${VT} ${CT} ${CG})
endif()

# ---------------------------------------------------------------------------
# Assemble the macOS plugin bundle
# ---------------------------------------------------------------------------
configure_file(cmake/Info.plist.in "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)

set(_bundle_adb_commands)
if(BUNDLE_ADB)
  list(APPEND _bundle_adb_commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_adb_src}" "${BUNDLE_CONTENTS}/Resources/adb/adb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/adb/NOTICE.txt"
      "${BUNDLE_CONTENTS}/Resources/adb/NOTICE.txt")
endif()

# A dedicated always-running target so data-file changes are picked up even
# when the module itself does not need relinking.
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
