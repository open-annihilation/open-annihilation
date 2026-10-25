# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Chords, and how a layout resolves to the bindings in use. The library
# reads no clock and does not link SDL. The numbers program checks the
# position table against SDL.
add_library(oa-ui-key-bindings
  src/key_codes.cpp
  src/keyboard_map.cpp
  src/chord.cpp
  src/key_names.cpp
  src/resolve.cpp
  src/clashes.cpp)
add_library(oa::ui::key_bindings ALIAS oa-ui-key-bindings)
target_include_directories(oa-ui-key-bindings PUBLIC include)
target_compile_features(oa-ui-key-bindings PUBLIC cxx_std_20)
target_link_libraries(oa-ui-key-bindings PUBLIC oa-data-input-actions PRIVATE oa-options)

if(BUILD_TESTING)
  # Parse, write, name and match, including every registry default.
  add_executable(oa-ui-key-bindings-chords-exe tests/chord_test.cpp)
  target_link_libraries(oa-ui-key-bindings-chords-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-chords COMMAND oa-ui-key-bindings-chords-exe)

  # Resolution, clashes and shadows, including every hack on oa-default.
  add_executable(oa-ui-key-bindings-layouts-exe tests/layout_test.cpp)
  target_link_libraries(oa-ui-key-bindings-layouts-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-layouts COMMAND oa-ui-key-bindings-layouts-exe)

  # The position numbers and the named keys' key codes, against SDL.
  add_executable(oa-ui-key-bindings-sdl-exe tests/sdl_numbers_test.cpp)
  target_link_libraries(oa-ui-key-bindings-sdl-exe PRIVATE oa-ui-key-bindings SDL3::SDL3
    oa-test-support oa-options)
  add_test(NAME ui-key-bindings-sdl COMMAND oa-ui-key-bindings-sdl-exe)
endif()
