# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# A computer seat played by a sandboxed Lua brain: the package is loaded,
# fits and start run, and the match calls the seat's hooks instead of the
# built-in order tick.
add_library(oa-sim-brains src/seat.cpp src/profile.cpp src/read_only.cpp src/lua_game.cpp
  src/check.cpp src/handles.cpp src/lua_me.cpp src/events.cpp src/deliver.cpp
  src/definitions.cpp src/lua_defs.cpp src/lua_map.cpp src/reach.cpp)
add_library(oa::sim::brains ALIAS oa-sim-brains)
target_include_directories(oa-sim-brains PUBLIC include)
target_compile_features(oa-sim-brains PUBLIC cxx_std_20)
target_link_libraries(oa-sim-brains PUBLIC oa-sim-match-runtime oa-data-script-host
  oa-data-brain-facts)
target_link_libraries(oa-sim-brains PRIVATE oa-base-sha256 oa-sim-unit-movement oa-options
  oa-data-field-schema oa-data-mission-types oa-data-unit-definitions)

if(BUILD_TESTING)
  add_executable(oa-sim-brains-seat-test tests/seat_test.cpp)
  target_link_libraries(oa-sim-brains-seat-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  add_test(NAME sim-brains-seat COMMAND oa-sim-brains-seat-test)
  set_tests_properties(sim-brains-seat PROPERTIES TIMEOUT 120)

  # The facts a brain reads, the read-only game table and a load check with
  # no match. The made-up brain is fact-reader.
  add_executable(oa-sim-brains-game-test tests/game_test.cpp)
  target_link_libraries(oa-sim-brains-game-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  target_compile_definitions(oa-sim-brains-game-test PRIVATE
    OA_FACT_READER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/fact-reader")
  add_test(NAME sim-brains-game COMMAND oa-sim-brains-game-test)
  set_tests_properties(sim-brains-game PROPERTIES TIMEOUT 120)

  add_executable(oa-sim-brains-me-test tests/me_test.cpp)
  target_include_directories(oa-sim-brains-me-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(
    oa-sim-brains-me-test PRIVATE
    "OA_SIDE_READER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/side-reader\"")
  target_link_libraries(oa-sim-brains-me-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-sim-state-hash)
  add_test(NAME sim-brains-me COMMAND oa-sim-brains-me-test)
  set_tests_properties(sim-brains-me PROPERTIES TIMEOUT 180)

  add_executable(oa-sim-brains-events-test tests/events_test.cpp)
  target_link_libraries(oa-sim-brains-events-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-sim-state-hash)
  add_test(NAME sim-brains-events COMMAND oa-sim-brains-events-test)
  set_tests_properties(sim-brains-events PROPERTIES TIMEOUT 180)

  # Unit, weapon and feature records. The made-up brain is definition-reader.
  # --data compares every installed unit with the value the match loaded.
  add_executable(oa-sim-brains-defs-test tests/defs_test.cpp)
  target_include_directories(oa-sim-brains-defs-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(
    oa-sim-brains-defs-test PRIVATE
    "OA_DEFINITION_READER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/definition-reader\"")
  target_link_libraries(oa-sim-brains-defs-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-base-game-math oa-test-match oa-options oa-data-field-schema
    oa-data-mission-types oa-data-unit-definitions oa-data-defs oa-data-defs-assets
    oa-formats-hpi oa-test-game-data)
  add_test(NAME sim-brains-defs COMMAND oa-sim-brains-defs-test)
  set_tests_properties(sim-brains-defs PROPERTIES TIMEOUT 120)
  oa_add_game_data_test(sim-brains-defs-data oa-sim-brains-defs-test --data)

  # The match map and movement regions. The made-up brain is map-reader.
  # No installed-map fixture exists, so there is no --data twin.
  add_executable(oa-sim-brains-map-test tests/map_test.cpp)
  target_include_directories(oa-sim-brains-map-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(
    oa-sim-brains-map-test PRIVATE
    "OA_MAP_READER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/map-reader\"")
  target_link_libraries(oa-sim-brains-map-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-sim-state-hash)
  add_test(NAME sim-brains-map COMMAND oa-sim-brains-map-test)
  set_tests_properties(sim-brains-map PROPERTIES TIMEOUT 180)
endif()
