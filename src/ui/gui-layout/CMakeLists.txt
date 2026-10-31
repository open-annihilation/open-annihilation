# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.24)
project(oa_ui_gui_layout LANGUAGES CXX)

include(CTest)

add_library(oa-ui-gui-layout src/gui_layout.cpp src/gadget_record.cpp)
add_library(oa::ui::gui_layout ALIAS oa-ui-gui-layout)
target_include_directories(oa-ui-gui-layout PUBLIC include)
target_compile_features(oa-ui-gui-layout PUBLIC cxx_std_20)
target_link_libraries(oa-ui-gui-layout PRIVATE oa-data-languages oa-base-text)
if(TARGET oa-options)
  target_link_libraries(oa-ui-gui-layout PUBLIC oa-options)
endif()

if(BUILD_TESTING)
  add_executable(oa-gui-layout-test tests/gui_layout_test.cpp)
  target_link_libraries(oa-gui-layout-test PRIVATE oa-ui-gui-layout)
  add_test(NAME gui-layout COMMAND oa-gui-layout-test)

  # Inside the engine, which provides the archive reader and the game-data
  # helper: the gadget table, and every GUI file of the installed game loaded
  # into it.
  if(COMMAND oa_add_game_data_test)
    add_executable(oa-gui-gadget-record-test tests/gadget_record_test.cpp)
    target_link_libraries(oa-gui-gadget-record-test PRIVATE oa-ui-gui-layout oa-formats-hpi oa-test-game-data)
    add_test(NAME gui-gadget-record COMMAND oa-gui-gadget-record-test)
    oa_add_game_data_test(gui-gadget-record-data oa-gui-gadget-record-test --data)
  endif()

  add_executable(oa-gui-layout-inspect EXCLUDE_FROM_ALL tools/inspect.cpp)
  target_link_libraries(oa-gui-layout-inspect PRIVATE oa-ui-gui-layout oa-platform-shims)
endif()
