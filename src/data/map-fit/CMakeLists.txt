# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Whether one map may be played with a game and a mod. The map fits when it
# only adds files and feature names, and every feature and unit it places can
# be loaded.
add_library(oa-data-map-fit src/map_fit.cpp src/map_files.cpp src/references.cpp)
add_library(oa::data::map_fit ALIAS oa-data-map-fit)
target_include_directories(oa-data-map-fit PUBLIC include)
target_compile_features(oa-data-map-fit PUBLIC cxx_std_20)
target_link_libraries(oa-data-map-fit
  PUBLIC oa-formats-hpi oa-formats-tdf oa-data-defs oa-data-map-pack
  PRIVATE oa-formats-tnt oa-platform-shims oa-options)

if(BUILD_TESTING)
  # A fixture game and, with --data, the installed game.
  add_executable(oa-data-map-fit-test tests/map_fit_test.cpp)
  target_link_libraries(oa-data-map-fit-test
    PRIVATE oa-data-map-fit oa-formats-hpi oa-formats-tnt oa-formats-tdf
      oa-test-support oa-test-game-data oa-options)
  add_test(NAME data-map-fit COMMAND oa-data-map-fit-test)
  set_tests_properties(data-map-fit PROPERTIES TIMEOUT 60)
  oa_add_game_data_test(data-map-fit-data oa-data-map-fit-test --data)
  set_tests_properties(data-map-fit-data PROPERTIES TIMEOUT 60)
endif()
