# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Boneyards chat, lobby, profile, login, network setup and battle list screens,
# plus the service boundary shared by every Boneyards package.
file(GLOB oa_boneyards_screens_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(oa-boneyards-screens ${oa_boneyards_screens_sources})
add_library(oa::boneyards::screens ALIAS oa-boneyards-screens)
target_include_directories(oa-boneyards-screens PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(oa-boneyards-screens PUBLIC oa-boneyards-toolkit oa-data-languages
  PRIVATE oa-options oa-platform-shims oa-present-colour-vision)
target_compile_features(oa-boneyards-screens PUBLIC cxx_std_20)
target_compile_definitions(oa-boneyards-screens PUBLIC
  OA_BONEYARDS_REGISTRATION_URL="${OA_BONEYARDS_REGISTRATION_URL}")

if(BUILD_TESTING)
  # Profile replies shared with the console's navigation check. The header
  # has its own directory so that check links it, instead of naming this
  # module's tests by a relative path.
  add_library(oa-boneyards-screens-replies INTERFACE)
  add_library(oa::boneyards::screens_replies ALIAS oa-boneyards-screens-replies)
  target_include_directories(oa-boneyards-screens-replies INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/share")
  file(GLOB oa_boneyards_screens_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_boneyards_screens_tests)
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REPLACE "_" "-" test_id "${test_name}")
    add_executable(oa-boneyards-screens-${test_id} "${test_source}")
    target_link_libraries(oa-boneyards-screens-${test_id} PRIVATE oa-boneyards-screens oa-options)
    if(TARGET oa-formats-hpi)
      target_link_libraries(oa-boneyards-screens-${test_id} PRIVATE oa-formats-hpi)
    endif()
    target_link_libraries(oa-boneyards-screens-${test_id} PRIVATE oa-boneyards-tests-installs
      oa-boneyards-screens-replies)
    if(test_id STREQUAL "lobby-widgets-test")
      target_link_libraries(oa-boneyards-screens-${test_id} PRIVATE oa-boneyards-toolkit-fixtures)
    endif()
    oa_boneyards_ctest_name(oa_ctest_name "boneyards-screens-${test_id}")
    add_test(NAME ${oa_ctest_name} COMMAND oa-boneyards-screens-${test_id})
    set_tests_properties(${oa_ctest_name} PROPERTIES TIMEOUT 60)
  endforeach()
  # The lobby widgets and the message box drawn from the BY1117a install
  # OA_GAME_DIR names.
  oa_boneyards_install_tests(boneyards-screens-lobby-widgets)
  add_test(NAME boneyards-screens-message-box-data COMMAND oa-boneyards-screens-message-box-test --data)
  set_tests_properties(boneyards-screens-message-box-data PROPERTIES TIMEOUT 60)
  oa_boneyards_install_tests(boneyards-screens-message-box-data)
endif()

