# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# 8-bit draw host of the gadget engine: record draws, the whole-panel draw
# and panel face blits.
add_library(oa-ui-gadget-render src/gadget_text.cpp src/gadget_types.cpp src/panel_draw.cpp)
add_library(oa::ui::gadget_render ALIAS oa-ui-gadget-render)
target_link_libraries(oa-ui-gadget-render PRIVATE oa-base-text oa-base-game-math oa-base-threads)
target_include_directories(oa-ui-gadget-render PUBLIC include)
target_compile_features(oa-ui-gadget-render PUBLIC cxx_std_20)
target_link_libraries(oa-ui-gadget-render
  PUBLIC oa-ui-gui-input oa-ui-gui-layout oa-present
  PRIVATE oa-ui-frontend-renderer oa-present-model oa-ui-services oa-data-defs oa-options)

if(BUILD_TESTING)
  add_executable(oa-gadget-render-test tests/gadget_render_test.cpp)
  target_link_libraries(oa-gadget-render-test PRIVATE oa-ui-gadget-render oa-options)
  add_test(NAME gadget-render COMMAND oa-gadget-render-test)
endif()
