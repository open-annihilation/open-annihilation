# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The toolchain of the iOS and iPadOS build (platforms/ios/CMakeLists.txt):
# arm64 code for iOS 15.0 and later, for the system image OA_IOS_SDK names,
# iphonesimulator (the simulator on this Mac, the default) or iphoneos
# (devices). Packages are found in the prefixes
# platforms/ios/tools/bootstrap_ios_deps.py builds, local/deps/ios-<sdk>/sdl
# local/deps/ios-<sdk>/freetype and local/deps/ios-<sdk>/harfbuzz, unless
# OA_IOS_DEPS_DIR names another root. CMake's iOS support finds packages
# only under the find roots, so each prefix is a find root as well as a
# prefix; anything built for the host (the macOS FreeType the engine would
# otherwise look for) stays out.
#
#   cmake -S platforms/ios -B build-ios-sim -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=platforms/ios/cmake/OaIosToolchain.cmake -DOA_IOS_SDK=iphonesimulator

set(CMAKE_SYSTEM_NAME iOS)
if(NOT OA_IOS_SDK)
  set(OA_IOS_SDK iphonesimulator)
endif()
if(NOT OA_IOS_SDK MATCHES "^(iphonesimulator|iphoneos)$")
  message(FATAL_ERROR "OA_IOS_SDK must be iphonesimulator or iphoneos, not \"${OA_IOS_SDK}\"")
endif()
set(OA_IOS_SDK "${OA_IOS_SDK}" CACHE STRING "The system image to build for: iphonesimulator or iphoneos")
set(CMAKE_OSX_SYSROOT "${OA_IOS_SDK}" CACHE STRING "The system image the build compiles against" FORCE)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "The processor the build compiles for")
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "The oldest iOS release the game runs on")

if(NOT OA_IOS_DEPS_DIR)
  get_filename_component(oa_ios_engine_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
  set(OA_IOS_DEPS_DIR "${oa_ios_engine_root}/local/deps/ios-${OA_IOS_SDK}")
endif()
set(CMAKE_PREFIX_PATH "${OA_IOS_DEPS_DIR}/sdl;${OA_IOS_DEPS_DIR}/freetype;${OA_IOS_DEPS_DIR}/harfbuzz")
set(CMAKE_FIND_ROOT_PATH "${OA_IOS_DEPS_DIR}/sdl;${OA_IOS_DEPS_DIR}/freetype;${OA_IOS_DEPS_DIR}/harfbuzz")
# The compile checks CMake runs in their own projects read this file again
# and need the same choices.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OA_IOS_SDK OA_IOS_DEPS_DIR)
