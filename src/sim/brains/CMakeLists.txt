# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# A computer seat played by a sandboxed Lua brain: the package is loaded,
# fits and start run, and the match calls the seat's hooks instead of the
# built-in order tick.
add_library(oa-sim-brains src/seat.cpp src/profile.cpp src/read_only.cpp src/lua_game.cpp
  src/check.cpp)
add_library(oa::sim::brains ALIAS oa-sim-brains)
target_include_directories(oa-sim-brains PUBLIC include)
target_compile_features(oa-sim-brains PUBLIC cxx_std_20)
target_link_libraries(oa-sim-brains PUBLIC oa-sim-match-runtime oa-data-script-host
  oa-data-brain-facts)
target_link_libraries(oa-sim-brains PRIVATE oa-base-sha256 oa-sim-unit-movement oa-options)

if(BUILD_TESTING)
  add_executable(oa-sim-brains-seat-test tests/seat_test.cpp)
  target_link_libraries(oa-sim-brains-seat-test PRIVATE oa-sim-brains oa-sim-unit-movement
    oa-base-sha256 oa-test-match oa-options)
  add_test(NAME sim-brains-seat COMMAND oa-sim-brains-seat-test)
  set_tests_properties(sim-brains-seat PROPERTIES TIMEOUT 120)

endif()
