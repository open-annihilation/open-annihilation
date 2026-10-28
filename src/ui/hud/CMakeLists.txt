# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

file(GLOB oa_ui_hud_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(oa-ui-hud ${oa_ui_hud_sources})
add_library(oa::ui::hud ALIAS oa-ui-hud)
target_link_libraries(oa-ui-hud PRIVATE oa-base-game-math)
target_include_directories(oa-ui-hud PUBLIC include)
target_link_libraries(oa-ui-hud PUBLIC oa-core-types oa-data-campaign oa-data-persist oa-present
  PRIVATE oa-data-defs oa-data-languages oa-formats-tdf oa-sim-feature-runtime oa-sim-messages
  oa-sim-unit-movement oa-ui-console oa-options)
target_compile_features(oa-ui-hud PUBLIC cxx_std_20)

if(BUILD_TESTING)
  file(GLOB oa_ui_hud_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_ui_hud_tests)
    # tests/<case>_test.cpp builds oa-ui-hud-<case>-test, run as
    # ui-hud-<case>.
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "_test$" "" test_case "${test_name}")
    string(REPLACE "_" "-" test_case "${test_case}")
    add_executable(oa-ui-hud-${test_case}-test "${test_source}")
    target_link_libraries(oa-ui-hud-${test_case}-test PRIVATE oa-ui-hud oa-data-defs oa-data-languages
      oa-sim-feature-runtime oa-sim-messages oa-base-text oa-options)
    add_test(NAME ui-hud-${test_case} COMMAND oa-ui-hud-${test_case}-test)
  endforeach()
  # The installed game's side layouts and build pages.
  foreach(test_case IN ITEMS hud-labels order-panel)
    target_link_libraries(oa-ui-hud-${test_case}-test PRIVATE oa-formats-hpi oa-test-game-data)
    oa_add_game_data_test(ui-hud-${test_case}-data oa-ui-hud-${test_case}-test --data)
  endforeach()
endif()
