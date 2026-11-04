# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The game's side of the Boneyards online client: the Rover table binding and
# its call wrappers, the Central Command session behind the Boneyards
# service, the (disabled) auto-update step and the Rover interface
# offline checks replay.
file(GLOB oa_boneyards_client_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(oa-boneyards-client ${oa_boneyards_client_sources})
add_library(oa::boneyards::client ALIAS oa-boneyards-client)
target_include_directories(oa-boneyards-client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# The replay hands Rover the records a launch brings and reads the ones
# Rover queues, through Rover's record and queue headers.
target_link_libraries(oa-boneyards-client
  PUBLIC oa-boneyards-screens oa-boneyards-rover
  PRIVATE oa-base-sha256 oa-boneyards-rover-detail oa-formats-hpi oa-options oa-platform-shims)
if(WIN32)
  target_link_libraries(oa-boneyards-client PRIVATE iphlpapi)
endif()
target_compile_features(oa-boneyards-client PUBLIC cxx_std_20)

if(BUILD_TESTING)
  file(GLOB oa_boneyards_client_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_boneyards_client_tests)
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "_test$" "" test_id "${test_name}")
    string(REPLACE "_" "-" test_id "${test_id}")
    add_executable(oa-boneyards-client-${test_id}-test "${test_source}")
    target_link_libraries(oa-boneyards-client-${test_id}-test
      PRIVATE oa-boneyards-client oa-boneyards-rover-detail oa-options)
    target_compile_definitions(oa-boneyards-client-${test_id}-test PRIVATE
      OA_BONEYARDS_CLIENT_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME boneyards-client-${test_id} COMMAND oa-boneyards-client-${test_id}-test)
    set_tests_properties(boneyards-client-${test_id} PROPERTIES TIMEOUT 60)
  endforeach()
  # The virtual-files test builds fixture archives and reads them back.
  if(TARGET oa-boneyards-client-virtual-files-test)
    target_link_libraries(oa-boneyards-client-virtual-files-test
      PRIVATE oa-formats-hpi oa-test-support)
  endif()
  # The case-paths test builds a fixture tree and reads the host spelling back.
  if(TARGET oa-boneyards-client-case-paths-test)
    target_link_libraries(oa-boneyards-client-case-paths-test PRIVATE oa-test-support)
  endif()
  # The sector refresh writes a fixture tree and checks the file's SHA-256.
  if(TARGET oa-boneyards-client-sector-refresh-test)
    target_link_libraries(oa-boneyards-client-sector-refresh-test
      PRIVATE oa-base-sha256 oa-test-support)
  endif()
  # The shipped profile definitions of the BY1117a install OA_GAME_DIR names.
  target_link_libraries(oa-boneyards-client-profile-templates-test PRIVATE oa-boneyards-tests-installs)
  # Enter Boneyards against a supplied interface list, through the main-menu overlay.
  if(TARGET oa-boneyards-client-network-link-test)
    target_link_libraries(oa-boneyards-client-network-link-test
      PRIVATE oa-boneyards-console oa-boneyards-tests-installs)
    oa_boneyards_install_tests(boneyards-client-network-link)
  endif()
  add_test(NAME boneyards-client-profile-templates-data COMMAND oa-boneyards-client-profile-templates-test --data)
  set_tests_properties(boneyards-client-profile-templates-data PROPERTIES TIMEOUT 60)
  oa_boneyards_install_tests(boneyards-client-profile-templates-data)
endif()
