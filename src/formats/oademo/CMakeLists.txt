# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The .oademo replay container: a zip of a description, a manifest, an optional
# timeline index and one recording, compressed as one zstd frame or as a
# deflated entry.
add_library(oa-formats-oademo src/oademo.cpp src/json_text.cpp src/timeline_index.cpp
  src/units.cpp src/export.cpp)
add_library(oa::formats::oademo ALIAS oa-formats-oademo)
target_include_directories(oa-formats-oademo PUBLIC include)
target_compile_features(oa-formats-oademo PUBLIC cxx_std_20)
target_link_libraries(oa-formats-oademo
  PUBLIC oa-options
  PRIVATE oa-formats-zip oa-formats-tad oa-formats-json oa-base-sha256 oa-base-zstd)

if(BUILD_TESTING)
  add_executable(oa-formats-oademo-test tests/oademo_test.cpp)
  target_link_libraries(oa-formats-oademo-test PRIVATE oa-formats-oademo oa-formats-zip
    oa-formats-tad oa-base-sha256 oa-test-support oa-options)
  add_test(NAME formats-oademo COMMAND oa-formats-oademo-test)
  set_tests_properties(formats-oademo PROPERTIES TIMEOUT 60)
endif()
