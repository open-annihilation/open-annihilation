# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# oa-tool's commands. The executable stays in the root CMakeLists.txt, where
# the Windows DLL copy and the packaged notices attach to it.
add_library(oa-tool-commands STATIC commands.cpp arguments.cpp files.cpp archive_commands.cpp
  pack.cpp pack_map.cpp check.cpp check_map.cpp check_brain.cpp check_mutator.cpp check_keys.cpp
  keys.cpp brain.cpp
  brain_net.cpp onnx.cpp
  mutator.cpp mutator_diff.cpp
  mutator_report.cpp catalogue.cpp engine_feed.cpp passphrase.cpp registry.cpp expansion.cpp
  interface_words.cpp)
add_library(oa::tool::commands ALIAS oa-tool-commands)
target_include_directories(oa-tool-commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The interface-word lists are headers in the modules that look the texts up.
# The command includes those headers and does not link the modules.
target_include_directories(oa-tool-commands PRIVATE
  ${PROJECT_SOURCE_DIR}/src/app/include
  ${PROJECT_SOURCE_DIR}/src/app/package-install/include
  ${PROJECT_SOURCE_DIR}/src/netgame/match/include
  ${PROJECT_SOURCE_DIR}/src/ui/engine-settings/include
  ${PROJECT_SOURCE_DIR}/src/ui/first-run/include
  ${PROJECT_SOURCE_DIR}/src/ui/folder-chooser/include
  ${PROJECT_SOURCE_DIR}/src/ui/frontend-dialogs/include
  ${PROJECT_SOURCE_DIR}/src/ui/frontend-multiplayer/include
  ${PROJECT_SOURCE_DIR}/src/ui/game-files/include
  ${PROJECT_SOURCE_DIR}/src/ui/library/include
  ${PROJECT_SOURCE_DIR}/src/ui/map-browser/include
  ${PROJECT_SOURCE_DIR}/src/ui/mutator-picker/include
  ${PROJECT_SOURCE_DIR}/src/ui/brain-picker/include
  ${PROJECT_SOURCE_DIR}/src/ui/touch-hud/include)
target_include_directories(oa-tool-commands SYSTEM PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/monocypher)
target_compile_features(oa-tool-commands PUBLIC cxx_std_20)
target_link_libraries(oa-tool-commands
  PRIVATE oa-options oa-app-brain-check oa-app-package-install oa-app-game-directory
    oa-data-content-bundles oa-data-languages
    oa-data-map-pack oa-data-map-fit oa-data-script-host oa-data-mutator-import
    oa-data-mutators oa-data-defs oa-data-defs-assets oa-formats-tdf
    oa-formats-oamod oa-formats-oamut oa-formats-oabrain oa-formats-oakeys oa-formats-oann
    oa-data-key-library oa-ui-key-bindings oa-ui-kit
    oa-data-neural
    oa-formats-soft-mutator oa-formats-json oa-formats-ota oa-formats-tnt
    oa-formats-zip oa-base-sha256 oa-base-text oa-platform-shims oa-base-signing oa-data-catalogue
    oa-data-engine-feed oa-data-registry oa-platform-random oa-app-content oa-netgame-http
    oa-formats-url
  PUBLIC oa-formats-hpi oa-formats-png oa-ui-decoded)
target_compile_definitions(oa-tool-commands PRIVATE "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")

if(BUILD_TESTING)
  add_executable(oa-tool-test tests/oa_tool_test.cpp)
  target_link_libraries(oa-tool-test PRIVATE oa-tool-commands oa-test-support oa-options)
  add_test(NAME tools-oa-tool COMMAND oa-tool-test)
  set_tests_properties(tools-oa-tool PROPERTIES TIMEOUT 60)

  # pack: a made-up mod and the pseudo language pack, the fixed order and
  # methods, identical bytes after the times change, and the refusals.
  add_executable(oa-tool-pack-test tests/pack_test.cpp)
  target_link_libraries(oa-tool-pack-test PRIVATE oa-tool-commands oa-app-package-install
    oa-data-languages oa-formats-zip oa-base-sha256 oa-platform-shims oa-test-support oa-options)
  add_test(NAME tools-oa-tool-pack COMMAND oa-tool-pack-test
    ${CMAKE_SOURCE_DIR}/src/data/languages/tests/pseudo-pack)
  set_tests_properties(tools-oa-tool-pack PROPERTIES TIMEOUT 60)

  # pack-map: two maps, a shared feature, a preview and the refusals. The
  # source and stand-in game folders are left in the test's working folder.
  add_executable(oa-tool-pack-map-test tests/pack_map_test.cpp)
  target_link_libraries(oa-tool-pack-map-test PRIVATE oa-tool-commands oa-data-map-pack
    oa-formats-zip oa-formats-png oa-formats-tnt oa-base-sha256 oa-platform-shims oa-test-support
    oa-options)
  add_test(NAME tools-oa-tool-pack-map COMMAND oa-tool-pack-map-test)
  set_tests_properties(tools-oa-tool-pack-map PROPERTIES TIMEOUT 60)

  # check: a made-up mod's catalogue facts, the refusals that still report
  # the file's size and hash, and a language manifest read on its own.
  add_executable(oa-tool-check-test tests/check_test.cpp)
  target_link_libraries(oa-tool-check-test PRIVATE oa-tool-commands oa-app-package-install
    oa-data-mod-profile oa-formats-json oa-base-sha256 ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME tools-oa-tool-check COMMAND oa-tool-check-test
    ${CMAKE_SOURCE_DIR}/src/data/languages/tests/pseudo-pack/language.yaml)
  set_tests_properties(tools-oa-tool-check PROPERTIES TIMEOUT 60)

  # check of a map pack: a stand-in game and, with --data, the installed game.
  add_executable(oa-tool-check-map-test tests/check_map_test.cpp)
  target_link_libraries(oa-tool-check-map-test PRIVATE oa-tool-commands oa-data-map-pack
    oa-app-package-install oa-formats-zip oa-formats-json oa-formats-hpi oa-formats-tnt
    oa-base-sha256 ZLIB::ZLIB oa-test-support oa-test-game-data oa-options)
  add_test(NAME tools-oa-tool-check-map COMMAND oa-tool-check-map-test)
  set_tests_properties(tools-oa-tool-check-map PROPERTIES TIMEOUT 120)
  oa_add_game_data_test(tools-oa-tool-check-map-data oa-tool-check-map-test --data)
  set_tests_properties(tools-oa-tool-check-map-data PROPERTIES TIMEOUT 120)

  # catalogue: one real keygen, then sign and verify against that key. The
  # Argon2id parameters are the ones a publisher key uses, so the test is
  # the slow one.
  add_executable(oa-tool-catalogue-test tests/catalogue_test.cpp)
  target_link_libraries(oa-tool-catalogue-test PRIVATE oa-tool-commands oa-test-support oa-options)
  add_test(NAME tools-oa-tool-catalogue COMMAND oa-tool-catalogue-test)
  set_tests_properties(tools-oa-tool-catalogue PROPERTIES TIMEOUT 120)

  # engine-feed: one real keygen, then write, sign and check against that key
  # and against the HTTP fixture. The fixture listens on 127.0.0.1 only.
  add_executable(oa-tool-engine-feed-test tests/engine_feed_test.cpp)
  target_link_libraries(oa-tool-engine-feed-test PRIVATE oa-tool-commands oa-netgame-http-fixture
    oa-data-engine-feed oa-base-sha256 oa-test-support oa-options)
  add_test(NAME tools-oa-tool-engine-feed COMMAND oa-tool-engine-feed-test)
  set_tests_properties(tools-oa-tool-engine-feed PROPERTIES TIMEOUT 120)

  # registry: one real keygen, then an empty registry and mirrors of it against
  # the HTTP fixture. The fixture listens on 127.0.0.1 only.
  add_executable(oa-tool-registry-test tests/registry_test.cpp)
  target_link_libraries(oa-tool-registry-test PRIVATE oa-tool-commands oa-netgame-http-fixture
    oa-app-content oa-data-catalogue oa-data-registry oa-base-sha256 oa-base-signing
    oa-formats-json oa-test-support oa-options)
  add_test(NAME tools-oa-tool-registry COMMAND oa-tool-registry-test)
  set_tests_properties(tools-oa-tool-registry PROPERTIES TIMEOUT 120)

  # interface-words: a wrapper handed a text no module lists fails this.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME tools-oa-tool-interface-words
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/interface_words_test.py
      $<TARGET_FILE:oa-tool> ${CMAKE_SOURCE_DIR})
  set_tests_properties(tools-oa-tool-interface-words PROPERTIES TIMEOUT 120)

  # The shipped catalogue covers every listed word in de, fr, es and it, and
  # a German lookup returns German. The file is the one copied beside the game.
  add_executable(oa-tool-interface-catalogue-test tests/interface_catalogue_test.cpp)
  target_link_libraries(oa-tool-interface-catalogue-test PRIVATE oa-tool-commands oa-data-languages
    oa-test-support oa-options)
  add_test(NAME tools-oa-tool-interface-catalogue COMMAND oa-tool-interface-catalogue-test
    ${CMAKE_SOURCE_DIR}/languages/interface.tdf)
  set_tests_properties(tools-oa-tool-interface-catalogue PROPERTIES TIMEOUT 60)

  # mutator: a made-up mutator packs to a .oamut, check reports its facts,
  # and a broken manifest, script, rule file, layout or icon is refused.
  add_executable(oa-tool-mutator-test tests/mutator_test.cpp)
  target_link_libraries(oa-tool-mutator-test PRIVATE oa-tool-commands oa-app-package-install
    oa-formats-zip oa-formats-json oa-formats-png oa-formats-oamut oa-base-sha256
    oa-platform-shims ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME tools-oa-tool-mutator COMMAND oa-tool-mutator-test)
  set_tests_properties(tools-oa-tool-mutator PROPERTIES TIMEOUT 60)

  # brain: a made-up brain packs to a .oabrain, check reports its facts,
  # and a broken manifest, script, layout, network or icon is refused.
  add_executable(oa-tool-brain-test tests/brain_test.cpp)
  target_link_libraries(oa-tool-brain-test PRIVATE oa-tool-commands oa-app-package-install
    oa-formats-zip oa-formats-json oa-formats-png oa-formats-oabrain oa-base-sha256
    oa-platform-shims ZLIB::ZLIB oa-test-support oa-options)
  add_test(NAME tools-oa-tool-brain COMMAND oa-tool-brain-test)
  set_tests_properties(tools-oa-tool-brain PROPERTIES TIMEOUT 60)


  # brain net: models this test writes, quantised and checked against the
  # integer network, and the refusals of a bad file or a bad invocation.
  add_executable(oa-tool-brain-net-test tests/brain_net_test.cpp)
  target_link_libraries(oa-tool-brain-net-test PRIVATE oa-tool-commands oa-formats-oann
    oa-data-neural oa-formats-json oa-base-sha256 oa-test-support oa-options)
  add_test(NAME tools-oa-tool-brain-net COMMAND oa-tool-brain-net-test)
  set_tests_properties(tools-oa-tool-brain-net PROPERTIES TIMEOUT 120)

  # brain check: usage without the game, and, with --data, seating made-up brains.
  add_executable(oa-tool-brain-check-test tests/brain_check_test.cpp)
  target_link_libraries(oa-tool-brain-check-test PRIVATE oa-tool-commands oa-app-package-install
    oa-formats-zip oa-formats-json oa-formats-oabrain oa-base-sha256 oa-test-support
    oa-test-game-data oa-options)
  add_test(NAME tools-oa-tool-brain-check COMMAND oa-tool-brain-check-test)
  set_tests_properties(tools-oa-tool-brain-check PROPERTIES TIMEOUT 60)
  oa_add_game_data_test(tools-oa-tool-brain-check-data oa-tool-brain-check-test --data)
  set_tests_properties(tools-oa-tool-brain-check-data PROPERTIES TIMEOUT 120)

  # mutator import: made-up .tam files and a zip become mutator folders.
  add_executable(oa-tool-mutator-import-test tests/mutator_import_test.cpp)
  target_link_libraries(oa-tool-mutator-import-test PRIVATE oa-tool-commands oa-test-support
    oa-options ZLIB::ZLIB)
  # The import writes ">= " and this version, as the commands are built with it.
  target_compile_definitions(oa-tool-mutator-import-test
    PRIVATE "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
  add_test(NAME tools-oa-tool-mutator-import COMMAND oa-tool-mutator-import-test)
  set_tests_properties(tools-oa-tool-mutator-import PROPERTIES TIMEOUT 60)

  # mutator diff: a stand-in game and, with --data, the installed game.
  add_executable(oa-tool-mutator-diff-test tests/mutator_diff_test.cpp)
  target_link_libraries(oa-tool-mutator-diff-test PRIVATE oa-tool-commands oa-data-defs
    oa-formats-hpi oa-base-sha256 oa-test-support oa-test-game-data oa-options ZLIB::ZLIB)
  add_test(NAME tools-oa-tool-mutator-diff COMMAND oa-tool-mutator-diff-test)
  set_tests_properties(tools-oa-tool-mutator-diff PROPERTIES TIMEOUT 60)
  oa_add_game_data_test(tools-oa-tool-mutator-diff-data oa-tool-mutator-diff-test --data)
  set_tests_properties(tools-oa-tool-mutator-diff-data PROPERTIES TIMEOUT 300)

  # An expansion packed and checked against the test table, and refused by
  # the commands, which use the built-in table.
  add_executable(oa-tool-expansion-test tests/expansion_test.cpp)
  target_link_libraries(oa-tool-expansion-test PRIVATE oa-tool-commands oa-data-content-bundles
    oa-formats-zip oa-base-sha256 oa-test-support oa-options)
  add_test(NAME tools-oa-tool-expansion COMMAND oa-tool-expansion-test)
  set_tests_properties(tools-oa-tool-expansion PROPERTIES TIMEOUT 30)
  if(WIN32)
    add_custom_command(TARGET oa-tool-registry-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DDLLS=$<TARGET_RUNTIME_DLLS:oa-tool-registry-test>"
        "-DDEST=$<TARGET_FILE_DIR:oa-tool-registry-test>"
        -P "${PROJECT_SOURCE_DIR}/tools/copy_runtime.cmake")
  endif()
endif()
