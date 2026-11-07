# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# AccessKit's C bindings (src/platform/screen-reader). tools/bootstrap_accesskit.py
# fetches the pinned release into local/deps. On macOS it joins the static
# libraries. This file only points at that tree. It does not download.
#
# OA_ACCESSKIT_DIR is that tree. A lane that prepares it somewhere else passes
# -DOA_ACCESSKIT_DIR=. A from-source/<triple>/ library is taken over the
# release's when the file is there.
#
# macOS, iOS and Linux link the static library. Windows never links it:
# accesskit.dll is copied beside a program and loaded at run time, and only
# on Windows 10 or later. A Windows 95 build, a build with OA_SCREEN_READER
# off, and a platform with no known library compile the bridge out and copy
# no DLL. The Visual Studio lane needs only the committed header, so a
# missing DLL does not stop that configuration.
include_guard(GLOBAL)

set(OA_ACCESSKIT_VERSION "0.23.2")
get_filename_component(oa_accesskit_default
  "${CMAKE_CURRENT_LIST_DIR}/../local/deps/accesskit-c-${OA_ACCESSKIT_VERSION}" ABSOLUTE)
set(OA_ACCESSKIT_DIR "${oa_accesskit_default}" CACHE PATH
  "Folder holding the AccessKit ${OA_ACCESSKIT_VERSION} tree tools/bootstrap_accesskit.py prepares")
get_filename_component(OA_ACCESSKIT_INCLUDE
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/accesskit-c" ABSOLUTE)

option(OA_SCREEN_READER "Build the screen-reader bridge" ON)

set(OA_ACCESSKIT_LIBRARY "")
set(OA_ACCESSKIT_DLL "")
set(OA_ACCESSKIT_SYSTEM_LIBS "")
set(OA_SCREEN_READER_COMPILED_OUT OFF)
set(oa_accesskit_status "")
# One string. Two quoted arguments would be a list, and the message would
# print a ';' between them.
set(oa_accesskit_fetch "Run python3 tools/bootstrap_accesskit.py, which")
string(APPEND oa_accesskit_fetch
  " fetches the pinned release into local/deps/accesskit-c-${OA_ACCESSKIT_VERSION}.")

# Stops the configuration when a library the bootstrap should have laid out is
# missing. hint is the command that produces it. The message names
# -DOA_ACCESSKIT_DIR= so a lane can point at its own tree.
function(oa_accesskit_require library_path hint)
  if(NOT EXISTS "${library_path}")
    message(FATAL_ERROR
      "AccessKit ${OA_ACCESSKIT_VERSION} is not at ${library_path}. "
      "${hint} "
      "Point the build at a prepared tree with -DOA_ACCESSKIT_DIR=.")
  endif()
endfunction()

# Prefer from-source/<triple>/<filename> when that file exists. Otherwise use
# the release path. An empty release path keeps the from-source path, which
# the caller then requires.
function(oa_accesskit_pick output_var triple filename release_relative)
  set(from_source "${OA_ACCESSKIT_DIR}/from-source/${triple}/${filename}")
  if(EXISTS "${from_source}")
    set(${output_var} "${from_source}" PARENT_SCOPE)
  elseif(NOT "${release_relative}" STREQUAL "")
    set(${output_var} "${OA_ACCESSKIT_DIR}/${release_relative}" PARENT_SCOPE)
  else()
    set(${output_var} "${from_source}" PARENT_SCOPE)
  endif()
endfunction()

function(oa_accesskit_source_hint output_var triple)
  set(${output_var}
    "Run python3 tools/bootstrap_accesskit.py --from-source --target ${triple}."
    PARENT_SCOPE)
endfunction()

if(NOT OA_SCREEN_READER)
  set(OA_SCREEN_READER_COMPILED_OUT ON)
  set(oa_accesskit_status "left out (OA_SCREEN_READER is off)")
elseif(OA_WINDOWS_95)
  set(OA_SCREEN_READER_COMPILED_OUT ON)
  set(oa_accesskit_status "left out (Windows 95)")
elseif(WIN32)
  # Arm is chosen before the pointer size: an arm64 process is also 8 bytes,
  # and the release has no arm64 DLL. That library is a source build.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64|arm64|AARCH64)$")
    oa_accesskit_pick(OA_ACCESSKIT_DLL "aarch64-pc-windows-gnullvm" "accesskit.dll" "")
    oa_accesskit_source_hint(oa_accesskit_hint "aarch64-pc-windows-gnullvm")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    oa_accesskit_pick(OA_ACCESSKIT_DLL "x86_64-pc-windows-gnu" "accesskit.dll"
      "lib/windows/x86_64/mingw/shared/accesskit.dll")
    set(oa_accesskit_hint "${oa_accesskit_fetch}")
  else()
    oa_accesskit_pick(OA_ACCESSKIT_DLL "i686-pc-windows-gnu" "accesskit.dll"
      "lib/windows/x86/mingw/shared/accesskit.dll")
    set(oa_accesskit_hint "${oa_accesskit_fetch}")
  endif()
  # The mingw packages ship the DLL. The Visual Studio check lane does not.
  if(MINGW)
    oa_accesskit_require("${OA_ACCESSKIT_DLL}" "${oa_accesskit_hint}")
    set(oa_accesskit_status "DLL beside the game (${OA_ACCESSKIT_DLL})")
  elseif(EXISTS "${OA_ACCESSKIT_DLL}")
    set(oa_accesskit_status "DLL beside the game (${OA_ACCESSKIT_DLL})")
  else()
    set(OA_ACCESSKIT_DLL "")
    set(oa_accesskit_status "header only (Visual Studio)")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  if(OA_IOS_SDK STREQUAL "iphoneos" OR CMAKE_OSX_SYSROOT MATCHES "iphoneos")
    oa_accesskit_pick(OA_ACCESSKIT_LIBRARY "aarch64-apple-ios" "libaccesskit.a"
      "lib/ios/arm64/static/libaccesskit.a")
  elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    oa_accesskit_pick(OA_ACCESSKIT_LIBRARY "x86_64-apple-ios" "libaccesskit.a"
      "lib/ios-simulator/x86_64/static/libaccesskit.a")
  else()
    oa_accesskit_pick(OA_ACCESSKIT_LIBRARY "aarch64-apple-ios-sim" "libaccesskit.a"
      "lib/ios-simulator/arm64/static/libaccesskit.a")
  endif()
  oa_accesskit_require("${OA_ACCESSKIT_LIBRARY}" "${oa_accesskit_fetch}")
  list(APPEND OA_ACCESSKIT_SYSTEM_LIBS
    "-framework UIKit" "-framework Foundation" "-framework CoreFoundation" objc)
  set(oa_accesskit_status "static library (${OA_ACCESSKIT_LIBRARY})")
elseif(APPLE)
  # A universal build keeps the joined library. One architecture uses a
  # source build of that architecture when the file is there.
  set(OA_ACCESSKIT_LIBRARY "${OA_ACCESSKIT_DIR}/lib/macos/universal/static/libaccesskit.a")
  set(oa_accesskit_single_arch OFF)
  if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    set(oa_accesskit_single_arch ON)
  elseif(CMAKE_OSX_ARCHITECTURES STREQUAL ""
      AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(oa_accesskit_single_arch ON)
  endif()
  if(oa_accesskit_single_arch)
    set(oa_accesskit_candidate
      "${OA_ACCESSKIT_DIR}/from-source/aarch64-apple-darwin/libaccesskit.a")
    if(EXISTS "${oa_accesskit_candidate}")
      set(OA_ACCESSKIT_LIBRARY "${oa_accesskit_candidate}")
    endif()
  elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
    set(oa_accesskit_candidate
      "${OA_ACCESSKIT_DIR}/from-source/x86_64-apple-darwin/libaccesskit.a")
    if(EXISTS "${oa_accesskit_candidate}")
      set(OA_ACCESSKIT_LIBRARY "${oa_accesskit_candidate}")
    endif()
  endif()
  oa_accesskit_require("${OA_ACCESSKIT_LIBRARY}" "${oa_accesskit_fetch}")
  list(APPEND OA_ACCESSKIT_SYSTEM_LIBS
    "-framework AppKit" "-framework Foundation" "-framework CoreFoundation" objc)
  set(oa_accesskit_status "static library (${OA_ACCESSKIT_LIBRARY})")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(oa_accesskit_known OFF)
  set(oa_accesskit_hint "${oa_accesskit_fetch}")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    oa_accesskit_pick(OA_ACCESSKIT_LIBRARY "aarch64-unknown-linux-gnu" "libaccesskit.a" "")
    oa_accesskit_source_hint(oa_accesskit_hint "aarch64-unknown-linux-gnu")
    set(oa_accesskit_known ON)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7l|armv7)$")
    oa_accesskit_pick(OA_ACCESSKIT_LIBRARY "armv7-unknown-linux-gnueabihf" "libaccesskit.a" "")
    oa_accesskit_source_hint(oa_accesskit_hint "armv7-unknown-linux-gnueabihf")
    set(oa_accesskit_known ON)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    oa_accesskit_pick(OA_ACCESSKIT_LIBRARY "x86_64-unknown-linux-gnu" "libaccesskit.a"
      "lib/linux/x86_64/static/libaccesskit.a")
    set(oa_accesskit_known ON)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i686|i386|x86)$")
    oa_accesskit_pick(OA_ACCESSKIT_LIBRARY "i686-unknown-linux-gnu" "libaccesskit.a"
      "lib/linux/x86/static/libaccesskit.a")
    set(oa_accesskit_known ON)
  endif()
  if(NOT oa_accesskit_known)
    set(OA_SCREEN_READER_COMPILED_OUT ON)
    set(oa_accesskit_status "left out (no AccessKit library for Linux ${CMAKE_SYSTEM_PROCESSOR})")
  else()
    oa_accesskit_require("${OA_ACCESSKIT_LIBRARY}" "${oa_accesskit_hint}")
    find_package(Threads REQUIRED)
    # pthread and dl live in their own libraries before glibc 2.34. Naming
    # them here does not depend on another library pulling them in. The C++
    # driver already links libgcc_s, which has the unwinder the Rust library
    # uses, so this link does not pass -static-libgcc.
    list(APPEND OA_ACCESSKIT_SYSTEM_LIBS Threads::Threads m)
    if(CMAKE_DL_LIBS)
      list(APPEND OA_ACCESSKIT_SYSTEM_LIBS ${CMAKE_DL_LIBS})
    endif()
    set(oa_accesskit_status "static library (${OA_ACCESSKIT_LIBRARY})")
  endif()
else()
  # Android, the BSDs and any other system: the bridge is left out, so the
  # game still links. A later port names its library here.
  set(OA_SCREEN_READER_COMPILED_OUT ON)
  set(oa_accesskit_status "left out (no AccessKit library for ${CMAKE_SYSTEM_NAME})")
endif()

message(STATUS "Screen reader: ${oa_accesskit_status}")

# Copies accesskit.dll beside a Windows program. Skipped when this is not
# Windows, when the bridge is compiled out, and when the DLL is absent (the
# Visual Studio lane). The program loads that copy itself; nothing links it.
function(oa_copy_accesskit_dll target)
  if(NOT WIN32 OR OA_SCREEN_READER_COMPILED_OUT OR NOT OA_ACCESSKIT_DLL)
    return()
  endif()
  if(NOT EXISTS "${OA_ACCESSKIT_DLL}")
    return()
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OA_ACCESSKIT_DLL}" "$<TARGET_FILE_DIR:${target}>/accesskit.dll"
    VERBATIM)
endfunction()
