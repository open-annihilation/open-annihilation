# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Text drawn with the fonts that travel with the game, through FreeType, on
# every platform. Bengali and Devanagari are shaped with HarfBuzz first.
# FreeType, HarfBuzz and the fonts come from cmake/OaTextFonts.cmake,
# which the top-level project includes.
add_library(oa-platform-text-font src/text_font.cpp src/font_directory.cpp)
add_library(oa::platform::text_font ALIAS oa-platform-text-font)
target_include_directories(oa-platform-text-font PUBLIC include)
target_compile_features(oa-platform-text-font PUBLIC cxx_std_20)
target_link_libraries(oa-platform-text-font
  PRIVATE Freetype::Freetype oa-options)
# Present only when cmake/OaTextFonts.cmake found HarfBuzz. The shaping
# code is compiled out otherwise, and this library does not link it.
if(TARGET harfbuzz::harfbuzz)
  target_link_libraries(oa-platform-text-font PRIVATE harfbuzz::harfbuzz)
endif()
if(BUILD_TESTING)
  # The fonts are copied beside the test, where bundled_font_directory()
  # finds them, as it finds them beside the game.
  add_executable(oa-platform-text-font-test tests/text_font_test.cpp)
  target_link_libraries(oa-platform-text-font-test
    PRIVATE oa-platform-text-font Freetype::Freetype oa-options oa-test-support)
  if(TARGET harfbuzz::harfbuzz)
    target_link_libraries(oa-platform-text-font-test PRIVATE harfbuzz::harfbuzz)
  endif()
  oa_copy_text_fonts(oa-platform-text-font-test "$<TARGET_FILE_DIR:oa-platform-text-font-test>")
  # The fixture face sits beside the test, not in fonts/, where the pack-face
  # cases open it. A missing file leaves the configuration able to run.
  if(EXISTS "${OA_TEXT_FONT_TESTS_DIR}/NotoSansCJKsc-Bold-Fixture.otf")
    add_custom_command(TARGET oa-platform-text-font-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OA_TEXT_FONT_TESTS_DIR}/NotoSansCJKsc-Bold-Fixture.otf"
        "$<TARGET_FILE_DIR:oa-platform-text-font-test>/NotoSansCJKsc-Bold-Fixture.otf"
      VERBATIM)
  endif()
  # The Bengali face is a shaping check's fixture. It is not copied into fonts/.
  if(EXISTS "${OA_TEXT_FONT_TESTS_DIR}/NotoSansBengali-Bold.ttf")
    add_custom_command(TARGET oa-platform-text-font-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OA_TEXT_FONT_TESTS_DIR}/NotoSansBengali-Bold.ttf"
        "$<TARGET_FILE_DIR:oa-platform-text-font-test>/NotoSansBengali-Bold.ttf"
      VERBATIM)
  endif()
  add_test(NAME platform-text-font COMMAND oa-platform-text-font-test)
  # A short mixed line pixel for pixel, as the pinned FreeType draws it; it
  # skips with another FreeType.
  add_test(NAME platform-text-font-pixels COMMAND oa-platform-text-font-test --pixels)
  # A Bengali syllable with a conjunct and a reordered vowel sign, checked
  # against HarfBuzz's own glyphs. It skips when the fixture face is absent,
  # and when this build does not shape.
  add_test(NAME platform-text-font-shaping COMMAND oa-platform-text-font-test --shape)
  set_tests_properties(platform-text-font platform-text-font-pixels platform-text-font-shaping
    PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
