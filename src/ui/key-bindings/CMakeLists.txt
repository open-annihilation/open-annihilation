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
  src/tags.cpp
  src/clashes.cpp
  src/dispatcher.cpp
  src/latches.cpp
  src/pointer_keys.cpp
  src/confirm.cpp
  src/order_wheel.cpp
  src/timing.cpp
  src/layout_file.cpp
  src/legacy_keys.cpp
  src/legacy_import.cpp)
add_library(oa::ui::key_bindings ALIAS oa-ui-key-bindings)
target_include_directories(oa-ui-key-bindings PUBLIC include)
target_compile_features(oa-ui-key-bindings PUBLIC cxx_std_20)
target_link_libraries(oa-ui-key-bindings PUBLIC oa-data-input-actions oa-formats-oakeys
  PRIVATE oa-options)

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

  # One binding per event, the gestures, and a modifier role read from the keys held now.
  add_executable(oa-ui-key-bindings-dispatcher-exe tests/dispatcher_test.cpp)
  target_link_libraries(oa-ui-key-bindings-dispatcher-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-dispatcher COMMAND oa-ui-key-bindings-dispatcher-exe)

  # Hold latches and sticky modifiers, on synthetic timelines.
  add_executable(oa-ui-key-bindings-latches-exe tests/latches_test.cpp)
  target_link_libraries(oa-ui-key-bindings-latches-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-latches COMMAND oa-ui-key-bindings-latches-exe)
  set_tests_properties(ui-key-bindings-latches PROPERTIES TIMEOUT 30)

  # Pointer keys: the ramp, clicks, and chords set aside while they are on.
  add_executable(oa-ui-key-bindings-pointer-keys-exe tests/pointer_keys_test.cpp)
  target_link_libraries(oa-ui-key-bindings-pointer-keys-exe PRIVATE oa-ui-key-bindings
    oa-test-support oa-options)
  add_test(NAME ui-key-bindings-pointer-keys COMMAND oa-ui-key-bindings-pointer-keys-exe)
  set_tests_properties(ui-key-bindings-pointer-keys PROPERTIES TIMEOUT 30)
  # Which confirmable actions need a hold, and a hold that finishes while held.
  add_executable(oa-ui-key-bindings-confirm-exe tests/confirm_test.cpp)
  target_link_libraries(oa-ui-key-bindings-confirm-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-confirm COMMAND oa-ui-key-bindings-confirm-exe)
  set_tests_properties(ui-key-bindings-confirm PROPERTIES TIMEOUT 30)

  # A held mouse button: a click, a drag, a long press and a cancel.
  add_executable(oa-ui-key-bindings-order-wheel-exe tests/order_wheel_test.cpp)
  target_link_libraries(oa-ui-key-bindings-order-wheel-exe PRIVATE oa-ui-key-bindings
    oa-test-support oa-options)
  add_test(NAME ui-key-bindings-order-wheel COMMAND oa-ui-key-bindings-order-wheel-exe)
  set_tests_properties(ui-key-bindings-order-wheel PROPERTIES TIMEOUT 30)

  # Slow keys, bounce keys, the player's times and the drag's tick limit.
  add_executable(oa-ui-key-bindings-timing-exe tests/timing_test.cpp)
  target_link_libraries(oa-ui-key-bindings-timing-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-timing COMMAND oa-ui-key-bindings-timing-exe)
  set_tests_properties(ui-key-bindings-timing PROPERTIES TIMEOUT 30)
  # A manifest's chords, warnings and the text form, against a layout.
  add_executable(oa-ui-key-bindings-files-exe tests/layout_file_test.cpp)
  target_link_libraries(oa-ui-key-bindings-files-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-files COMMAND oa-ui-key-bindings-files-exe)

  # The position numbers and the named keys' key codes, against SDL.
  add_executable(oa-ui-key-bindings-sdl-exe tests/sdl_numbers_test.cpp)
  target_link_libraries(oa-ui-key-bindings-sdl-exe PRIVATE oa-ui-key-bindings SDL3::SDL3
    oa-test-support oa-options)
  add_test(NAME ui-key-bindings-sdl COMMAND oa-ui-key-bindings-sdl-exe)

  # Windows virtual-key numbers, tdraw's [Settings] keys and cnc-ddraw's hotkeys.
  add_executable(oa-ui-key-bindings-legacy-exe tests/legacy_import_test.cpp)
  target_link_libraries(oa-ui-key-bindings-legacy-exe PRIVATE oa-ui-key-bindings oa-test-support
    oa-options)
  add_test(NAME ui-key-bindings-legacy COMMAND oa-ui-key-bindings-legacy-exe)
  set_tests_properties(ui-key-bindings-legacy PROPERTIES TIMEOUT 30)
endif()
