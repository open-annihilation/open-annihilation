# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-formats-png src/read.cpp src/write.cpp)
add_library(oa::formats::png ALIAS oa-formats-png)
target_include_directories(oa-formats-png PUBLIC include)
target_link_libraries(oa-formats-png PRIVATE ZLIB::ZLIB oa-options)
target_compile_features(oa-formats-png PUBLIC cxx_std_20)
if(BUILD_TESTING)
  add_executable(oa-png-format-test tests/png_test.cpp)
  target_include_directories(oa-png-format-test PRIVATE src)
  target_link_libraries(oa-png-format-test PRIVATE oa-formats-png ZLIB::ZLIB oa-options)
  add_test(NAME png-format COMMAND oa-png-format-test)
endif()

if(BUILD_TESTING)
  # The row unfilter is private to the library; its test reads src/.
  add_executable(oa-png-unfilter-row-test tests/unfilter_row_test.cpp)
  target_include_directories(oa-png-unfilter-row-test PRIVATE src)
  target_link_libraries(oa-png-unfilter-row-test PRIVATE oa-formats-png oa-options)
  add_test(NAME png-unfilter-row COMMAND oa-png-unfilter-row-test)
endif()

if(BUILD_TESTING)
  add_executable(oa-png-text-chunks-test tests/text_chunks_test.cpp)
  target_include_directories(oa-png-text-chunks-test PRIVATE src)
  target_link_libraries(oa-png-text-chunks-test PRIVATE oa-formats-png oa-base-sha256 oa-options)
  add_test(NAME png-text-chunks COMMAND oa-png-text-chunks-test)
endif()
