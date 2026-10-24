# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The brain seat picker's model and its screen: which brain a computer seat
# plays, at which level, with which options, the kit window that draws the
# list, the details and the footer, and the load question a saved game asks.
# It reads no file, clock, preference or network.
add_library(oa-ui-brain-picker src/draw.cpp src/input.cpp src/layout.cpp src/model.cpp
  src/text.cpp src/prompts.cpp)
add_library(oa::ui::brain_picker ALIAS oa-ui-brain-picker)
target_include_directories(oa-ui-brain-picker PUBLIC include)
target_compile_features(oa-ui-brain-picker PUBLIC cxx_std_20)
target_link_libraries(oa-ui-brain-picker PUBLIC oa-ui-library oa-ui-mutator-picker oa-ui-kit
  oa-data-brain-library
  PRIVATE oa-options)

if(BUILD_TESTING)
  # Opening on a seat's choice, levels and options kept or mended, a brain
  # that cannot play, every change, the rows, the details, the summary, the
  # seat's label and hover card, and a lookup that translates.
  add_executable(oa-ui-brain-picker-model-test tests/model_test.cpp)
  target_link_libraries(oa-ui-brain-picker-model-test PRIVATE oa-ui-brain-picker oa-test-support
    oa-options)
  add_test(NAME ui-brain-picker-model COMMAND oa-ui-brain-picker-model-test)
  set_tests_properties(ui-brain-picker-model PROPERTIES TIMEOUT 60)

  # The pointer, a finger, the keys, the wheel and the controller, and a
  # choice that still fits the model after each one.
  add_executable(oa-ui-brain-picker-input-test tests/input_test.cpp)
  target_link_libraries(oa-ui-brain-picker-input-test PRIVATE oa-ui-brain-picker oa-ui-kit
    oa-test-support oa-options)
  add_test(NAME ui-brain-picker-input COMMAND oa-ui-brain-picker-input-test)
  set_tests_properties(ui-brain-picker-input PROPERTIES TIMEOUT 60)

  # The screen at Compact, Regular and Large: parts inside the window, no two
  # controls meeting except a row and the list, the rows, the options, Tab's
  # order, the names, and the texts fitting.
  add_executable(oa-ui-brain-picker-layout-test tests/layout_test.cpp)
  target_link_libraries(oa-ui-brain-picker-layout-test PRIVATE oa-ui-brain-picker oa-ui-kit
    oa-test-support oa-options)
  add_test(NAME ui-brain-picker-layout COMMAND oa-ui-brain-picker-layout-test)
  set_tests_properties(ui-brain-picker-layout PROPERTIES TIMEOUT 60)

  # Each picture drawn twice in the installed game's fonts, byte for byte the
  # same and nothing outside the window; with --frames DIR it writes them.
  add_executable(oa-ui-brain-picker-draw-test tests/draw_test.cpp)
  target_link_libraries(oa-ui-brain-picker-draw-test PRIVATE oa-ui-brain-picker oa-ui-kit
    oa-data-languages oa-formats-png oa-formats-hpi oa-platform-text-font oa-present
    oa-test-game-data oa-test-support oa-options)
  oa_copy_text_fonts(oa-ui-brain-picker-draw-test
    "$<TARGET_FILE_DIR:oa-ui-brain-picker-draw-test>")
  oa_add_game_data_test(ui-brain-picker-draw-data oa-ui-brain-picker-draw-test --data
    "${PROJECT_SOURCE_DIR}/languages/interface.tdf")
  set_tests_properties(ui-brain-picker-draw-data PROPERTIES TIMEOUT 60)

  # "Load with the built-in player?": one missing brain and two, the buttons,
  # the texts fitting at every class, and a lookup that translates.
  add_executable(oa-ui-brain-picker-prompts-test tests/prompts_test.cpp)
  target_link_libraries(oa-ui-brain-picker-prompts-test PRIVATE oa-ui-brain-picker oa-ui-kit
    oa-test-support)
  add_test(NAME ui-brain-picker-prompts COMMAND oa-ui-brain-picker-prompts-test)
  set_tests_properties(ui-brain-picker-prompts PROPERTIES TIMEOUT 30)
endif()
