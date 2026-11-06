# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The built-in module: its libraries, the analogue atlas, coreprime.ccx and
# the extension oa-game links. Included from the root list while OA_BONEYARDS
# is on, after oa-netgame-test-installs exists and before the extension list
# is finished, so this extension is registered last and is asked first.

# Unfinished features. Off, Matchmaking stays behind an Admin or Beta Tester
# account and the registration form behind the BetaLoginUX preference.
# QuickPlay and the Locator are released to every account and have no option.
option(OA_BONEYARDS_MATCHMAKING_RELEASED "Show Matchmaking to every account" OFF)
option(OA_BONEYARDS_REGISTRATION_FORM_RELEASED
  "Show the in-game registration form to every account" OFF)

set(OA_BONEYARDS_REGISTRATION_URL "https://coreprime.net/register" CACHE STRING
  "Registration address drawn as the QR code in coreprime.ccx")

# The installs the module's tests read. OA_GAME_DIR is the engine's. This one
# names the multiplayer install a launch is checked against. It takes the
# environment variable of its name, into an empty entry too. A configure that
# has neither, on a machine that keeps that install beside the other
# checkouts, uses that install so the launch checks are registered.
if(NOT DEFINED CACHE{OA_LAUNCHER_GAME_DIR})
  set(oa_boneyards_launcher_default "$ENV{OA_LAUNCHER_GAME_DIR}")
  if(oa_boneyards_launcher_default STREQUAL ""
      AND IS_DIRECTORY "$ENV{HOME}/go/src/github.com/coreprime/game-assets/total-annihilation/taby-coreprime-poc")
    set(oa_boneyards_launcher_default
      "$ENV{HOME}/go/src/github.com/coreprime/game-assets/total-annihilation/taby-coreprime-poc")
  endif()
  set(OA_LAUNCHER_GAME_DIR "${oa_boneyards_launcher_default}" CACHE PATH
    "The multiplayer install a launch is checked against, which some tests also run on")
endif()
if(OA_LAUNCHER_GAME_DIR STREQUAL "" AND NOT "$ENV{OA_LAUNCHER_GAME_DIR}" STREQUAL "")
  set_property(CACHE OA_LAUNCHER_GAME_DIR PROPERTY VALUE "$ENV{OA_LAUNCHER_GAME_DIR}")
endif()
if(NOT OA_LAUNCHER_GAME_DIR STREQUAL "" AND NOT IS_ABSOLUTE "${OA_LAUNCHER_GAME_DIR}")
  message(FATAL_ERROR "OA_LAUNCHER_GAME_DIR (\"${OA_LAUNCHER_GAME_DIR}\") is a relative path; "
    "give the absolute path of the multiplayer install")
endif()

# Marks registered tests as reading an install: the engine's game-data
# marking, plus OA_LAUNCHER_GAME_DIR in their environment when it is set.
function(oa_boneyards_install_tests)
  oa_game_data_tests(${ARGN})
  if(OA_LAUNCHER_GAME_DIR)
    foreach(test IN LISTS ARGN)
      set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "OA_LAUNCHER_GAME_DIR=${OA_LAUNCHER_GAME_DIR}")
    endforeach()
  endif()
endfunction()

# ctest names drop the word "test" (tools/check_ctest_names.py). Executable
# target names may keep it.
function(oa_boneyards_ctest_name output name)
  string(REPLACE "-test" "" stripped "${name}")
  set("${output}" "${stripped}" PARENT_SCOPE)
endfunction()

# Pages' fonts. Liberation Sans has to be there; the build does not fetch.
# A configure that passes -DOA_FONTS_DIR uses that folder (the suite's
# prepared cache). Otherwise the engine's own local/deps/fonts is first,
# then OA_DEPS_DIR/fonts, the pre-fold checkout's local/deps/fonts,
# deps-boneyards/fonts up to four directories above this tree, and the
# pre-fold checkout beside the engine checkout a linked local/ points at.
function(oa_boneyards_font_candidates output)
  set(candidates "")
  list(APPEND candidates "${open_annihilation_SOURCE_DIR}/local/deps/fonts")
  if(NOT "$ENV{OA_DEPS_DIR}" STREQUAL "")
    list(APPEND candidates "$ENV{OA_DEPS_DIR}/fonts")
  endif()
  if(NOT "$ENV{OA_BONEYARDS_REPO}" STREQUAL "")
    list(APPEND candidates "$ENV{OA_BONEYARDS_REPO}/local/deps/fonts")
  endif()
  set(walk "${open_annihilation_SOURCE_DIR}")
  foreach(step RANGE 1 4)
    get_filename_component(walk "${walk}" DIRECTORY)
    list(APPEND candidates "${walk}/deps-boneyards/fonts")
  endforeach()
  if(IS_SYMLINK "${open_annihilation_SOURCE_DIR}/local")
    get_filename_component(oa_boneyards_local_real "${open_annihilation_SOURCE_DIR}/local" REALPATH)
    get_filename_component(oa_boneyards_engine_checkout "${oa_boneyards_local_real}" DIRECTORY)
    get_filename_component(oa_boneyards_engine_parent "${oa_boneyards_engine_checkout}" DIRECTORY)
    list(APPEND candidates
      "${oa_boneyards_engine_parent}/open-annihilation-boneyards/local/deps/fonts")
  endif()
  set("${output}" "${candidates}" PARENT_SCOPE)
endfunction()

# -DOA_FONTS_DIR names the folder. An empty cache entry is not a choice.
if(NOT DEFINED CACHE{OA_FONTS_DIR} OR "${OA_FONTS_DIR}" STREQUAL "")
  oa_boneyards_font_candidates(oa_boneyards_font_candidates)
  set(oa_boneyards_fonts_found "")
  foreach(dir IN LISTS oa_boneyards_font_candidates)
    if(EXISTS "${dir}/liberation/LiberationSans-Regular.ttf")
      set(oa_boneyards_fonts_found "${dir}")
      break()
    endif()
  endforeach()
  if(oa_boneyards_fonts_found STREQUAL "")
    message(FATAL_ERROR
      "LiberationSans-Regular.ttf was not found. Looked in: ${oa_boneyards_font_candidates}")
  endif()
  set(OA_FONTS_DIR "${oa_boneyards_fonts_found}" CACHE PATH
    "Bundled fonts and their licences, copied into the game's files folder")
endif()
if(NOT EXISTS "${OA_FONTS_DIR}/liberation/LiberationSans-Regular.ttf")
  message(FATAL_ERROR
    "OA_FONTS_DIR (\"${OA_FONTS_DIR}\") has no liberation/LiberationSans-Regular.ttf")
endif()
if(NOT TARGET Freetype::Freetype)
  message(FATAL_ERROR "Freetype::Freetype is not available, so the pages' text cannot be drawn")
endif()
get_filename_component(OA_FONT_DEPS_DIR "${OA_FONTS_DIR}" DIRECTORY)
# The analogue atlas is committed. The build copies it into the game's files.
# It does not draw it, so a cross or container build needs no host rasterizer.
# tools/boneyards/build_font_atlases.py remains the maintainer's tool.
set(OA_FONT_ATLASES_DIR "${open_annihilation_SOURCE_DIR}/data/boneyards/fonts")
set(OA_BONEYARDS_ATLAS_FILE "${OA_FONT_ATLASES_DIR}/boneyards-analogues.bin")
if(NOT EXISTS "${OA_BONEYARDS_ATLAS_FILE}")
  message(FATAL_ERROR
    "The analogue font atlas is missing (${OA_BONEYARDS_ATLAS_FILE}). "
    "Run tools/boneyards/build_font_atlases.py and commit the file it writes.")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(BUILD_TESTING)
  # Redraws the committed atlas and fails when the bytes differ, as
  # live-shaders does for its file. A full redraw is a few seconds; the
  # limit leaves room on a busy host. A machine without the fonts or a
  # host compiler skips.
  add_test(NAME boneyards-font-atlas
    COMMAND ${Python3_EXECUTABLE}
      "${open_annihilation_SOURCE_DIR}/tools/boneyards/build_font_atlases.py"
      --check
      --deps "${OA_FONT_DEPS_DIR}"
      --engine "${open_annihilation_SOURCE_DIR}"
      --text-fonts "${OA_TEXT_FONTS_DIR}")
  set_tests_properties(boneyards-font-atlas PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# The faces that travel with the game, for the module's tests. oa-game
# compiles the same source itself and does not link this library.
add_library(oa-app-modern-text STATIC "${open_annihilation_SOURCE_DIR}/src/app/modern_text.cpp")
add_library(oa::app::modern_text ALIAS oa-app-modern-text)
target_link_libraries(oa-app-modern-text
  PUBLIC oa-app-headers oa-platform-text-font oa-present oa-base-threads SDL3::SDL3
  PRIVATE oa-options)
target_compile_features(oa-app-modern-text PUBLIC cxx_std_20)

set(OA_BONEYARDS_MASTER_SERVERS
  "${open_annihilation_SOURCE_DIR}/data/boneyards/master-servers.byd" CACHE FILEPATH
  "Server list baked into Rover, in the format of boneyards/server.byd")
if(NOT IS_ABSOLUTE "${OA_BONEYARDS_MASTER_SERVERS}")
  get_filename_component(OA_BONEYARDS_MASTER_SERVERS "${OA_BONEYARDS_MASTER_SERVERS}" ABSOLUTE
    BASE_DIR "${open_annihilation_SOURCE_DIR}")
  set(OA_BONEYARDS_MASTER_SERVERS "${OA_BONEYARDS_MASTER_SERVERS}" CACHE FILEPATH
    "Server list baked into Rover, in the format of boneyards/server.byd" FORCE)
endif()
if(NOT EXISTS "${OA_BONEYARDS_MASTER_SERVERS}" OR IS_DIRECTORY "${OA_BONEYARDS_MASTER_SERVERS}")
  message(FATAL_ERROR
    "OA_BONEYARDS_MASTER_SERVERS (\"${OA_BONEYARDS_MASTER_SERVERS}\") does not name a server list file")
endif()

add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/tests"
  "${CMAKE_BINARY_DIR}/src/boneyards/tests")
# The relay tunnel, on the sockets network play provides.
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/tunnel"
  "${CMAKE_BINARY_DIR}/src/boneyards/tunnel")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/rover"
  "${CMAKE_BINARY_DIR}/src/boneyards/rover")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/platform"
  "${CMAKE_BINARY_DIR}/src/boneyards/platform")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/toolkit"
  "${CMAKE_BINARY_DIR}/src/boneyards/toolkit")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/html"
  "${CMAKE_BINARY_DIR}/src/boneyards/html")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/screens"
  "${CMAKE_BINARY_DIR}/src/boneyards/screens")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/console"
  "${CMAKE_BINARY_DIR}/src/boneyards/console")
# The Help-page check reads the loose overrides from the source tree, the
# same files the game copies beside the program.
if(TARGET oa-boneyards-console-help-pages)
  target_compile_definitions(oa-boneyards-console-help-pages PRIVATE
    "OA_BONEYARDS_HELP_DIR=\"${open_annihilation_SOURCE_DIR}/data/boneyards/help\"")
endif()
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/client"
  "${CMAKE_BINARY_DIR}/src/boneyards/client")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/reporter"
  "${CMAKE_BINARY_DIR}/src/boneyards/reporter")
add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/extension"
  "${CMAKE_BINARY_DIR}/src/boneyards/extension")
# The module's checks, like the engine's, are compiled only into a build that
# holds its self-checks. A release package leaves this directory out.
if(OA_SELF_CHECKS)
  add_subdirectory("${open_annihilation_SOURCE_DIR}/src/boneyards/checks"
    "${CMAKE_BINARY_DIR}/src/boneyards/checks")
endif()

# coreprime.ccx and the mod profile, written into the build tree. The archive
# holds the new animations and every file under data/boneyards/coreprime.
# The war map is not one of them.
set(oa_coreprime_dir "${CMAKE_BINARY_DIR}/coreprime")
set(oa_coreprime_ccx "${oa_coreprime_dir}/coreprime.ccx")
set(oa_coreprime_profile "${oa_coreprime_dir}/oamod.yaml")
set(oa_coreprime_art_dir "${open_annihilation_SOURCE_DIR}/art/boneyards")
set(oa_coreprime_data_dir "${open_annihilation_SOURCE_DIR}/data/boneyards/coreprime")
set(oa_coreprime_profile_source "${open_annihilation_SOURCE_DIR}/data/boneyards/oamod.yaml")
file(GLOB_RECURSE oa_coreprime_art CONFIGURE_DEPENDS
  "${oa_coreprime_art_dir}/*.png"
  "${oa_coreprime_art_dir}/*.csv")
file(GLOB_RECURSE oa_coreprime_data CONFIGURE_DEPENDS "${oa_coreprime_data_dir}/*")
set(oa_coreprime_command
  ${Python3_EXECUTABLE}
  "${open_annihilation_SOURCE_DIR}/tools/boneyards/build_coreprime_ccx.py"
  --art "${oa_coreprime_art_dir}"
  --data "${oa_coreprime_data_dir}"
  --profile "${oa_coreprime_profile_source}"
  --registration-url "${OA_BONEYARDS_REGISTRATION_URL}"
  --output "${oa_coreprime_dir}")
set(oa_coreprime_input_lines ${oa_coreprime_art} ${oa_coreprime_data})
list(SORT oa_coreprime_input_lines)
list(APPEND oa_coreprime_input_lines "registration-url=${OA_BONEYARDS_REGISTRATION_URL}")
list(JOIN oa_coreprime_input_lines "\n" oa_coreprime_input_text)
string(APPEND oa_coreprime_input_text "\n")
set(oa_coreprime_listing "${oa_coreprime_dir}/inputs.txt")
set(oa_coreprime_listing_old "")
if(EXISTS "${oa_coreprime_listing}")
  file(READ "${oa_coreprime_listing}" oa_coreprime_listing_old)
endif()
if(NOT oa_coreprime_listing_old STREQUAL oa_coreprime_input_text)
  file(MAKE_DIRECTORY "${oa_coreprime_dir}")
  file(WRITE "${oa_coreprime_listing}" "${oa_coreprime_input_text}")
endif()
set(oa_coreprime_depends
  "${open_annihilation_SOURCE_DIR}/tools/boneyards/build_coreprime_ccx.py"
  "${open_annihilation_SOURCE_DIR}/tools/boneyards/qr.py"
  "${oa_coreprime_profile_source}"
  "${oa_coreprime_listing}"
  ${oa_coreprime_art}
  ${oa_coreprime_data})
add_custom_command(
  OUTPUT "${oa_coreprime_ccx}" "${oa_coreprime_profile}"
  COMMAND ${oa_coreprime_command}
  DEPENDS ${oa_coreprime_depends}
  VERBATIM
  COMMENT "Building coreprime.ccx"
)
add_custom_target(coreprime-archive ALL DEPENDS "${oa_coreprime_ccx}" "${oa_coreprime_profile}")
if(BUILD_TESTING)
  add_executable(oa-coreprime-ccx-check
    "${open_annihilation_SOURCE_DIR}/src/boneyards/tests/coreprime/coreprime_ccx.cpp")
  target_link_libraries(oa-coreprime-ccx-check PRIVATE
    oa-formats-hpi oa-formats-gaf oa-test-support oa-options)
  add_dependencies(oa-coreprime-ccx-check coreprime-archive)
  add_test(NAME coreprime-ccx
    COMMAND oa-coreprime-ccx-check
      "${oa_coreprime_ccx}"
      "${Python3_EXECUTABLE}"
      "${open_annihilation_SOURCE_DIR}/tools/boneyards/qr.py"
      "${OA_BONEYARDS_REGISTRATION_URL}")
  set_tests_properties(coreprime-ccx PROPERTIES TIMEOUT 60)
endif()

foreach(oa_coreprime_user IN ITEMS
    oa-boneyards-console-gui-screen
    oa-boneyards-console-quickplay
    oa-boneyards-console-matchmaking
    oa-boneyards-console-locator
    oa-boneyards-console-battle-console
    oa-boneyards-console-registration
    oa-boneyards-console-registration-form
    oa-boneyards-console-side-prompt
    oa-boneyards-console-boneyards-navigation
    oa-boneyards-console-gamepad)
  if(TARGET ${oa_coreprime_user})
    add_dependencies(${oa_coreprime_user} coreprime-archive)
  endif()
endforeach()

if(TARGET oa-game)
  # The game's files are copied after it links. The atlas is one of those
  # files, so a new committed atlas is copied on the next link.
  set_property(TARGET oa-game APPEND PROPERTY LINK_DEPENDS "${OA_BONEYARDS_ATLAS_FILE}")
  add_dependencies(oa-game coreprime-archive)
endif()

# The extension library is created here, in this directory, after every
# library it links. A deferred call would run after the layout dump, so the
# dump would not see the library, and the sources this file's launch library
# exports would look unlinked. Registration only records the library; the
# extension list is finished later, in the top project, which is the same
# directory when this repository is the top project. The engine directory is
# an argument so a project that adds this repository still passes the paths
# this file computed.
function(oa_boneyards_add_extension engine_source atlas_dir)
  if(NOT TARGET oa-boneyards-extension-launch OR NOT TARGET oa-game)
    return()
  endif()
  # menu_check and the --check-* runs are self-checks. A release build, which
  # compiles network play's menu check out, leaves these out too, or the game
  # asks the linker for symbols that build does not define.
  set(oa_boneyards_check_sources "")
  set(oa_boneyards_check_libraries "")
  if(OA_SELF_CHECKS)
    if(NOT TARGET oa-boneyards-checks OR NOT TARGET oa-boneyards-extension-launch-check)
      return()
    endif()
    set(oa_boneyards_check_sources
      "${engine_source}/src/boneyards/extension/boneyards_inactive_check.cpp"
      "${engine_source}/src/boneyards/extension/boneyards_menu_check.cpp"
      "${engine_source}/src/boneyards/extension/boneyards_self_checks.cpp"
      "${engine_source}/src/boneyards/extension/boneyards_strip_click_check.cpp")
    set(oa_boneyards_check_libraries
      oa-boneyards-extension-launch-check oa-boneyards-checks
      oa-ui-frontend-multiplayer)
  endif()
  add_library(oa-boneyards-extension STATIC
    "${engine_source}/src/boneyards/extension/boneyards_entry.cpp"
    "${engine_source}/src/boneyards/extension/boneyards_extension.cpp"
    ${oa_boneyards_check_sources}
    "${engine_source}/src/boneyards/extension/expansion_folder.cpp"
    "${engine_source}/src/boneyards/extension/shipped_archive.cpp")
  add_library(oa::boneyards::extension ALIAS oa-boneyards-extension)
  # boneyards_self_checks.cpp includes the checks' header through the link.
  # This library does not name that module's directory.
  target_link_libraries(oa-boneyards-extension
    PUBLIC oa-app-netgame-sdk
    PRIVATE oa-boneyards-extension-options oa-boneyards-extension-launch
      ${oa_boneyards_check_libraries} oa::app::headers oa-app-game-directory
      oa-app-mod-profile oa-ui-engine-settings
      oa-app-game-options oa-boneyards-console-app oa-boneyards-console oa-boneyards-client
      oa-boneyards-screens oa-boneyards-html-host-fonts oa-boneyards-platform-fonts
      oa-platform-preferences oa-data-languages oa-platform-shims
      oa-ui-frontend-multiplayer oa-formats-hpi
      SDL3::SDL3 oa-options
      oa-boneyards-toolkit-chat oa-boneyards-console-private)
  add_dependencies(oa-boneyards-extension coreprime-archive)
  set(oa_game_files_dir "$<GENEX_EVAL:$<TARGET_PROPERTY:oa-game,OA_GAME_FILES_DIR>>")
  # The text-font rule empties that fonts folder and then copies only the
  # text faces (cmake/OaTextFonts.cmake). On a fresh tree it is the last
  # writer, so a copy done earlier in the link does not stay. These commands
  # are the end of that rule: the page fonts and the atlas are put back in
  # the same step that emptied the folder. The link copies them too.
  set_property(GLOBAL PROPERTY OA_TEXT_FONT_STAGE_FOLLOWS_oa-game
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${OA_FONTS_DIR}" "${oa_game_files_dir}/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${OA_BONEYARDS_ATLAS_FILE}" "${oa_game_files_dir}/fonts/boneyards-analogues.bin")
  oa_add_extension(oa-boneyards-extension INIT oa_extension_init_boneyards SWITCHES cy
    GAME_FILES
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${OA_FONTS_DIR}" "${oa_game_files_dir}/fonts"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${atlas_dir}/boneyards-analogues.bin"
        "${oa_game_files_dir}/fonts/boneyards-analogues.bin"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_BINARY_DIR}/coreprime/coreprime.ccx"
        "${oa_game_files_dir}/coreprime.ccx"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${oa_game_files_dir}/languages"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${engine_source}/languages/boneyards.tdf"
        "${oa_game_files_dir}/languages/boneyards.tdf"
      # Loose Help pages, at the paths the expansion uses. Not in coreprime.ccx.
      # The reader takes these before the expansion's copies.
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${engine_source}/data/boneyards/help"
        "${oa_game_files_dir}/boneyards/Help")
endfunction()

oa_boneyards_add_extension("${open_annihilation_SOURCE_DIR}" "${OA_FONT_ATLASES_DIR}")
