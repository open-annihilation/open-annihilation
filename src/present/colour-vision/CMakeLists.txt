# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Player colours chosen for a colour-vision deficiency.
add_library(oa-present-colour-vision src/colour_vision.cpp)
add_library(oa::present::colour_vision ALIAS oa-present-colour-vision)
target_include_directories(oa-present-colour-vision PUBLIC include)
target_link_libraries(oa-present-colour-vision PRIVATE oa-formats-gaf)
target_compile_features(oa-present-colour-vision PUBLIC cxx_std_20)

if(BUILD_TESTING)
  # The distance check, and, with --data, the stock palette's anchor indices
  # and the team ramps read from the installed logos.
  add_executable(oa-present-colour-vision-test tests/colour_vision_test.cpp)
  target_link_libraries(oa-present-colour-vision-test PRIVATE oa-present-colour-vision
    oa-formats-gaf oa-formats-hpi oa-test-game-data oa-options)
  add_test(NAME present-colour-vision COMMAND oa-present-colour-vision-test)
  set_tests_properties(present-colour-vision PROPERTIES TIMEOUT 120)
  oa_add_game_data_test(present-colour-vision-data oa-present-colour-vision-test --data)
  set_tests_properties(present-colour-vision-data PROPERTIES TIMEOUT 120)
endif()
