# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# How the game is laid out where it is built.
#
# On macOS the game is an application bundle, open-annihilation.app, whose
# executable is Contents/MacOS/open-annihilation: the bundle's Info.plist
# (src/app/Info.plist.in) gives it the name the Dock, the application
# switcher and the menu bar show, "Open Annihilation", and its identifier.
# The bundle takes the executable's name because CMake names a bundle after
# its target's OUTPUT_NAME; the macOS release packages (tools/release_macos.sh)
# name it as its Info.plist does, "Open Annihilation.app". On the other Apple
# systems (iOS and iPadOS) the game is a flat application bundle: the
# executable, its Info.plist and the files that travel with it all sit at the
# bundle's root, with no Contents/ or Resources/ folder (a Resources/ folder
# there stops the system from installing the bundle), and the project that
# builds for that system (platforms/ios) gives the bundle its Info.plist.
# Elsewhere the game is the executable alone.
#
# The files that travel with the game, such as the licence notices and what
# the extensions add (their GAME_FILES, cmake/OaExtensions.cmake), go in the folder
# SDL_GetBasePath() names at run time: the bundle's Contents/Resources on
# macOS, the flat bundle itself on the other Apple systems, the executable's
# own folder elsewhere. oa_game_bundle() stores that
# folder, as a generator expression, in the target's OA_GAME_FILES_DIR
# property; a command names it as
#   $<GENEX_EVAL:$<TARGET_PROPERTY:oa-game,OA_GAME_FILES_DIR>>
include_guard(GLOBAL)

# The Info.plist template of the bundle, and the bundle's icon, made from
# the Open Annihilation icon by tools/make_icons.py.
set(OA_GAME_INFO_PLIST "${CMAKE_CURRENT_LIST_DIR}/../src/app/Info.plist.in")
set(OA_GAME_ICNS "${CMAKE_CURRENT_LIST_DIR}/../branding/open-annihilation.icns")

# Sets OA_MACOS_MINIMUM_VERSION in the caller's scope to the oldest macOS
# release the build's code runs on: CMAKE_OSX_DEPLOYMENT_TARGET when it is
# set, else the release the compiler builds for by default, which it states
# as major * 10000 + minor * 100 in __ENVIRONMENT_OS_VERSION_MIN_REQUIRED__.
function(oa_macos_minimum_version)
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(OA_MACOS_MINIMUM_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}" PARENT_SCOPE)
    return()
  endif()
  set(sysroot_arguments "")
  if(CMAKE_OSX_SYSROOT)
    set(sysroot_arguments -isysroot "${CMAKE_OSX_SYSROOT}")
  endif()
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" ${sysroot_arguments} -dM -E -x c++ /dev/null
    OUTPUT_VARIABLE macros RESULT_VARIABLE result ERROR_QUIET)
  if(NOT result EQUAL 0
      OR NOT macros MATCHES "__ENVIRONMENT_OS_VERSION_MIN_REQUIRED__ ([0-9]+)")
    message(FATAL_ERROR "${CMAKE_CXX_COMPILER} states no minimum macOS release; "
      "set CMAKE_OSX_DEPLOYMENT_TARGET")
  endif()
  math(EXPR major "${CMAKE_MATCH_1} / 10000")
  math(EXPR minor "${CMAKE_MATCH_1} / 100 % 100")
  set(OA_MACOS_MINIMUM_VERSION "${major}.${minor}" PARENT_SCOPE)
endfunction()

# Lays out the game target: an application bundle on macOS, with the
# project's version and the icon in its Resources; a flat bundle with the
# project's version on the other Apple systems, whose Info.plist the
# building project sets (MACOSX_BUNDLE_INFO_PLIST); and the
# OA_GAME_FILES_DIR property everywhere. Sets
# OA_MACOS_MINIMUM_VERSION in the caller's scope on macOS, where the
# Info.plist template reads it; call it from the directory that defines the
# target.
function(oa_game_bundle target)
  if(NOT APPLE)
    set_target_properties(${target} PROPERTIES OA_GAME_FILES_DIR "$<TARGET_FILE_DIR:${target}>")
    return()
  endif()
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(${target} PROPERTIES
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_BUNDLE_VERSION "${open_annihilation_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${open_annihilation_VERSION}"
      OA_GAME_FILES_DIR "$<TARGET_BUNDLE_DIR:${target}>")
    return()
  endif()
  oa_macos_minimum_version()
  set(OA_MACOS_MINIMUM_VERSION "${OA_MACOS_MINIMUM_VERSION}" PARENT_SCOPE)
  set_target_properties(${target} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${OA_GAME_INFO_PLIST}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${open_annihilation_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${open_annihilation_VERSION}"
    MACOSX_BUNDLE_ICON_FILE open-annihilation.icns
    OA_GAME_FILES_DIR "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources")
  # oa-update is copied into Contents/MacOS from the root CMakeLists,
  # after that target exists. This function runs earlier.
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OA_GAME_ICNS}"
      "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Resources/open-annihilation.icns"
    VERBATIM)
endfunction()
