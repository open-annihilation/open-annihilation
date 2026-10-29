# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Registry descriptors, the player's registry file and the rules for which
# registries a player may add. Nothing here fetches.
add_library(oa-data-registry src/descriptor.cpp src/player_registries.cpp src/trust.cpp
  src/yaml_text.cpp)
add_library(oa::data::registry ALIAS oa-data-registry)
target_include_directories(oa-data-registry PUBLIC include)
target_compile_features(oa-data-registry PUBLIC cxx_std_20)
target_link_libraries(oa-data-registry PUBLIC oa-base-signing oa-formats-url oa-options
  PRIVATE oa-formats-oamod oa-platform-shims)

if(BUILD_TESTING)
  # Descriptor rules, the player's registry file and replacing that file only
  # through the platform's file replace.
  add_executable(oa-data-registry-test tests/descriptor_test.cpp)
  target_link_libraries(oa-data-registry-test PRIVATE oa-data-registry oa-test-support)
  add_test(NAME data-registry COMMAND oa-data-registry-test)
  # Which registries a player may add, the built-in folder and the registries
  # in effect.
  add_executable(oa-data-registry-trust-test tests/trust_test.cpp)
  target_link_libraries(oa-data-registry-trust-test PRIVATE oa-data-registry oa-data-catalogue
    oa-test-support)
  add_test(NAME data-registry-trust COMMAND oa-data-registry-trust-test)
  # The registries the game ships with, against the keys compiled into the engine.
  add_executable(oa-data-registry-builtin-test tests/builtin_test.cpp)
  target_link_libraries(oa-data-registry-builtin-test PRIVATE oa-data-registry oa-test-support)
  add_test(NAME data-registry-builtin COMMAND oa-data-registry-builtin-test
    "${PROJECT_SOURCE_DIR}/registries")
  set_tests_properties(data-registry data-registry-trust data-registry-builtin PROPERTIES TIMEOUT 30)
endif()
