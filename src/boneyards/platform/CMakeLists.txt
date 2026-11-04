# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Text service over the bundled fonts, standing in for GDI's; only the
# Boneyards pages set text through it.
if(TARGET Freetype::Freetype)
  add_library(oa-boneyards-platform-fonts src/fonts.cpp src/font_faces.cpp)
  add_library(oa::boneyards::platform_fonts ALIAS oa-boneyards-platform-fonts)
  target_include_directories(oa-boneyards-platform-fonts PUBLIC include)
  target_compile_features(oa-boneyards-platform-fonts PUBLIC cxx_std_20)
  target_link_libraries(oa-boneyards-platform-fonts PRIVATE Freetype::Freetype oa-options
    oa-platform-shims)
  if(BUILD_TESTING)
    add_executable(oa-boneyards-platform-fonts-test tests/fonts_test.cpp)
    target_link_libraries(oa-boneyards-platform-fonts-test PRIVATE oa-boneyards-platform-fonts
      Freetype::Freetype oa-options oa-platform-shims)
    target_compile_definitions(oa-boneyards-platform-fonts-test PRIVATE OA_FONTS_DIR="${OA_FONTS_DIR}")
    add_test(NAME platform-fonts COMMAND oa-boneyards-platform-fonts-test)
    set_tests_properties(platform-fonts PROPERTIES TIMEOUT 60)
  endif()
endif()
