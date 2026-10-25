# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Mod packages (.oamod): a package read and checked before anything is
# written, where it installs in the player's Mods folder and what it asks,
# its files unpacked into a staging folder a piece at a time, the renames
# that put them in place and keep one earlier version in the mod's .backup,
# the recovery of a change a stop left half done and the deletion of the
# folders a change drops; the packages waiting for the main menu and the
# change waiting for its run to end; the prompts' texts; and the hand-off
# of a second start's packages to the copy already running. No SDL.
add_library(oa-app-package-install src/package.cpp src/names.cpp src/plan_oamod.cpp src/plan_oamap.cpp
  src/plan_oalang.cpp src/change.cpp src/unpack.cpp src/files.cpp src/inbox.cpp src/prompts.cpp
  src/handoff.cpp src/kinds.cpp src/prompts_oamod.cpp src/prompts_oamap.cpp src/prompts_oalang.cpp
  src/prompts_oaexp.cpp src/plan_oaexp.cpp src/plan_oamut.cpp src/prompts_oamut.cpp
  src/plan_oabrain.cpp src/prompts_oabrain.cpp src/plan_oakeys.cpp
  src/origin.cpp)
add_library(oa::app::package_install ALIAS oa-app-package-install)
target_include_directories(oa-app-package-install PUBLIC include)
target_compile_features(oa-app-package-install PUBLIC cxx_std_20)
target_link_libraries(oa-app-package-install PUBLIC oa-formats-zip oa-formats-hpi oa-data-mod-profile
  oa-data-brain-library oa-data-map-pack oa-data-map-fit oa-platform-preferences
  oa-ui-engine-settings oa-base-sha256
  PRIVATE oa-formats-oamod oa-formats-oamut oa-formats-oabrain oa-formats-oakeys
  oa-data-mutator-library oa-data-key-library oa-ui-key-bindings
  oa-data-content-bundles oa-formats-png oa-data-languages
  oa-data-registry oa-app-mod-profile oa-app-user-folder oa-app-game-directory
  oa-ui-game-files oa-platform-shims oa-platform-file-types
  oa-platform-text-font oa-base-threads oa-options)
set_property(SOURCE src/plan_oamap.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
set_property(SOURCE src/plan_oalang.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
set_property(SOURCE src/plan_oamut.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
set_property(SOURCE src/plan_oabrain.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
set_property(SOURCE src/plan_oakeys.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")

if(BUILD_TESTING)
  # Packages read and checked: a Finder-made package with its top folder
  # stripped, a package with its profile at its top, its own .backup left
  # out, and each refusal; the portable name rule and the folder-safe
  # version.
  add_executable(oa-app-package-install-package-test tests/package_test.cpp)
  target_link_libraries(oa-app-package-install-package-test PRIVATE oa-app-package-install oa-formats-zip
    ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME app-mod-install-package COMMAND oa-app-package-install-package-test)
  # The planner's decision table, row by row, and each prompt's title,
  # buttons, first mark and sentences, each fitting its height.
  add_executable(oa-app-package-install-plan-test tests/plan_test.cpp)
  target_link_libraries(oa-app-package-install-plan-test PRIVATE oa-app-package-install oa-test-support
    oa-options)
  add_test(NAME app-mod-install-plan COMMAND oa-app-package-install-plan-test)
  # The renames in a scratch Mods folder: an install, three replaces that
  # keep one .backup, a reinstall, two roll backs, a rename refused at
  # each step, every crash point settled, the lock, the retries, the
  # unpacking and the deletion of discard folders.
  add_executable(oa-app-package-install-change-test tests/change_test.cpp)
  target_link_libraries(oa-app-package-install-change-test PRIVATE oa-app-package-install oa-formats-zip
    oa-platform-shims ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME app-mod-install-change COMMAND oa-app-package-install-change-test)
  # Uninstalls in a scratch Mods folder: a mod and its .backup moved aside
  # whole and deleted a step at a time, links never followed, names that are
  # not one plain folder, folders of the player's own, a changed folder, an
  # unfinished change and the lock refused, a refused rename leaving the mod
  # whole, and a deletion stopped halfway finished by the next recovery.
  add_executable(oa-app-package-install-uninstall-test tests/uninstall_test.cpp)
  target_link_libraries(oa-app-package-install-uninstall-test PRIVATE oa-app-package-install
    oa-test-support oa-options)
  add_test(NAME app-package-install-uninstall COMMAND oa-app-package-install-uninstall-test)
  set_tests_properties(app-package-install-uninstall PROPERTIES TIMEOUT 30)
  # The instance lock and the hand-off folder.
  add_executable(oa-app-package-install-handoff-test tests/handoff_test.cpp)
  target_link_libraries(oa-app-package-install-handoff-test PRIVATE oa-app-package-install oa-test-support
    oa-options)
  add_test(NAME app-mod-install-handoff COMMAND oa-app-package-install-handoff-test)
  # A file opened in the game: each extension's queue, and a repeat dropped.
  add_executable(oa-app-package-install-inbox-test tests/inbox_test.cpp)
  target_link_libraries(oa-app-package-install-inbox-test PRIVATE oa-app-package-install
    oa-test-support oa-options)
  add_test(NAME app-package-install-inbox COMMAND oa-app-package-install-inbox-test)
  set_tests_properties(app-package-install-inbox PROPERTIES TIMEOUT 30)
  add_executable(oa-app-package-install-kinds-test tests/kinds_test.cpp)
  target_link_libraries(oa-app-package-install-kinds-test PRIVATE oa-app-package-install oa-test-support oa-options)
  add_test(NAME app-package-install-kinds COMMAND oa-app-package-install-kinds-test)
  set_tests_properties(app-package-install-kinds PROPERTIES TIMEOUT 30)
  # Origin records: the text, an install's hash, replace, roll back,
  # reinstall and recovery carrying the record, and the catalogue outcomes.
  add_executable(oa-app-package-install-origin-test tests/origin_test.cpp)
  target_link_libraries(oa-app-package-install-origin-test PRIVATE oa-app-package-install
    oa-formats-zip oa-base-sha256 oa-platform-shims ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME app-package-install-origin COMMAND oa-app-package-install-origin-test)
  set_tests_properties(app-package-install-origin PROPERTIES TIMEOUT 60)
  # A map pack: its manifest, the files it may hold, its previews, the engine
  # it needs, a catalogue replace and a question, and a fit check through hooks.
  add_executable(oa-app-package-install-oamap-test tests/oamap_test.cpp)
  target_link_libraries(oa-app-package-install-oamap-test PRIVATE oa-app-package-install
    oa-formats-zip oa-formats-png oa-base-sha256 oa-data-map-pack oa-data-map-fit
    ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME app-package-install-oamap COMMAND oa-app-package-install-oamap-test)
  set_tests_properties(app-package-install-oamap PROPERTIES TIMEOUT 30)
  # A mutator: its manifest, the files it may hold, its scripts and icon, the
  # engine it needs, a catalogue replace, a question, and each refusal.
  add_executable(oa-app-package-install-oamut-test tests/oamut_test.cpp)
  target_link_libraries(oa-app-package-install-oamut-test PRIVATE oa-app-package-install
    oa-formats-zip oa-formats-png oa-formats-oamut oa-base-sha256 ZLIB::ZLIB oa-test-support
    oa-options)
  add_test(NAME app-package-install-oamut COMMAND oa-app-package-install-oamut-test)
  set_tests_properties(app-package-install-oamut PROPERTIES TIMEOUT 30)
  # A brain: its manifest, the files it may hold, its scripts, icon, networks
  # and tables, the engine it needs, a catalogue replace, a question, a staged
  # network hash, and each refusal.
  add_executable(oa-app-package-install-oabrain-test tests/oabrain_test.cpp)
  target_link_libraries(oa-app-package-install-oabrain-test PRIVATE oa-app-package-install
    oa-formats-zip oa-formats-png oa-formats-oabrain oa-base-sha256 ZLIB::ZLIB oa-test-support
    oa-options)
  add_test(NAME app-package-install-oabrain COMMAND oa-app-package-install-oabrain-test)
  set_tests_properties(app-package-install-oabrain PROPERTIES TIMEOUT 30)
  # A language pack: its manifest, the engine it needs, its tables, fonts and
  # warm-up, a question, a replace that keeps .backup, and a font that does
  # not open after unpacking.
  add_executable(oa-app-package-install-oalang-test tests/oalang_test.cpp)
  target_link_libraries(oa-app-package-install-oalang-test PRIVATE oa-app-package-install
    oa-formats-zip oa-data-languages oa-platform-text-font oa-base-sha256 ZLIB::ZLIB
    oa-test-support oa-options)
  oa_copy_text_fonts(oa-app-package-install-oalang-test
    "$<TARGET_FILE_DIR:oa-app-package-install-oalang-test>")
  get_filename_component(oa_pseudo_pack
    "${CMAKE_CURRENT_SOURCE_DIR}/../../data/languages/tests/pseudo-pack" ABSOLUTE)
  add_test(NAME app-package-install-oalang COMMAND oa-app-package-install-oalang-test
    "${oa_pseudo_pack}")
  set_tests_properties(app-package-install-oalang PROPERTIES TIMEOUT 60)
  # An expansion: the built-in table refuses the fixture and writes nothing;
  # the test table installs it, a reinstall drops a stray file, uninstall
  # leaves a sibling, and a link in the expansion's place is left alone.
  add_executable(oa-app-package-install-oaexp-test tests/oaexp_test.cpp)
  target_link_libraries(oa-app-package-install-oaexp-test PRIVATE oa-app-package-install
    oa-data-content-bundles oa-formats-zip oa-base-sha256 ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME app-package-install-oaexp COMMAND oa-app-package-install-oaexp-test)
  set_tests_properties(app-package-install-oaexp PROPERTIES TIMEOUT 30)
endif()
