# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

file(GLOB oa_present_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(oa-present ${oa_present_sources})
add_library(oa::present ALIAS oa-present)
target_include_directories(oa-present PUBLIC include)
target_link_libraries(oa-present PUBLIC oa-core-types PRIVATE oa-options oa-base-game-math oa-base-bytes oa-base-text
  oa-base-threads)
target_compile_features(oa-present PUBLIC cxx_std_20)

if(BUILD_TESTING)
  add_executable(oa-present-surface-c-test tests/surface_c_test.c)
  set_target_properties(oa-present-surface-c-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(oa-present-surface-c-test PRIVATE oa-present oa-options)
  add_test(NAME present-surface-c COMMAND oa-present-surface-c-test)
  file(GLOB oa_present_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_present_tests)
    # tests/<case>_test.cpp builds oa-present-<case>-test, run as
    # present-<case>.
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "_test$" "" test_case "${test_name}")
    string(REPLACE "_" "-" test_case "${test_case}")
    add_executable(oa-present-${test_case}-test "${test_source}")
    target_link_libraries(oa-present-${test_case}-test PRIVATE oa-present oa-options)
    add_test(NAME present-${test_case} COMMAND oa-present-${test_case}-test)
    set_tests_properties(present-${test_case} PROPERTIES TIMEOUT 120)
  endforeach()
  target_link_libraries(oa-present-gaf-sprites-test PRIVATE oa-formats-gaf)
  target_link_libraries(oa-present-game-text-test PRIVATE oa-test-support)
  target_link_libraries(oa-present-typed-text-test PRIVATE oa-test-support)
  target_link_libraries(oa-present-picture-captions-test PRIVATE oa-test-support)
  # The installed game's GAF files (compared with the GAF reader), palette
  # tables and PCX images.
  foreach(test_case IN ITEMS gaf-sprites palette-tables pcx)
    target_link_libraries(oa-present-${test_case}-test PRIVATE oa-formats-hpi oa-test-game-data)
    oa_add_game_data_test(present-${test_case}-data oa-present-${test_case}-test --data)
    set_tests_properties(present-${test_case}-data PROPERTIES TIMEOUT 300)
  endforeach()
endif()

add_subdirectory(model)
