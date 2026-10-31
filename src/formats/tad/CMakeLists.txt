# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The TA Demo (.tad) recording format, and oa-formats-tad-dump, which
# summarises recordings: header, players, packet timing and record types.
add_library(oa-formats-tad src/tad.cpp)
add_library(oa::formats::tad ALIAS oa-formats-tad)
target_include_directories(oa-formats-tad PUBLIC include)
target_compile_features(oa-formats-tad PUBLIC cxx_std_20)
target_link_libraries(oa-formats-tad PUBLIC oa-formats-sqsh oa-options PRIVATE oa-base-bytes)

add_executable(oa-formats-tad-dump tools/tad_dump.cpp)
target_link_libraries(oa-formats-tad-dump PRIVATE oa-formats-tad oa-platform-shims)

if(BUILD_TESTING)
  add_executable(oa-formats-tad-test tests/tad_test.cpp)
  target_link_libraries(oa-formats-tad-test PRIVATE oa-formats-tad)
  add_test(NAME tad-format COMMAND oa-formats-tad-test)
endif()
