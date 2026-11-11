# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The photo panel: chips, folding sections, guides and the toast. It reads
# no file, clock, preference or match. The application fills the model.
add_library(oa-ui-photo-panel
  src/crop.cpp
  src/draw.cpp
  src/input.cpp
  src/layout.cpp)
add_library(oa::ui::photo_panel ALIAS oa-ui-photo-panel)
target_include_directories(oa-ui-photo-panel PUBLIC include)
target_compile_features(oa-ui-photo-panel PUBLIC cxx_std_20)
target_link_libraries(oa-ui-photo-panel PUBLIC oa-ui-kit oa-ui-screen-registry)

if(BUILD_TESTING)
  # The window at each size and class, both looks, and the three visits.
  add_executable(oa-ui-photo-panel-layout-test tests/layout_test.cpp)
  target_link_libraries(oa-ui-photo-panel-layout-test PRIVATE oa-ui-photo-panel oa-ui-kit
    oa-platform-text-font oa-test-support)
  oa_copy_text_fonts(oa-ui-photo-panel-layout-test
    "$<TARGET_FILE_DIR:oa-ui-photo-panel-layout-test>")
  add_test(NAME ui-photo-panel-layout COMMAND oa-ui-photo-panel-layout-test)
  set_tests_properties(ui-photo-panel-layout PROPERTIES TIMEOUT 60)

  # A crop's shape, and the guide after the one showing.
  add_executable(oa-ui-photo-panel-crop-test tests/crop_test.cpp)
  target_link_libraries(oa-ui-photo-panel-crop-test PRIVATE oa-ui-photo-panel oa-test-support)
  add_test(NAME ui-photo-panel-crop COMMAND oa-ui-photo-panel-crop-test)
  set_tests_properties(ui-photo-panel-crop PROPERTIES TIMEOUT 30)

  # Tab shows the ring. Arrows and Return do not, until it shows.
  add_executable(oa-ui-photo-panel-input-test tests/input_test.cpp)
  target_link_libraries(oa-ui-photo-panel-input-test PRIVATE oa-ui-photo-panel oa-ui-kit
    oa-test-support)
  add_test(NAME ui-photo-panel-input COMMAND oa-ui-photo-panel-input-test)
  set_tests_properties(ui-photo-panel-input PROPERTIES TIMEOUT 30)

  # The panel in the three states at the three sizes, in both looks.
  add_executable(oa-ui-photo-panel-draw-test tests/draw_test.cpp)
  target_link_libraries(oa-ui-photo-panel-draw-test PRIVATE oa-ui-photo-panel oa-ui-kit
    oa-base-sha256 oa-data-languages oa-formats-png oa-formats-hpi oa-platform-text-font
    oa-present oa-test-game-data oa-test-support)
  target_compile_definitions(oa-ui-photo-panel-draw-test PRIVATE
    OA_PHOTO_PANEL_FRAMES="${PROJECT_SOURCE_DIR}/src/ui/photo-panel/tests/frames.txt")
  oa_copy_text_fonts(oa-ui-photo-panel-draw-test "$<TARGET_FILE_DIR:oa-ui-photo-panel-draw-test>")
  add_test(NAME ui-photo-panel-draw COMMAND oa-ui-photo-panel-draw-test)
  set_tests_properties(ui-photo-panel-draw PROPERTIES TIMEOUT 120)
  oa_add_game_data_test(ui-photo-panel-draw-data oa-ui-photo-panel-draw-test --data)
  set_tests_properties(ui-photo-panel-draw-data PROPERTIES TIMEOUT 120)
endif()
