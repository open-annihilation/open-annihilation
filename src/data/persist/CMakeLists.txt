# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-data-persist
  src/squash.cpp
  src/hapibank.cpp
  src/hapibank_file.cpp
  src/save_sections.cpp
  src/save_features.cpp
  src/save_units.cpp
  src/save_orders.cpp
  src/save_profile.cpp
  src/save_mutators.cpp
  src/save_brains.cpp)
add_library(oa::data::persist ALIAS oa-data-persist)
target_include_directories(oa-data-persist PUBLIC include)
target_compile_features(oa-data-persist PUBLIC cxx_std_20)
target_link_libraries(oa-data-persist PUBLIC oa-core-types oa-sim-world-environment PRIVATE oa-base-text oa-base-bytes oa-platform-shims oa-formats-sqsh ZLIB::ZLIB oa-options oa-data-defs oa-data-mission-types)

if(BUILD_TESTING)
  foreach(test_id IN ITEMS squash hapibank sections orders features profile mutators brains)
    add_executable(oa-persist-${test_id}-test tests/${test_id}_test.cpp)
    target_link_libraries(oa-persist-${test_id}-test PRIVATE oa-data-persist oa-formats-sqsh oa-test-support
      oa-base-bytes oa-base-text oa-options)
    add_test(NAME persist-${test_id} COMMAND oa-persist-${test_id}-test)
  endforeach()
  # Features blobs under counts far past the records they hold.
  add_test(NAME persist-features-count-bounds COMMAND oa-persist-features-test --count-bounds)
  add_executable(oa-persist-conditions-test tests/conditions_test.cpp)
  target_link_libraries(oa-persist-conditions-test PRIVATE oa-sim-scenario oa-options)
  add_test(NAME persist-conditions COMMAND oa-persist-conditions-test)
endif()

if(BUILD_TESTING)
  # The SQSH packer's and bank container's bytes, pinned for every platform.
  foreach(test_id IN ITEMS squash-golden bank-golden)
    string(REPLACE "-" "_" source ${test_id})
    add_executable(oa-persist-${test_id}-test tests/${source}_test.cpp)
    target_link_libraries(oa-persist-${test_id}-test PRIVATE oa-data-persist oa-options)
    add_test(NAME persist-${test_id} COMMAND oa-persist-${test_id}-test)
  endforeach()
endif()
