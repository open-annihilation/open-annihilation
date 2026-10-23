# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Integer network files (.oann): read, check and write format 1.
add_library(oa-formats-oann src/read.cpp src/check.cpp src/write.cpp)
add_library(oa::formats::oann ALIAS oa-formats-oann)
target_include_directories(oa-formats-oann PUBLIC include)
target_compile_features(oa-formats-oann PUBLIC cxx_std_20)
target_link_libraries(oa-formats-oann PRIVATE oa-options)

if(BUILD_TESTING)
  add_executable(oa-formats-oann-test tests/oann_test.cpp)
  target_link_libraries(oa-formats-oann-test PRIVATE
    oa-formats-oann oa-base-sha256 oa-test-support oa-options)
  add_test(NAME formats-oann COMMAND oa-formats-oann-test)
  set_tests_properties(formats-oann PROPERTIES TIMEOUT 30)
endif()
