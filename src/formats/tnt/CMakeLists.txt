# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.24)
project(oa_formats_tnt LANGUAGES CXX)

include(CTest)

add_library(oa-formats-tnt src/tnt.cpp)
add_library(oa::formats::tnt ALIAS oa-formats-tnt)
target_include_directories(oa-formats-tnt PUBLIC include)
target_compile_features(oa-formats-tnt PUBLIC cxx_std_20)
target_link_libraries(oa-formats-tnt PUBLIC oa-formats-ota PRIVATE oa-base-bytes)
if(TARGET oa-options)
  target_link_libraries(oa-formats-tnt PUBLIC oa-options)
endif()

if(BUILD_TESTING)
  add_executable(oa-map-format-test tests/tnt_test.cpp)
  target_link_libraries(oa-map-format-test PRIVATE oa-formats-tnt)
  add_test(NAME map-format COMMAND oa-map-format-test)

  add_executable(oa-map-format-inspect EXCLUDE_FROM_ALL tools/inspect.cpp)
  target_link_libraries(oa-map-format-inspect PRIVATE oa-formats-tnt oa-platform-shims)
endif()
