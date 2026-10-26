# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# oa-update's steps. The executable stays in the root CMakeLists.txt, where
# the Windows manifest, the DLL copy and the packaged notices attach to it.
# The launcher script has one source. The header is generated text, not a
# second copy, and it is rebuilt when the script changes.
file(READ "${PROJECT_SOURCE_DIR}/packaging/linux/open-annihilation-launcher.sh"
  OA_LINUX_LAUNCHER_SCRIPT)
if(OA_LINUX_LAUNCHER_SCRIPT MATCHES "OA_LAUNCHER")
  message(FATAL_ERROR "the launcher script contains the embed delimiter")
endif()
set(_oa_launcher_header "${CMAKE_CURRENT_BINARY_DIR}/generated/linux_launcher.hpp")
set(_oa_launcher_prefix [=[// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <string_view>

namespace oa::update_helper {

inline constexpr std::string_view linux_launcher_text = R"OA_LAUNCHER(]=])
set(_oa_launcher_suffix [=[)OA_LAUNCHER";

}
]=])
file(WRITE "${_oa_launcher_header}"
  "${_oa_launcher_prefix}${OA_LINUX_LAUNCHER_SCRIPT}${_oa_launcher_suffix}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/packaging/linux/open-annihilation-launcher.sh")

add_library(oa-update-core STATIC
  src/journal.cpp
  src/lock.cpp
  src/log.cpp
  src/route_windows.cpp
  src/route_linux.cpp
  src/routes.cpp
  src/run.cpp
  src/start.cpp
  src/unpack.cpp
  src/verify.cpp
  src/wait.cpp
  src/window.cpp)
add_library(oa::update::core ALIAS oa-update-core)
target_include_directories(oa-update-core PUBLIC include)
target_include_directories(oa-update-core PRIVATE ${PROJECT_SOURCE_DIR}/src/app/include)
target_include_directories(oa-update-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(oa-update-core PUBLIC cxx_std_20)
# The root's per-source definitions do not reach a subdirectory target.
# Only a self-check build accepts --feed-key and the plain-folder route.
target_compile_definitions(oa-update-core PUBLIC "OA_SELF_CHECKS_BUILT=$<BOOL:${OA_SELF_CHECKS}>")
target_link_libraries(oa-update-core
  PUBLIC oa-app-update
  PRIVATE oa-data-engine-feed oa-base-sha256 oa-base-signing oa-formats-zip oa-formats-json
    oa-ui-kit oa-ui-paint oa-platform-text-font oa-platform-shims oa-options SDL3::SDL3)
if(APPLE)
  # The new bundle's signature, and Launch Services, which starts it. AppKit
  # is where NSWorkspace lives; Security and CoreFoundation do not provide it.
  target_sources(oa-update-core PRIVATE src/route_macos.mm)
  set_source_files_properties(src/route_macos.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(oa-update-core PRIVATE
    "-framework Security" "-framework CoreFoundation" "-framework AppKit")
endif()

if(BUILD_TESTING)
  add_executable(oa-update-test
    tests/route_windows_test.cpp tests/update_test.cpp tests/route_linux_test.cpp tests/route_macos_test.cpp)
  target_link_libraries(oa-update-test
    PRIVATE oa-update-core oa-app-update oa-data-engine-feed oa-data-catalogue oa-base-signing
      oa-base-sha256 oa-formats-zip oa-formats-url oa-ui-kit oa-test-support oa-options)
  add_test(NAME tools-oa-update COMMAND oa-update-test)
  set_tests_properties(tools-oa-update PROPERTIES TIMEOUT 60)
  if(WIN32)
    add_custom_command(TARGET oa-update-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DDLLS=$<TARGET_RUNTIME_DLLS:oa-update-test>"
        "-DDEST=$<TARGET_FILE_DIR:oa-update-test>" -P "${PROJECT_SOURCE_DIR}/tools/copy_runtime.cmake")
  endif()
endif()
