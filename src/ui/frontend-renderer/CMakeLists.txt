# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-ui-frontend-renderer src/frontend_renderer.cpp src/gadget_draw.cpp src/scroll_bars.cpp
  src/artless.cpp src/panel_shade.cpp src/game_text.cpp)
add_library(oa::ui::frontend_renderer ALIAS oa-ui-frontend-renderer)
target_include_directories(oa-ui-frontend-renderer PUBLIC include)
target_link_libraries(oa-ui-frontend-renderer PUBLIC
  oa-formats-hpi oa-formats-fnt oa-ui-gui-layout oa-ui-gui-input oa-formats-gaf oa-present
  oa-ui-decoded oa-data-defs oa-options oa-base-threads PRIVATE oa-data-languages)

add_executable(oa-render-main-menu EXCLUDE_FROM_ALL tools/render_main_menu.cpp)
target_link_libraries(oa-render-main-menu PRIVATE oa-ui-frontend-renderer)

if(BUILD_TESTING)
  add_executable(oa-frontend-renderer-test tests/frontend_renderer_test.cpp)
  target_link_libraries(oa-frontend-renderer-test PRIVATE oa-ui-frontend-renderer oa-data-languages)
  add_test(NAME frontend-renderer COMMAND oa-frontend-renderer-test)

  add_executable(oa-scroll-bars-test tests/scroll_bars_test.cpp)
  target_link_libraries(oa-scroll-bars-test PRIVATE oa-ui-frontend-renderer)
  add_test(NAME frontend-scroll-bars COMMAND oa-scroll-bars-test)

  add_executable(oa-gadget-draw-test tests/gadget_draw_test.cpp)
  target_link_libraries(oa-gadget-draw-test PRIVATE oa-ui-frontend-renderer)
  add_test(NAME frontend-gadget-draw COMMAND oa-gadget-draw-test)

  # Drawing without the game's art: fills, blends, bevels, outlines, text
  # and marks, placed and scaled from source pixels; with --data, text in the
  # installed game's fonts against the fonts' own drawing.
  add_executable(oa-artless-draw-test tests/artless_test.cpp)
  target_link_libraries(oa-artless-draw-test PRIVATE oa-ui-frontend-renderer oa-options oa-test-game-data)
  add_test(NAME frontend-artless-draw COMMAND oa-artless-draw-test)
  oa_add_game_data_test(frontend-artless-draw-data oa-artless-draw-test --data)

  # The installed game's MAINMENU.GUI, the one laid out for FrontendX, which
  # the main menu takes with no overlay drawn over it, and a mod folder's own.
  add_executable(oa-main-menu-layout-test tests/main_menu_layout_test.cpp)
  target_link_libraries(oa-main-menu-layout-test PRIVATE oa-ui-frontend-renderer oa-test-game-data)
  oa_add_game_data_test(frontend-main-menu-layout oa-main-menu-layout-test)
  set_tests_properties(frontend-main-menu-layout PROPERTIES TIMEOUT 120)
endif()
