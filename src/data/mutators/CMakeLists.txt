# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(
  oa-data-mutators
  src/allow_list.cpp
  src/choice.cpp
  src/clashes.cpp
  src/clone.cpp
  src/collections.cpp
  src/collections_sides.cpp
  src/dry_run.cpp
  src/files_layer.cpp
  src/hacks.cpp
  src/hold_range.cpp
  src/integrity.cpp
  src/journal.cpp
  src/loadout.cpp
  src/map_rules.cpp
  src/maps.cpp
  src/menus.cpp
  src/pipeline.cpp
  src/rules_lua.cpp
  src/verdicts.cpp
  src/weapon_ids.cpp
)
add_library(oa::data::mutators ALIAS oa-data-mutators)
target_include_directories(oa-data-mutators PUBLIC include)
target_compile_features(oa-data-mutators PUBLIC cxx_std_20)
target_link_libraries(
  oa-data-mutators
  PUBLIC oa-base-sha256 oa-base-threads oa-data-defs oa-data-mod-profile oa-data-mutation-rules
         oa-data-script-host oa-formats-hpi oa-formats-oamut
  PRIVATE oa-data-defs-assets oa-data-field-schema oa-data-languages oa-data-mutator-library
          oa-formats-json oa-formats-tdf oa-platform-shims oa-options
)
if(BUILD_TESTING)
  add_executable(oa-data-mutators-allow-list-test tests/allow_list_test.cpp)
  target_link_libraries(oa-data-mutators-allow-list-test PRIVATE oa-data-mutators oa-test-support)
  add_test(NAME data-mutators-allow-list COMMAND oa-data-mutators-allow-list-test)

  add_executable(oa-data-mutators-charge-test tests/charge_test.cpp)
  target_include_directories(
    oa-data-mutators-charge-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  target_link_libraries(
    oa-data-mutators-charge-test PRIVATE oa-data-mutators oa-data-script-host oa-test-support
  )
  add_test(NAME data-mutators-charge COMMAND oa-data-mutators-charge-test)

  add_executable(oa-data-mutators-choice-test tests/choice_test.cpp)
  target_link_libraries(oa-data-mutators-choice-test PRIVATE oa-data-mutators oa-test-support)
  add_test(NAME data-mutators-choice COMMAND oa-data-mutators-choice-test)

  add_executable(oa-data-mutators-clashes-test tests/clashes_test.cpp)
  target_link_libraries(
    oa-data-mutators-clashes-test PRIVATE oa-data-mutators oa-formats-tdf oa-test-support
  )
  add_test(NAME data-mutators-clashes COMMAND oa-data-mutators-clashes-test)

  add_executable(oa-data-mutators-clone-test tests/clone_test.cpp)
  target_link_libraries(oa-data-mutators-clone-test PRIVATE oa-data-mutators oa-test-support)
  add_test(NAME data-mutators-clone COMMAND oa-data-mutators-clone-test)
  set_tests_properties(data-mutators-clone PROPERTIES TIMEOUT 30)

  add_executable(oa-data-mutators-collections-test tests/collections_test.cpp)
  target_link_libraries(
    oa-data-mutators-collections-test PRIVATE oa-data-mutators oa-data-script-host oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-collections-test
    PRIVATE "OA_MUTATORS_COLLECTIONS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/collections\""
  )
  add_test(NAME data-mutators-collections COMMAND oa-data-mutators-collections-test)

  add_executable(oa-data-mutators-hold-range-test tests/hold_range_test.cpp)
  target_link_libraries(
    oa-data-mutators-hold-range-test
    PRIVATE oa-data-mutators oa-data-defs oa-sim-map-runtime oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-hold-range-test
    PRIVATE "OA_MUTATORS_TEST_DATA=\"${CMAKE_SOURCE_DIR}/tests/data/mutators\""
  )
  add_test(NAME data-mutators-hold-range COMMAND oa-data-mutators-hold-range-test)
  set_tests_properties(data-mutators-hold-range PROPERTIES TIMEOUT 60)

  add_executable(oa-data-mutators-dry-run-test tests/dry_run_test.cpp)
  target_link_libraries(oa-data-mutators-dry-run-test PRIVATE oa-data-mutators oa-test-support)
  target_compile_definitions(
    oa-data-mutators-dry-run-test
    PRIVATE "OA_MUTATORS_TEST_DATA=\"${CMAKE_SOURCE_DIR}/tests/data/mutators\""
  )
  add_test(NAME data-mutators-dry-run COMMAND oa-data-mutators-dry-run-test)
  set_tests_properties(data-mutators-dry-run PROPERTIES TIMEOUT 60)

  add_executable(oa-data-mutators-files-layer-test tests/files_layer_test.cpp)
  target_link_libraries(
    oa-data-mutators-files-layer-test
    PRIVATE oa-data-mutators oa-formats-hpi oa-test-support oa-options
  )
  add_test(NAME data-mutators-files-layer COMMAND oa-data-mutators-files-layer-test)

  add_executable(oa-data-mutators-loadout-test tests/loadout_test.cpp)
  target_link_libraries(
    oa-data-mutators-loadout-test PRIVATE oa-data-mutators oa-data-mutator-library oa-test-support
  )
  add_test(NAME data-mutators-loadout COMMAND oa-data-mutators-loadout-test)
  set_tests_properties(data-mutators-loadout PROPERTIES TIMEOUT 30)

  add_executable(oa-data-mutators-rules-lua-test tests/rules_lua_test.cpp)
  target_link_libraries(
    oa-data-mutators-rules-lua-test PRIVATE oa-data-mutators oa-data-script-host oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-rules-lua-test
    PRIVATE "OA_MUTATORS_RULES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/rules_lua\""
  )
  add_test(NAME data-mutators-rules-lua COMMAND oa-data-mutators-rules-lua-test)

  add_executable(oa-data-mutators-journal-test tests/journal_test.cpp)
  target_link_libraries(oa-data-mutators-journal-test PRIVATE oa-data-mutators oa-test-support)
  target_compile_definitions(
    oa-data-mutators-journal-test
    PRIVATE "OA_MUTATORS_TEST_DATA=\"${CMAKE_SOURCE_DIR}/tests/data/mutators\""
  )
  add_test(NAME data-mutators-journal COMMAND oa-data-mutators-journal-test)
  set_tests_properties(data-mutators-journal PROPERTIES TIMEOUT 60)

  add_executable(oa-data-mutators-maps-test tests/maps_test.cpp)
  target_link_libraries(
    oa-data-mutators-maps-test
    PRIVATE oa-data-mutators oa-data-campaign oa-test-support oa-test-game-data
  )
  target_compile_definitions(
    oa-data-mutators-maps-test
    PRIVATE "OA_MUTATORS_TEST_DATA=\"${CMAKE_SOURCE_DIR}/tests/data/mutators\""
  )
  add_test(NAME data-mutators-maps COMMAND oa-data-mutators-maps-test)
  set_tests_properties(data-mutators-maps PROPERTIES TIMEOUT 30)

  add_executable(oa-data-mutators-map-rules-test tests/map_rules_test.cpp)
  target_link_libraries(
    oa-data-mutators-map-rules-test PRIVATE oa-data-mutators oa-formats-tdf oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-map-rules-test
    PRIVATE "OA_MUTATORS_MAP_RULES_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/map_rules\""
  )
  add_test(NAME data-mutators-map-rules COMMAND oa-data-mutators-map-rules-test)
  set_tests_properties(data-mutators-map-rules PROPERTIES TIMEOUT 30)
  oa_add_game_data_test(data-mutators-maps-data oa-data-mutators-maps-test --data)
  set_tests_properties(data-mutators-maps-data PROPERTIES TIMEOUT 300)

  add_executable(oa-data-mutators-pipeline-test tests/pipeline_test.cpp)
  target_link_libraries(
    oa-data-mutators-pipeline-test PRIVATE oa-data-mutators oa-data-defs-assets oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-pipeline-test
    PRIVATE "OA_MUTATORS_TEST_DATA=\"${CMAKE_SOURCE_DIR}/tests/data/mutators\""
  )
  add_test(NAME data-mutators-pipeline COMMAND oa-data-mutators-pipeline-test)
  set_tests_properties(data-mutators-pipeline PROPERTIES TIMEOUT 60)

  add_executable(oa-data-mutators-integrity-test tests/integrity_test.cpp)
  target_link_libraries(
    oa-data-mutators-integrity-test PRIVATE oa-data-mutators oa-data-defs-assets oa-test-support
  )
  target_compile_definitions(
    oa-data-mutators-integrity-test
    PRIVATE "OA_MUTATORS_INTEGRITY_DIR=\"${CMAKE_SOURCE_DIR}/tests/data/mutators/integrity\""
  )
  add_test(NAME data-mutators-integrity COMMAND oa-data-mutators-integrity-test)
  set_tests_properties(data-mutators-integrity PROPERTIES TIMEOUT 60)

  add_executable(oa-data-mutators-menus-test tests/menus_test.cpp)
  target_link_libraries(
    oa-data-mutators-menus-test
    PRIVATE oa-data-mutators
            oa-formats-tdf
            oa-test-support
            oa-ui-hud
            oa-ui-gui-layout
            oa-test-game-data
  )
  add_test(NAME data-mutators-menus COMMAND oa-data-mutators-menus-test)
  set_tests_properties(data-mutators-menus PROPERTIES TIMEOUT 60)
  oa_add_game_data_test(data-mutators-menus-data oa-data-mutators-menus-test --data)
  set_tests_properties(data-mutators-menus-data PROPERTIES TIMEOUT 180)

  add_executable(oa-data-mutators-weapon-ids-test tests/weapon_ids_test.cpp)
  target_link_libraries(
    oa-data-mutators-weapon-ids-test PRIVATE oa-data-mutators oa-formats-tdf oa-test-support
  )
  add_test(NAME data-mutators-weapon-ids COMMAND oa-data-mutators-weapon-ids-test)
  set_tests_properties(data-mutators-weapon-ids PROPERTIES TIMEOUT 30)

  add_executable(oa-data-mutators-verdicts-test tests/verdicts_test.cpp)
  target_link_libraries(oa-data-mutators-verdicts-test PRIVATE oa-data-mutators oa-test-support)
  add_test(NAME data-mutators-verdicts COMMAND oa-data-mutators-verdicts-test)
  set_tests_properties(data-mutators-verdicts PROPERTIES TIMEOUT 30)

  add_executable(oa-data-mutators-sides-test tests/collections_sides_test.cpp)
  target_link_libraries(
    oa-data-mutators-sides-test PRIVATE oa-data-mutators oa-test-support oa-test-game-data
  )
  add_test(NAME data-mutators-sides COMMAND oa-data-mutators-sides-test)
  set_tests_properties(data-mutators-sides PROPERTIES TIMEOUT 30)
  oa_add_game_data_test(data-mutators-sides-data oa-data-mutators-sides-test --data)
  set_tests_properties(data-mutators-sides-data PROPERTIES TIMEOUT 120)

  add_executable(oa-data-mutators-hacks-test tests/hacks_test.cpp)
  target_link_libraries(oa-data-mutators-hacks-test PRIVATE oa-data-mutators oa-test-support)
  add_test(NAME data-mutators-hacks COMMAND oa-data-mutators-hacks-test)
  set_tests_properties(data-mutators-hacks PROPERTIES TIMEOUT 30)
endif()
