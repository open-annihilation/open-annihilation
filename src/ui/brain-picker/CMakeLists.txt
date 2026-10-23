# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The brain seat picker's model: which brain a computer seat plays, at which
# level, with which options, and the words each row, the details, the footer,
# the seat's label and its hover card show. It reads no file, clock,
# preference or network.
add_library(oa-ui-brain-picker src/model.cpp src/text.cpp src/prompts.cpp)
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

  # "Load with the built-in player?": one missing brain and two, the buttons,
  # the texts fitting at every class, and a lookup that translates.
  add_executable(oa-ui-brain-picker-prompts-test tests/prompts_test.cpp)
  target_link_libraries(oa-ui-brain-picker-prompts-test PRIVATE oa-ui-brain-picker oa-ui-kit
    oa-test-support)
  add_test(NAME ui-brain-picker-prompts COMMAND oa-ui-brain-picker-prompts-test)
  set_tests_properties(ui-brain-picker-prompts PROPERTIES TIMEOUT 30)
endif()
