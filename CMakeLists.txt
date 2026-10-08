# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.24)
project(open_annihilation VERSION 0.7.3 LANGUAGES C CXX)
# A target that links one static library both directly and through
# another library gets it once on its link line, in dependency order, rather
# than once for each path that reaches it. The linker then has nothing to
# ignore; a cycle between libraries is still repeated as it needs. CMake
# releases before 3.31 do not read this and keep their default order.
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)
# The Check build type, the build the test suite is validated in
# (cmake/OaBuildTypes.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaBuildTypes.cmake")
# End-to-end app checks run the real game headless; a loaded machine can
# stretch their wall time several-fold without any extra CPU work.
set(OA_NATIVE_CHECK_TIMEOUT 1800)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(OA_BUILD_PLATFORM "Build the SDL3 image and audio shell" ON)
option(OA_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer; any report stops the process" OFF)
option(OA_REQUIRE_GAME "Fail configuration when oa-game cannot be built" OFF)
include(CTest)
find_package(ZLIB REQUIRED)
# SDL3, for the game, its shell and the tests that open a window; found here,
# before the targets and tests below that ask whether it was.
if(OA_BUILD_PLATFORM)
  find_package(SDL3 3.2 CONFIG REQUIRED)
endif()
# FreeType and the fonts text outside the game's own fonts is drawn with,
# from tools/bootstrap_text_fonts.py (cmake/OaTextFonts.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaTextFonts.cmake")
# The engine's language packs, copied beside the fonts (cmake/OaLanguages.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaLanguages.cmake")

# OA_GAME_DIR, the installed game the game-data tests and native checks read,
# and the helpers that register those tests (cmake/OaGameData.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaGameData.cmake")
# OA_DEMO_INSTALLER, the installer of the Total Annihilation demo (1997) the
# demo installer tests read (cmake/OaDemoInstaller.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaDemoInstaller.cmake")
# oa-test-support, the headers test programs share: OA_CHECK, the check a
# test makes in place of assert(), kept in every build type (oa/test/check.hpp),
# make_scratch_directory(), a temporary directory no other run shares
# (oa/test/scratch_directory.hpp), and zip archives built field by field
# (oa/test/raw_zip.hpp, which needs ZLIB::ZLIB), in tests/support/include.
add_library(oa-test-support INTERFACE)
add_library(oa::test::support ALIAS oa-test-support)
target_include_directories(oa-test-support INTERFACE "${CMAKE_CURRENT_LIST_DIR}/tests/support/include")
target_compile_features(oa-test-support INTERFACE cxx_std_20)
# What a test match asks of its application, done the plain way
# (oa/test/match_services.hpp), and a handle that stays on one projectile
# from tick to tick (oa/test/projectile_handle.hpp).
add_library(oa-test-match INTERFACE)
add_library(oa::test::match ALIAS oa-test-match)
target_link_libraries(oa-test-match INTERFACE oa-test-support oa-sim-match-runtime oa-core-types)

# oa-options, the compile options every target that compiles engine code
# links (cmake/OaOptions.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaOptions.cmake")
# What the platform the game is built for can do: OA_PROCESS_SPAWNING,
# OA_NATIVE_FOLDER_DIALOG, OA_NATIVE_DENSITY_WINDOWS and OA_TOUCH_FIRST
# (cmake/OaPlatformCapabilities.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaPlatformCapabilities.cmake")
# The sanitizers instrument every target in this tree, not only those that
# link oa-options, and stop at the first report so the test that made it
# fails. oa-options carries the same flags, so the targets of a project that
# adds this engine are instrumented like the engine's when they link
# oa-options. Clang leaves alignment unchecked only in the code
# cmake/sanitizer-ignorelist.txt lists, which reads the packed canonical
# records' fields in place; GCC cannot limit the check that way, so it
# drops it everywhere. Until platform-shims adapts to a sanitizer build,
# run the suite the way the CI sanitizer job does:
#   ASAN_OPTIONS=detect_leaks=0:handle_sigfpe=0 ctest
# platform-shims ends a child with SIGFPE on purpose.
if(OA_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "OA_SANITIZERS needs Clang or GCC: this compiler has no UndefinedBehaviorSanitizer")
  endif()
  set(oa_sanitizer_link_flags -fsanitize=address,undefined)
  set(oa_sanitizer_flags ${oa_sanitizer_link_flags} -fno-sanitize-recover=all -fno-omit-frame-pointer)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND oa_sanitizer_flags
      "-fsanitize-ignorelist=${open_annihilation_SOURCE_DIR}/cmake/sanitizer-ignorelist.txt")
  else()
    list(APPEND oa_sanitizer_flags -fno-sanitize=alignment)
    message(STATUS "OA_SANITIZERS: alignment is not checked; only Clang can leave out just the packed-record code")
  endif()
  # A compiler without the sanitizer runtimes would otherwise fail only when
  # the first program links. A failed check is forgotten so the next
  # configuration tries again.
  include(CMakePushCheckState)
  include(CheckCSourceCompiles)
  include(CheckCXXSourceCompiles)
  cmake_push_check_state(RESET)
  list(JOIN oa_sanitizer_flags " " CMAKE_REQUIRED_FLAGS)
  set(CMAKE_REQUIRED_LINK_OPTIONS ${oa_sanitizer_link_flags})
  check_c_source_compiles("int main(void) { return 0; }" OA_SANITIZERS_C_LINKS)
  check_cxx_source_compiles("int main() { return 0; }" OA_SANITIZERS_CXX_LINKS)
  cmake_pop_check_state()
  if(NOT OA_SANITIZERS_C_LINKS OR NOT OA_SANITIZERS_CXX_LINKS)
    unset(OA_SANITIZERS_C_LINKS CACHE)
    unset(OA_SANITIZERS_CXX_LINKS CACHE)
    list(JOIN oa_sanitizer_flags " " oa_sanitizer_flag_text)
    set(oa_sanitizer_ignorelist_note "")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      string(CONCAT oa_sanitizer_ignorelist_note
        "; or the compiler rejected cmake/sanitizer-ignorelist.txt, "
        "which the configure log under CMakeFiles shows")
    endif()
    message(FATAL_ERROR "OA_SANITIZERS: the C or C++ compiler cannot build and link a program with "
      "${oa_sanitizer_flag_text}; install its AddressSanitizer and UndefinedBehaviorSanitizer "
      "runtimes, or configure with a compiler that has them${oa_sanitizer_ignorelist_note}")
  endif()
  add_compile_options(${oa_sanitizer_flags})
  add_link_options(${oa_sanitizer_link_flags})
  target_compile_options(oa-options INTERFACE ${oa_sanitizer_flags})
  target_link_options(oa-options INTERFACE ${oa_sanitizer_link_flags})
endif()

# SDL3 is found here, before any module, so that every directory below links
# the one imported target. A module that also finds it, to configure on its
# own, then reuses that target instead of importing the library again, which
# would put it on a program's link line once for each directory that
# imported it.
if(OA_BUILD_PLATFORM)
  find_package(SDL3 3.2 CONFIG REQUIRED)
endif()

add_subdirectory(src/base/bytes)
add_subdirectory(src/base/game-loop)
add_subdirectory(src/platform/preferences)
add_subdirectory(src/app/command-line)
add_subdirectory(src/app/card)
add_subdirectory(src/app/mod-install)
add_subdirectory(src/data/mission-types)
add_subdirectory(src/data/limits)
add_subdirectory(src/data/languages)
add_subdirectory(src/data/match-rules)
add_subdirectory(src/sim/unit-activation)
add_subdirectory(src/base/game-math)
add_subdirectory(src/base/geometry)
add_subdirectory(src/base/sha256)
add_subdirectory(src/base/text)
add_subdirectory(src/base/float-precision)
add_subdirectory(src/base/threads)
add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/platform/xp-runtime)
add_subdirectory(src/platform/win95-runtime)
add_subdirectory(src/platform/steady-clock)
add_subdirectory(src/platform/app-menu)
add_subdirectory(src/platform/text-font)
add_subdirectory(src/platform/job-pool)
add_subdirectory(src/platform/render-probe)
add_subdirectory(src/platform/sound-device)
add_subdirectory(src/platform/locale)
add_subdirectory(src/platform/display-modes)
add_subdirectory(src/platform/file-types)
# Total Annihilation folders found where Steam, Heroic, Lutris or Bottles put
# them (docs/installation/steam-deck.md).
add_subdirectory(src/platform/game-installs)
add_subdirectory(src/ui/decoded)
add_subdirectory(src/ui/services)
add_subdirectory(src/present)
add_subdirectory(src/ui/frontend-state)
add_subdirectory(src/sim/script-state)
add_subdirectory(src/sim/script-vm)
add_subdirectory(src/sim/unit-script)
add_subdirectory(src/formats/cob)
add_subdirectory(src/formats/smacker)
add_subdirectory(src/media)
add_subdirectory(src/ui/gui-layout)
add_subdirectory(src/ui/gui-input)
add_subdirectory(src/formats/gaf)
add_subdirectory(src/formats/fnt)
add_subdirectory(src/formats/png)
add_subdirectory(src/formats/pe)
add_subdirectory(src/formats/oascript)
add_subdirectory(src/formats/oamod)
add_subdirectory(src/data/mod-profile)
add_subdirectory(src/formats/zip)
add_subdirectory(src/sim/sprite-animation)
add_subdirectory(src/formats/tnt)
add_subdirectory(src/formats/ota)
add_subdirectory(src/sim/simulation-state)
add_subdirectory(src/sim/unit-spawn)
add_subdirectory(src/sim/ballistics)
add_subdirectory(src/sim/combat-state)
add_subdirectory(src/formats/tdf)
add_subdirectory(src/data/unit-definitions)
add_subdirectory(src/formats/objects3d)
add_subdirectory(src/sim/model-runtime)
add_subdirectory(src/sim/gameplay-input)
add_subdirectory(src/sim/spatial-state)
add_subdirectory(src/sim/visibility-state)
add_subdirectory(src/sim/map-runtime)
add_subdirectory(src/sim/feature-runtime)
add_subdirectory(src/sim/unit-movement)
add_subdirectory(src/sim/world-environment)
add_subdirectory(src/sim/ground-orders)
add_subdirectory(src/sim/air)
add_subdirectory(src/data/persist)
add_subdirectory(src/sim/scenario)
add_subdirectory(src/data/campaign)
add_subdirectory(src/sim/unit-health)
add_subdirectory(src/sim/weapon-execution)
add_subdirectory(src/sim/match-runtime)
add_subdirectory(src/sim/ai)
add_subdirectory(src/sim/unit-effects)
add_subdirectory(src/sim/effect-particles)
add_subdirectory(src/sim/selection)
add_subdirectory(src/sim/mission-units)
add_subdirectory(src/sim/messages)
add_subdirectory(src/sim/speed)
add_subdirectory(src/sim/profile)
add_subdirectory(src/sim/detection)
add_subdirectory(src/sim/trace)
add_subdirectory(src/sim/state-hash)
add_subdirectory(src/ui)
add_subdirectory(src/ui/display-layout)
# The gamepad controls' model (docs/controllers.md).
add_subdirectory(src/ui/pad-controls)
# The touch controls' gesture recogniser and model (docs/touch-controls.md).
add_subdirectory(src/ui/touch-gestures)
add_subdirectory(src/ui/touch-hud)
add_subdirectory(src/ui/game-files)
# The in-engine game folder chooser's model.
add_subdirectory(src/ui/folder-chooser)
add_subdirectory(src/ui/screen-registry)
add_subdirectory(src/sim/session)
if(TARGET oa-media-intro-player)
  add_executable(oa-intro src/media/intro_main.cpp)
  target_link_libraries(oa-intro PRIVATE oa-media-intro-player oa-options)
endif()

add_subdirectory(src/formats/sqsh)
add_subdirectory(src/formats/hpi)
add_subdirectory(src/data/defs)
add_subdirectory(src/present/world-renderer)
add_subdirectory(src/present/gpu-world)
add_subdirectory(src/present/unit-playout)
add_subdirectory(src/ui/frontend-renderer)
add_subdirectory(src/ui/engine-settings)
add_subdirectory(src/audio)
# The app's headers: its extension table, Runtime and the other headers an
# extension compiles against.
add_library(oa-app-headers INTERFACE)
add_library(oa::app::headers ALIAS oa-app-headers)
target_include_directories(oa-app-headers INTERFACE src/app/include)
# app.hpp and render_policy.hpp include the engine settings' header,
# runtime.hpp the gamepad controls' model and game_directory.hpp the found
# installs' header.
target_link_libraries(oa-app-headers INTERFACE oa-base-sha256 oa-data-languages oa-data-limits
  oa-data-mod-profile oa-ui-engine-settings oa-ui-pad-controls oa-platform-game-installs)
# oa-game's command line, which the option tests parse without a running app.
add_library(oa-app-game-options src/app/startup.cpp)
add_library(oa::app::game_options ALIAS oa-app-game-options)
target_link_libraries(oa-app-game-options PUBLIC oa-app-headers)
target_link_libraries(oa-app-game-options PUBLIC oa-ui-frontend-renderer oa-ui-frontend-state oa-sim-scenario
  oa-ui-screen-registry oa-app-command-line oa-platform-preferences oa-app-mod-profile
  PRIVATE oa-app-mod-install oa-platform-job-pool oa-platform-display-modes oa-options)
# oa-game's game folder: --game-dir, the remembered folder, a folder found
# where Steam, Heroic or Lutris put it, or the folder dialog's choice, the
# installation checks, and the installer of the Total Annihilation demo
# (1997) recognised and its archive unpacked.
add_library(oa-app-game-directory src/app/game_directory.cpp src/app/demo_installer.cpp)
add_library(oa::app::game_directory ALIAS oa-app-game-directory)
target_link_libraries(oa-app-game-directory PUBLIC oa-app-headers oa-base-sha256 oa-platform-preferences
  oa-app-mod-profile oa-platform-game-installs PRIVATE oa-formats-hpi oa-formats-pe oa-platform-shims oa-options)
# oa-game's mod profile: a folder's oamod.yaml found and resolved with the
# settings it binds, the mod folders a game folder offers, the layout and
# discovery a profile sets, the --mod and --print-profile options, and the
# Mods page's row for a folder: its title, version and description, and its
# badge decoded.
add_library(oa-app-mod-profile src/app/mod_profile_loader.cpp src/app/mod_folder.cpp
  src/app/mod_summary.cpp)
add_library(oa::app::mod_profile ALIAS oa-app-mod-profile)
target_link_libraries(oa-app-mod-profile PUBLIC oa-app-headers oa-data-mod-profile oa-data-defs
  oa-formats-hpi oa-platform-preferences PRIVATE oa-formats-png oa-options)
# Bringing the player's game files into the game's own storage, with no SDL:
# the files copied and left out, the name check, the plan, the space it
# needs, the listing and the copy on worker threads, the state file, the
# commit and recovery, and the additions and removals of the management
# state (game_files_import.hpp, docs/game-files.md).
add_library(oa-app-game-files src/app/game_files_import.cpp src/app/game_files_scan.cpp
  src/app/game_files_run.cpp src/app/game_files_state.cpp)
add_library(oa::app::game_files ALIAS oa-app-game-files)
target_link_libraries(oa-app-game-files PUBLIC oa-app-headers oa-app-game-directory oa-app-mod-profile
  oa-base-threads PRIVATE oa-formats-zip oa-platform-preferences oa-options)
# The player's own folder: where it is, the folders it holds, a folder for
# each mod in Saves, Screenshots, Films and Recordings, the one-time moves
# of saved games from beside the preferences file and loose in Saves, and of
# recordings from beside the preferences file, into those folders, and the
# main menu's notice of the saved games' moves; and the warning, in the same
# look, that a mod's games cannot start until its files are in its folder.
# The game shows a folder in the system's file manager itself
# (user_folder_open.cpp).
add_library(oa-app-user-folder src/app/user_folder.cpp)
add_library(oa::app::user_folder ALIAS oa-app-user-folder)
target_link_libraries(oa-app-user-folder PUBLIC oa-app-headers oa-platform-preferences
  oa-ui-engine-settings PRIVATE oa-options)
# The renderer records: what the engine has seen of each render driver on
# this machine, and the sentinel of the stage a start has reached. Their text
# and rules are pure (renderer_records.hpp); renderer_state.hpp keeps them in
# their two files beside the preferences file. The game's renderer host
# reads them at start and keeps them through the run.
add_library(oa-app-renderer-records src/app/renderer_records.cpp)
add_library(oa::app::renderer_records ALIAS oa-app-renderer-records)
target_link_libraries(oa-app-renderer-records PUBLIC oa-app-headers PRIVATE oa-options)
add_library(oa-app-renderer-state src/app/renderer_state.cpp)
add_library(oa::app::renderer_state ALIAS oa-app-renderer-state)
target_link_libraries(oa-app-renderer-state PUBLIC oa-app-renderer-records
  PRIVATE oa-platform-preferences oa-options)
# The game's window icon: the Open Annihilation icon at 256 by 256 pixels
# (branding/, made by tools/make_icons.py), embedded as a PNG file and
# decoded when the window opens; the settings dialog's header and the OA
# buttons draw it too.
# A mod profile's display rules as the records and decisions of the modules
# that carry them out (view_rules.hpp).
add_library(oa-app-view-rules src/app/view_rules.cpp)
add_library(oa::app::view_rules ALIAS oa-app-view-rules)
target_link_libraries(oa-app-view-rules PUBLIC oa-app-headers oa-data-mod-profile oa-sim-match-runtime
  oa-ui-frontend-state oa-present-world-renderer oa-ui-engine-settings
  PRIVATE oa-ui-frontend-multiplayer oa-options)
if(BUILD_TESTING)
  add_executable(oa-app-view-rules-test src/app/view_rules_test.cpp)
  target_link_libraries(oa-app-view-rules-test PRIVATE oa-app-view-rules oa-test-support oa-options)
  add_test(NAME app-view-rules COMMAND oa-app-view-rules-test)
endif()
set(oa_window_icon_png "${CMAKE_CURRENT_SOURCE_DIR}/branding/open-annihilation-256.png")
set(oa_window_icon_source "${CMAKE_CURRENT_BINARY_DIR}/generated/window_icon_png.cpp")
add_custom_command(OUTPUT "${oa_window_icon_source}"
  COMMAND ${CMAKE_COMMAND} "-DINPUT=${oa_window_icon_png}" "-DOUTPUT=${oa_window_icon_source}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OaEmbedWindowIcon.cmake"
  DEPENDS "${oa_window_icon_png}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OaEmbedWindowIcon.cmake"
  COMMENT "Embedding the window icon"
  VERBATIM)
add_library(oa-app-window-icon src/app/window_icon.cpp "${oa_window_icon_source}")
add_library(oa::app::window_icon ALIAS oa-app-window-icon)
target_link_libraries(oa-app-window-icon PUBLIC oa-app-headers PRIVATE oa-formats-png oa-options)
# The Full tier's fog of war on the card (full_fog.hpp): the fog grid's
# tiles as passes of quads with corner alphas, the greyed terrain pass from
# the greyed pages, its dithered form and the black pass; pure, so that
# runtime_full.cpp draws from it and a test checks it alone.
add_library(oa-app-full-fog src/app/full_fog.cpp)
add_library(oa::app::full_fog ALIAS oa-app-full-fog)
target_include_directories(oa-app-full-fog PUBLIC src/app)
target_link_libraries(oa-app-full-fog PUBLIC oa-app-card oa-present-gpu-world
  oa-present-world-renderer PRIVATE oa-options)
if(BUILD_TESTING)
  # The fog passes over a seeded map's atlas and fog grids built over
  # synthetic sight: the greyed pass covering the ground out of sight once,
  # whole tiles and quarters at their texels and corner alphas, at two
  # levels and through a placement; the black and dithered passes with
  # their runs merged; the refusals.
  add_executable(oa-app-full-fog-test src/app/full_fog_test.cpp)
  target_link_libraries(oa-app-full-fog-test PRIVATE oa-app-full-fog oa-sim-visibility-state
    oa-test-support oa-options)
  add_test(NAME app-full-fog COMMAND oa-app-full-fog-test)
  set_tests_properties(app-full-fog PROPERTIES TIMEOUT 60)
endif()
# The render policy (render_policy.hpp): the walk of SDL's render drivers,
# the tier each frame is drawn in, what a crash or a failure counts for at
# the next start and the records that keep it, the step-down ladder, the
# chrome's filter and the tiles of a texture larger than the renderer
# allows, as pure functions with no SDL and no files; with the memory guard
# of the accelerated tier (memory_guard.hpp), the samples of the system's
# memory judged against thresholds scaled to physical memory, as a pure
# state machine with no clock. The game uses its walk of the render
# drivers, with the drivers the records hold failed, its texture limit,
# the tier each frame is drawn in, the step-down and the memory guard.
add_library(oa-app-render-policy src/app/render_policy.cpp src/app/memory_guard.cpp)
add_library(oa::app::render_policy ALIAS oa-app-render-policy)
target_link_libraries(oa-app-render-policy PUBLIC oa-app-headers oa-platform-shims
  PRIVATE oa-options)
# The Full tier's terrain builder (full_terrain.hpp): the level rule for a
# zoom and the terrain tiles' quads from a terrain atlas, appended to a card
# command list; pure, so that runtime_full.cpp draws from it and a test
# holds it without a renderer.
add_library(oa-app-full-terrain src/app/full_terrain.cpp)
add_library(oa::app::full_terrain ALIAS oa-app-full-terrain)
target_include_directories(oa-app-full-terrain PUBLIC src/app)
target_link_libraries(oa-app-full-terrain PUBLIC oa-app-card oa-present-gpu-world
  PRIVATE oa-options)
if(BUILD_TESTING)
  # The level rule at every kind of zoom; the tiles' quads over a small map
  # on three pages: places, texels, batches, the range within the map and
  # the frame well formed.
  add_executable(oa-app-full-terrain-test src/app/full_terrain_test.cpp)
  target_link_libraries(oa-app-full-terrain-test PRIVATE oa-app-full-terrain oa-test-support
    oa-options)
  add_test(NAME app-full-terrain COMMAND oa-app-full-terrain-test)
  set_tests_properties(app-full-terrain PROPERTIES TIMEOUT 60)
endif()
# The Full tier's anti-aliasing (full_supersampling.hpp): how a frame at a
# zoom is drawn through the world target at a supersample factor and
# reduced to the window, by the factor's halvings from zoom 1 up and by the
# two-level blend of a part drawn at zoom 1 below it; pure, so that
# runtime_full.cpp draws by it and the render tiers check reads back by it.
add_library(oa-app-full-supersampling src/app/full_supersampling.cpp)
add_library(oa::app::full_supersampling ALIAS oa-app-full-supersampling)
target_include_directories(oa-app-full-supersampling PUBLIC src/app)
target_link_libraries(oa-app-full-supersampling PUBLIC oa-app-card PRIVATE oa-options)
if(BUILD_TESTING)
  # No target at a factor of 1; the whole texture resolved from zoom 1 up;
  # below zoom 1 an even part within the texture reduced by the two-level
  # blend at a scale within one half to 1, the destination covering the
  # battlefield, over battlefields of either parity and zooms across the
  # range, each reduction batch well formed.
  add_executable(oa-app-full-supersampling-test src/app/full_supersampling_test.cpp)
  target_link_libraries(oa-app-full-supersampling-test PRIVATE oa-app-full-supersampling
    oa-test-support oa-options)
  add_test(NAME app-full-supersampling COMMAND oa-app-full-supersampling-test)
  set_tests_properties(app-full-supersampling PROPERTIES TIMEOUT 60)
endif()
# What the settings dialog says of the renderer (acceleration_status.hpp):
# Hardware acceleration's status and locks and Vertical sync's lock, from
# what the run knows of its renderer, its machine and the match.
add_library(oa-app-acceleration-status src/app/acceleration_status.cpp)
add_library(oa::app::acceleration_status ALIAS oa-app-acceleration-status)
target_link_libraries(oa-app-acceleration-status PUBLIC oa-app-headers oa-ui-engine-settings
  oa-app-render-policy PRIVATE oa-options)
# Painting for the touch controls' layer: RGBA rectangles, the controls'
# icon marks and their text (touch_paint.hpp, docs/touch-controls.md), and
# the gamepad's button glyphs (pad_glyphs.hpp, docs/controllers.md).
add_library(oa-app-touch-paint src/app/touch_paint.cpp src/app/pad_glyphs.cpp)
add_library(oa::app::touch_paint ALIAS oa-app-touch-paint)
target_compile_features(oa-app-touch-paint PUBLIC cxx_std_20)
# The text is drawn with the fonts that travel with the game.
target_link_libraries(oa-app-touch-paint PUBLIC oa-platform-text-font oa-ui-pad-controls PRIVATE oa-options)
if(BUILD_TESTING)
  # Rectangles clipped to the canvas, the icon marks and the text.
  add_executable(oa-app-touch-paint-test src/app/touch_paint_test.cpp)
  target_link_libraries(oa-app-touch-paint-test PRIVATE oa-app-touch-paint oa-test-support oa-options)
  # The text checks read the bundled fonts beside the test, as the game does.
  oa_copy_text_fonts(oa-app-touch-paint-test "$<TARGET_FILE_DIR:oa-app-touch-paint-test>")
  add_test(NAME app-touch-paint COMMAND oa-app-touch-paint-test)
endif()
if(OA_BUILD_PLATFORM)
  # What the platform the game runs on provides beyond SDL (haptics, the
  # default game folder and its advice, the window's opening, and bringing
  # game files in), which a platform's extension init fills, and the touch
  # and pen hints the touch controls read input with (platform_hooks.hpp,
  # game_files_hooks.hpp, input_hints.hpp).
  add_library(oa-app-platform-hooks src/app/platform_hooks.cpp src/app/input_hints.cpp
    src/app/game_files_hooks.cpp)
  add_library(oa::app::platform_hooks ALIAS oa-app-platform-hooks)
  target_link_libraries(oa-app-platform-hooks PUBLIC oa-app-headers PRIVATE SDL3::SDL3 oa-options)
endif()
# The developer's video capture (--capture-video): presented frames and the
# mix SDL plays, encoded by the ffmpeg program (docs/capture.md).
if(OA_BUILD_PLATFORM)
  add_library(oa-app-video-capture src/app/video_capture.cpp)
  add_library(oa::app::video_capture ALIAS oa-app-video-capture)
  target_link_libraries(oa-app-video-capture PUBLIC oa-app-headers SDL3::SDL3 PRIVATE oa-options)
  # The game's renderer (render_host.hpp): made by walking SDL's render
  # drivers in SDL's own order, skipping those the renderer records hold
  # failed, logging each refusal, with the framebuffer hint set before SDL's
  # software renderer, or by SDL's own call under SDL_RENDER_DRIVER; then
  # described, its first frame's tier decided after the start-up function
  # test, guarded by its trial record, where it could be accelerated, and
  # logged (graphics_report.hpp); the records and the sentinel kept through
  # the run;
  # made again after a failure while the game runs; its window-size
  # textures, as tiles beyond its texture limit, and the accelerated tier's
  # drawing on the card (scaled_world.hpp), with the frames' conversion for
  # the textures (xrgb_conversion.hpp).
  add_library(oa-app-render-host src/app/render_host.cpp src/app/graphics_report.cpp
    src/app/scaled_world.cpp src/app/xrgb_conversion.cpp)
  add_library(oa::app::render_host ALIAS oa-app-render-host)
  target_link_libraries(oa-app-render-host PUBLIC oa-app-headers oa-app-render-policy
    oa-app-renderer-state oa-platform-render-probe oa-ui-engine-settings
    oa-app-acceleration-status
    oa-platform-job-pool oa-present-world-renderer PRIVATE oa-platform-shims
    oa-base-float-precision SDL3::SDL3 oa-options)
  # The Full tier's model stage (runtime_full.hpp): the planner's units, 3D
  # features, projectiles, debris pieces and shatter fragments as triangles
  # the card draws from the models' meshes and the texture pages, with the
  # palette's tables approximated as the design says, and their shadows
  # into a shadow target composed once at half darkness; and the comparison
  # of such a frame with the processor's raster, for the checks. Nothing of
  # the match reaches it yet but the render tiers check.
  add_library(oa-app-full-models src/app/runtime_full_models.cpp)
  add_library(oa::app::full_models ALIAS oa-app-full-models)
  target_link_libraries(oa-app-full-models PUBLIC oa-app-card-executor
    oa-present-gpu-world oa-present-model
    oa-present oa-present-world-renderer oa-formats-gaf oa-sim-effect-particles
    oa-sim-model-runtime oa-core-types PRIVATE oa-base-game-math oa-options)
  if(BUILD_TESTING)
    # The model stage on SDL's software renderer over a transparent target,
    # read back against the processor's raster of the same list drawn
    # through the bridge: synthetic scenes of flat and textured models at
    # rest and turned, a depth-sorted unit, a lit building and its bright
    # page, a cloaked unit, a unit under construction, units under water
    # of both sides, carried units, shadows into the target and straight
    # into the frame, shadows lighter at zoom 0.5 and none at a quarter on
    # both sides, a projectile, a debris piece and a fragment, a
    # back-facing polygon left out, the pages placed once and uploaded, and
    # a frame at zoom 2; with --data, every unit model of the installed
    # game at zoom 1, drawn from a cached image with and without a depth
    # plane, held to the bounds the figures print.
    add_executable(oa-app-full-models-test src/app/runtime_full_models_test.cpp
      src/app/world_draws.cpp)
    target_link_libraries(oa-app-full-models-test PRIVATE oa-app-full-models oa-formats-hpi
      oa-formats-tdf oa-platform-job-pool SDL3::SDL3 oa-test-game-data oa-test-support
      oa-options)
    add_test(NAME app-full-models COMMAND oa-app-full-models-test)
    set_tests_properties(app-full-models PROPERTIES TIMEOUT 180
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    add_test(NAME app-full-models-data COMMAND oa-app-full-models-test --data)
    set_tests_properties(app-full-models-data PROPERTIES TIMEOUT 900
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    oa_game_data_tests(app-full-models-data)
    add_executable(oa-app-video-capture-test src/app/video_capture_test.cpp)
    target_link_libraries(oa-app-video-capture-test PRIVATE oa-app-video-capture oa-options)
    add_test(NAME app-video-capture COMMAND oa-app-video-capture-test)
    # The walk of the render drivers through stand-in hooks: SDL's order,
    # one driver at a time, every refusal logged, the first driver that
    # starts kept, recorded drivers skipped but never software, the
    # framebuffer hint before software, a hint not taken logged, a second
    # walk when nothing could present, and SDL's own call alone under
    # SDL_RENDER_DRIVER. Then on the dummy video driver: the walk ends on
    # the renderer SDL's own choice makes, with the hint set; a named
    # driver that does not exist ends the start with one attempt and no
    # hint; software named is made with no hint; the start-up function test
    # passes on SDL's software renderer; the start's tier, logged with its
    # reason, accelerated only with --hardware-acceleration and
    # --force-capable from 2 GiB; and a rebuild drops the tier. The renderer
    # records in scratch folders: the trial before the function test, the
    # sentinel, a lost device recorded, a clean end, and a profile that
    # cannot be written. app-render-host-env names the missing driver
    # through the environment variable itself.
    add_executable(oa-app-render-host-test src/app/render_host_test.cpp)
    target_link_libraries(oa-app-render-host-test PRIVATE oa-app-render-host SDL3::SDL3
      oa-platform-preferences oa-test-support oa-options)
    add_test(NAME app-render-host COMMAND oa-app-render-host-test)
    set_tests_properties(app-render-host PROPERTIES TIMEOUT 60
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
      ENVIRONMENT_MODIFICATION "SDL_RENDER_DRIVER=unset:;SDL_FRAMEBUFFER_ACCELERATION=unset:")
    add_test(NAME app-render-host-env COMMAND oa-app-render-host-test --case named-missing)
    set_tests_properties(app-render-host-env PROPERTIES TIMEOUT 60
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=missing"
      ENVIRONMENT_MODIFICATION "SDL_FRAMEBUFFER_ACCELERATION=unset:")
    # Window-size textures beyond the renderer's texture limit, as tiles with
    # gutters, on SDL's software renderer over a surface: tiles under a
    # limit of 1024 read back as one texture with no limit does, uploaded,
    # updated, blended and drawn in part. Then the accelerated tier's drawing
    # on the card, on the same renderer: the battlefield's scene magnified
    # and the interface scaled, read back against that renderer's own
    # LINEAR, modelled on the processor, within 2 levels; NEAREST exact at
    # whole-number scales; PIXELART as NEAREST, as that renderer gives it;
    # scenes and prescale targets in tiles with their gutters, drawing as
    # one texture does; the overlay over a picture; no prescale target
    # made or drawn again needlessly; and the menus' frame under each way of
    # Menu scaling, its rectangle whole, centred and where it is drawn, and
    # the pointer mapped onto it there.
    add_executable(oa-app-scaled-world-test src/app/scaled_world_test.cpp)
    target_link_libraries(oa-app-scaled-world-test PRIVATE oa-app-render-host
      oa-present-world-renderer SDL3::SDL3 oa-test-support oa-options)
    add_test(NAME app-scaled-world-software COMMAND oa-app-scaled-world-test)
    set_tests_properties(app-scaled-world-software PROPERTIES TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    # The check host's table (check_host.hpp) and the parts of it that need
    # no running game: the pointer events it makes, the gadgets it finds and
    # the text it hands back.
    add_executable(oa-app-check-host-test src/app/check_host_test.cpp src/app/check_host_input.cpp)
    target_link_libraries(oa-app-check-host-test PRIVATE oa-app-headers oa-ui-gui-layout
      oa-ui-screen-registry SDL3::SDL3 oa-options)
    add_test(NAME app-check-host COMMAND oa-app-check-host-test)
    # The Full tier's sprite stage (runtime_full.hpp) on SDL's software
    # renderer over a surface: a synthetic scene of sprites, blended
    # sprites, particle squares, lines and selection lines in one list,
    # drawn by the card from the sprite pages and by the bands on the
    # processor; the card's picture equals the processor's exactly where
    # sprites are opaque, at zooms 1 and 2, and within 2 levels where the
    # alpha table blends them, with the lines held to the game's lines and
    # the squares exact; the stage's order, batches, fog states (a sprite
    # under a cell out of sight greyed, under a never-mapped cell left out),
    # a feature's frame cut off where the map the view shows ends, the
    # refusals, the pages overflowing a frame, a pinned digest of the
    # frame, and a feature's shadow frames, drawn and blended, as dark as
    # the list's shadow level on both sides and left out at level 0. With
    # --data, a scene of the installed game's GAF frames
    # through its palette and alpha table.
    add_executable(oa-app-full-sprites-test src/app/full_sprites_test.cpp
      src/app/runtime_full_sprites.cpp src/app/world_draws.cpp)
    target_link_libraries(oa-app-full-sprites-test PRIVATE oa-app-card-executor
      oa-present-gpu-world oa-present oa-present-model oa-present-world-renderer
      oa-formats-gaf oa-formats-hpi oa-sim-effect-particles oa-sim-model-runtime
      oa-platform-job-pool SDL3::SDL3 oa-test-game-data oa-test-support oa-options)
    add_test(NAME app-full-sprites COMMAND oa-app-full-sprites-test)
    set_tests_properties(app-full-sprites PROPERTIES TIMEOUT 120
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    add_test(NAME app-full-sprites-data COMMAND oa-app-full-sprites-test --data)
    set_tests_properties(app-full-sprites-data PROPERTIES TIMEOUT 300
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    oa_game_data_tests(app-full-sprites-data)
  endif()
endif()
# The files a director render writes (--render-script), the ffmpeg runs that
# encode them, and the director bundle (.oamovie) that carries a script with
# its recording (director_output.hpp, docs/director.md).
if(OA_BUILD_PLATFORM)
  add_library(oa-app-director-output src/app/director_output.cpp)
  add_library(oa::app::director_output ALIAS oa-app-director-output)
  target_link_libraries(oa-app-director-output PUBLIC oa-app-headers oa-formats-zip oa-formats-oascript
    oa-base-sha256 SDL3::SDL3 PRIVATE oa-platform-shims oa-audio-offline-mix oa-base-threads
    oa-formats-png oa-options)
  if(BUILD_TESTING)
    # File names, encoder arguments, the files of a render with encoding
    # off, stills, the environment's settings and bundles, malformed ones
    # included.
    add_executable(oa-app-director-output-test src/app/director_output_test.cpp)
    target_link_libraries(oa-app-director-output-test PRIVATE oa-app-director-output
      oa-audio-offline-mix oa-formats-png oa-formats-zip oa-base-sha256 oa-options)
    add_test(NAME app-director-output COMMAND oa-app-director-output-test)
  endif()
endif()
if(BUILD_TESTING)
  add_executable(oa-app-window-icon-test src/app/window_icon_test.cpp)
  target_link_libraries(oa-app-window-icon-test PRIVATE oa-app-window-icon oa-formats-png oa-options)
  add_test(NAME app-window-icon COMMAND oa-app-window-icon-test)
  add_executable(oa-game-options-test src/app/startup_test.cpp)
  target_link_libraries(oa-game-options-test PRIVATE oa-app-game-options oa-options)
  add_test(NAME game-options COMMAND oa-game-options-test)
  # A folder's profile found without case, the profile printed with its
  # hashes, and the errors of one the engine cannot use.
  add_executable(oa-app-mod-profile-test src/app/mod_profile_loader_test.cpp)
  target_link_libraries(oa-app-mod-profile-test PRIVATE oa-app-mod-profile oa-test-support oa-options)
  add_test(NAME app-mod-profile COMMAND oa-app-mod-profile-test)
  # The Mods page's rows over scratch folders: a profile's name, version and
  # description, a folder without a profile or with a damaged one, and
  # badges of each colour type, scaled down, damaged or too large.
  add_executable(oa-app-mod-summary-test src/app/mod_summary_test.cpp)
  target_link_libraries(oa-app-mod-summary-test PRIVATE oa-app-mod-profile oa-formats-png oa-test-support
    oa-options)
  add_test(NAME app-mod-summary COMMAND oa-app-mod-summary-test)
  # The player's own folder in scratch folders: where it is, each mod's
  # folders, a folder mod's without a profile and an Image Output Directory
  # the player chose, the saved games moved once and nothing overwritten,
  # renames and copies refused, the records of the moves and the notice
  # they give; and what keeps a mod's games from starting, and the warning
  # that says so.
  add_executable(oa-app-user-folder-test src/app/user_folder_test.cpp)
  target_link_libraries(oa-app-user-folder-test PRIVATE oa-app-user-folder oa-app-mod-profile
    oa-test-support oa-options)
  add_test(NAME app-user-folder COMMAND oa-app-user-folder-test)
  add_executable(oa-game-directory-test src/app/game_directory_test.cpp)
  target_link_libraries(oa-game-directory-test PRIVATE oa-app-game-directory oa-app-headers oa-formats-hpi
    oa-platform-preferences oa-platform-shims oa-test-game-data oa-options)
  add_test(NAME game-directory COMMAND oa-game-directory-test)
  # A profile names installation archives: a file resolves from the mod's
  # archive, then the named archive, then the base archive, the same for a
  # mod folder and a copied install. A missing named archive makes the
  # folder unusable.
  add_executable(oa-app-installation-archives-test src/app/installation_archives_test.cpp)
  target_link_libraries(oa-app-installation-archives-test PRIVATE oa-app-game-directory
    oa-formats-hpi oa-test-support oa-options)
  add_test(NAME app-installation-archives COMMAND oa-app-installation-archives-test)
  # The installation OA_GAME_DIR names passes the folder dialog's check.
  oa_add_game_data_test(game-directory-install oa-game-directory-test --install)
  # The demo's installer recognised and its archive unpacked, over a
  # synthetic installer; with --data, over the one OA_DEMO_INSTALLER names.
  add_executable(oa-demo-installer-test src/app/demo_installer_test.cpp)
  target_link_libraries(oa-demo-installer-test PRIVATE oa-app-game-directory oa-app-headers oa-base-sha256
    oa-formats-hpi oa-formats-pe-test-image oa-test-game-data oa-options)
  add_test(NAME demo-installer COMMAND oa-demo-installer-test)
  add_test(NAME demo-installer-data COMMAND oa-demo-installer-test --data)
  set_tests_properties(demo-installer-data PROPERTIES TIMEOUT 300)
  oa_demo_installer_tests(demo-installer-data)
  # The import core over synthetic folders: the leave-out table, the name
  # check, the plan and its space, the copy, stopping and resuming, the
  # commit, recovery, adopting and the additions and removals; with --data,
  # a subset of OA_GAME_DIR and the demo route over OA_DEMO_INSTALLER.
  add_executable(oa-app-game-files-import-test src/app/game_files_import_test.cpp)
  target_link_libraries(oa-app-game-files-import-test PRIVATE oa-app-game-files oa-test-support
    oa-test-game-data oa-options)
  add_test(NAME app-game-files-import COMMAND oa-app-game-files-import-test)
  oa_add_game_data_test(app-game-files-import-data oa-app-game-files-import-test --data)
  set_tests_properties(app-game-files-import-data PROPERTIES TIMEOUT 300)
  oa_demo_installer_tests(app-game-files-import-data)
  # The renderer records by table: the text of every key and value, values
  # that cannot be read, strikes that become records and what clears them,
  # and the native-density key a run writes or erases as it ends.
  add_executable(oa-app-renderer-records-test src/app/renderer_records_test.cpp)
  target_link_libraries(oa-app-renderer-records-test PRIVATE oa-app-renderer-records oa-test-support
    oa-options)
  add_test(NAME app-renderer-records COMMAND oa-app-renderer-records-test)
  # The renderer records' two files in scratch folders: missing, garbled
  # and read-only ones, writes made only on a change, the trial and the
  # sentinel.
  add_executable(oa-app-renderer-state-test src/app/renderer_state_test.cpp)
  target_link_libraries(oa-app-renderer-state-test PRIVATE oa-app-renderer-state oa-platform-preferences
    oa-test-support oa-options)
  add_test(NAME app-renderer-state COMMAND oa-app-renderer-state-test)
  # The draw states the match renderer keeps per unit slot and per 3D feature.
  add_executable(oa-match-model-draws-test src/app/match_model_draws_test.cpp)
  target_link_libraries(oa-match-model-draws-test PRIVATE oa-app-headers oa-present-model oa-sim-feature-runtime
    oa-options)
  add_test(NAME match-model-draws COMMAND oa-match-model-draws-test)
  # What frames between two ticks show: the blends of places, angles and
  # piece poses, the poses each tick leaves, and projectiles and debris
  # followed from one tick to the next.
  add_executable(oa-app-presentation-interpolation-test src/app/presentation_interpolation_test.cpp
    src/app/presentation_interpolation.cpp)
  target_link_libraries(oa-app-presentation-interpolation-test PRIVATE oa-app-headers
    oa-present-model oa-sim-effect-particles oa-options)
  add_test(NAME app-presentation-interpolation COMMAND oa-app-presentation-interpolation-test)
  # The settings dialog's word on the renderer: Hardware acceleration's
  # status for the machine's memory against the 2 GiB threshold, the
  # setting, the flags, the environment's driver, a shared game or a replay
  # and the renderer, looked at or not, lacking a feature or failed in the
  # run, and in use at a budget; its lock; what the graphics card does at
  # each rung; and Vertical sync's lock on SDL's software renderer and on a
  # renderer whose device it resets.
  add_executable(oa-app-acceleration-status-test src/app/acceleration_status_test.cpp)
  target_link_libraries(oa-app-acceleration-status-test PRIVATE oa-app-acceleration-status
    oa-test-support oa-options)
  add_test(NAME app-acceleration-status COMMAND oa-app-acceleration-status-test)
  # When the match clock steps: menus, the outcome, the pause bit, a frame
  # behind the last step and frames across the turn of the clock's reading.
  add_executable(oa-app-match-clock-test src/app/match_clock_test.cpp src/app/match_clock.cpp)
  target_link_libraries(oa-app-match-clock-test PRIVATE oa-base-game-loop oa-ui-console oa-options)
  add_test(NAME app-match-clock COMMAND oa-app-match-clock-test)
  # The memory guard by table: its thresholds, its once-a-second samples,
  # committed memory, free memory held low for 3 s, the memory pressure and
  # hard page faults standing in for it, and the check before a new buffer.
  add_executable(oa-app-memory-guard-test src/app/memory_guard_test.cpp)
  target_link_libraries(oa-app-memory-guard-test PRIVATE oa-app-render-policy oa-test-support
    oa-options)
  add_test(NAME app-memory-guard COMMAND oa-app-memory-guard-test)
  # How a frame draws the battlefield: without a draw scale, and at the
  # zoom, the scene is the battlefield; at another scale it covers the
  # battlefield's map pixels with a margin, in even sizes, and holds every
  # pixel the nearest resample reads over the zoom range; on a window at
  # native density the card draws it at the zoom times the density.
  add_executable(oa-app-world-scaling-test src/app/world_scaling_test.cpp src/app/world_scaling.cpp)
  target_link_libraries(oa-app-world-scaling-test PRIVATE oa-app-headers oa-present-world-renderer
    oa-app-render-policy oa-test-support oa-options)
  add_test(NAME app-world-scaling COMMAND oa-app-world-scaling-test)
  # The view's zoom floor for each Maximum zoom out choice and the whole
  # map's fit on maps and battlefields of every shape; the far view's
  # terrain pyramid against the means of the map pixels it stands for, the
  # level each zoom reads, the filter against a sum over the texels and the
  # same on one and on several drawing threads; and the units' dots, framed
  # and clipped.
  add_executable(oa-app-far-view-test src/app/far_view_test.cpp src/app/far_view.cpp)
  target_link_libraries(oa-app-far-view-test PRIVATE oa-app-headers oa-formats-tnt oa-formats-hpi
    oa-platform-job-pool oa-ui-engine-settings oa-test-support oa-options)
  add_test(NAME app-far-view COMMAND oa-app-far-view-test)
  # The battlefield's thin lines drawn band by band: lines of the frame and
  # selection lines of the model bridge, 2 pixels thick as a reduced scene
  # draws them, cover the pixels a square pen gives across the bands' edges,
  # the same on 1 to 8 bands and drawing threads; 1 pixel thick, the line's
  # own pixels. A feature's shadow frame mixed toward the game's shadow by
  # the frame's shadow level and left out at 0, and a frame's shadows set
  # from its zoom. An explosion's flash lighting the entry under each pixel
  # through the light table's row the pixel names, flashes compounding, at
  # half the row when reduced and on 1 to 8 bands alike. A projectile's lens
  # moving the pixels its rule names, as the 8-bit lens sprite does, at
  # scales 1 to 3 and within the clip, its battlefield test, and the frame
  # point it is drawn at, raised by half an odd height rounded down where
  # other things round to the nearest pixel; an explosion record's flash
  # and sprite placed the same way, on the point the record is culled on.
  # A file over the decoded threshold drawn a frame at a time, its frames
  # rendered from its bytes through the frame cache: its budget, the frame
  # drawn longest ago going first, a frame over the budget drawn without
  # being kept, and a failed frame not read again.
  add_executable(oa-app-world-draws-test src/app/world_draws_test.cpp src/app/world_draws.cpp)
  target_link_libraries(oa-app-world-draws-test PRIVATE oa-present oa-present-model
    oa-present-world-renderer oa-formats-gaf oa-sim-effect-particles oa-sim-model-runtime
    oa-platform-job-pool oa-test-support oa-options)
  add_test(NAME app-world-draws COMMAND oa-app-world-draws-test)
  # The application loop's pacing over a fake clock: frames evenly spaced
  # at the rate asked for, 30 ticks a second at any frame rate, the fraction
  # of a tick each frame shows, the cap just below the display's rate while
  # Vertical sync is in effect, the camera's scroll for a frame's real time,
  # and the +stats overlay's figures, frame history, grades and table.
  add_executable(oa-app-frame-pacing-test src/app/frame_pacing_test.cpp src/app/frame_pacing.cpp)
  target_link_libraries(oa-app-frame-pacing-test PRIVATE oa-app-headers oa-base-game-loop oa-ui-hud
    oa-options)
  add_test(NAME app-frame-pacing COMMAND oa-app-frame-pacing-test)
  # The +stats overlay's panel: its rows, columns, notes and graph laid out
  # over texts of known widths and kept in place whatever the figures, its
  # place and scale on windows of every size, the height of each frame's
  # bar and the colour of each grade.
  add_executable(oa-app-frame-stats-panel-test src/app/frame_stats_panel_test.cpp
    src/app/frame_stats_panel.cpp src/app/frame_pacing.cpp)
  target_link_libraries(oa-app-frame-stats-panel-test PRIVATE oa-app-headers oa-base-game-loop
    oa-ui-hud oa-ui-display-layout oa-options)
  add_test(NAME app-frame-stats-panel COMMAND oa-app-frame-stats-panel-test)
  # A screen size applied to the window at once, over made-up windows: how
  # full screen shows a size on each window system, the frame scaled to the
  # screen, and the steps for a window, a maximised window, full screen at a
  # mode of the size or on the desktop's mode, a mode the display refuses,
  # and Desktop.
  add_executable(oa-app-screen-mode-test src/app/screen_mode_test.cpp src/app/screen_mode.cpp)
  target_link_libraries(oa-app-screen-mode-test PRIVATE oa-platform-display-modes oa-test-support
    oa-options)
  add_test(NAME app-screen-mode COMMAND oa-app-screen-mode-test)
  # The render policy's decisions by table: the tier over every input, the
  # driver walk and its framebuffer hint, strikes and records, their values
  # written and read back, the sentinel and the trial through a run, present
  # stalls, the starting rung and the step-down fed synthetic frames, the
  # window's density, the chrome's filter at the display's scale, the
  # prescale budget and the tile grids; and acting on
  # the tier as the game does: a player's run through the setting, a shared
  # game and a failed function test, beside the runs that stay standard.
  add_executable(oa-app-render-policy-test src/app/render_policy_test.cpp)
  target_link_libraries(oa-app-render-policy-test PRIVATE oa-app-render-policy oa-test-support oa-options)
  add_test(NAME app-render-policy COMMAND oa-app-render-policy-test)
  # The tests that link SDL, when it was found.
  if(TARGET SDL3::SDL3)
    # The frame's conversion into the window's 32-bit pixels: each pixel
    # packed through the gamma table, rows of any width, and the same bytes
    # on pools of 2, 3, 4 and 8 threads as on the calling thread alone.
    add_executable(oa-app-xrgb-conversion-test src/app/xrgb_conversion_test.cpp
      src/app/xrgb_conversion.cpp)
    target_link_libraries(oa-app-xrgb-conversion-test PRIVATE oa-platform-job-pool SDL3::SDL3
      oa-options)
    add_test(NAME app-xrgb-conversion COMMAND oa-app-xrgb-conversion-test)
    # The window's flags, at native density among them, and the edge
    # scroll's depth there; Alt+Enter: the key, the mode it asks for while
    # the window is still switching, and a window of SDL's dummy video
    # driver switched and back;
    # the pointer held on the window in full screen with the focus and let go
    # in a window or without the focus; a window that leaves full screen
    # brought back onto its display, at a size asked for, and a window asked
    # for a size kept on its display.
    add_executable(oa-app-full-screen-test src/app/full_screen_test.cpp src/app/full_screen.cpp)
    target_link_libraries(oa-app-full-screen-test PRIVATE oa-app-headers SDL3::SDL3 oa-options)
    add_test(NAME app-full-screen COMMAND oa-app-full-screen-test)
    set_tests_properties(app-full-screen PROPERTIES TIMEOUT 60
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
    # The texture limit the render policy corrects from the probe's report,
    # the line the game logs once its renderer is made, naming the driver,
    # the adapter, the texture limit and the tier with what it does or why
    # the processor draws everything, what the render policy reads of a
    # renderer, and the adapter the +stats overlay names; SDL's software
    # renderer on the dummy video driver reported as the game reports it.
    add_executable(oa-app-graphics-report-test src/app/graphics_report_test.cpp
      src/app/graphics_report.cpp)
    target_link_libraries(oa-app-graphics-report-test PRIVATE oa-platform-render-probe
      oa-app-render-policy oa-ui-engine-settings SDL3::SDL3 oa-test-support oa-options)
    add_test(NAME app-graphics-report COMMAND oa-app-graphics-report-test)
    set_tests_properties(app-graphics-report PROPERTIES TIMEOUT 60
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
      ENVIRONMENT_MODIFICATION "SDL_RENDER_DRIVER=unset:")
    # An extension opens an http or https address through a stub opener, and
    # every other scheme is refused. Availability follows the machine.
    add_executable(oa-app-web-address-test src/app/web_address_test.cpp src/app/web_address.cpp)
    target_link_libraries(oa-app-web-address-test PRIVATE oa-app-headers oa-platform-shims
      SDL3::SDL3 oa-test-support oa-options)
    add_test(NAME app-web-address COMMAND oa-app-web-address-test)
    # An extension reads one file from the game's files: a loose file wins
    # over the same path in an archive, an archive-only file is read, and a
    # missing path returns false.
    add_executable(oa-app-game-file-test src/app/game_file_test.cpp)
    target_link_libraries(oa-app-game-file-test PRIVATE oa-formats-hpi oa-test-support oa-options)
    add_test(NAME app-read-game-file COMMAND oa-app-game-file-test)
    # An extension reads the modern text faces at a size, the text settings
    # written and read back, and the focused field's place through a stub.
    add_executable(oa-app-extension-text-test src/app/extension_text_test.cpp
      src/app/modern_text.cpp src/app/text_input_area.cpp)
    # It also draws from several threads at once, so it links the threads.
    target_link_libraries(oa-app-extension-text-test PRIVATE oa-app-headers oa-platform-text-font
      oa-present oa-base-threads SDL3::SDL3 oa-test-support oa-options)
    oa_copy_text_fonts(oa-app-extension-text-test "$<TARGET_FILE_DIR:oa-app-extension-text-test>")
    add_test(NAME app-extension-text COMMAND oa-app-extension-text-test)
    set_tests_properties(app-extension-text PROPERTIES TIMEOUT 120)
  endif()
endif()
set(oa_game_sources src/app/main.cpp src/app/full_screen.cpp src/app/screen_size.cpp
  src/app/screen_mode.cpp src/app/runtime_screen_size.cpp
  src/app/game_directory_dialog.cpp src/app/runtime.cpp
  src/app/runtime_builtin_screens.cpp
  src/app/runtime_camera.cpp src/app/runtime_campaign.cpp src/app/runtime_checks.cpp
  src/app/runtime_console.cpp src/app/runtime_console_options.cpp src/app/runtime_console_debug.cpp
  src/app/runtime_console_sound.cpp src/app/runtime_console_display.cpp
  src/app/runtime_console_units.cpp
  src/app/runtime_console_cheats.cpp src/app/runtime_poster.cpp
  src/app/runtime_order_overlays.cpp
  src/app/runtime_events.cpp src/app/runtime_frontend_host.cpp src/app/runtime_gaf.cpp
  src/app/runtime_hotkeys.cpp src/app/runtime_hud.cpp src/app/runtime_loading.cpp
  src/app/runtime_match_hud.cpp src/app/runtime_match_menus.cpp src/app/runtime_load_game.cpp
  src/app/runtime_order_panel.cpp src/app/runtime_order_check.cpp
  src/app/runtime_match_render.cpp src/app/runtime_messages.cpp src/app/runtime_pointer.cpp
  src/app/runtime_present.cpp src/app/runtime_display.cpp
  src/app/runtime_accelerated.cpp src/app/runtime_render_tier.cpp src/app/runtime_tier_watch.cpp
  src/app/runtime_full.cpp src/app/runtime_full_sprites.cpp src/app/runtime_full_overlays.cpp
  src/app/runtime_render_tiers_check.cpp src/app/runtime_smooth_pan_check.cpp
  src/app/runtime_terrain_filter.cpp src/app/runtime_reclaim_check.cpp src/app/runtime_patrol_check.cpp src/app/runtime_fog.cpp
  src/app/runtime_reclaim_cursor_check.cpp src/app/runtime_build_preview_check.cpp
  src/app/runtime_pointer_press.cpp src/app/runtime_pointer_check.cpp
  src/app/runtime_dgun_check.cpp src/app/runtime_attack_command_check.cpp
  src/app/runtime_tracking_zoom_check.cpp
  src/app/runtime_match_panel_keys_check.cpp
  src/app/runtime_turret_draw_check.cpp
  src/app/runtime_selection.cpp src/app/runtime_selection_shortcuts.cpp
  src/app/runtime_view_panels.cpp src/app/runtime_whiteboard.cpp src/app/runtime_commander_placement.cpp
  src/app/runtime_megamap.cpp src/app/runtime_megamap_check.cpp src/app/runtime_radar_orders_check.cpp
  src/app/runtime_skirmish_host.cpp
  src/app/runtime_skirmish_start.cpp src/app/runtime_map_units.cpp src/app/runtime_world_draw.cpp src/app/world_draws.cpp src/app/runtime_benchmark.cpp src/app/runtime_music.cpp
  src/app/runtime_saveload.cpp src/app/runtime_sides.cpp src/app/asset_files.cpp
  src/app/runtime_platform.cpp src/app/runtime_inactive_loop_check.cpp
  src/app/runtime_endgame.cpp src/app/runtime_radar.cpp
  src/app/runtime_factory_check.cpp src/app/runtime_unit_speech_check.cpp
  src/app/runtime_build_page_check.cpp
  src/app/runtime_language_check.cpp
  src/app/runtime_language_switch_check.cpp
  src/app/runtime_side_column_check.cpp
  src/app/runtime_match_bars_check.cpp
  src/app/runtime_unit_pages_check.cpp
  src/app/runtime_kill_board.cpp src/app/runtime_frontend_controls_check.cpp
  src/app/runtime_briefing_narration_check.cpp
  src/app/runtime_build_site_check.cpp src/app/runtime_multiplayer_check.cpp
  src/app/runtime_check_host.cpp src/app/check_host_input.cpp src/app/runtime_automation_host.cpp
  src/app/runtime_notices.cpp src/app/runtime_notice_check.cpp src/app/web_link.cpp
  src/app/web_address.cpp
  src/app/game_file.cpp
  src/app/modern_text.cpp src/app/text_input_area.cpp
  src/app/runtime_user_folder.cpp src/app/runtime_user_folder_check.cpp
  src/app/runtime_mod_switch_check.cpp
  src/app/runtime_mod_warning.cpp src/app/runtime_mod_warning_check.cpp
  src/app/runtime_mod_install.cpp src/app/runtime_mod_install_check.cpp src/app/mod_install_watch.cpp
  src/app/user_folder_open.cpp
  src/app/runtime_mission_orders.cpp src/app/runtime_showcase.cpp src/app/match_clock.cpp
  src/app/world_scaling.cpp src/app/far_view.cpp
  src/app/frame_pacing.cpp src/app/frame_stats_panel.cpp src/app/runtime_frame_stats.cpp
  src/app/runtime_team_panels.cpp src/app/runtime_team_panel_check.cpp
  src/app/runtime_scroll_bars.cpp src/app/runtime_scroll_bar_check.cpp
  src/app/runtime_director_view.cpp src/app/runtime_director.cpp src/app/runtime_stage.cpp
  src/app/presentation_interpolation.cpp src/app/runtime_interpolation_check.cpp
  src/app/runtime_unit_playout_check.cpp
  src/app/runtime_engine_settings.cpp src/app/runtime_engine_settings_wiring_check.cpp
  src/app/runtime_engine_settings_menu.cpp src/app/runtime_engine_settings_check.cpp
  src/app/runtime_engine_settings_match.cpp src/app/runtime_engine_settings_match_check.cpp
  src/app/runtime_engine_settings_app_menu.cpp src/app/runtime_engine_settings_dialog_check.cpp
  src/app/runtime_view_rules.cpp src/app/runtime_game_text.cpp src/app/runtime_language.cpp
  src/app/language_packs.cpp
  src/app/runtime_picture_captions.cpp
  src/app/runtime_renderer.cpp src/app/runtime_renderer_ladder_check.cpp
  src/app/runtime_visual_rules_check.cpp
  src/app/runtime_touch.cpp src/app/runtime_touch_actions.cpp src/app/runtime_touch_camera.cpp
  src/app/runtime_input_modifiers.cpp src/app/runtime_touch_check.cpp src/app/runtime_touch_hud.cpp
  src/app/runtime_phone_hud.cpp src/app/runtime_lifecycle.cpp
  src/app/game_files_screen.cpp src/app/game_files_paint.cpp src/app/game_files_dialog.cpp
  src/app/game_files_check.cpp src/app/runtime_game_files.cpp
  src/app/runtime_pad.cpp src/app/runtime_pad_actions.cpp src/app/runtime_pad_pointer.cpp
  src/app/runtime_pad_haptics.cpp src/app/runtime_pad_check.cpp src/app/runtime_text_input.cpp
  src/app/folder_chooser_screen.cpp src/app/runtime_found_install.cpp
  src/app/simulation_hash.cpp)
# The run manifest of a director render, the settings dialog's header and the
# Game files screen's header name the engine's version, and the renderer
# records are written under it.
set_property(SOURCE src/app/runtime_director.cpp src/app/runtime_engine_settings.cpp
  src/app/main.cpp src/app/game_files_screen.cpp src/app/runtime_game_files.cpp APPEND
  PROPERTY COMPILE_DEFINITIONS "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
set(oa_game_libraries oa-formats-hpi oa-app-game-options oa-app-game-directory oa-app-mod-profile
  oa-app-user-folder
  oa-app-view-rules
  oa-app-window-icon
  oa-app-video-capture oa-app-render-host
  oa-ui-frontend-renderer oa-ui-gui-input oa-ui-frontend-state oa-audio-sdl
  oa-platform-preferences oa-app-command-line oa-formats-tnt oa-sim-unit-spawn-runtime oa-data-mission-types oa-data-limits oa-present-world-renderer oa-sim-combat-state oa-sim-match-runtime oa-sim-unit-effects oa-sim-map-runtime oa-base-game-loop oa-sim-gameplay-input oa-sim-sprite-animation oa-ui-screen-registry oa-options
  oa-ui-hud oa-present-model oa-present-unit-playout oa-ui-frontend oa-ui-campaign oa-sim-session oa-ui-console
  oa-ui-services oa-sim-mission-units oa-sim-match-runtime-mission-units oa-sim-selection oa-sim-messages oa-sim-speed
  oa-data-campaign-assets oa-sim-profile oa-sim-state-hash oa-app-extensions
  oa-ui-engine-settings oa-platform-app-menu oa-platform-text-font oa-platform-job-pool
  oa-platform-render-probe oa-app-render-policy oa-ui-decoded oa-base-bytes oa-base-float-precision oa-base-text
  oa-app-acceleration-status oa-formats-png oa-data-languages oa-formats-oamod oa-platform-locale
  oa-platform-display-modes oa-platform-display-modes-sdl
  oa-app-full-terrain oa-app-full-supersampling oa-app-full-fog oa-app-card-executor
  oa-present-gpu-world oa-app-full-models oa-app-platform-hooks oa-app-touch-paint
  oa-ui-touch-gestures oa-ui-touch-hud oa-app-game-files oa-ui-game-files oa-app-mod-install
  oa-ui-pad-controls oa-ui-folder-chooser oa-platform-game-installs oa-platform-file-types
  oa-ui-frontend-multiplayer)
# The table that combines the extensions' tables of oa-game
# (extension_list.hpp); the extensions themselves are registered with
# oa_add_extension (cmake/OaExtensions.cmake).
add_library(oa-app-extensions src/app/extension_list.cpp)
add_library(oa::app::extensions ALIAS oa-app-extensions)
target_link_libraries(oa-app-extensions PUBLIC oa-app-headers
  PRIVATE oa-app-command-line oa-ui-console oa-ui-hud oa-sim-messages oa-options)
if(BUILD_TESTING)
  # Each rule by which it combines two extensions' hooks.
  add_executable(oa-app-extension-list-test src/app/extension_list_test.cpp)
  target_link_libraries(oa-app-extension-list-test PRIVATE oa-app-extensions oa-app-command-line
    oa-ui-console oa-ui-hud oa-sim-messages oa-options)
  add_test(NAME app-extension-list COMMAND oa-app-extension-list-test)
  # The guarded call of the hooks (hook_call.hpp): a hook of each handling
  # that throws, alone and in the combined table.
  add_executable(oa-app-hook-call-test src/app/hook_call_test.cpp)
  target_link_libraries(oa-app-hook-call-test PRIVATE oa-app-extensions oa-app-command-line
    oa-test-support oa-options)
  add_test(NAME app-hook-call COMMAND oa-app-hook-call-test)
  # The files a language pack answers for the game data's language folders:
  # a briefing under camps/briefs-<word> of its own word, and no other path.
  add_executable(oa-app-language-packs-test src/app/language_packs_test.cpp
    src/app/language_packs.cpp)
  target_link_libraries(oa-app-language-packs-test PRIVATE oa-app-headers oa-data-languages
    oa-formats-oamod oa-formats-tdf oa-test-support oa-options)
  add_test(NAME app-language-packs COMMAND oa-app-language-packs-test
    ${CMAKE_CURRENT_SOURCE_DIR}/src/data/languages/tests)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaExtensions.cmake")
# Without SDL3's intro player, oa-game links the one that skips each movie
# with one logged line.
if(TARGET oa-media-intro-player)
  set(oa_game_intro_player oa-media-intro-player)
else()
  set(oa_game_intro_player oa-media-intro-player-null)
endif()
if(OA_BUILD_PLATFORM)
  add_executable(oa-game ${oa_game_sources})
  # The game players start is open-annihilation (open-annihilation.exe on
  # Windows); the target keeps its name, and tests reach the file through
  # $<TARGET_FILE:oa-game>. On macOS it is the application bundle
  # open-annihilation.app, and the files that travel with it go in the
  # folder its OA_GAME_FILES_DIR property names (cmake/OaGameBundle.cmake).
  set_target_properties(oa-game PROPERTIES OUTPUT_NAME open-annihilation)
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaGameBundle.cmake")
  oa_game_bundle(oa-game)
  if(WIN32)
    # A Windows program, so a game started from Explorer opens no console
    # window; its output goes to the log folder, or to whatever captures it.
    set_target_properties(oa-game PROPERTIES WIN32_EXECUTABLE ON)
    # open-annihilation.exe's icon and version information, and its manifest,
    # which lets it open paths longer than 259 characters where Windows has
    # long paths turned on. Visual Studio's linker merges the manifest into
    # the one it writes; other toolchains take it as a resource.
    set(OA_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/branding/open-annihilation.ico")
    set(oa_windows_manifest "${CMAKE_CURRENT_SOURCE_DIR}/src/app/open-annihilation.manifest")
    if(MSVC)
      set(OA_WINDOWS_MANIFEST_RESOURCE "")
      target_sources(oa-game PRIVATE "${oa_windows_manifest}")
    else()
      # Resource 1 of type 24: the manifest Windows reads as it starts the
      # program.
      set(OA_WINDOWS_MANIFEST_RESOURCE "1 24 \"${oa_windows_manifest}\"")
    endif()
    configure_file(src/app/open-annihilation.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/open-annihilation.rc" @ONLY)
    target_sources(oa-game PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/open-annihilation.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/open-annihilation.rc" APPEND PROPERTY
      OBJECT_DEPENDS "${oa_windows_manifest}")
  endif()
  target_link_libraries(oa-game PRIVATE oa-ui-display-layout oa-ui-gadget-render)
  target_link_libraries(oa-game PRIVATE oa-media-director oa-media-director-planner oa-audio-offline-mix
    oa-formats-oascript oa-formats-zip oa-app-director-output)
  target_link_libraries(oa-game PRIVATE ${oa_game_intro_player} SDL3::SDL3 ${oa_game_libraries}
    oa-ui-frontend-dialogs)
  if(WIN32)
    # The shell's open verb shows the player's folders (user_folder_open.cpp).
    target_link_libraries(oa-game PRIVATE shell32 ole32)
  endif()
  if(BUILD_TESTING)
    # The native checks run oa-game over the installation OA_GAME_DIR names;
    # without one they are not added.
    if(IS_DIRECTORY "${OA_GAME_DIR}")
      file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
      # Part way into the first Arm mission every cheat typed through the
      # chat line takes effect, as in a skirmish, and +sing sings with 3.1c's
      # honk and sing; then restart from the exit menu must start the mission
      # over with the units its first start created and cheats still allowed.
      add_test(NAME native-campaign-restart COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --headless-check --campaign "Arm Campaign" --mission 0
        --match-ticks 60 --restart-at 30
        --preferences-file "${CMAKE_CURRENT_BINARY_DIR}/native-checks/campaign-restart.conf")
      set_tests_properties(native-campaign-restart PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
      # The same under a mod profile that names its own texts: +sing sings
      # with its two sounds (strings.cheat.sing-sounds), never with 3.1c's,
      # and the log's new-leader and elimination lines and the leave question
      # are its strings.message texts.
      set(oa_sing_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/campaign-sing-sounds")
      file(MAKE_DIRECTORY "${oa_sing_check_dir}")
      file(WRITE "${oa_sing_check_dir}/sing-sounds.oamod.yaml"
        "oamod: 1\n"
        "id: sing-sounds\n"
        "name: Sing sounds\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n"
        "strings:\n"
        "  status: {nanolathing: Building, paralyzed: Stunned}\n"
        "  message:\n"
        "    exit-confirm: \"Abandon this made-up war?\"\n"
        "    kill-lead: \"%d kills put %s ahead\"\n"
        "    elimination: [have folded, have been made up, are out]\n"
        "  gadget: {credits: Thanks}\n"
        "  cheat: {sing-sounds: [beep, boop]}\n")
      add_test(NAME native-campaign-sing-sounds COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --headless-check --campaign "Arm Campaign" --mission 0
        --match-ticks 60 --restart-at 30 --mod "${oa_sing_check_dir}/sing-sounds.oamod.yaml"
        --preferences-file "${oa_sing_check_dir}/campaign-sing-sounds.conf")
      set_tests_properties(native-campaign-sing-sounds PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT} WORKING_DIRECTORY "${oa_sing_check_dir}"
        FAIL_REGULAR_EXPRESSION "sounds/(honk|sing)\\.wav")
      # The start plays the movies a mod profile names (media.movies): with
      # an empty logo and 5.zrb as its intro, only 5.zrb, and never 3.1c's
      # 1.zrb or 2.zrb, before the main menu; a game without the movies
      # finds 5.zrb missing.
      set(oa_movies_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/intro-movies")
      file(MAKE_DIRECTORY "${oa_movies_check_dir}")
      file(WRITE "${oa_movies_check_dir}/intro-movies.oamod.yaml"
        "oamod: 1\n"
        "id: intro-movies\n"
        "name: Intro movies\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-05, packager: Open Annihilation}\n"
        "media:\n"
        "  movies: {logo: \"\", intro: 5.zrb}\n")
      add_test(NAME native-intro-movies COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --mute --headless-check --frames 1 --mod "${oa_movies_check_dir}/intro-movies.oamod.yaml"
        --preferences-file "${oa_movies_check_dir}/intro-movies.conf")
      set_tests_properties(native-intro-movies PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT} WORKING_DIRECTORY "${oa_movies_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
        PASS_REGULAR_EXPRESSION "intro (5\\.[Zz][Rr][Bb]: decoded 1 frame|missing: \"[^\"]*5\\.zrb\").*native check: the game offers"
        FAIL_REGULAR_EXPRESSION "[^0-9][12]\\.[Zz][Rr][Bb]")
      # The match frame presented through SDL on dummy devices must equal the
      # CPU composition headless runs use, with the overlays visible, and a
      # nano particle fills a square two pixels on a side at zoom 1 and four
      # at zoom 2; with the battlefield's scene drawn at draw scale 1 apart
      # from the world layer, two on the scene, and the zoom's square at the
      # zoom times its place on the scene in the world layer resampled from
      # it. That scene drawn apart from
      # the world layer and resampled nearest at zoom 1 gives the world layer
      # byte for byte, and at zoom 1, 0.5 and 2 leaves what the match reads
      # back from drawing as the zoom's own frame does. A won skirmish then
      # leaves its VICTORY frame on its own for the end screen, which darkens
      # that frame at the match's size.
      set(oa_layer_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/match-layers")
      file(MAKE_DIRECTORY "${oa_layer_check_dir}")
      add_test(NAME native-match-layers COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-match-layers
        --preferences-file "${oa_layer_check_dir}/match-layers.conf")
      set_tests_properties(native-match-layers PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_layer_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The accelerated presentation, switched on by --hardware-acceleration
      # once the start-up function test passed, on SDL's software renderer
      # taken as capable, and off and on again as the flags switch it: the
      # main menu and a fight's match read back at zoom 1, by the area pass
      # at 0.5, 0.6 and 0.75 and magnified at 1.37, 2 and 4 must equal the
      # processor's composition or keep within that renderer's own filters;
      # a frame must not depend on those before it, the first after the tier
      # is switched on or the window resized among them; the picture kept for
      # screenshots must be the standard tier's and ease nothing; a slow
      # scroll at zoom 2.5 and 0.5 must move the battlefield read back by at
      # most a pixel a frame, at the scroll's rate, while the camera steps
      # whole map pixels, where at 2.5 the standard tier jumps them, and the
      # pointer must pick the unit drawn under it with the view between map
      # pixels at zoom 4; a zoom ease must make no texture; prescale targets
      # are drawn once a painted frame; switched off, every frame is the
      # standard tier's. Shadows are drawn at their zoom's level: the game's
      # own at zoom 1, half as dark at 0.5 in the standard tier and on the
      # card, lighter at a third and none at a quarter and the Full floor.
      # Health bars keep the game's 35 by 5 pixels at zoom 1 and in, and
      # shrink zoomed out to 13 by 3 at the Full floor, alike in every tier.
      # Zoomed out to the whole map, past the zoom units are drawn whole at,
      # each tier's frame is the processor's far view, which plans no model,
      # Full's presented without leaving Full. A shot of render type 2 whose
      # lens's centre lies off the battlefield draws no lens, and a laser's
      # shot after it lists the lines it lists alone.
      # Pictures of one moment at zoom 0.5, 1 and 2.5 in both tiers, and of
      # the shadows at each of those zooms, go to local/reports. Skips under
      # 2 GiB of memory.
      set(oa_render_tiers_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/render-tiers")
      file(MAKE_DIRECTORY "${oa_render_tiers_check_dir}")
      add_test(NAME native-render-tiers COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-render-tiers --hardware-acceleration --force-capable
        --preferences-file "${oa_render_tiers_check_dir}/render-tiers.conf")
      set_tests_properties(native-render-tiers PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_render_tiers_check_dir}" SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same check under a mod profile that turns the visual rules on
      # over the installed game: everything above, with the profile's
      # overlays drawn, then the whiteboard's dot and the build tools' line
      # of sites drawn where the frame draws their map points in the
      # standard and Basic tiers, at zoom 0.5, 1 and 2.5 and with the view
      # between map pixels, the pointer finding the map pixel drawn under
      # it, the build preview drawn over its site at the scene's draw scale,
      # the megamap opening only with the Mouse wheel zoom setting off and
      # closing when it is turned on, the open megamap presented as the
      # processor composes it, a chat line
      # in the system's fonts presented as composed at each zoom, and a
      # named screenshot keeping the standard tier's battlefield; then in
      # the Full tier the dot and the line of sites down to its zoom floor of
      # a sixth, a marker's label whose shadow, outline and letter edges the
      # card draws as asked, the build preview drawn by the card, the
      # megamap at the floor too, and the chat line with what of it lies
      # beside its backdrop the card's (check_visual_rule_overlays).
      set(oa_render_visual_check_dir
        "${CMAKE_CURRENT_BINARY_DIR}/native-checks/render-tiers-visual-rules")
      file(MAKE_DIRECTORY "${oa_render_visual_check_dir}")
      file(WRITE "${oa_render_visual_check_dir}/visual-rules.oamod.yaml"
        "oamod: 1\n"
        "id: visual-rules\n"
        "name: Visual rules\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n"
        "hacks:\n"
        "  ui.megamap: true\n"
        "  ui.whiteboard: true\n"
        "  ui.resource-panel: true\n"
        "  ui.camera-sharing: true\n"
        "  ui.allied-unit-display: true\n"
        "  ui.build-tools: true\n"
        "  ui.click-snap: true\n"
        "  ui.build-preview: true\n"
        "  ui.text-rendering: {unicode: true, chat-backdrop: true}\n"
        "  ui.veterancy-label: true\n"
        "  ui.effects-tweaks: true\n"
        "  ui.interface-fixes: true\n"
        "  ui.display-modes: true\n")
      add_test(NAME native-render-tiers-visual-rules COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-render-tiers --hardware-acceleration --force-capable
        --mod "${oa_render_visual_check_dir}/visual-rules.oamod.yaml"
        --preferences-file "${oa_render_visual_check_dir}/render-tiers.conf")
      set_tests_properties(native-render-tiers-visual-rules PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_render_visual_check_dir}" SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same again with the modern fonts on: every
      # line of game text, the clock and the resource panel among them, in
      # those fonts, so that in the Full tier what of it lies over the
      # battlefield is left to the card, held to what its quads ask beside
      # the processor's picture, under the fog and over the kill board.
      set(oa_render_modern_check_dir
        "${CMAKE_CURRENT_BINARY_DIR}/native-checks/render-tiers-modern-fonts")
      file(MAKE_DIRECTORY "${oa_render_modern_check_dir}")
      file(WRITE "${oa_render_modern_check_dir}/render-tiers.conf"
        "open-annihilation-preferences 1\n"
        "\"open-annihilation.modern-fonts\" \"1\"\n")
      add_test(NAME native-render-tiers-modern-fonts COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-render-tiers --hardware-acceleration --force-capable
        --mod "${oa_render_visual_check_dir}/visual-rules.oamod.yaml"
        --preferences-file "${oa_render_modern_check_dir}/render-tiers.conf")
      set_tests_properties(native-render-tiers-modern-fonts PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_render_modern_check_dir}" SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same check with --native-density: the window opens at the
      # display's own density, which on the dummy video driver is 1, and the
      # match is laid out in window points, stretched over the display by
      # logical presentation, read back at the display's size, at zoom 1 the
      # processor's composition enlarged by nearest replication, and picking
      # the unit drawn under the pointer. On a display above density 1 the
      # same case runs by hand, without SDL_VIDEO_DRIVER. Skips under 2 GiB.
      set(oa_render_density_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/render-density")
      file(MAKE_DIRECTORY "${oa_render_density_check_dir}")
      add_test(NAME native-render-tiers-density COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-render-tiers --hardware-acceleration --force-capable
        --native-density
        --preferences-file "${oa_render_density_check_dir}/render-density.conf")
      set_tests_properties(native-render-tiers-density PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_render_density_check_dir}" SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same case with the Native pixel density setting on in the
      # preferences file in place of --native-density.
      set(oa_render_density_setting_dir
        "${CMAKE_CURRENT_BINARY_DIR}/native-checks/render-density-setting")
      file(MAKE_DIRECTORY "${oa_render_density_setting_dir}")
      file(WRITE "${oa_render_density_setting_dir}/render-density.conf"
        "open-annihilation-preferences 1\n"
        "\"open-annihilation.native-density\" \"1\"\n")
      add_test(NAME native-render-tiers-density-setting COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-render-tiers --hardware-acceleration --force-capable
        --preferences-file "${oa_render_density_setting_dir}/render-density.conf")
      set_tests_properties(native-render-tiers-density-setting PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_render_density_setting_dir}" SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # LOADGAME.GUI from Single Player and, through the SDL presenter, as the
      # save and load dialogs over a paused skirmish: each centred on the
      # screen over the darkened panel below on 640x480, 1280x960 and
      # 1920x1080 windows, its bitmap in that palette, and each previewing the
      # chosen save's radar image and side. A click on the name field and
      # CANCEL save nothing, OK and Return save, and Escape leaves the dialog.
      # The load dialog's CANCEL and Escape return to Single Player and to the
      # in-game menu, with saves listed and with none. A press on either
      # dialog's buttons acts only once released over the button: held, or
      # released away from it, it does nothing.
      set(oa_loadsave_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/loadsave")
      file(MAKE_DIRECTORY "${oa_loadsave_check_dir}")
      add_test(NAME native-loadsave-screens COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-load-save
        --preferences-file "${oa_loadsave_check_dir}/loadsave.conf")
      set_tests_properties(native-loadsave-screens PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_loadsave_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # A 4K monitor's modes as SDL lists them on Windows, at two refresh
      # rates, with the television sizes, which the checks below hand the
      # game as the display's (--display-modes): the dummy video driver
      # reports none.
      set(oa_four_k_monitor "3840x2160@60,3840x2160@30,2560x1600@60,2560x1440@60,1920x1200@60,\
1920x1080@60,1680x1050@60,1600x1200@60,1600x900@60,1440x900@60,1366x768@60,1280x1024@60,\
1280x960@60,1280x800@60,1280x720@60,1024x768@60,800x600@60,720x576@50,720x480@60,640x480@60")
      # Every option of SKIRMISH.GUI, NEWGAME.GUI and the options sub-panels
      # clicked through the SDL presenter shows the setting it changed (its
      # staged caption, frame or text) before Start; the clicked options tab
      # shows pressed and HELPTEXT shows the help under the pointer. VISUALS'
      # Screen Size lists the 4K monitor's sizes from 640x480 to 3840x2160,
      # opens on the window's size, or on Custom for a window dragged to a
      # size the monitor does not offer, and on OK and not on CANCEL sets the
      # Screen size setting and snaps the window to it; RESTORE shows the
      # setting's default, Desktop, as the size it plays at, UNDO brings back
      # the size it opened on, and OK after RESTORE sets the setting to
      # Desktop, leaving the window as it is. In a match a Screen size
      # resizes the window, and in full screen the match is drawn at it and
      # letterboxed, the pointer in the bars resting on the frame's edge.
      # NEWGAME.GUI's Campaign list selects the clicked campaign, steps with
      # Up and Down until a button takes the focus, scrolls with its arrow and
      # starts a double-clicked one.
      set(oa_frontend_controls_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/frontend-controls")
      file(MAKE_DIRECTORY "${oa_frontend_controls_dir}")
      add_test(NAME native-frontend-controls COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-frontend-controls --display-modes "${oa_four_k_monitor}"
        --snapshot "${oa_frontend_controls_dir}/frontend-controls.ppm"
        --preferences-file "${oa_frontend_controls_dir}/frontend-controls.conf")
      set_tests_properties(native-frontend-controls PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_frontend_controls_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same under a mod profile that seeds ten skirmish players: each of
      # the ten rows shows its own metal and energy, also after a saved game,
      # and each player starts with its row's.
      set(oa_frontend_ten_players_dir
        "${CMAKE_CURRENT_BINARY_DIR}/native-checks/frontend-controls-ten-players")
      file(MAKE_DIRECTORY "${oa_frontend_ten_players_dir}")
      file(WRITE "${oa_frontend_ten_players_dir}/ten-players.oamod.yaml"
        "oamod: 1\n"
        "id: ten-players\n"
        "name: Ten players\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-05, packager: Open Annihilation}\n"
        "settings:\n"
        "  registry-seeds: {NumSkirmishPlayers: 10}\n")
      add_test(NAME native-frontend-controls-ten-players COMMAND oa-game
        --game-dir "${OA_GAME_DIR}" --skip-intro --mute --check-frontend-controls
        --mod "${oa_frontend_ten_players_dir}/ten-players.oamod.yaml"
        --snapshot "${oa_frontend_ten_players_dir}/frontend-controls.ppm"
        --preferences-file "${oa_frontend_ten_players_dir}/frontend-controls.conf")
      set_tests_properties(native-frontend-controls-ten-players PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT} WORKING_DIRECTORY "${oa_frontend_ten_players_dir}"
        FAIL_REGULAR_EXPRESSION "after a saved game the [0-9] rows"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The scroll bars of the options' SOUND panel, SELMAP.GUI's map list,
      # NEWGAME.GUI's mission list and the match's preferences, driven
      # through the SDL presenter: each draws
      # its knob and arrows, a press beside the knob steps it one position, a
      # drag moves it pixel for pixel, an arrow steps and repeats, and the
      # preferences' sub-panel shows whole over the battlefield in a window
      # taller than the chrome. The map list, focused as SELMAP.GUI opens,
      # steps with Down and Up, previewing each map and scrolling a row past
      # the page's edge with its knob following, and Down moves the focus on
      # from Select Map. The mission list also steps with Up and
      # starts a double-clicked mission, and a click on Any Mission's Campaign
      # list takes the focus, so Up and Down step it and refill the missions.
      set(oa_scroll_bars_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/scroll-bars")
      file(MAKE_DIRECTORY "${oa_scroll_bars_dir}")
      add_test(NAME native-scroll-bars COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-scroll-bars
        --preferences-file "${oa_scroll_bars_dir}/scroll-bars.conf")
      set_tests_properties(native-scroll-bars PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_scroll_bars_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The Open Annihilation settings through the SDL presenter: the OA
      # button on the main menu and under Resume in the in-game menu, the
      # dialog over each, its sections, controls and keys, its scrolling,
      # OK, Cancel and Restore defaults, the preferences it writes, the locks
      # a game, the command line and the renderer put on it, and each setting
      # taking effect. --force-capable lifts the software renderer's lock on
      # Vertical sync, which the dialog turns On and reads back from the
      # renderer, and on Hardware acceleration, which one step sets to Basic,
      # Full and Off through the dialog, from 2 GiB, to see the accelerated
      # tier follow it at once, Full drawn as Basic with the status saying
      # so, and come back after a drop or a function test forced to fail;
      # every other step leaves it Off, so every frame compared is drawn on
      # the processor. Screen size offers Desktop and the 4K monitor's sizes,
      # opens on the window's own, and applies 1280x720 when OK is pressed,
      # in the menus and in a game, which keeps its tick and world.
      set(oa_engine_settings_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/engine-settings")
      file(MAKE_DIRECTORY "${oa_engine_settings_dir}")
      add_test(NAME native-engine-settings COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-engine-settings --force-capable
        --display-modes "${oa_four_k_monitor}"
        --preferences-file "${oa_engine_settings_dir}/engine-settings.conf")
      set_tests_properties(native-engine-settings PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_engine_settings_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The simulation hash an extension reads: the plain baseline with no
      # mod, a mod profile's when that profile changes the simulation, a
      # different hash after a Developer Mode override of a rule in the same
      # start, and the first hash again once the settings are put back.
      set(oa_simulation_hash_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/simulation-hash")
      file(MAKE_DIRECTORY "${oa_simulation_hash_dir}")
      file(WRITE "${oa_simulation_hash_dir}/changed.oamod.yaml"
        "oamod: 1\n"
        "id: changed-rules\n"
        "name: Changed rules\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-09, packager: Open Annihilation}\n"
        "hacks:\n"
        "  units.id-reuse-delay: 90\n")
      add_test(NAME native-simulation-hash COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-simulation-hash
        --preferences-file "${oa_simulation_hash_dir}/simulation-hash.conf")
      add_test(NAME native-simulation-hash-mod COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-simulation-hash
        --mod "${oa_simulation_hash_dir}/changed.oamod.yaml"
        --preferences-file "${oa_simulation_hash_dir}/simulation-hash-mod.conf")
      set_tests_properties(native-simulation-hash native-simulation-hash-mod PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT} WORKING_DIRECTORY "${oa_simulation_hash_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The player's own folder beside the check's preferences file: saved
      # games where earlier versions kept them, beside the preferences file
      # and loose in Saves, moved into Saves/default once, one named as a
      # saved game there kept beside it, a mod's into its own folder; saved
      # games read from the earlier folders while they are there; a save, a
      # screenshot and a film in the folders of the mod played, without a
      # mod and with a made-up one; the notice of the moves shown once over
      # the main menu in the settings dialog's look, its Open folder button
      # and Enter; and the settings' Your files buttons and the Mods
      # folder's mods, through a recorded opener.
      set(oa_user_folder_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/user-folder")
      file(MAKE_DIRECTORY "${oa_user_folder_dir}")
      add_test(NAME native-user-folder COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-user-folder
        --preferences-file "${oa_user_folder_dir}/user-folder.conf")
      set_tests_properties(native-user-folder PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_user_folder_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The settings' Mods page switches the mod ten times, between No Mod
      # and two test profiles it writes into the player's own Mods folder:
      # each switch stores the mod and builds the game afresh on the same
      # window, which plays the mod chosen, listed first; what the heap holds
      # as the last round's runs reach the main menu stays level with the
      # first round's after the first start.
      set(oa_mod_switch_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/mod-switch")
      file(MAKE_DIRECTORY "${oa_mod_switch_dir}")
      add_test(NAME native-mod-switch COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-mod-switch
        --preferences-file "${oa_mod_switch_dir}/mod-switch.conf"
        --user-folder "${oa_mod_switch_dir}/Open Annihilation")
      set_tests_properties(native-mod-switch PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_mod_switch_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Made-up mod packages installed through the game: a missing file
      # refused, a package the Finder made installed with no question from a
      # dropped file, an update asked and cancelled then replaced, three
      # revisions keeping one .backup, a reinstall, an older revision
      # declined, another version installed alongside, a folder that holds
      # another mod left as it is, a profile that does not resolve refused,
      # and a kept version that cannot be played refused; ROLL BACK twice on
      # the Mods page; then, over soft restarts, PLAY NOW, a replace of the
      # mod played and its roll back, each put in place between runs, and a
      # stop mid-replace settled before the mod folder is resolved.
      set(oa_mod_install_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/mod-install")
      file(MAKE_DIRECTORY "${oa_mod_install_dir}")
      add_test(NAME native-mod-install COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-mod-install
        --install-mod "${oa_mod_install_dir}/missing.oamod"
        --preferences-file "${oa_mod_install_dir}/mod-install.conf"
        --user-folder "${oa_mod_install_dir}/Open Annihilation")
      set_tests_properties(native-mod-install PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_mod_install_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # A folder without a profile whose second side lacks its LOGO: the
      # Mods page refuses it. A folder without a profile, and a profile,
      # whose second side's intgaf and font name files no folder holds: the
      # Mods page switches to each, the main menu warns once of both files
      # and that its games still start, and a skirmish on that side starts
      # and plays. A mod folder whose unit files are missing: the main menu
      # warns once that its games cannot start, and Skirmish's Start warns
      # again and stays on the setup; a profile whose commander's file the
      # unit catalog drops, and whose second side's intgaf names a GAF no
      # folder holds, warns of that commander and that GAF alone; a profile
      # that plays whole warns of nothing and starts its skirmish.
      set(oa_mod_warning_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/mod-warning")
      file(MAKE_DIRECTORY "${oa_mod_warning_dir}")
      add_test(NAME native-mod-warning COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-mod-warning
        --preferences-file "${oa_mod_warning_dir}/mod-warning.conf"
        --user-folder "${oa_mod_warning_dir}/Open Annihilation")
      set_tests_properties(native-mod-warning PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_mod_warning_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Each renderer failure the game handles while it runs, forced on the
      # renderer the walk of SDL's render drivers makes, which on the dummy
      # video driver is SDL's software renderer: the walk itself, a present
      # that fails in a match and on a loading frame, a device reset in a
      # match and during a load, a lost device, a device that waits for its
      # reset, present stalls, a changed floating-point setting, and layers
      # beyond a texture limit of 2048 in tiles. SDL_RENDER_DRIVER is left
      # unset so that the walk runs. native-renderer-ladder-create forces
      # every driver but software to refuse at start.
      set(oa_renderer_ladder_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/renderer-ladder")
      file(MAKE_DIRECTORY "${oa_renderer_ladder_dir}")
      add_test(NAME native-renderer-ladder COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-renderer-ladder
        --preferences-file "${oa_renderer_ladder_dir}/renderer-ladder.conf")
      add_test(NAME native-renderer-ladder-create COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-renderer-ladder --render-fault create
        --preferences-file "${oa_renderer_ladder_dir}/renderer-ladder-create.conf")
      set_tests_properties(native-renderer-ladder native-renderer-ladder-create PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_renderer_ladder_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
        ENVIRONMENT_MODIFICATION "SDL_RENDER_DRIVER=unset:;SDL_FRAMEBUFFER_ACCELERATION=unset:")
      # The memory guard's case of the accelerated tier, which the check
      # switches on over the same renderer as --hardware-acceleration and
      # --force-capable would: through the check's own sample of the
      # system's memory, the guard refuses the tier's buffers, then drops it
      # for the run. The same for the Full tier, switched on as
      # --hardware-acceleration=full would: the guard refuses Full's pages
      # and drops Full before Basic. Each skips under 2 GiB.
      add_test(NAME native-renderer-ladder-memory COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-renderer-ladder --render-fault memory
        --preferences-file "${oa_renderer_ladder_dir}/renderer-ladder-memory.conf")
      add_test(NAME native-renderer-ladder-full-memory COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-renderer-ladder --render-fault full-memory
        --preferences-file "${oa_renderer_ladder_dir}/renderer-ladder-full-memory.conf")
      set_tests_properties(native-renderer-ladder-memory native-renderer-ladder-full-memory
        PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_renderer_ladder_dir}"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
        ENVIRONMENT_MODIFICATION "SDL_RENDER_DRIVER=unset:;SDL_FRAMEBUFFER_ACCELERATION=unset:")
      # The first mission's briefing opened from NEWGAME.GUI with sound on the
      # dummy device: its narration is heard exactly while SHUTUP is on,
      # SHUTUP silences it, plays it anew and turns off once it is over,
      # PrevMenu, Start and their keys Escape and Enter silence it, and every
      # briefing opens with SHUTUP on.
      set(oa_briefing_narration_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/briefing-narration")
      file(MAKE_DIRECTORY "${oa_briefing_narration_dir}")
      add_test(NAME native-briefing-narration COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --check-briefing-narration
        --snapshot "${oa_briefing_narration_dir}/briefing-narration.ppm"
        --preferences-file "${oa_briefing_narration_dir}/briefing-narration.conf")
      set_tests_properties(native-briefing-narration PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_briefing_narration_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # F4 in a skirmish slides the kills board in at the battlefield's top
      # right through the SDL presenter, with no dialog, and away again.
      # Space selects nothing and moves no camera; held, it slides the board
      # in and raises the status strip over the battlefield's bottom left,
      # every frame leaving the bottom bar as it was. The console's clock
      # hides while Space is held and the strip and the board close, then
      # fades back in.
      set(oa_kill_board_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/kill-board")
      file(MAKE_DIRECTORY "${oa_kill_board_check_dir}")
      add_test(NAME native-kill-board COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-kill-board
        --preferences-file "${oa_kill_board_check_dir}/kill-board.conf")
      set_tests_properties(native-kill-board PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_kill_board_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same in the Full tier, on SDL's software renderer taken as capable:
      # the board's foreground painted on the overlay canvas and its shading
      # a black quad the card blends under it at the shade level's alpha.
      set(oa_kill_board_full_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/kill-board-full")
      file(MAKE_DIRECTORY "${oa_kill_board_full_check_dir}")
      add_test(NAME native-kill-board-full COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-kill-board --hardware-acceleration=full --force-capable
        --preferences-file "${oa_kill_board_full_check_dir}/kill-board.conf")
      set_tests_properties(native-kill-board-full PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_kill_board_full_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The headless skirmish's first ticks played in director mode, drawn,
      # reach the world the same ticks replayed there undrawn reach; director
      # frames show the battlefield alone, move by half a map pixel and zoom
      # past the player's range, and the fight's sounds reach the director's
      # hooks.
      set(oa_director_view_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/director-view")
      file(MAKE_DIRECTORY "${oa_director_view_dir}")
      add_test(NAME native-director-view COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --mute --check-director-view
        --snapshot "${oa_director_view_dir}/director-view.ppm"
        --preferences-file "${oa_director_view_dir}/director-view.conf")
      set_tests_properties(native-director-view PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_director_view_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
      # A small director script rendered over the headless skirmish with
      # encoding off, whole and its second chunk alone: every chunk's frames,
      # sound and manifests are written, neither a chunk's sound nor the
      # world depends on the chunks drawn before it, and the render reaches
      # the world the generator's undrawn replay reaches. The frames and the
      # sound are pinned; at 60 frames a second every other frame lies half
      # way between two ticks, and is drawn so, frames zoomed out draw
      # their shadows lighter, and explosions flash as Explosion flash's
      # default, Full, draws them.
      set(oa_director_render_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/director-render")
      set(oa_director_render_hashes
        "director render: frames 04f6532a6c155ceeba64c70335f624fb828bae95417f5d2ba36418379fd14140 sound 98718c0ce9d785c0a09f409493bd394e92371b25a8a79bd3d72b6d5bab2234f3")
      file(MAKE_DIRECTORY "${oa_director_render_dir}")
      add_test(NAME native-director-render COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --mute --check-director-render
        --preferences-file "${oa_director_render_dir}/director-render.conf")
      set_tests_properties(native-director-render PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_director_render_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
        PASS_REGULAR_EXPRESSION "${oa_director_render_hashes}"
        FAIL_REGULAR_EXPRESSION "director render check:")
      # The same render with its files and preferences named relative to the
      # working directory, without a folder: the run writes them there and
      # gives the same frames and sound.
      set(oa_director_relative_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/director-render-relative")
      file(MAKE_DIRECTORY "${oa_director_relative_dir}")
      add_test(NAME native-director-render-relative COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --mute --check-director-render --snapshot director-render.ppm
        --preferences-file director-render.conf)
      set_tests_properties(native-director-render-relative PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_director_relative_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
        PASS_REGULAR_EXPRESSION "${oa_director_render_hashes}"
        FAIL_REGULAR_EXPRESSION "director render check:|filesystem error")
      # Frames drawn between ticks over the headless skirmish's fight leave the
      # world and every whole tick's frame as they are, show units, turrets
      # and projectiles part of the way from one tick to the next, and move
      # a unit, and the ground under a camera tracking it, along the even
      # path frame by frame at 120 frames a second.
      set(oa_interpolation_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/interpolation")
      file(MAKE_DIRECTORY "${oa_interpolation_dir}")
      add_test(NAME native-interpolation COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --mute --check-interpolation
        --preferences-file "${oa_interpolation_dir}/interpolation.conf")
      set_tests_properties(native-interpolation PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_interpolation_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
        PASS_REGULAR_EXPRESSION "interpolation: [0-9]+ frames between ticks drew"
        FAIL_REGULAR_EXPRESSION "interpolation check:")
      # The other player taken as another machine's, whose records arrive in
      # bursts: its runner is drawn on its playout, moving evenly at 120
      # frames a second where its records alone hold it still and make it
      # jump, on whole ticks too, with the tracking camera and the pointer's
      # pick where it is drawn; the local runner and the world are as without
      # the playout, and with no such player so is every frame.
      set(oa_unit_playout_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/unit-playout")
      file(MAKE_DIRECTORY "${oa_unit_playout_dir}")
      add_test(NAME native-unit-playout COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --mute --check-unit-playout
        --preferences-file "${oa_unit_playout_dir}/unit-playout.conf")
      set_tests_properties(native-unit-playout PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_unit_playout_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy"
        PASS_REGULAR_EXPRESSION "unit playout: [0-9]+ frames drew the other machine's runner evenly"
        FAIL_REGULAR_EXPRESSION "unit playout check:")
      # native-intro-switch, which starts the game with -y, is registered
      # once the extensions are known (cmake/OaExtensions.cmake).
      # The skirmish commander starts with its FBI standing orders; the order
      # page's standing order and toggle buttons clicked through SDL show the
      # selection's orders and the units carry them out. With Shift held, the
      # target markers of moves queued onto gray and black ground show over
      # the fog, which stays around them; without Shift they are not drawn.
      set(oa_orders_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/match-orders")
      file(MAKE_DIRECTORY "${oa_orders_check_dir}")
      add_test(NAME native-match-orders COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-match-orders
        --preferences-file "${oa_orders_check_dir}/match-orders.conf")
      set_tests_properties(native-match-orders PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_orders_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # A Kbot Lab sent to a point with MOVE and PATROL through SDL input on
      # dummy devices: each Peewee it then builds heads there.
      set(oa_factory_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/factory-orders")
      file(MAKE_DIRECTORY "${oa_factory_check_dir}")
      add_test(NAME native-factory-orders COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-factory-orders
        --preferences-file "${oa_factory_check_dir}/factory-orders.conf")
      set_tests_properties(native-factory-orders PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_factory_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Through SDL input on dummy devices the commander says its select line
      # when clicked and its order line when then clicked onto open ground,
      # each with a sound its SOUND.TDF section offers; and so in a second
      # skirmish started after the first, which nothing the first one said
      # may silence; with +Sing on, the select line sings.
      set(oa_unit_speech_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/unit-speech")
      file(MAKE_DIRECTORY "${oa_unit_speech_check_dir}")
      add_test(NAME native-unit-speech COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-unit-speech
        --preferences-file "${oa_unit_speech_check_dir}/unit-speech.conf")
      set_tests_properties(native-unit-speech PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_unit_speech_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # ARMLAB's second page is the ARMDL.GUI download page with the Warrior
      # and Flea its download TDFs name; through SDL input on dummy devices
      # they queue, come off the middle and end of the queue and build.
      set(oa_download_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/download-builds")
      file(MAKE_DIRECTORY "${oa_download_check_dir}")
      add_test(NAME native-download-builds COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-download-builds
        --preferences-file "${oa_download_check_dir}/download-builds.conf")
      set_tests_properties(native-download-builds PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_download_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Every unit whose first weapon stockpiles, selected through SDL input
      # on dummy devices, opens on its weapon page; the page queues and drops
      # rounds by one and by five, and a round built with another queued
      # stays, with its count, through a save and a load.
      set(oa_stockpile_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/stockpile-builds")
      file(MAKE_DIRECTORY "${oa_stockpile_check_dir}")
      add_test(NAME native-stockpile-builds COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-stockpile-builds
        --preferences-file "${oa_stockpile_check_dir}/stockpile-builds.conf")
      set_tests_properties(native-stockpile-builds PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_stockpile_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Through SDL input on dummy devices, ORDERS, BUILD and the page buttons
      # and keys change the page the commander keeps: selecting a lab and the
      # commander again reopens it, the keys turn through the order page, a
      # page with each repeat of a held one, a save and a load keep it, a unit
      # with no build pages keeps its general page, and with two selected the
      # panel shows the general page and turns no unit's page.
      set(oa_unit_page_memory_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/unit-page-memory")
      file(MAKE_DIRECTORY "${oa_unit_page_memory_check_dir}")
      add_test(NAME native-unit-page-memory COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-unit-page-memory
        --preferences-file "${oa_unit_page_memory_check_dir}/unit-page-memory.conf")
      set_tests_properties(native-unit-page-memory PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_unit_page_memory_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # A skirmish paused with the Pause key and saved from the in-game menu
      # loads running, with its pause bit clear; Pause then holds it and
      # lets it go again, and so does the in-game menu.
      set(oa_paused_save_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/paused-save")
      file(MAKE_DIRECTORY "${oa_paused_save_check_dir}")
      add_test(NAME native-paused-save COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-paused-save
        --preferences-file "${oa_paused_save_check_dir}/paused-save.conf")
      set_tests_properties(native-paused-save PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_paused_save_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Units' names and descriptions in each language the game data holds,
      # and in the language the system's preferred locale chooses: through
      # SDL input on dummy devices, the commander's build buttons show their
      # unit files' names and descriptions in the language at the bottom
      # bar's NAME and DESCRIPTION, and the commander its name at UNITNAME;
      # put back to English, the same places show the files' own texts. In
      # the language the in-game menu's exit menu captions RESTART as the
      # game data translates it, and no caption of it runs past its button.
      foreach(oa_language_case IN ITEMS "english;en;en" "german;de;de" "french;fr;fr" "italian;it;it"
          "spanish;es;es" "system;system;fr")
        list(GET oa_language_case 0 oa_language_name)
        list(GET oa_language_case 1 oa_language_choice)
        list(GET oa_language_case 2 oa_language_tag)
        set(oa_language_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/unit-language-${oa_language_name}")
        file(MAKE_DIRECTORY "${oa_language_check_dir}")
        file(WRITE "${oa_language_check_dir}/unit-language.conf"
          "open-annihilation-preferences 1\n\"open-annihilation.language\" \"${oa_language_choice}\"\n")
        add_test(NAME native-unit-language-${oa_language_name} COMMAND oa-game --game-dir "${OA_GAME_DIR}"
          --skip-intro --mute --check-unit-language ${oa_language_tag}
          --preferences-file "${oa_language_check_dir}/unit-language.conf")
        set_tests_properties(native-unit-language-${oa_language_name} PROPERTIES
          TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
          WORKING_DIRECTORY "${oa_language_check_dir}"
          ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_PREFERRED_LOCALES=fr_CA,de_DE")
      endforeach()
      # The language changed in the settings over the main menu and over a
      # skirmish's in-game menu, from Simplified Chinese to English, German
      # and back: after each change, the settings closed with OK, the screen
      # under them shows what it shows opened again in the language, and
      # back in Chinese what it showed as the run started in it. Needs the
      # engine's Simplified Chinese pack beside the game.
      set(oa_language_switch_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/language-switch")
      file(MAKE_DIRECTORY "${oa_language_switch_check_dir}")
      file(WRITE "${oa_language_switch_check_dir}/language-switch.conf"
        "open-annihilation-preferences 1\n\"open-annihilation.language\" \"zh-Hans\"\n")
      add_test(NAME native-language-switch COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-language-switch
        --preferences-file "${oa_language_switch_check_dir}/language-switch.conf")
      set_tests_properties(native-language-switch PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_language_switch_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The commander's build pages on windows from 640x480 to 5120x2880,
      # walked with NEXT through SDL input on dummy devices: each fits the
      # side column, the same build buttons are reachable on every window and
      # the strip under the column builds nothing.
      set(oa_side_column_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/side-column")
      file(MAKE_DIRECTORY "${oa_side_column_check_dir}")
      add_test(NAME native-side-column COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-side-column
        --preferences-file "${oa_side_column_check_dir}/side-column.conf")
      set_tests_properties(native-side-column PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_side_column_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The top and bottom bars of a skirmish played on each of two sides, on
      # windows of 4:3 and wider from 640x480 to 2560x1080: each reaches the
      # window's right edge with art in every column, and holds the side's
      # PANELTOP and PANELBOT where the game places them on a screen that
      # wide.
      set(oa_match_bars_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/match-bars")
      file(MAKE_DIRECTORY "${oa_match_bars_check_dir}")
      add_test(NAME native-match-bars COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-match-bars
        --preferences-file "${oa_match_bars_check_dir}/match-bars.conf")
      set_tests_properties(native-match-bars PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_match_bars_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same with ui.resource-panel on: on the 4:3 and 5:4 windows wider
      # than 1024 pixels the chrome is drawn smaller, so that the top bar
      # holds the clock line, with the game time and, with the console's
      # Clock on, without it; without it the line also goes in the bar at
      # 1024x600, where the bar reaches past it, and stays on the battlefield
      # at 640x480, 800x600 and 1024x768, where it does not; the bars still
      # reach the window's right edge with the side's art where the game
      # places it.
      set(oa_match_bars_panel_dir
        "${CMAKE_CURRENT_BINARY_DIR}/native-checks/match-bars-resource-panel")
      file(MAKE_DIRECTORY "${oa_match_bars_panel_dir}")
      file(WRITE "${oa_match_bars_panel_dir}/resource-panel.oamod.yaml"
        "oamod: 1\n"
        "id: resource-panel\n"
        "name: Resource panel\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-07, packager: Open Annihilation}\n"
        "hacks:\n"
        "  ui.resource-panel: true\n")
      add_test(NAME native-match-bars-resource-panel COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-match-bars
        --mod "${oa_match_bars_panel_dir}/resource-panel.oamod.yaml"
        --preferences-file "${oa_match_bars_panel_dir}/match-bars.conf")
      set_tests_properties(native-match-bars-resource-panel PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_match_bars_panel_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same with HUD scaling Off: the side column and both bars at the
      # original game's size on every window, still reaching the window's
      # right edge with the side's art, and the clock line placed by the
      # same rule: on the battlefield at 1024x768 and in two sections at
      # 1280x1024 and 1920x1080; with the console's Clock on, in the top
      # bar's first section at 800x600 and 1024x768, and on the battlefield
      # at 640x480.
      set(oa_match_bars_unscaled_dir
        "${CMAKE_CURRENT_BINARY_DIR}/native-checks/match-bars-hud-scaling-off")
      file(MAKE_DIRECTORY "${oa_match_bars_unscaled_dir}")
      file(WRITE "${oa_match_bars_unscaled_dir}/match-bars.conf"
        "open-annihilation-preferences 1\n"
        "\"open-annihilation.hud-scaling\" \"0\"\n")
      add_test(NAME native-match-bars-hud-scaling-off COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-match-bars
        --mod "${oa_match_bars_panel_dir}/resource-panel.oamod.yaml"
        --preferences-file "${oa_match_bars_unscaled_dir}/match-bars.conf")
      set_tests_properties(native-match-bars-hud-scaling-off PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_match_bars_unscaled_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The pages of a builder of one page, one of several, a factory and a
      # unit that builds nothing, on windows from 640x480 to 2560x1440,
      # walked with NEXT, PREV, ORDERS and BUILD through SDL input on dummy
      # devices: the side column keeps the interface's width and scale, every
      # page is drawn whole as its file places it, and each control the side
      # column draws, the radar and the battlefield's edge take a click where
      # they are drawn.
      set(oa_unit_pages_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/unit-pages")
      file(MAKE_DIRECTORY "${oa_unit_pages_check_dir}")
      add_test(NAME native-unit-pages COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-unit-pages whole:ARMMLV,ARMCOM,ARMLAB,ARMPW
        --preferences-file "${oa_unit_pages_check_dir}/unit-pages.conf")
      set_tests_properties(native-unit-pages PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_unit_pages_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # An ARMCK given PATROL through SDL input past map features with the
      # metal store empty takes RepairPatrol, reclaims one on its way, is
      # credited with it and patrols on.
      set(oa_patrol_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/patrol-reclaim")
      file(MAKE_DIRECTORY "${oa_patrol_check_dir}")
      add_test(NAME native-patrol-reclaim COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-patrol-reclaim
        --preferences-file "${oa_patrol_check_dir}/patrol-reclaim.conf")
      set_tests_properties(native-patrol-reclaim PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_patrol_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The selected commander's pointer moved over several kinds of tree,
      # another reclaimable feature and a wreck through SDL input shows
      # Reclaim wherever its ground point is on their footprints and Move on
      # the open ground around them. Under the fog a tree on mapped ground
      # shows Reclaim and a click there gives a Reclaim, which the commander
      # carries out; a tree on ground never mapped shows Move and takes a Move.
      # A construction kbot's and a flying construction aircraft's Reclaim of
      # a tree stand on the ground where the tree does, so that their markers
      # are drawn on it; the order overlays are drawn into
      # reclaim-order-overlays.png in the test's folder.
      set(oa_reclaim_cursor_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/reclaim-cursor")
      file(MAKE_DIRECTORY "${oa_reclaim_cursor_dir}")
      add_test(NAME native-reclaim-cursor COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-reclaim-cursor
        --preferences-file "${oa_reclaim_cursor_dir}/reclaim-cursor.conf")
      set_tests_properties(native-reclaim-cursor PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_reclaim_cursor_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # A tower placed under the build cursor with ui.build-preview on, in
      # the standard, Basic and Full tiers at zoom 0.5, 1 and 2: drawn at
      # every tick of two of its pulses, each frame twice, its pictures
      # repeat every pulse and at no shorter period, a frame drawn again
      # shows the same, and it lies within the finished tower's picture at
      # its site, reaching the same top. Skips under 2 GiB.
      set(oa_build_preview_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/build-preview")
      file(MAKE_DIRECTORY "${oa_build_preview_dir}")
      file(WRITE "${oa_build_preview_dir}/build-preview.oamod.yaml"
        "oamod: 1\n"
        "id: build-preview\n"
        "name: Build preview\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-06, packager: Open Annihilation}\n"
        "hacks:\n"
        "  ui.build-preview: true\n")
      add_test(NAME native-build-preview COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-build-preview --hardware-acceleration=full --force-capable
        --mod "${oa_build_preview_dir}/build-preview.oamod.yaml"
        --preferences-file "${oa_build_preview_dir}/build-preview.conf")
      set_tests_properties(native-build-preview PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_build_preview_dir}" SKIP_RETURN_CODE 77
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The screen's edges scrolling the camera, in full screen and in a
      # window, and both interface types' pointer buttons, through SDL input:
      # left clicks and right presses on open ground, units and the radar,
      # shift clicks taking queued orders and a build site back, mouse look,
      # right clicks beside the in-game menu, one dropping the selection
      # closing it and resuming the game, the Pause key's pause kept, a
      # group's moves and patrols in its shape, a solar collector selected
      # with it given no patrol, and a factory build button's right click
      # taking its type off the queue;
      # and the system's pointer in place of the game's cursor without the focus.
      set(oa_pointer_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/pointer-interfaces")
      file(MAKE_DIRECTORY "${oa_pointer_check_dir}")
      add_test(NAME native-pointer-interfaces COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-pointer-interfaces
        --preferences-file "${oa_pointer_check_dir}/pointer-interfaces.conf")
      set_tests_properties(native-pointer-interfaces PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_pointer_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # With the request held, the frame hook keeps being called while the
      # window is inactive; released, the loop waits for an event.
      set(oa_inactive_loop_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/running-while-inactive")
      file(MAKE_DIRECTORY "${oa_inactive_loop_dir}")
      add_test(NAME native-running-while-inactive COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-running-while-inactive
        --preferences-file "${oa_inactive_loop_dir}/running-while-inactive.conf")
      set_tests_properties(native-running-while-inactive PROPERTIES TIMEOUT 300
        WORKING_DIRECTORY "${oa_inactive_loop_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # What the megamap (ui.megamap) draws: the map's own colours, only its
      # indestructible features by their pictures, no rectangle for the main
      # view and dark grey bars; then its clicks against the battlefield's in
      # both interface types: orders, selection, deselection, guard, an
      # armed command given and cancelled, and a patrol that gives a solar
      # collector selected beside the mover no order, under a profile that
      # turns the megamap on.
      set(oa_megamap_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/megamap-clicks")
      file(MAKE_DIRECTORY "${oa_megamap_check_dir}")
      file(WRITE "${oa_megamap_check_dir}/megamap-clicks.oamod.yaml"
        "oamod: 1\n"
        "id: megamap-clicks\n"
        "name: Megamap clicks\n"
        "version: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-06, packager: Open Annihilation}\n"
        "hacks:\n"
        "  ui.megamap: true\n")
      add_test(NAME native-megamap-clicks COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-megamap-clicks
        --mod "${oa_megamap_check_dir}/megamap-clicks.oamod.yaml"
        --preferences-file "${oa_megamap_check_dir}/megamap-clicks.conf")
      set_tests_properties(native-megamap-clicks PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_megamap_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Presses on the minimap (the radar) in both interface types, on a
      # skirmish of three with one ally: the cursor, the orders, the
      # selection, the armed command and the view for every armed command
      # and the default order over open ground, units' dots and wrecks on
      # mapped and unmapped ground, Shift, a press released over the
      # battlefield, a building placed by the battlefield's last site, and
      # the view moves the radar keeps.
      set(oa_radar_orders_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/radar-orders")
      file(MAKE_DIRECTORY "${oa_radar_orders_check_dir}")
      add_test(NAME native-radar-orders COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-radar-orders
        --preferences-file "${oa_radar_orders_check_dir}/radar-orders.conf")
      set_tests_properties(native-radar-orders PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_radar_orders_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # --check-touch-controls: the touch controls, driven by finger events through the
      # dispatcher, on a skirmish: taps, boxes, latches, the radial, placement, the
      # minimap, pinch and pan, the lifecycle pause, and the tablet (1180x820) or
      # phone (852x393) layout; it writes touch-<case>.ppm snapshots for people.
      set(oa_touch_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/touch-controls")
      file(MAKE_DIRECTORY "${oa_touch_check_dir}")
      add_test(NAME native-touch-controls COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-touch-controls --resolution 1180x820
        --preferences-file "${oa_touch_check_dir}/touch-controls.conf")
      set_tests_properties(native-touch-controls PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_touch_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same for the phone layout, on a window of an iPhone's size.
      set(oa_touch_phone_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/touch-controls-phone")
      file(MAKE_DIRECTORY "${oa_touch_phone_check_dir}")
      add_test(NAME native-touch-controls-phone COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-touch-controls --resolution 852x393
        --preferences-file "${oa_touch_phone_check_dir}/touch-controls-phone.conf")
      set_tests_properties(native-touch-controls-phone PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_touch_phone_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # --check-pad-controls: the gamepad controls, driven through the event
      # path by SDL virtual pads imitating a Steam Deck and an Xbox pad, on a
      # skirmish at the Deck's screen size (docs/controllers.md).
      set(oa_pad_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/pad-controls")
      file(MAKE_DIRECTORY "${oa_pad_check_dir}")
      add_test(NAME native-pad-controls COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-pad-controls --resolution 1280x800
        --preferences-file "${oa_pad_check_dir}/pad-controls.conf")
      set_tests_properties(native-pad-controls PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_pad_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The same with touch controls on, as an iPhone and an iPad always
      # have them, at their sizes: the touch controls with the pad's badges,
      # and on the phone layout the build ring for the side panel's buttons.
      foreach(oa_pad_touch IN ITEMS "phone;874x402" "tablet;1180x820")
        list(GET oa_pad_touch 0 oa_pad_touch_name)
        list(GET oa_pad_touch 1 oa_pad_touch_size)
        set(oa_pad_touch_dir
          "${CMAKE_CURRENT_BINARY_DIR}/native-checks/pad-controls-touch-${oa_pad_touch_name}")
        file(MAKE_DIRECTORY "${oa_pad_touch_dir}")
        add_test(NAME native-pad-controls-touch-${oa_pad_touch_name} COMMAND oa-game
          --game-dir "${OA_GAME_DIR}" --skip-intro --mute --check-pad-controls --touch-controls
          --resolution ${oa_pad_touch_size}
          --preferences-file "${oa_pad_touch_dir}/pad-controls.conf")
        set_tests_properties(native-pad-controls-touch-${oa_pad_touch_name} PROPERTIES
          TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
          WORKING_DIRECTORY "${oa_pad_touch_dir}"
          ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      endforeach()
      # --check-multiplayer-menu: MULTI clicked through SDL input opens the
      # multiplayer screens, which network play's check walks. Scribbled
      # frees turn a focus handed back to a closed window into a fault.
      set(oa_multiplayer_check_dir "${CMAKE_CURRENT_BINARY_DIR}/native-checks/multiplayer-menu")
      file(MAKE_DIRECTORY "${oa_multiplayer_check_dir}")
      add_test(NAME native-multiplayer-menu COMMAND oa-game --game-dir "${OA_GAME_DIR}"
        --skip-intro --mute --check-multiplayer-menu
        --preferences-file "${oa_multiplayer_check_dir}/multiplayer-menu.conf")
      set_tests_properties(native-multiplayer-menu PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_multiplayer_check_dir}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software;MallocScribble=1")
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND)
        add_test(NAME native-saveload COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_saveload.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-saveload PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # --check-game-files: the Game files screen driven through its routes
        # by taps and keys, over a subset of OA_GAME_DIR linked into a work
        # folder, writing game-files-<variant>-<nn>-<step>-*.png pictures for
        # people (tools/check_native_game_files.py, docs/game-files.md). The
        # -resume and -manage-next-start halves reuse the work folder their
        # first halves leave.
        set(oa_game_files_variants folder phone demo copy-yourself stop resume not-game short-space
          manage manage-next-start)
        foreach(variant IN LISTS oa_game_files_variants)
          if(variant STREQUAL "folder")
            set(oa_game_files_test native-game-files)
          else()
            set(oa_game_files_test native-game-files-${variant})
          endif()
          set(oa_game_files_work ${variant})
          set(oa_game_files_extra "")
          if(variant STREQUAL "resume")
            set(oa_game_files_work stop)
            list(APPEND oa_game_files_extra --keep-work)
          elseif(variant STREQUAL "manage-next-start")
            set(oa_game_files_work manage)
            list(APPEND oa_game_files_extra --keep-work)
          elseif(variant STREQUAL "phone")
            list(APPEND oa_game_files_extra --resolution 852x393)
          elseif(variant STREQUAL "demo")
            list(APPEND oa_game_files_extra "--demo-installer=${OA_DEMO_INSTALLER}")
          endif()
          add_test(NAME ${oa_game_files_test} COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_game_files.py"
            --game $<TARGET_FILE:oa-game>
            --work "${CMAKE_CURRENT_BINARY_DIR}/native-checks/game-files-${oa_game_files_work}"
            --variant ${variant} --game-dir "${OA_GAME_DIR}" --skip-code ${OA_GAME_DATA_SKIP_CODE}
            ${oa_game_files_extra})
          set_tests_properties(${oa_game_files_test} PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
            PASS_REGULAR_EXPRESSION "game-files check: .*: passed")
          oa_game_data_tests(${oa_game_files_test})
        endforeach()
        oa_demo_installer_tests(native-game-files-demo)
        set_tests_properties(native-game-files-stop PROPERTIES FIXTURES_SETUP game-files-stop)
        set_tests_properties(native-game-files-resume PROPERTIES FIXTURES_REQUIRED game-files-stop)
        set_tests_properties(native-game-files-manage PROPERTIES FIXTURES_SETUP game-files-manage)
        set_tests_properties(native-game-files-manage-next-start PROPERTIES
          FIXTURES_REQUIRED game-files-manage)
        # The game's start on the dummy video driver, with no render driver
        # named: SDL's walk ends on its software renderer, which has no
        # adapter and no texture limit, and the start-up line names it, its
        # video driver and the standard tier, word for word, and the start
        # ends with status 0.
        add_test(NAME native-renderer-report COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_renderer_report.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-renderer-report PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The walk of SDL's render drivers in the game's log, on the dummy
        # video driver: with no render driver named, every driver before
        # SDL's software renderer refuses, each logged once with its reason
        # before the start-up line, and the start ends on software; with
        # SDL_RENDER_DRIVER=software no driver is walked; with a driver
        # named that does not exist the start fails as it always has.
        add_test(NAME native-renderer-walk COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_renderer_walk.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-renderer-walk PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The headless menu, match, composed-frame, message log, speed and
        # build placement checks, from fresh preferences each run; among
        # them the wheel's zoom about the pointer and within each Maximum
        # zoom out and Maximum zoom in choice, out to the whole map and back.
        # One test for each group of the check (--check-navigation GROUP),
        # which stands alone: the menus' screens and a skirmish's first
        # orders; a builder's orders and the match's panels; a skirmish's
        # outcomes; the zoom and its limits on the game's own screen; the
        # zoom's limits on each larger window; and a campaign.
        set(oa_navigation_groups screens orders outcomes zoom zoom-1366x768 zoom-1920x1080
          zoom-2560x1440 campaign)
        foreach(oa_navigation_group IN LISTS oa_navigation_groups)
          add_test(NAME native-navigation-${oa_navigation_group} COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_navigation.py"
            --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks"
            --group ${oa_navigation_group})
          set_tests_properties(native-navigation-${oa_navigation_group} PROPERTIES
            TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        endforeach()
        # --trace-digest streams of seeded headless skirmishes repeat byte
        # for byte and follow the trace stream layout.
        add_test(NAME native-trace COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_trace.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-trace PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # --stage places units and asks for stockpiled rounds in a headless
        # skirmish, which plays to the same digest each time, and stops at a
        # line naming a type the game lacks.
        add_test(NAME native-stage COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_stage.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-stage PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # A stage's lines timed for later ticks place units, move a group,
        # send one on patrol and set one on another's nearest units, and stop
        # at a line that does not read; a director script that names the
        # stage renders it with its stills, drawing the same frames twice.
        add_test(NAME native-stage-render COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_stage_render.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-stage-render PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # A block of fighters and a row of kbots, each moved to one point as
        # a group, keep their shape: the fighters land each on its own site
        # around the point, the row's middle kbots reach their own points and
        # its ends, out of the group's reach, go to the point.
        add_test(NAME native-group-orders COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_group_orders.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-group-orders PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The Open Annihilation settings leave the game as it plays without
        # them: the seeded skirmish, drawn, writes the same trace stream and
        # last frame with every key at its default as with no preferences;
        # the look and input settings and every enhanced anti-aliasing level
        # leave its world alone; Pathfinding cycles at 2x and a unit limit of
        # 500 repeat; and the director render keeps its pinned frames and
        # sound at every default and at 16x anti-aliasing.
        add_test(NAME native-engine-settings-determinism COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_engine_settings.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks"
          --director-hashes "${oa_director_render_hashes}")
        set_tests_properties(native-engine-settings-determinism PROPERTIES
          TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The seeded headless skirmish, its army marching, played through
        # the match clock and drawn every frame at 30, 60, 120 and 144
        # frames a second writes one trace stream and reaches one world
        # digest; at 120 the scrolled camera moves the same distance every
        # frame, each frame shows a quarter of a tick more, and a marching
        # unit drawn between ticks moves on nearly every frame, none carrying
        # more than half the most it moves in a tick; a camera tracking the
        # unit keeps it at one place of the screen on every frame; and from a
        # clock just before 2^32 milliseconds the match steps on through the
        # turn of the clock's reading to 0, at 30 frames a second each frame
        # running a tick but the first past it.
        add_test(NAME native-frame-rate COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_frame_rate.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-frame-rate PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The seeded skirmish's fight of 120 units writes one trace stream and
        # reaches one world digest whatever is drawn or heard: from any camera,
        # at any frame rate (10 leaves most ticks undrawn), drawn after every
        # tick, at any zoom and with enhanced anti-aliasing, however many of
        # its unit announcements the frames present.
        add_test(NAME native-sim-independent-of-drawing COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_sim_independent_of_drawing.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-sim-independent-of-drawing PROPERTIES
          TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The seeded headless skirmish's fight drawn frame by frame with the
        # camera's scroll held, at zoom 1, 1.37 and 0.6, and a longer busy
        # fight (--busy-combat: missiles, nano particles, a loaded transport,
        # selection boxes), into its explosions, debris and fragments, with
        # enhanced anti-aliasing off and at 4x, on 1, 2, 3, 4 and 7 drawing
        # threads, which draw the battlefield's units and effects in as many
        # bands of rows: every count draws the same frames and reaches the
        # same world.
        add_test(NAME native-draw-threads COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_draw_threads.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-draw-threads PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # Arm mission 9 (AC09) at medium, its commander left idle, is lost to
        # the two Pyros its script sends: defeat follows the commander's death
        # after the outcome countdown, with no failed tick.
        add_test(NAME native-campaign-defeat COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_campaign_defeat.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-campaign-defeat PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # Core Contingency mission 6 (EXP1CC06) at medium: the scripted
        # factories hold their 'w' waits for 30 ticks a second and the
        # computer makes nothing before the first one ends.
        add_test(NAME native-campaign-waits COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_campaign_waits.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-campaign-waits PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The installation offers its movies and draws the same main menu
        # without its Data folder: the rules for data with no movies are the
        # demo's, not an installation's that keeps its movies on the disc.
        add_test(NAME native-menu-without-movies COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_menu_without_movies.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-menu-without-movies PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
          SKIP_RETURN_CODE ${OA_GAME_DATA_SKIP_CODE})
        # A mod folder whose Solar Collector script is not a COB the game
        # reads: the skirmish loads every unit type, says once that the unit
        # plays without a script, and plays on.
        add_test(NAME native-unreadable-unit-script COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_unreadable_script.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-unreadable-unit-script PROPERTIES
          TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # A mod folder whose sides name commanders of their own, without the
        # commander flag, and bar colours of their own, with a third side that
        # draws CORE's panels: each side's HUD draws the panels its intgaf
        # names and its own section's bars, the skirmish starts with the
        # commanders, the death of one ends the game under the commander rule
        # and only then, the computer player builds with its commander, and
        # the network loopback and player records checks find them. In a
        # network game, a player giving every unit gives its commander too,
        # as its type is in no Commander category, and both machines agree.
        # One test for each part (--part), over a mod folder of its own: the
        # HUDs, the commander rule, the loopback, the give, and the player
        # records, which runs each group of the navigation check over the
        # mod as a test of its own.
        foreach(oa_side_commanders_part art rule loopback give)
          add_test(NAME native-side-commanders-${oa_side_commanders_part}
            COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_side_commanders.py"
            --native $<TARGET_FILE:oa-game> --oa-tool $<TARGET_FILE:oa-tool>
            --game-dir "${OA_GAME_DIR}"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks"
            --part ${oa_side_commanders_part})
          set_tests_properties(native-side-commanders-${oa_side_commanders_part} PROPERTIES
            TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        endforeach()
        foreach(oa_navigation_group IN LISTS oa_navigation_groups)
          add_test(NAME native-side-commanders-records-${oa_navigation_group}
            COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_side_commanders.py"
            --native $<TARGET_FILE:oa-game> --oa-tool $<TARGET_FILE:oa-tool>
            --game-dir "${OA_GAME_DIR}"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks"
            --part records --navigation-group ${oa_navigation_group})
          set_tests_properties(native-side-commanders-records-${oa_navigation_group} PROPERTIES
            TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        endforeach()
        # A mod folder whose sides name interface art and a font of their
        # own: each side's HUD draws the panels its intgaf names and the
        # numbers in the font it names, or in COMIX, unmeasured, when it
        # names none; an intgaf and a font the game data lacks are drawn as
        # none named; and SIDEDATA and the art it names held only in a
        # language's folders are drawn in that language alone.
        add_test(NAME native-side-panels COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_side_panels.py"
          --native $<TARGET_FILE:oa-game> --oa-tool $<TARGET_FILE:oa-tool>
          --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-side-panels PROPERTIES
          TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
        # The game folder, the preferences file beside the saved games and a
        # mod folder more than 256 characters deep, and as deep as the system
        # opens, with a user name of 200 characters: the main menu opens and
        # a skirmish saves and loads back.
        add_test(NAME native-long-paths COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_long_paths.py"
          --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-long-paths PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
          SKIP_RETURN_CODE ${OA_GAME_DATA_SKIP_CODE})
        # A mod's copied install (OA_MOD_GAME_DIR) and the same files as a
        # mod folder over the game folder load the same unit types through
        # the layout of its profile among OA_MOD_PROFILES_DIR's, and never
        # look up a directory the profile renames by its base name.
        if(DEFINED OA_MOD_GAME_DIR AND DEFINED OA_MOD_PROFILES_DIR)
          add_test(NAME native-mod-layout-data COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_mod_layout.py"
            --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
            --mod-install "${OA_MOD_GAME_DIR}" --profiles "${OA_MOD_PROFILES_DIR}"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
          set_tests_properties(native-mod-layout-data PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
            SKIP_RETURN_CODE ${OA_GAME_DATA_SKIP_CODE})
          # Two machines in one process play a network game on that install
          # by its profile's network and recorder rules, end with equal
          # worlds, and every recording they made plays back.
          add_test(NAME native-mod-net-loopback COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_mod_loopback.py"
            --native $<TARGET_FILE:oa-game>
            --mod-install "${OA_MOD_GAME_DIR}" --profiles "${OA_MOD_PROFILES_DIR}"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
          set_tests_properties(native-mod-net-loopback PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
            SKIP_RETURN_CODE ${OA_GAME_DATA_SKIP_CODE})
          # Every unit that stockpiles on that install works its weapon page
          # by its profile, as native-stockpile-builds checks on the game.
          add_test(NAME native-mod-stockpile-builds COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_mod_stockpile.py"
            --native $<TARGET_FILE:oa-game>
            --mod-install "${OA_MOD_GAME_DIR}" --profiles "${OA_MOD_PROFILES_DIR}"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
          set_tests_properties(native-mod-stockpile-builds PROPERTIES
            TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT} SKIP_RETURN_CODE ${OA_GAME_DATA_SKIP_CODE})
          # The pages of a commander, a lab, a unit that builds nothing and one
          # that stockpiles on that install fit the side column by its
          # profile, as native-unit-pages checks on the game, drawn as authored
          # in a column narrowed as a whole to the profile's tallest page.
          add_test(NAME native-mod-unit-pages COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_mod_unit_pages.py"
            --native $<TARGET_FILE:oa-game>
            --mod-install "${OA_MOD_GAME_DIR}" --profiles "${OA_MOD_PROFILES_DIR}"
            --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
          set_tests_properties(native-mod-unit-pages PROPERTIES
            TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT} SKIP_RETURN_CODE ${OA_GAME_DATA_SKIP_CODE})
        endif()
        # Arm mission 1 (AC01) played with --give-orders is won at the
        # Galactic Gate and goes on through the end screen into AC02; both
        # briefings are drawn in their background's palette and ARM's colours.
        add_test(NAME native-campaign-advance COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_campaign_advance.py"
          --native $<TARGET_FILE:oa-game> --tool $<TARGET_FILE:oa-tool> --game-dir "${OA_GAME_DIR}"
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        set_tests_properties(native-campaign-advance PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
      endif()
    else()
      message(STATUS "Native checks not added: OA_GAME_DIR names no installed game")
    endif()
    # oa-game started on the folder that holds the installer of the Total
    # Annihilation demo (1997) OA_DEMO_INSTALLER names unpacks its archive
    # into a scratch data folder, checks it, mounts it and opens the main
    # menu with no dialog over it, and on a second start checks and mounts it
    # again; skipped without an installer.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME native-demo-installer COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_demo_installer.py"
        --native $<TARGET_FILE:oa-game> --verifier $<TARGET_FILE:oa-demo-installer-test>
        --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
      # oa-game's own checks over the demo, started the same way: the
      # navigation check's notices, grayed-out entries and campaign, the
      # grayed-out save and load entries, the notice MULTI shows over data
      # with no multiplayer map, and the accelerated presentation on SDL's
      # software renderer taken as capable over the main menu and the first
      # Arm mission (--check-render-tiers --hardware-acceleration
      # --force-capable), which skips under 2 GiB of memory.
      set(oa_demo_game_checks navigation saved-games multiplayer-menu render-tiers)
      foreach(check IN LISTS oa_demo_game_checks)
        add_test(NAME native-demo-${check} COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_demo_installer.py"
          --native $<TARGET_FILE:oa-game> --check ${check}
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
      endforeach()
      list(TRANSFORM oa_demo_game_checks PREPEND native-demo- OUTPUT_VARIABLE oa_demo_native_tests)
      # The demo's last Arm mission, AC03, won at easy with --give-orders,
      # leaves its end screen through the ending for the main menu.
      add_test(NAME native-demo-campaign-ending COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_campaign_ending.py"
        --native $<TARGET_FILE:oa-game> --mission 2
        --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
      list(APPEND oa_demo_native_tests native-demo-campaign-ending)
      # Each of the demo's Arm missions, AC01 to AC03 (--mission 0 to 2),
      # starts from its briefing and plays without a failed tick, and a save
      # made inside each loads back into the same world and plays on.
      foreach(mission IN ITEMS 0 1 2)
        math(EXPR mission_number "${mission} + 1")
        add_test(NAME native-demo-mission-ac0${mission_number} COMMAND ${Python3_EXECUTABLE}
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_demo_missions.py"
          --native $<TARGET_FILE:oa-game> --check mission --mission ${mission}
          --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
        list(APPEND oa_demo_native_tests native-demo-mission-ac0${mission_number})
      endforeach()
      add_test(NAME native-demo-mission-saveload COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_native_demo_missions.py"
        --native $<TARGET_FILE:oa-game> --check saveload
        --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
      list(APPEND oa_demo_native_tests native-demo-mission-saveload)
      set_tests_properties(native-demo-installer ${oa_demo_native_tests} PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
      oa_demo_installer_tests(native-demo-installer ${oa_demo_native_tests})
    endif()
  endif()
endif()
if(OA_REQUIRE_GAME AND NOT TARGET oa-game)
  message(FATAL_ERROR "OA_REQUIRE_GAME is ON, but oa-game is not built: it needs OA_BUILD_PLATFORM=ON")
endif()
add_executable(oa-tool tools/oa-tool/main.cpp)
target_link_libraries(oa-tool PRIVATE oa-formats-hpi oa-formats-png oa-ui-decoded)
# The Steam Deck's artwork and files for the Steam Deck package.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaSteamDeck.cmake")

if(OA_BUILD_PLATFORM)
  add_executable(oa-platform tools/platform-probe/platform_probe.cpp)
  target_link_libraries(oa-platform PRIVATE SDL3::SDL3 oa-options)
endif()

if(BUILD_TESTING)
  add_executable(oa-display-layout-test tests/display_layout_test.cpp)
  target_link_libraries(oa-display-layout-test PRIVATE oa-ui-display-layout)
  target_link_libraries(oa-display-layout-test PRIVATE oa-options)
  add_test(NAME display-layout COMMAND oa-display-layout-test)
  add_executable(oa-hpi-codec-test tests/hpi_codec_test.cpp)
  target_link_libraries(oa-hpi-codec-test PRIVATE oa-formats-sqsh)
  add_test(NAME hpi-codec COMMAND oa-hpi-codec-test)
  add_executable(oa-assets-test tests/assets_test.cpp)
  target_link_libraries(oa-assets-test PRIVATE oa-formats-hpi oa-base-bytes)
  add_test(NAME assets COMMAND oa-assets-test)
  set_tests_properties(hpi-codec assets PROPERTIES TIMEOUT 30)
  # The shared match fixture's projectile handle.
  add_executable(oa-projectile-handle-test tests/support/tests/projectile_handle_test.cpp)
  target_link_libraries(oa-projectile-handle-test PRIVATE oa-test-match oa-sim-weapon-execution oa-options)
  add_test(NAME projectile-handle COMMAND oa-projectile-handle-test)
  if(OA_BUILD_PLATFORM)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
      add_test(NAME platform-smoke COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_platform.py $<TARGET_FILE:oa-platform>)
      set_tests_properties(platform-smoke PROPERTIES TIMEOUT 60)
    endif()
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    if(UNIX)
      find_program(OA_BASH_EXECUTABLE bash)
      if(OA_BASH_EXECUTABLE)
        add_test(NAME launcher COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_launcher.py)
      endif()
      # tools/release_macos.sh packages the built game as the macOS release
      # does, as the zip and the installer package, and checks both; the
      # test signs ad hoc, and sends nothing to Apple.
      if(APPLE AND OA_BASH_EXECUTABLE AND TARGET oa-game)
        add_test(NAME release-macos-package COMMAND ${OA_BASH_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/release_macos.sh --app $<TARGET_BUNDLE_DIR:oa-game> --unsigned
          --out ${CMAKE_CURRENT_BINARY_DIR}/release-macos-package)
        set_tests_properties(release-macos-package PROPERTIES TIMEOUT 300)
      endif()
    endif()
    # The icons made from the Open Annihilation icon (tools/make_icons.py)
    # have the sizes the builds and the platforms want.
    add_test(NAME branding-icons COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icons.py --check)
    set_tests_properties(branding-icons PROPERTIES TIMEOUT 60)
    # The macOS and iOS bundles declare the .oamod mod package as the game's own type and open
    # it as its owner, with the icon each system takes (tools/check_bundle_file_types.py).
    add_test(NAME app-bundle-file-types COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_bundle_file_types.py
      ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Info.plist.in ${CMAKE_CURRENT_SOURCE_DIR}/platforms/ios/Info.plist.in)
    set_tests_properties(app-bundle-file-types PROPERTIES TIMEOUT 60)
    # The engine's sources keep to the style rules of tools/check_style.py
    # with exactly the counts per file that tools/style-baseline.json holds,
    # a baseline that may only shrink; the self-test plants a breach of every
    # rule in small trees and requires the check to find exactly those.
    add_test(NAME style-ratchet COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_style.py
      --root ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME style-ratchet-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_style_test.py)
    set_tests_properties(style-ratchet style-ratchet-selftest PROPERTIES TIMEOUT 300)
    # The tables and records generated from the OAMOD registry
    # (tools/gen_mod_registry.py) are what the registry gives, and the
    # registry is consistent.
    add_test(NAME mod-registry-sync COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_mod_registry.py --check)
    add_test(NAME mod-registry-sync-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_mod_registry.py --self-test)
    set_tests_properties(mod-registry-sync mod-registry-sync-selftest PROPERTIES TIMEOUT 60)
    # The standard hack and script extension pages in docs/mods, their
    # tables in the README and table 7.2 of the OAMOD standard hold what the
    # OAMOD registry gives (tools/gen_mod_docs.py), and the tool still keeps
    # prose and finds changed blocks in a small built-in registry.
    add_test(NAME mod-docs-sync COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_mod_docs.py --check)
    add_test(NAME mod-docs-sync-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_mod_docs.py --self-test)
    set_tests_properties(mod-docs-sync mod-docs-sync-selftest PROPERTIES TIMEOUT 60)
    # Relative links, anchors and repository paths in the engine's Markdown
    # files resolve (tools/check_links.py), and the check still finds what it
    # should in a small built-in tree.
    add_test(NAME doc-links COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_links.py
      --root ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME doc-links-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_links.py --self-test)
    set_tests_properties(doc-links doc-links-selftest PROPERTIES TIMEOUT 60)
    # The engine's C, C++ and Objective-C sources are laid out as
    # .clang-format says, by the clang-format release that
    # tools/format/requirements.txt pins (tools/format_sources.py, whose
    # --setup installs it under local/); without that release the check
    # skips.
    add_test(NAME format-check COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/format_sources.py
      --root ${CMAKE_CURRENT_SOURCE_DIR} --check)
    set_tests_properties(format-check PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
    # Every file of the engine states its copyright and licence: an SPDX
    # header, an annotation in REUSE.toml, or a licence text's own terms
    # (tools/spdx_headers.py).
    add_test(NAME licensing-check COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/spdx_headers.py
      --root ${CMAKE_CURRENT_SOURCE_DIR} --check)
    set_tests_properties(licensing-check PROPERTIES TIMEOUT 60)
    # The self-tests run each whole-tree tool on small cases whose results
    # are known.
    add_test(NAME fixed-width-rewrite-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/rewrite_fixed_width.py --self-test)
    add_test(NAME format-check-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/format_sources.py --self-test)
    add_test(NAME licensing-check-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/spdx_headers.py --self-test)
    # tools/bootstrap_sdl.py reads its command line as its help says: the
    # pinned releases, a release given with its SHA-256, the refusals, and
    # the folders each release builds in; and its SDL patches read and
    # apply (tools/sdl-patches/README.md). It downloads nothing.
    add_test(NAME sdl-bootstrap-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/bootstrap_sdl.py --self-test)
    # The macOS and iOS bootstraps reuse an installed SDL only when it
    # records this run's settings, its patches among them, so an SDL built
    # before a patch was added or changed is built again. They build and
    # download nothing.
    add_test(NAME macos-deps-bootstrap-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/bootstrap_macos_deps.py --self-test)
    add_test(NAME ios-deps-bootstrap-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/platforms/ios/tools/bootstrap_ios_deps.py --self-test)
    # tools/bootstrap_text_fonts.py's pins, the licence texts they must
    # match and the characters the cut CJK font keeps. It downloads nothing.
    add_test(NAME text-fonts-bootstrap-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/bootstrap_text_fonts.py --self-test)
    # tools/language_pack_check.py on the tests' pseudo-language pack, and
    # on the engine's Simplified Chinese pack against the game data when
    # OA_GAME_DIR is set: each pack reads, and its coverage is told. A
    # missing pack is skipped.
    add_test(NAME language-pack-check-pseudo COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/language_pack_check.py
      ${CMAKE_CURRENT_SOURCE_DIR}/src/data/languages/tests/pseudo-pack)
    set(oa_language_pack_game_data "")
    if(OA_GAME_DIR)
      set(oa_language_pack_game_data --game-dir "${OA_GAME_DIR}" --oa-tool "$<TARGET_FILE:oa-tool>")
    endif()
    add_test(NAME language-pack-check COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/language_pack_check.py
      ${CMAKE_CURRENT_SOURCE_DIR}/languages/zh-Hans ${oa_language_pack_game_data})
    set_tests_properties(language-pack-check PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(fixed-width-rewrite-selftest format-check-selftest licensing-check-selftest
      sdl-bootstrap-selftest macos-deps-bootstrap-selftest ios-deps-bootstrap-selftest
      text-fonts-bootstrap-selftest language-pack-check-pseudo language-pack-check
      PROPERTIES TIMEOUT 60)
  endif()
endif()

if(WIN32)
  set(oa_executables oa-tool)
  if(OA_BUILD_PLATFORM)
    list(APPEND oa_executables oa-platform)
  endif()
  if(BUILD_TESTING)
    list(APPEND oa_executables oa-hpi-codec-test oa-assets-test)
  endif()
  foreach(target IN LISTS oa_executables)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DDLLS=$<TARGET_RUNTIME_DLLS:${target}>"
        "-DDEST=$<TARGET_FILE_DIR:${target}>" -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_runtime.cmake)
  endforeach()
endif()

add_subdirectory(src/ui/gadget-render)
add_subdirectory(src/ui/frontend-dialogs)
add_subdirectory(tests/content)
if(TARGET oa-game)
  target_sources(oa-game PRIVATE src/app/runtime_dialog_checks.cpp)
endif()

# Network play: the multiplayer screens, the network session and match, the
# .tad recording format and its playback, and oa-game's extension
# oa-app-netgame, which takes the 3.1c network switches -e -h -n -p -t the
# game otherwise refuses. Its state for each runtime
# (src/app/netgame/network_play.hpp) is its extension's own, and Runtime's
# friend. Registered before any other extension, so that the others come
# after it in the extension list.
add_subdirectory(src/formats/tad)
add_subdirectory(src/ui/frontend-multiplayer)
add_subdirectory(src/netgame)
add_subdirectory(src/session/demo)
add_subdirectory(src/app/netgame)
if(TARGET oa-app-netgame)
  oa_add_extension(oa-app-netgame INIT oa_extension_init_netgame SWITCHES ehnpt)
endif()
# The automation endpoint: the automation protocol's codec, and the
# endpoint a program on this machine drives the game through, oa-game's
# extension oa-app-automation, which takes --fark, --fark-address and
# --fark-file and does nothing without --fark. Registered after network
# play's, so that it comes after it in the extension list.
add_subdirectory(src/app/automation)
if(TARGET oa-app-automation)
  oa_add_extension(oa-app-automation INIT oa_extension_init_automation)
endif()
# The tests' view of the installation network play's tests read
# (oa/test/netgame_installs.hpp), and network play's launch bound to a
# record, for tests that check what a caller of network play's API asked
# of it (oa/test/netgame_launch_record.hpp).
add_library(oa-netgame-test-installs INTERFACE)
add_library(oa::netgame::test_installs ALIAS oa-netgame-test-installs)
target_link_libraries(oa-netgame-test-installs INTERFACE oa-test-game-data oa-formats-hpi)
add_library(oa-netgame-test-launch STATIC tests/support/src/netgame_launch_record.cpp)
add_library(oa::netgame::test_launch ALIAS oa-netgame-test-launch)
target_include_directories(oa-netgame-test-launch PUBLIC tests/support/include)
target_link_libraries(oa-netgame-test-launch PUBLIC oa-app-netgame-launch PRIVATE oa-app-netgame-launch-binding)

# A project that adds this engine with add_subdirectory registers its
# extension libraries with oa_add_extension afterwards
# (cmake/OaExtensions.cmake); a library that includes runtime.hpp links
# oa::extension-sdk. Before adding the engine, one of those projects may set
# OA_RUNTIME_EXTENSION_MEMBERS, the absolute path of a header of members
# runtime.hpp includes inside class Runtime. It is frozen: it may only shrink
# (tools/check_runtime_surface.py) until hooks and declared headers replace
# it. oa-game's own sources and, through the SDK, the extensions' see the
# same Runtime; a unit that sees another fails to link
# (src/app/extension_members.cpp). tests/extension checks that it does, and
# holds the recorder test extensions, which name their own members header
# here when OA_RECORD_EXTENSION_HOOKS is on.
add_subdirectory(tests/extension)
if(TARGET oa-game)
  set(oa_runtime_members_definition "")
  if(OA_RUNTIME_EXTENSION_MEMBERS)
    if(NOT IS_ABSOLUTE "${OA_RUNTIME_EXTENSION_MEMBERS}" OR NOT EXISTS "${OA_RUNTIME_EXTENSION_MEMBERS}")
      message(FATAL_ERROR "OA_RUNTIME_EXTENSION_MEMBERS (\"${OA_RUNTIME_EXTENSION_MEMBERS}\") "
        "must be the absolute path of an existing header")
    endif()
    set(oa_runtime_members_definition "OA_RUNTIME_EXTENSION_MEMBERS=\"${OA_RUNTIME_EXTENSION_MEMBERS}\"")
  endif()
  # What an extension library compiles and links against: extension.hpp,
  # and runtime.hpp with the libraries oa-game's own sources use (those
  # above and the frontend dialogs).
  add_library(oa-extension-sdk INTERFACE)
  add_library(oa::extension::sdk ALIAS oa-extension-sdk)
  # The name the projects that build on the engine link.
  add_library(oa::extension-sdk ALIAS oa-extension-sdk)
  target_link_libraries(oa-extension-sdk INTERFACE oa-app-headers)
  target_link_libraries(oa-extension-sdk INTERFACE oa-ui-display-layout)
  target_link_libraries(oa-extension-sdk INTERFACE SDL3::SDL3 ${oa_game_libraries} oa-ui-frontend-dialogs)
  target_compile_definitions(oa-extension-sdk INTERFACE ${oa_runtime_members_definition})
  target_compile_definitions(oa-game PRIVATE ${oa_runtime_members_definition})
  target_sources(oa-game PRIVATE src/app/extension_members.cpp)
  # The registered extensions are listed and linked when the configure ends.
  oa_schedule_extension_list()
endif()
# tools/check_runtime_surface.py, which holds an extension's members header,
# when it has one, and its use of Runtime's private names to
# tools/runtime-surface-baseline.json,
# still measures what it should in a small built-in tree; and every name that
# baseline allows is still a private name of Runtime, so renaming, removing
# or publishing one changes the baseline, and the extension, with it.
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME runtime-surface-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_runtime_surface.py --self-test)
    add_test(NAME runtime-surface-names COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_runtime_surface.py --baseline-names)
    set_tests_properties(runtime-surface-selftest runtime-surface-names PROPERTIES TIMEOUT 60)
    # Engine code calls every hook of the extension tables through call_hook
    # (hook_call.hpp), and each hook's handling there agrees with its
    # documentation.
    add_test(NAME hook-calls-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_hook_calls.py --self-test)
    add_test(NAME hook-calls COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_hook_calls.py)
    set_tests_properties(hook-calls-selftest hook-calls PROPERTIES TIMEOUT 60)
    # Every test of this build is named as the naming rule says
    # (docs/development/conventions.md); the check asks ctest for the
    # tests, so it sees every directory's.
    add_test(NAME ctest-names-selftest COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ctest_names.py --self-test)
    add_test(NAME ctest-names COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ctest_names.py --ctest ${CMAKE_CTEST_COMMAND}
      --build ${CMAKE_CURRENT_BINARY_DIR} --config "$<CONFIG>" --skip-code ${OA_GAME_DATA_SKIP_CODE})
    set_tests_properties(ctest-names-selftest ctest-names PROPERTIES TIMEOUT 120)
  endif()
endif()
# The project's licence, the attributions and the licence texts they link to
# travel beside every executable the packages ship; with the game they go in
# its OA_GAME_FILES_DIR, the bundle's Resources on macOS, and follow
# whatever the extensions add there (their GAME_FILES, which the script
# cmake/OaExtensions.cmake writes runs). The executables may share a
# directory, so the copies take turns on one lock. An executable copies them
# when it links, and a change to the notices alone links it again (in Ninja
# and Makefile builds), so a build tree never keeps older notices than its
# sources.
# The fonts travel with the game in the fonts folder of its
# OA_GAME_FILES_DIR, where oa::platform::text_font::bundled_font_directory()
# finds them (cmake/OaTextFonts.cmake).
if(TARGET oa-game)
  oa_copy_text_fonts(oa-game "$<GENEX_EVAL:$<TARGET_PROPERTY:oa-game,OA_GAME_FILES_DIR>>")
  # The engine's language packs go in its languages folder, where the game
  # reads them beside the interface catalogue files.
  oa_copy_language_packs(oa-game "$<GENEX_EVAL:$<TARGET_PROPERTY:oa-game,OA_GAME_FILES_DIR>>")
endif()
file(GLOB oa_notice_licenses CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/licenses/*")
set(oa_notice_files "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${CMAKE_CURRENT_SOURCE_DIR}/ATTRIBUTIONS.md"
  ${oa_notice_licenses} "${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_notices.cmake")
foreach(target IN ITEMS oa-game oa-intro oa-tool)
  if(NOT TARGET ${target})
    continue()
  endif()
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${oa_notice_files})
  set(oa_extension_files "")
  set(oa_notices_dir "$<TARGET_FILE_DIR:${target}>")
  if(target STREQUAL "oa-game")
    set(oa_extension_files COMMAND ${CMAKE_COMMAND}
      -P "${CMAKE_CURRENT_BINARY_DIR}/generated/extension-game-files-$<CONFIG>.cmake")
    set(oa_notices_dir "$<GENEX_EVAL:$<TARGET_PROPERTY:oa-game,OA_GAME_FILES_DIR>>")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${oa_notices_dir}"
    ${oa_extension_files}
    COMMAND ${CMAKE_COMMAND} "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DDEST=${oa_notices_dir}"
      "-DLOCK_FILE=${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/oa-notices.lock"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_notices.cmake"
    VERBATIM)
endforeach()
# oa-doc-check compiles the public headers with Clang's documentation
# warnings as errors (cmake/OaDocumentationCheck.cmake).
include("${CMAKE_CURRENT_LIST_DIR}/cmake/OaDocumentationCheck.cmake")
oa_add_documentation_check()

# Every module keeps to the layout: no include directory or source reaches
# into another module, every include has a link path, links follow the
# layer order and names follow the naming rule (tools/check_layout.py,
# within tools/layout-baseline.json). The check reads the targets as this
# configuration defines them.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OaLayout.cmake")
oa_register_layout_check()
