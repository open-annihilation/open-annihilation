# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The key editor's model and its screen: every rebindable action, the draft
# of the player's own layout, and the kit window that lists them. It reads
# no file, clock, preference or network.
add_library(oa-ui-key-editor
  src/capture.cpp
  src/clash.cpp
  src/draw.cpp
  src/input.cpp
  src/keyboard_view.cpp
  src/layout.cpp
  src/list_geom.cpp
  src/menu_geom.cpp
  src/model.cpp
  src/mouse_view.cpp
  src/names.cpp
  src/overlay.cpp
  src/text.cpp)
add_library(oa::ui::key_editor ALIAS oa-ui-key-editor)
target_include_directories(oa-ui-key-editor PUBLIC include)
target_compile_features(oa-ui-key-editor PUBLIC cxx_std_20)
target_link_libraries(oa-ui-key-editor PUBLIC oa-ui-kit oa-ui-key-bindings oa-formats-oakeys
  oa-data-input-actions)

if(BUILD_TESTING)
  # Categories, filters, the draft, latch and hold, number keys, layouts and clashes.
  add_executable(oa-ui-key-editor-model-test tests/model_test.cpp)
  target_link_libraries(oa-ui-key-editor-model-test PRIVATE oa-ui-key-editor oa-test-support)
  add_test(NAME ui-key-editor-model COMMAND oa-ui-key-editor-model-test)
  set_tests_properties(ui-key-editor-model PROPERTIES TIMEOUT 60)

  # The window at Compact, Regular and Large: names, Tab, meeting controls and fitted text.
  add_executable(oa-ui-key-editor-layout-test tests/layout_test.cpp)
  target_link_libraries(oa-ui-key-editor-layout-test PRIVATE oa-ui-key-editor oa-ui-kit oa-test-support)
  add_test(NAME ui-key-editor-layout COMMAND oa-ui-key-editor-layout-test)
  set_tests_properties(ui-key-editor-layout PROPERTIES TIMEOUT 60)

  # The keys overlay: groups, collapsed ranges, the mod's hacks, the latches and the panel.
  add_executable(oa-ui-key-editor-overlay-test tests/overlay_test.cpp)
  target_link_libraries(oa-ui-key-editor-overlay-test PRIVATE oa-ui-key-editor oa-ui-kit oa-test-support)
  add_test(NAME ui-key-editor-overlay COMMAND oa-ui-key-editor-overlay-test)
  set_tests_properties(ui-key-editor-overlay PROPERTIES TIMEOUT 30)

  # The keyboard and mouse pictures: labels, layers, the card, a click and the scheme.
  add_executable(oa-ui-key-editor-views-test tests/views_test.cpp)
  target_link_libraries(oa-ui-key-editor-views-test PRIVATE oa-ui-key-editor oa-ui-kit oa-formats-oakeys
    oa-test-support)
  add_test(NAME ui-key-editor-views COMMAND oa-ui-key-editor-views-test)
  set_tests_properties(ui-key-editor-views PROPERTIES TIMEOUT 60)

  # The search, Escape, the list's arrows, Compact's details page and a finger.
  add_executable(oa-ui-key-editor-input-test tests/input_test.cpp)
  target_link_libraries(oa-ui-key-editor-input-test PRIVATE oa-ui-key-editor oa-ui-kit oa-test-support)
  add_test(NAME ui-key-editor-input COMMAND oa-ui-key-editor-input-test)
  set_tests_properties(ui-key-editor-input PROPERTIES TIMEOUT 60)

  # Capturing a chord, settling a clash, and finding by key.
  add_executable(oa-ui-key-editor-capture-test tests/capture_test.cpp)
  target_link_libraries(oa-ui-key-editor-capture-test PRIVATE oa-ui-key-editor oa-test-support)
  add_test(NAME ui-key-editor-capture COMMAND oa-ui-key-editor-capture-test)
  set_tests_properties(ui-key-editor-capture PROPERTIES TIMEOUT 60)

  # The list, the keyboard, the mouse, the capture, clash and card, and the keys overlay, in the block font and again in the installed game's fonts.
  add_executable(oa-ui-key-editor-draw-test tests/draw_test.cpp)
  target_link_libraries(oa-ui-key-editor-draw-test PRIVATE oa-ui-key-editor oa-ui-kit oa-base-sha256
    oa-data-languages oa-formats-png oa-formats-hpi oa-platform-text-font oa-present oa-test-game-data
    oa-test-support)
  target_compile_definitions(oa-ui-key-editor-draw-test PRIVATE
    OA_KEY_EDITOR_FRAMES="${PROJECT_SOURCE_DIR}/src/ui/key-editor/tests/frames.txt")
  oa_copy_text_fonts(oa-ui-key-editor-draw-test "$<TARGET_FILE_DIR:oa-ui-key-editor-draw-test>")
  add_test(NAME ui-key-editor-draw COMMAND oa-ui-key-editor-draw-test)
  set_tests_properties(ui-key-editor-draw PROPERTIES TIMEOUT 60)
  oa_add_game_data_test(ui-key-editor-draw-data oa-ui-key-editor-draw-test --data)
  set_tests_properties(ui-key-editor-draw-data PROPERTIES TIMEOUT 60)
endif()
