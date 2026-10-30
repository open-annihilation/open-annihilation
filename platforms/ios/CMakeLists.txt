# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Open Annihilation for iPhone and iPad: builds the engine for iOS and makes
# the game an application bundle the simulator and devices install. It adds
# the engine from OA_IOS_ENGINE_ROOT (the checkout this folder belongs to),
# with the platform's capabilities set before it is added, registers the
# iOS platform's part of the game (src/ios_platform.mm) as an extension of
# oa-game, and gives the bundle its Info.plist and launch screen.
# platforms/ios/README.md says how to build, install and run it; configure
# it with cmake/OaIosToolchain.cmake as the toolchain file.
cmake_minimum_required(VERSION 3.24)

set(OA_IOS_ENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
  "The open-annihilation checkout whose engine the iOS build builds")
get_filename_component(oa_ios_engine_root "${OA_IOS_ENGINE_ROOT}" ABSOLUTE
  BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
# The bundle's version is the engine's, as its own project() states it.
file(STRINGS "${oa_ios_engine_root}/CMakeLists.txt" oa_ios_engine_project
  REGEX "^project\\(open_annihilation VERSION [0-9.]+ ")
if(NOT oa_ios_engine_project MATCHES "VERSION ([0-9.]+) ")
  message(FATAL_ERROR "OA_IOS_ENGINE_ROOT=${OA_IOS_ENGINE_ROOT} does not hold the open-annihilation engine")
endif()
project(open_annihilation_ios VERSION ${CMAKE_MATCH_1} LANGUAGES C CXX OBJCXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "platforms/ios builds for iOS: configure it with "
    "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_LIST_DIR}/cmake/OaIosToolchain.cmake")
endif()

# The engine's Check build type, declared before any package is found so
# imported targets map to it.
include("${oa_ios_engine_root}/cmake/OaBuildTypes.cmake")
# The packages the engine and the platform's part link. The iOS build needs
# SDL 3.4 (the pinch gesture, the safe area and the pencil's hover on iOS);
# the engine itself still accepts 3.2 on every other platform.
find_package(ZLIB REQUIRED)
find_package(SDL3 3.4 CONFIG REQUIRED)

# What iOS can do, set before the engine reads its options
# (cmake/OaPlatformCapabilities.cmake and src/platform/app-menu): no macOS
# menu bar, no other programs started, no folder dialog; the window at the
# display's own pixel density and the touch controls on from the start. The
# engine's tests run on the desktop, not here: on iOS every test program
# would be an application bundle of its own.
set(OA_NATIVE_APP_MENU OFF CACHE BOOL "Build the macOS application menu's Settings item" FORCE)
set(OA_PROCESS_SPAWNING OFF CACHE BOOL "The game may start other programs" FORCE)
set(OA_SELF_UPDATE OFF CACHE BOOL "" FORCE)
set(OA_NATIVE_FOLDER_DIALOG OFF CACHE BOOL "Ask for the game folder with the system's folder dialog" FORCE)
set(OA_NATIVE_DENSITY_WINDOWS ON CACHE BOOL "Open the game's window at the display's own pixel density" FORCE)
set(OA_TOUCH_FIRST ON CACHE BOOL "Touch controls are on from the start" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Build the tests" FORCE)

# A copy of the game's data built into the bundle, for a device without
# the Files app's copy: the folder named here is copied into the bundle as
# "Total Annihilation" after every build, and the game uses it when the
# Documents folder holds no "Total Annihilation" folder. Empty (the
# default): nothing is copied. The data never enters the repository.
set(OA_IOS_BUNDLED_GAME_DIR "" CACHE PATH
  "A Total Annihilation folder to build into the bundle (empty: none)")
set(OA_IOS_BUNDLE_IDENTIFIER "net.coreprime.open-annihilation" CACHE STRING
  "The bundle identifier the game installs under")

add_subdirectory("${oa_ios_engine_root}" engine)

# The bundle's Info.plist, configured now; CMake copies it into the bundle.
set(OA_IOS_EXECUTABLE open-annihilation)
set(OA_IOS_VERSION "${PROJECT_VERSION}")
set(OA_IOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(OA_IOS_SDK STREQUAL "iphoneos")
  set(OA_IOS_PLATFORM iPhoneOS)
else()
  set(OA_IOS_PLATFORM iPhoneSimulator)
endif()
set(oa_ios_info_plist "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
configure_file(Info.plist.in "${oa_ios_info_plist}" @ONLY)
get_target_property(oa_ios_game_name oa-game OUTPUT_NAME)
if(NOT oa_ios_game_name STREQUAL OA_IOS_EXECUTABLE)
  message(FATAL_ERROR "oa-game builds ${oa_ios_game_name}, not ${OA_IOS_EXECUTABLE}: update the Info.plist")
endif()
set_target_properties(oa-game PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${oa_ios_info_plist}"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${OA_IOS_BUNDLE_IDENTIFIER}"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${OA_IOS_BUNDLE_IDENTIFIER}"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

# The launch screen, compiled by the Xcode tools whenever it changes.
find_program(OA_IOS_XCRUN xcrun REQUIRED)
set(oa_ios_launch_screen "${CMAKE_CURRENT_BINARY_DIR}/LaunchScreen.storyboardc")
add_custom_command(OUTPUT "${oa_ios_launch_screen}/Info.plist"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${oa_ios_launch_screen}"
  COMMAND "${OA_IOS_XCRUN}" ibtool --errors --warnings --output-format human-readable-text
    --minimum-deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    --target-device iphone --target-device ipad
    --compile "${oa_ios_launch_screen}" "${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard"
  COMMENT "Compiling the launch screen"
  VERBATIM)
add_custom_target(oa-ios-launch-screen DEPENDS "${oa_ios_launch_screen}/Info.plist")

# The app icon, made from the game's icon in branding/ whenever it changes
# (tools/make_app_icon.py: the metal plate cut square and opaque, so iOS's own
# rounded mask falls on it) and compiled by the Xcode tools into the icon
# files and Assets.car. The icon never enters the repository. Info.plist.in
# names the icon files the compiler writes (CFBundleIcons).
find_program(OA_IOS_PYTHON NAMES python3 REQUIRED)
set(oa_ios_icon_source "${oa_ios_engine_root}/branding/open-annihilation-icon.png")
set(oa_ios_icon_catalogue "${CMAKE_CURRENT_BINARY_DIR}/Assets.xcassets")
set(oa_ios_icon_compiled "${CMAKE_CURRENT_BINARY_DIR}/AppIcon")
add_custom_command(OUTPUT "${oa_ios_icon_compiled}/Assets.car"
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${oa_ios_icon_catalogue}" "${oa_ios_icon_compiled}"
  COMMAND "${OA_IOS_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_app_icon.py"
    --source "${oa_ios_icon_source}" --catalogue "${oa_ios_icon_catalogue}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${oa_ios_icon_compiled}"
  COMMAND "${OA_IOS_XCRUN}" actool --output-format human-readable-text --errors --warnings
    --platform "${OA_IOS_SDK}" --minimum-deployment-target "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    --target-device iphone --target-device ipad --app-icon AppIcon
    --output-partial-info-plist "${CMAKE_CURRENT_BINARY_DIR}/AppIcon-Info.plist"
    --compile "${oa_ios_icon_compiled}" "${oa_ios_icon_catalogue}"
  DEPENDS "${oa_ios_icon_source}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_app_icon.py"
  COMMENT "Making the app icon"
  VERBATIM)
add_custom_target(oa-ios-app-icon DEPENDS "${oa_ios_icon_compiled}/Assets.car")

# The privacy manifest (PrivacyInfo.plist here, PrivacyInfo.xcprivacy in the
# bundle): no tracking, no data collected, and the reasons for the system
# calls Apple asks apps to explain. README.md records how the calls were found.
set(oa_ios_privacy_manifest "${CMAKE_CURRENT_SOURCE_DIR}/PrivacyInfo.plist")

# Device builds made in Xcode (scripts/xcode.sh) sign with the entitlements
# file when OA_IOS_MULTICAST_ENTITLEMENT is on: it asks for the multicast
# networking entitlement, which LAN games need to find each other by
# broadcast and which Apple grants to a developer team on request. Off by
# default, so a team without it (every personal team) can still sign; the
# simulator never uses it.
option(OA_IOS_MULTICAST_ENTITLEMENT
  "Sign device builds with the multicast networking entitlement (granted by Apple on request)" OFF)
if(OA_IOS_MULTICAST_ENTITLEMENT AND OA_IOS_SDK STREQUAL "iphoneos")
  set_target_properties(oa-game PROPERTIES XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
    "${CMAKE_CURRENT_SOURCE_DIR}/OpenAnnihilation.entitlements")
endif()

# The iOS platform's part of the game: the SDL hints iOS needs, the
# platform's hooks (haptics, the default game folder, its advice and the
# Check again button, the window's editing gestures) and the Game files
# screen's hooks (src/ios_game_files.mm: the document picker, access to what
# the player chose, coordinated reads that download from iCloud Drive and
# other file providers, free space, background time, device backups and the
# platform's words), installed by its extension init. oa::app::platform_hooks
# holds both seams.
add_library(oa-ios-platform STATIC src/ios_platform.mm src/ios_game_files.mm)
set_target_properties(oa-ios-platform PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
set_source_files_properties(src/ios_platform.mm src/ios_game_files.mm
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(oa-ios-platform PRIVATE oa::extension::sdk oa::app::platform_hooks SDL3::SDL3
  oa-options "-framework UIKit" "-framework Foundation" "-framework UniformTypeIdentifiers"
  "-framework FileProvider")
if(OA_IOS_BUNDLED_GAME_DIR)
  get_filename_component(oa_ios_bundled_game "${OA_IOS_BUNDLED_GAME_DIR}" ABSOLUTE)
  if(NOT IS_DIRECTORY "${oa_ios_bundled_game}")
    message(FATAL_ERROR "OA_IOS_BUNDLED_GAME_DIR=${OA_IOS_BUNDLED_GAME_DIR} is not a folder")
  endif()
  target_compile_definitions(oa-ios-platform PRIVATE OA_IOS_BUNDLED_GAME=1)
else()
  target_compile_definitions(oa-ios-platform PRIVATE OA_IOS_BUNDLED_GAME=0)
endif()
add_dependencies(oa-ios-platform oa-ios-launch-screen oa-ios-app-icon)

# What travels in the bundle beside the game: the compiled launch screen, the
# app icon, the privacy manifest, and the bundled game data when
# OA_IOS_BUNDLED_GAME_DIR names it.
set(oa_ios_bundle "$<TARGET_BUNDLE_DIR:oa-game>")
set(oa_ios_game_files
  COMMAND ${CMAKE_COMMAND} -E rm -rf "${oa_ios_bundle}/LaunchScreen.storyboardc"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${oa_ios_launch_screen}"
    "${oa_ios_bundle}/LaunchScreen.storyboardc"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${oa_ios_icon_compiled}" "${oa_ios_bundle}"
  COMMAND ${CMAKE_COMMAND} -E copy "${oa_ios_privacy_manifest}"
    "${oa_ios_bundle}/PrivacyInfo.xcprivacy")
if(OA_IOS_BUNDLED_GAME_DIR)
  # About a gigabyte: copied again only where a file differs, where CMake can tell.
  set(oa_ios_copy_game copy_directory_if_different)
  if(CMAKE_VERSION VERSION_LESS 3.26)
    set(oa_ios_copy_game copy_directory)
  endif()
  list(APPEND oa_ios_game_files
    COMMAND ${CMAKE_COMMAND} -E ${oa_ios_copy_game} "${oa_ios_bundled_game}"
      "${oa_ios_bundle}/Total Annihilation")
else()
  # A copy an earlier configuration built in leaves the bundle.
  list(APPEND oa_ios_game_files
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${oa_ios_bundle}/Total Annihilation")
endif()
# A changed launch screen, icon or privacy manifest links the game again, so
# the copy step above runs.
set_property(TARGET oa-game APPEND PROPERTY LINK_DEPENDS "${oa_ios_launch_screen}/Info.plist"
  "${oa_ios_icon_compiled}/Assets.car" "${oa_ios_privacy_manifest}")
oa_add_extension(oa-ios-platform INIT oa_extension_init_ios_platform GAME_FILES ${oa_ios_game_files})
