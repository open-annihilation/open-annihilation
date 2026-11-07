# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The screen-reader bridge (include/oa/platform/screen_reader.hpp). AccessKit
# comes from cmake/OaAccessKit.cmake, which the top-level project includes.
# macOS, iOS and Linux link the static library normally. A program that calls
# the bridge pulls that code in. The game does not call it yet, so the game
# does not carry it. Windows does not link it.
if(NOT TARGET SDL3::SDL3)
  return()
endif()

add_library(oa-platform-screen-reader src/screen_reader.cpp)
add_library(oa::platform::screen_reader ALIAS oa-platform-screen-reader)
target_include_directories(oa-platform-screen-reader PUBLIC include)
target_compile_features(oa-platform-screen-reader PUBLIC cxx_std_20)
target_link_libraries(oa-platform-screen-reader PRIVATE oa-options)

if(OA_SCREEN_READER_COMPILED_OUT)
  target_compile_definitions(oa-platform-screen-reader PUBLIC OA_SCREEN_READER_COMPILED_OUT=1)
else()
  # The published header uses anonymous structs. A system include keeps the
  # engine's warning set from rejecting that header.
  target_include_directories(oa-platform-screen-reader SYSTEM PUBLIC "${OA_ACCESSKIT_INCLUDE}")
  if(MSVC)
    target_compile_options(oa-platform-screen-reader PRIVATE /external:W0)
  endif()
  target_link_libraries(oa-platform-screen-reader PRIVATE SDL3::SDL3)
  if(OA_ACCESSKIT_LIBRARY)
    add_library(oa-platform-screen-reader-accesskit STATIC IMPORTED)
    add_library(oa::platform::screen_reader_accesskit ALIAS oa-platform-screen-reader-accesskit)
    set_target_properties(oa-platform-screen-reader-accesskit PROPERTIES
      IMPORTED_LOCATION "${OA_ACCESSKIT_LIBRARY}")
    target_link_libraries(oa-platform-screen-reader PUBLIC oa-platform-screen-reader-accesskit)
    target_link_libraries(oa-platform-screen-reader PUBLIC ${OA_ACCESSKIT_SYSTEM_LIBS})
  endif()
endif()

if(BUILD_TESTING)
  add_executable(oa-platform-screen-reader-test tests/screen_reader_test.cpp)
  target_link_libraries(oa-platform-screen-reader-test PRIVATE oa-platform-screen-reader
    oa-test-support oa-options)
  if(MSVC AND NOT OA_SCREEN_READER_COMPILED_OUT)
    target_compile_options(oa-platform-screen-reader-test PRIVATE /external:W0)
  endif()
  oa_copy_accesskit_dll(oa-platform-screen-reader-test)
  add_test(NAME platform-screen-reader COMMAND oa-platform-screen-reader-test)
  set_tests_properties(platform-screen-reader PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
