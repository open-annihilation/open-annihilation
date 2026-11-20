# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-present-model src/mesh_raster.cpp src/model_library.cpp src/model_draw.cpp src/rgb_bridge.cpp
  src/shadow_fade.cpp src/sprite_placement.cpp src/unit_supersampling.cpp)
add_library(oa::present::model ALIAS oa-present-model)
target_link_libraries(oa-present-model PRIVATE oa-base-game-math)
target_include_directories(oa-present-model PUBLIC include)
target_link_libraries(oa-present-model PUBLIC oa-present oa-present-colour-vision
  oa-formats-objects3d oa-formats-gaf oa-sim-sprite-animation oa-formats-hpi
  oa-sim-model-runtime oa-core-types oa-sim-effect-particles oa-data-limits
  PRIVATE oa-base-geometry oa-options oa-sim-feature-runtime)
target_compile_features(oa-present-model PUBLIC cxx_std_20)

if(BUILD_TESTING)
  file(GLOB oa_model_render_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_model_render_tests)
    # tests/<case>_test.cpp builds oa-model-render-<case>-test, run as
    # model-render-<case>.
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "_test$" "" test_case "${test_name}")
    string(REPLACE "_" "-" test_case "${test_case}")
    add_executable(oa-model-render-${test_case}-test "${test_source}")
    target_link_libraries(oa-model-render-${test_case}-test PRIVATE oa-present-model oa-options)
    add_test(NAME model-render-${test_case} COMMAND oa-model-render-${test_case}-test)
    set_tests_properties(model-render-${test_case} PROPERTIES TIMEOUT 120)
  endforeach()
  # The feature records the feature plans read.
  target_link_libraries(oa-model-render-sprite-placement-test PRIVATE oa-sim-feature-runtime)
  # The installed game's texture archives.
  target_link_libraries(oa-model-render-model-library-test PRIVATE oa-test-game-data)
  oa_add_game_data_test(model-render-model-library-data oa-model-render-model-library-test --data)
  set_tests_properties(model-render-model-library-data PROPERTIES TIMEOUT 120)
endif()
