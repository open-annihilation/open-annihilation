# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The replay playback bar: its layout, its hit tests, the game time it writes
# and when it hides (replay_viewer.hpp).
add_library(oa-ui-replay-viewer src/replay_viewer.cpp)
add_library(oa::ui::replay_viewer ALIAS oa-ui-replay-viewer)
target_include_directories(oa-ui-replay-viewer PUBLIC include)
target_compile_features(oa-ui-replay-viewer PUBLIC cxx_std_20)
target_link_libraries(oa-ui-replay-viewer PUBLIC oa-ui-kit PRIVATE oa-data-languages)

if(BUILD_TESTING)
  # The bar at 640x480, 1280x720 and 1920x1080, marker hit areas, the time
  # text, the hide and pin timing, and a drawing of the bar and both cards
  # whose buttons and cards are filled across their whole rectangles.
  add_executable(oa-ui-replay-viewer-test tests/replay_viewer_test.cpp)
  target_link_libraries(
    oa-ui-replay-viewer-test PRIVATE oa-ui-replay-viewer oa-ui-paint oa-test-support
  )
  oa_copy_text_fonts(oa-ui-replay-viewer-test "$<TARGET_FILE_DIR:oa-ui-replay-viewer-test>")
  add_test(NAME ui-replay-viewer COMMAND oa-ui-replay-viewer-test)
endif()
