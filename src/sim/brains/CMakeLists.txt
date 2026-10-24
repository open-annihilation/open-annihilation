# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# A computer seat played by a sandboxed Lua brain: the package is loaded,
# fits and start run, and the match calls the seat's hooks instead of the
# built-in order tick.

# The Classic fingerprint is computed at build time. A missing or empty
# brains/classic folder writes an empty value, and then no package is Classic.
set(oa_brains_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(oa_classic_fingerprint_header "${oa_brains_generated}/classic_fingerprint.hpp")
set(oa_classic_sample_header "${oa_brains_generated}/classic_sample_fingerprint.hpp")
file(GLOB_RECURSE oa_classic_tree CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/brains/classic/*")
file(GLOB_RECURSE oa_classic_sample_tree CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/classic-sample/*")
add_custom_command(
  OUTPUT "${oa_classic_fingerprint_header}"
  COMMAND "${CMAKE_COMMAND}" -D "SOURCE_DIR=${PROJECT_SOURCE_DIR}/brains/classic"
          -D "OUTPUT=${oa_classic_fingerprint_header}" -D CONSTANT=classic_fingerprint_builtin
          -P "${CMAKE_CURRENT_SOURCE_DIR}/classic_fingerprint.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/classic_fingerprint.cmake" ${oa_classic_tree}
  VERBATIM)
add_custom_command(
  OUTPUT "${oa_classic_sample_header}"
  COMMAND "${CMAKE_COMMAND}" -D "SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/classic-sample"
          -D "OUTPUT=${oa_classic_sample_header}" -D CONSTANT=classic_sample_fingerprint
          -P "${CMAKE_CURRENT_SOURCE_DIR}/classic_fingerprint.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/classic_fingerprint.cmake" ${oa_classic_sample_tree}
  VERBATIM)

add_library(oa-sim-brains src/seat.cpp src/profile.cpp src/read_only.cpp src/lua_game.cpp
  src/check.cpp src/handles.cpp src/lua_me.cpp src/events.cpp src/deliver.cpp src/classic.cpp
  src/random.cpp src/timers.cpp src/definitions.cpp src/lua_defs.cpp src/lua_chat.cpp
  src/failure.cpp src/save.cpp src/lua_profile.cpp src/lua_map.cpp src/reach.cpp src/lua_sight.cpp
  src/lua_orders.cpp src/lua_sites.cpp src/lua_networks.cpp src/lua_grids.cpp src/seat_calls.cpp
  src/shadow.cpp
  "${oa_classic_fingerprint_header}")
add_library(oa::sim::brains ALIAS oa-sim-brains)
target_include_directories(oa-sim-brains PUBLIC include PRIVATE "${oa_brains_generated}")
target_compile_features(oa-sim-brains PUBLIC cxx_std_20)
target_link_libraries(oa-sim-brains PUBLIC oa-sim-match-runtime oa-data-script-host
  oa-data-brain-facts)
target_link_libraries(oa-sim-brains PRIVATE oa-base-sha256 oa-data-neural oa-formats-oann
  oa-sim-unit-movement oa-options oa-data-field-schema oa-data-mission-types
  oa-data-unit-definitions oa-sim-simulation-state)

if(BUILD_TESTING)
  add_executable(oa-sim-brains-seat-test tests/seat_test.cpp)
  target_link_libraries(oa-sim-brains-seat-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-formats-oann oa-test-match oa-options)
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

  # What a seat may see, and the search a full-awareness brain may run.
  # The made-up brains are fog-reader and full-sight.
  add_executable(oa-sim-brains-sight-test tests/sight_test.cpp)
  target_include_directories(oa-sim-brains-sight-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(
    oa-sim-brains-sight-test PRIVATE
    "OA_FOG_READER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/fog-reader\""
    "OA_FULL_SIGHT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/full-sight\"")
  target_link_libraries(oa-sim-brains-sight-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-sim-state-hash)
  add_test(NAME sim-brains-sight COMMAND oa-sim-brains-sight-test)
  set_tests_properties(sim-brains-sight PROPERTIES TIMEOUT 180)

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

  add_executable(oa-sim-brains-random-test tests/random_test.cpp "${oa_classic_fingerprint_header}"
    "${oa_classic_sample_header}")
  target_include_directories(oa-sim-brains-random-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${oa_brains_generated}")
  target_compile_definitions(oa-sim-brains-random-test PRIVATE
    "OA_CLASSIC_SAMPLE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/classic-sample\"")
  target_link_libraries(oa-sim-brains-random-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  add_test(NAME sim-brains-random COMMAND oa-sim-brains-random-test)
  set_tests_properties(sim-brains-random PROPERTIES TIMEOUT 60)

  add_executable(oa-sim-brains-timers-test tests/timers_test.cpp)
  target_link_libraries(oa-sim-brains-timers-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  add_test(NAME sim-brains-timers COMMAND oa-sim-brains-timers-test)
  set_tests_properties(sim-brains-timers PROPERTIES TIMEOUT 60)

  add_executable(oa-sim-brains-chat-test tests/chat_test.cpp)
  target_compile_definitions(
    oa-sim-brains-chat-test PRIVATE
    "OA_CHAT_LISTENER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/chat-listener\"")
  target_link_libraries(oa-sim-brains-chat-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-sim-state-hash)
  add_test(NAME sim-brains-chat COMMAND oa-sim-brains-chat-test)
  set_tests_properties(sim-brains-chat PROPERTIES TIMEOUT 120)

  # A failed turn hands the seat back. The made-up brain is steady-commander.
  add_executable(oa-sim-brains-failure-test tests/failure_test.cpp)
  target_link_libraries(oa-sim-brains-failure-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  add_test(NAME sim-brains-failure COMMAND oa-sim-brains-failure-test)
  set_tests_properties(sim-brains-failure PROPERTIES TIMEOUT 120)

  # AI profile lines, build lists and strategic strengths. The made-up brain
  # is profile-reader. --data checks every installed ai/*.txt profile.
  add_executable(oa-sim-brains-profile-test tests/profile_test.cpp)
  target_include_directories(oa-sim-brains-profile-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(
    oa-sim-brains-profile-test PRIVATE
    "OA_PROFILE_READER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/profile-reader\"")
  target_link_libraries(oa-sim-brains-profile-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-data-defs oa-formats-hpi oa-test-game-data)
  add_test(NAME sim-brains-profile COMMAND oa-sim-brains-profile-test)
  set_tests_properties(sim-brains-profile PROPERTIES TIMEOUT 120)
  oa_add_game_data_test(sim-brains-profile-data oa-sim-brains-profile-test --data)

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

  # Orders a seated brain gives, beside the same calls from C++. The made-up
  # brain is order-giver.
  add_executable(oa-sim-brains-orders-test tests/orders_test.cpp)
  target_include_directories(oa-sim-brains-orders-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(
    oa-sim-brains-orders-test PRIVATE
    "OA_ORDER_GIVER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/data/order-giver\"")
  target_link_libraries(oa-sim-brains-orders-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-sim-state-hash oa-sim-gameplay-input
    oa-data-mission-types)
  add_test(NAME sim-brains-orders COMMAND oa-sim-brains-orders-test)
  set_tests_properties(sim-brains-orders PROPERTIES TIMEOUT 180)

  # Building sites a seated brain judges, beside the match's own checks.
  add_executable(oa-sim-brains-sites-test tests/sites_test.cpp)
  target_link_libraries(oa-sim-brains-sites-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  add_test(NAME sim-brains-sites COMMAND oa-sim-brains-sites-test)
  set_tests_properties(sim-brains-sites PROPERTIES TIMEOUT 120)

  # Networks, tensors and map grids. The made-up brain is steady-commander
  # and the made-up mod is ridge. No installed-map fixture is read.
  add_executable(oa-sim-brains-networks-test tests/networks_test.cpp)
  target_include_directories(oa-sim-brains-networks-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(oa-sim-brains-networks-test PRIVATE oa-sim-brains oa-data-neural
    oa-formats-oann oa-base-sha256 oa-sim-unit-movement oa-test-match oa-options)
  add_test(NAME sim-brains-networks COMMAND oa-sim-brains-networks-test)
  set_tests_properties(sim-brains-networks PROPERTIES TIMEOUT 60)

  # The logging host, the recording squad view, and the comparison.
  add_executable(oa-sim-brains-shadow-test tests/shadow_test.cpp)
  target_link_libraries(oa-sim-brains-shadow-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  add_test(NAME sim-brains-shadow COMMAND oa-sim-brains-shadow-test)
  set_tests_properties(sim-brains-shadow PROPERTIES TIMEOUT 120)

  # steady-shadow beside the built-in player for 1,200 ticks.
  add_executable(oa-sim-brains-shadow-match-test tests/shadow_match_test.cpp)
  target_compile_definitions(
    oa-sim-brains-shadow-match-test PRIVATE
    "OA_STEADY_SHADOW_DIR=\"${PROJECT_SOURCE_DIR}/tests/data/brains/steady-shadow\"")
  target_link_libraries(oa-sim-brains-shadow-match-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-sim-state-hash)
  add_test(NAME sim-brains-shadow-match COMMAND oa-sim-brains-shadow-match-test)
  set_tests_properties(sim-brains-shadow-match PROPERTIES TIMEOUT 180)

  # Classic beside the built-in player on the computer-player fake.
  add_executable(oa-sim-brains-classic-test tests/classic_test.cpp)
  target_link_libraries(oa-sim-brains-classic-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options oa-data-unit-definitions)
  add_test(NAME sim-brains-classic COMMAND oa-sim-brains-classic-test
    "${PROJECT_SOURCE_DIR}/brains/classic")
  set_tests_properties(sim-brains-classic PROPERTIES TIMEOUT 180)
endif()
