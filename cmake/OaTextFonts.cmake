# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# FreeType and the fonts the engine draws text with (src/platform/text-font),
# which tools/bootstrap_text_fonts.py fetches, pinned, and builds under
# local/deps. A configuration without them stops here and says how to get
# them.
#
# FreeType is the one an including project has already found
# (Freetype::Freetype), else the one CMAKE_PREFIX_PATH or a cross-build's
# prefixes hold (tools/bootstrap_macos_deps.py and
# tools/bootstrap_windows_deps.py build it beside SDL), else the bootstrap's
# local/deps/freetype-install-<release>, else the system's.
#
# OA_TEXT_FONTS_DIR names the folder of the fonts: the files OA_TEXT_FONT_FILES
# lists, which oa_copy_text_fonts() copies beside a program, into the folder
# oa::platform::text_font::bundled_font_directory() finds at run time. The list
# is the module's face_files, then its interface_face_files
# (oa/platform/text_font.hpp), and the bootstrap's FONT_FILES; the three name
# the same files, in Face order. Every one is required: the base faces draw
# game text (the endonym faces among them: the CJK cut and the Bengali cut),
# and the interface faces, Barlow and Barlow Condensed, draw OA's own screens
# in the modern look.
#
# OA_TEXT_FONT_TESTS_DIR holds the test fixture's face. The build never copies
# it beside the game. A missing fixture face is reported and does not stop
# the configuration: the checks that need it fail later and name the bootstrap.
include_guard(GLOBAL)

set(OA_TEXT_FONT_FILES DejaVuSans-Bold.ttf DejaVuSans.ttf
  NotoSansCJKsc-Bold-Endonyms.otf NotoEmoji.ttf NotoSansBengali-Bold-Endonyms.ttf
  Barlow-Regular.ttf Barlow-Medium.ttf Barlow-SemiBold.ttf Barlow-Bold.ttf
  BarlowCondensed-SemiBold.ttf BarlowCondensed-Bold.ttf)
get_filename_component(oa_text_fonts_default "${CMAKE_CURRENT_LIST_DIR}/../local/deps/text-fonts" ABSOLUTE)
set(OA_TEXT_FONTS_DIR "${oa_text_fonts_default}" CACHE PATH
  "Folder holding the fonts tools/bootstrap_text_fonts.py makes, which the build copies beside the game")
foreach(font IN LISTS OA_TEXT_FONT_FILES)
  if(NOT EXISTS "${OA_TEXT_FONTS_DIR}/${font}")
    message(FATAL_ERROR "The text font ${font} is not in ${OA_TEXT_FONTS_DIR}. "
      "Run python3 tools/bootstrap_text_fonts.py, which fetches and builds FreeType and the fonts "
      "into local/deps, or name the folder that holds them with -DOA_TEXT_FONTS_DIR=.")
  endif()
endforeach()
# The default follows OA_TEXT_FONTS_DIR, including a path given with -D, so the
# fixture face sits in the folder beside the fonts the configuration uses.
get_filename_component(oa_text_font_tests_default "${OA_TEXT_FONTS_DIR}/../text-fonts-tests" ABSOLUTE)
set(OA_TEXT_FONT_TESTS_DIR "${oa_text_font_tests_default}" CACHE PATH
  "Folder holding the test fixture's font, which the build never copies beside the game")
set(OA_TEXT_FONT_TEST_FILES NotoSansCJKsc-Bold-Fixture.otf NotoSansBengali-Bold.ttf)
foreach(font IN LISTS OA_TEXT_FONT_TEST_FILES)
  if(NOT EXISTS "${OA_TEXT_FONT_TESTS_DIR}/${font}")
    message(STATUS "The test font ${font} is not in ${OA_TEXT_FONT_TESTS_DIR}. "
      "Run python3 tools/bootstrap_text_fonts.py, which cuts the test fixture's font.")
  endif()
endforeach()

if(NOT TARGET Freetype::Freetype)
  get_filename_component(oa_freetype_deps "${CMAKE_CURRENT_LIST_DIR}/../local/deps" ABSOLUTE)
  file(GLOB oa_freetype_bootstrap_prefixes LIST_DIRECTORIES true "${oa_freetype_deps}/freetype-install-*")
  list(APPEND CMAKE_PREFIX_PATH ${oa_freetype_bootstrap_prefixes})
  find_package(freetype 2.10 CONFIG QUIET)
  if(NOT TARGET Freetype::Freetype)
    find_package(Freetype 2.10 MODULE QUIET)
  endif()
  if(NOT TARGET Freetype::Freetype)
    message(FATAL_ERROR "FreeType 2.10 or later was not found. Run python3 tools/bootstrap_text_fonts.py, "
      "which builds the pinned FreeType into local/deps/freetype-install-<release>, where this "
      "configuration looks for it.")
  endif()
endif()
if(DEFINED freetype_VERSION)
  set(OA_FREETYPE_VERSION "${freetype_VERSION}")
elseif(DEFINED FREETYPE_VERSION_STRING)
  set(OA_FREETYPE_VERSION "${FREETYPE_VERSION_STRING}")
else()
  set(OA_FREETYPE_VERSION "unknown")
endif()
message(STATUS "Text fonts: FreeType ${OA_FREETYPE_VERSION}, fonts from ${OA_TEXT_FONTS_DIR}")

# HarfBuzz shapes Bengali and Devanagari. It is the one an including project
# has already found (harfbuzz::harfbuzz), else the one harfbuzz_DIR names,
# else the one CMAKE_PREFIX_PATH or a cross-build's prefixes hold, else the
# bootstrap's local/deps/harfbuzz-install-<release>, as FreeType's install
# is found. A release build and the iOS and MinGW toolchains name their
# own prefix. A named prefix that is not there is not replaced by another
# install. A configuration that does not find HarfBuzz still succeeds:
# complex text shaping is off, and a language that needs shaping is not
# offered. OA_REQUIRE_TEXT_SHAPING makes that a configure error, for a lane
# that must ship shaping. FreeType stays built without HarfBuzz, so faces
# that are not shaped keep today's advances. OA_TEXT_SHAPING says this
# build has HarfBuzz.
option(OA_REQUIRE_TEXT_SHAPING
  "Fail configuration when the pinned HarfBuzz is not found" OFF)
if(NOT TARGET harfbuzz::harfbuzz)
  # The installed package links Threads::Threads, so that target has to exist
  # before the package is read.
  find_package(Threads REQUIRED)
  # harfbuzz_DIR-NOTFOUND is a failed search, not a prefix a caller named.
  set(oa_harfbuzz_named FALSE)
  if(DEFINED harfbuzz_DIR AND NOT "${harfbuzz_DIR}" MATCHES "-NOTFOUND$")
    set(oa_harfbuzz_named TRUE)
  endif()
  if(oa_harfbuzz_named)
    if(EXISTS "${harfbuzz_DIR}/harfbuzzConfig.cmake")
      find_package(harfbuzz CONFIG QUIET)
    endif()
  else()
    get_filename_component(oa_harfbuzz_deps "${CMAKE_CURRENT_LIST_DIR}/../local/deps" ABSOLUTE)
    file(GLOB oa_harfbuzz_prefixes LIST_DIRECTORIES true
      "${oa_harfbuzz_deps}/harfbuzz-install-*")
    list(APPEND CMAKE_PREFIX_PATH ${oa_harfbuzz_prefixes})
    find_package(harfbuzz CONFIG QUIET)
  endif()
endif()
if(TARGET harfbuzz::harfbuzz)
  # HarfBuzz 15.1.0 records its headers as /harfbuzz when it is configured
  # without CMAKE_INSTALL_INCLUDEDIR. They are installed in include/harfbuzz.
  if(DEFINED harfbuzz_DIR)
    get_target_property(oa_harfbuzz_includes harfbuzz::harfbuzz INTERFACE_INCLUDE_DIRECTORIES)
    if(oa_harfbuzz_includes STREQUAL "/harfbuzz")
      get_filename_component(oa_harfbuzz_prefix "${harfbuzz_DIR}/../../.." ABSOLUTE)
      set_target_properties(harfbuzz::harfbuzz PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${oa_harfbuzz_prefix}/include/harfbuzz")
    endif()
  endif()
  add_compile_definitions(OA_TEXT_SHAPING=1)
  if(DEFINED harfbuzz_VERSION)
    message(STATUS "Text shaping: HarfBuzz ${harfbuzz_VERSION}")
  else()
    message(STATUS "Text shaping: HarfBuzz")
  endif()
elseif(OA_REQUIRE_TEXT_SHAPING)
  message(FATAL_ERROR "HarfBuzz was not found. Run python3 tools/bootstrap_text_fonts.py, "
    "which builds the pinned HarfBuzz into local/deps/harfbuzz-install-<release>, where this "
    "configuration looks for it.")
else()
  message(STATUS "HarfBuzz not found: complex text shaping is off in this build")
endif()

# Stages the text fonts into <directory>/fonts once for that directory, and
# makes <target> depend on the staging. Programs that share a directory must
# pass the same <directory> text, generator expressions included: one rule
# then owns the folder. The rule empties the folder before it copies, so a
# font the list drops is not left where a program could open it. A module
# that also ships fonts there records them on
# OA_TEXT_FONT_STAGE_FOLLOWS_<target>, a list of COMMAND arguments, before
# this function runs for that target. Those commands run after the text
# fonts, in the same step, so the emptying cannot leave them out. Nothing
# else deletes a folder this rule writes.
#
# The directory is often $<TARGET_FILE_DIR:...> or another target generator
# expression. Those are not allowed as the output of a custom command, so
# this is a custom target and it runs whenever a program that needs the
# folder is built. A wiped fonts folder is filled again on the next build,
# and a changed font is copied then too.
#
# Two directory texts can still name one folder: in a multi-config build
# several programs are written into <build>/<config>. The staging rules
# therefore take turns, so no rule empties a folder another is copying
# into: each waits for the rule made before it, which every generator
# honours, and Ninja also runs them in one job pool. The programs
# themselves still build in parallel.
#   oa_copy_text_fonts(<target> <directory>)
# The directory may be a generator expression, such as the game's
# OA_GAME_FILES_DIR.
set_property(GLOBAL APPEND PROPERTY JOB_POOLS oa_text_fonts=1)
function(oa_copy_text_fonts target directory)
  string(SHA256 oa_font_dir_key "${directory}")
  string(SUBSTRING "${oa_font_dir_key}" 0 32 oa_font_dir_key)
  set(oa_font_stage_target "oa-stage-text-fonts-${oa_font_dir_key}")
  if(NOT TARGET ${oa_font_stage_target})
    set(sources "")
    foreach(font IN LISTS OA_TEXT_FONT_FILES)
      list(APPEND sources "${OA_TEXT_FONTS_DIR}/${font}")
    endforeach()
    # Empty when nobody recorded copies. An unset property reads back as
    # NOTFOUND, which must not become a command.
    set(oa_font_follows "")
    get_property(oa_font_follows_set GLOBAL PROPERTY "OA_TEXT_FONT_STAGE_FOLLOWS_${target}" SET)
    if(oa_font_follows_set)
      get_property(oa_font_follows GLOBAL PROPERTY "OA_TEXT_FONT_STAGE_FOLLOWS_${target}")
    endif()
    add_custom_target(${oa_font_stage_target}
      COMMAND ${CMAKE_COMMAND} -E rm -rf "${directory}/fonts"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}/fonts"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${sources} "${directory}/fonts"
      ${oa_font_follows}
      DEPENDS ${sources}
      COMMENT "Staging text fonts"
      JOB_POOL oa_text_fonts
      VERBATIM)
    get_property(oa_font_stage_previous GLOBAL PROPERTY OA_TEXT_FONT_STAGE_LAST)
    if(oa_font_stage_previous)
      add_dependencies(${oa_font_stage_target} ${oa_font_stage_previous})
    endif()
    set_property(GLOBAL PROPERTY OA_TEXT_FONT_STAGE_LAST ${oa_font_stage_target})
  endif()
  add_dependencies(${target} ${oa_font_stage_target})
endfunction()
