# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

file(GLOB oa_boneyards_toolkit_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# The faces that travel with the game are compiled into the game and the chat
# test, not into this library, so a toolkit test keeps linking without them.
list(FILTER oa_boneyards_toolkit_sources EXCLUDE REGEX "modern_chat_engine\\.cpp$")
add_library(oa-boneyards-toolkit ${oa_boneyards_toolkit_sources})
add_library(oa::boneyards::toolkit ALIAS oa-boneyards-toolkit)
target_include_directories(oa-boneyards-toolkit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(oa-boneyards-toolkit PUBLIC oa-present)
target_link_libraries(oa-boneyards-toolkit PRIVATE oa-formats-gaf oa-options)
target_compile_features(oa-boneyards-toolkit PUBLIC cxx_std_20)
target_compile_definitions(oa-boneyards-toolkit PUBLIC
  OA_BONEYARDS_MATCHMAKING_RELEASED=$<BOOL:${OA_BONEYARDS_MATCHMAKING_RELEASED}>
  OA_BONEYARDS_REGISTRATION_FORM_RELEASED=$<BOOL:${OA_BONEYARDS_REGISTRATION_FORM_RELEASED}>)

# The faces that travel with the game. The extension and the chat tests link
# this; the toolkit library does not, so a toolkit test keeps linking without them.
add_library(oa-boneyards-toolkit-chat STATIC src/modern_chat_engine.cpp)
add_library(oa::boneyards::toolkit_chat ALIAS oa-boneyards-toolkit-chat)
target_link_libraries(oa-boneyards-toolkit-chat
  PUBLIC oa-boneyards-toolkit oa-app-headers oa-present
  PRIVATE oa-platform-text-font SDL3::SDL3 oa-options)
target_compile_features(oa-boneyards-toolkit-chat PUBLIC cxx_std_20)

if(BUILD_TESTING)
  # Installed-data fixtures shared with the screen tests. The header has its
  # own directory so those tests link it, instead of naming this module's
  # tests by a relative path.
  add_library(oa-boneyards-toolkit-fixtures INTERFACE)
  add_library(oa::boneyards::toolkit_fixtures ALIAS oa-boneyards-toolkit-fixtures)
  target_include_directories(oa-boneyards-toolkit-fixtures INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/share")
  file(GLOB oa_boneyards_toolkit_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_boneyards_toolkit_tests)
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REPLACE "_" "-" test_id "${test_name}")
    add_executable(oa-boneyards-${test_id} "${test_source}")
    target_link_libraries(oa-boneyards-${test_id} PRIVATE oa-boneyards-toolkit oa-formats-hpi
      oa-boneyards-tests-installs oa-boneyards-toolkit-fixtures oa-options)
    oa_boneyards_ctest_name(oa_ctest_name "boneyards-${test_id}")
    add_test(NAME ${oa_ctest_name} COMMAND oa-boneyards-${test_id})
    set_tests_properties(${oa_ctest_name} PROPERTIES TIMEOUT 120)
    # The analogue atlas is the committed file. This case reads the install's
    # bitmap faces and that atlas, so it is a game-data test.
    if(test_id STREQUAL "font-atlases-test")
      oa_boneyards_install_tests(${oa_ctest_name})
      set_property(TEST ${oa_ctest_name} APPEND PROPERTY
        ENVIRONMENT "OA_FONT_ATLASES_DIR=${OA_FONT_ATLASES_DIR}")
    endif()
    # Chat at the player's Text size uses the faces that travel with the game.
    if(test_id STREQUAL "modern-chat-test")
      target_link_libraries(oa-boneyards-${test_id} PRIVATE oa-boneyards-toolkit-chat oa-app-modern-text
        oa-platform-text-font SDL3::SDL3)
      if(NOT OA_TEXT_FONT_FILES)
        message(FATAL_ERROR "the engine's text font list is not available for the chat test")
      endif()
      oa_copy_text_fonts(oa-boneyards-${test_id} "$<TARGET_FILE_DIR:oa-boneyards-${test_id}>")
    endif()
  endforeach()
  # The resources and the controls and edit drawn with the BY1117a
  # install's by.ccx art, OA_GAME_DIR's.
  foreach(test_id IN ITEMS resources-test controls-test single-edit-test)
    oa_boneyards_ctest_name(oa_ctest_name "boneyards-${test_id}-data")
    add_test(NAME ${oa_ctest_name} COMMAND oa-boneyards-${test_id} --data)
    set_tests_properties(${oa_ctest_name} PROPERTIES TIMEOUT 120)
    oa_boneyards_install_tests(${oa_ctest_name})
  endforeach()
endif()
