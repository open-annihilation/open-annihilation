# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Layout manifests (oakeys.yaml) and the short text of a layout's bindings.
add_library(oa-formats-oakeys src/manifest.cpp src/write.cpp src/text_form.cpp)
add_library(oa::formats::oakeys ALIAS oa-formats-oakeys)
target_include_directories(oa-formats-oakeys PUBLIC include)
target_compile_features(oa-formats-oakeys PUBLIC cxx_std_20)
target_link_libraries(oa-formats-oakeys PUBLIC oa-formats-oamut oa-formats-oamod PRIVATE oa-options)

if(BUILD_TESTING)
  add_executable(oa-formats-oakeys-test tests/manifest_test.cpp)
  target_link_libraries(oa-formats-oakeys-test PRIVATE oa-formats-oakeys oa-test-support oa-options)
  add_test(NAME formats-oakeys COMMAND oa-formats-oakeys-test)
  set_tests_properties(formats-oakeys PROPERTIES TIMEOUT 30)
endif()
