# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Brain manifests (oabrain.yaml): the rules, the text written back, and which
# file of a package is which part of a brain.
add_library(oa-formats-oabrain src/manifest.cpp src/write.cpp src/layout.cpp)
add_library(oa::formats::oabrain ALIAS oa-formats-oabrain)
target_include_directories(oa-formats-oabrain PUBLIC include)
target_compile_features(oa-formats-oabrain PUBLIC cxx_std_20)
target_link_libraries(oa-formats-oabrain PUBLIC oa-formats-oamut oa-formats-oamod PRIVATE oa-options)

if(BUILD_TESTING)
  add_executable(oa-formats-oabrain-test tests/manifest_test.cpp)
  target_link_libraries(oa-formats-oabrain-test PRIVATE oa-formats-oabrain oa-test-support oa-options)
  add_test(NAME formats-oabrain COMMAND oa-formats-oabrain-test)
  set_tests_properties(formats-oabrain PROPERTIES TIMEOUT 30)
endif()
