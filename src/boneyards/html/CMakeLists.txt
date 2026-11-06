# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

file(GLOB oa_boneyards_html_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(oa-boneyards-html ${oa_boneyards_html_sources})
add_library(oa::boneyards::html ALIAS oa-boneyards-html)
target_include_directories(oa-boneyards-html PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(oa-boneyards-html PUBLIC oa-present oa-formats-fnt
  PRIVATE oa-formats-png oa-platform-text-font oa-options)
if(TARGET Freetype::Freetype)
  target_link_libraries(oa-boneyards-html PRIVATE Freetype::Freetype)
endif()
target_compile_features(oa-boneyards-html PUBLIC cxx_std_20)

if(BUILD_TESTING)
  file(GLOB oa_boneyards_html_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_boneyards_html_tests)
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REPLACE "_" "-" test_id "${test_name}")
    add_executable(oa-boneyards-html-${test_id} "${test_source}")
    target_link_libraries(oa-boneyards-html-${test_id} PRIVATE oa-boneyards-html oa-formats-png ZLIB::ZLIB oa-options)
    oa_boneyards_ctest_name(oa_ctest_name "boneyards-html-${test_id}")
    add_test(NAME ${oa_ctest_name} COMMAND oa-boneyards-html-${test_id})
    set_tests_properties(${oa_ctest_name} PROPERTIES TIMEOUT 120)
  endforeach()
endif()

# The markup text service over the platform's bundled fonts.
if(TARGET oa-boneyards-platform-fonts)
  add_library(oa-boneyards-html-host-fonts host/src/host_fonts.cpp)
  add_library(oa::boneyards::html_host_fonts ALIAS oa-boneyards-html-host-fonts)
  target_include_directories(oa-boneyards-html-host-fonts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/host/include")
  target_link_libraries(oa-boneyards-html-host-fonts
    PUBLIC oa-boneyards-html PRIVATE oa-boneyards-platform-fonts oa-options)
  # The extension installs the service for the pages at start-up.
  if(BUILD_TESTING)
    add_executable(oa-boneyards-html-host-fonts-test host/tests/host_fonts_test.cpp)
    target_link_libraries(oa-boneyards-html-host-fonts-test
      PRIVATE oa-boneyards-html-host-fonts oa-boneyards-platform-fonts oa-options)
    target_compile_definitions(oa-boneyards-html-host-fonts-test PRIVATE
      OA_FONTS_DIR="${OA_FONTS_DIR}"
      OA_TEXT_FONTS_DIR="${OA_TEXT_FONTS_DIR}")
    add_test(NAME boneyards-html-host-fonts COMMAND oa-boneyards-html-host-fonts-test)
    set_tests_properties(boneyards-html-host-fonts PROPERTIES TIMEOUT 120)
  endif()
endif()
