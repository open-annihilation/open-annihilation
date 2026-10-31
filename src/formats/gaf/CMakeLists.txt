# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.24)
project(oa_formats_gaf LANGUAGES CXX)

include(CTest)

add_library(oa-formats-gaf src/gaf.cpp)
add_library(oa::formats::gaf ALIAS oa-formats-gaf)
target_include_directories(oa-formats-gaf PUBLIC include)
target_compile_features(oa-formats-gaf PUBLIC cxx_std_20)
if(TARGET oa-options)
  target_link_libraries(oa-formats-gaf PUBLIC oa-options PRIVATE oa-base-bytes)
endif()

if(BUILD_TESTING)
  add_executable(oa-sprite-format-test tests/gaf_test.cpp)
  target_link_libraries(oa-sprite-format-test PRIVATE oa-formats-gaf)
  add_test(NAME sprite-format COMMAND oa-sprite-format-test)

  add_executable(oa-sprite-format-inspect EXCLUDE_FROM_ALL tools/inspect.cpp)
  target_link_libraries(oa-sprite-format-inspect PRIVATE oa-formats-gaf oa-platform-shims)
endif()
