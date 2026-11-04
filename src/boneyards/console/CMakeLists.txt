# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

file(GLOB oa_boneyards_console_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(oa-boneyards-console ${oa_boneyards_console_sources})
add_library(oa::boneyards::console ALIAS oa-boneyards-console)
target_include_directories(oa-boneyards-console PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(oa-boneyards-console PUBLIC oa-boneyards-toolkit oa-boneyards-screens oa-boneyards-client
  oa-formats-gaf oa-boneyards-html PRIVATE oa-formats-png ZLIB::ZLIB oa-options oa-formats-tdf
  oa-ui-gui-layout oa-data-languages oa-platform-shims)
target_compile_features(oa-boneyards-console PUBLIC cxx_std_20)

# Headers beside the console sources (galaxy drawing, the console's state).
# They are not under include/, so they are not a public header of the module.
add_library(oa-boneyards-console-private INTERFACE)
add_library(oa::boneyards::console_private ALIAS oa-boneyards-console-private)
target_include_directories(oa-boneyards-console-private INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oa-boneyards-console-private INTERFACE oa-boneyards-console)

# Main-menu overlay registration for the native application, which the
# extension links into oa-game.
if(TARGET oa-ui-screen-registry AND TARGET oa-ui-frontend-renderer)
  add_library(oa-boneyards-console-app app/console_screens.cpp)
  add_library(oa::boneyards::console_app ALIAS oa-boneyards-console-app)
  target_link_libraries(oa-boneyards-console-app
    PUBLIC oa-ui-screen-registry PRIVATE oa-boneyards-console oa-ui-frontend-renderer oa-ui-frontend-state
    oa-formats-hpi oa-options)
  if(BUILD_TESTING)
    add_executable(oa-boneyards-console-screens-test app/console_screens_test.cpp)
    target_link_libraries(oa-boneyards-console-screens-test
      PRIVATE oa-boneyards-console-app oa-boneyards-console oa-boneyards-client oa-ui-frontend-renderer
      oa-ui-frontend-state oa-options)
    target_include_directories(oa-boneyards-console-screens-test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(oa-boneyards-console-screens-test PRIVATE oa-boneyards-tests-installs)
    add_test(NAME boneyards-console-screens COMMAND oa-boneyards-console-screens-test)
    set_tests_properties(boneyards-console-screens PROPERTIES TIMEOUT 120)
    # The main-menu overlay over the BY1117a install OA_GAME_DIR names.
    oa_boneyards_install_tests(boneyards-console-screens)
    # The main menu's layout under the overlay, read from that install's
    # archives.
    add_executable(oa-boneyards-console-main-menu-layout-test app/main_menu_layout_test.cpp)
    target_link_libraries(oa-boneyards-console-main-menu-layout-test
      PRIVATE oa-ui-frontend-renderer oa-boneyards-tests-installs oa-options)
    add_test(NAME boneyards-console-main-menu-layout COMMAND oa-boneyards-console-main-menu-layout-test)
    set_tests_properties(boneyards-console-main-menu-layout PROPERTIES TIMEOUT 120)
    oa_boneyards_install_tests(boneyards-console-main-menu-layout)
  endif()
endif()

if(BUILD_TESTING)
  file(GLOB oa_boneyards_console_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  foreach(test_source IN LISTS oa_boneyards_console_tests)
    get_filename_component(test_name "${test_source}" NAME_WE)
    string(REGEX REPLACE "_test$" "" test_id "${test_name}")
    string(REPLACE "_" "-" test_id "${test_id}")
    add_executable(oa-boneyards-console-${test_id} "${test_source}")
    target_link_libraries(oa-boneyards-console-${test_id}
      PRIVATE oa-boneyards-console oa-boneyards-html oa-boneyards-tests-installs oa-options ZLIB::ZLIB)
    # The QuickPlay, Locator, registration and side prompt checks write their
    # faces as pictures with --frames DIR.
    if(test_id STREQUAL "ad-banner" OR test_id STREQUAL "quickplay" OR test_id STREQUAL "locator" OR
       test_id STREQUAL "registration" OR test_id STREQUAL "registration-form" OR
       test_id STREQUAL "side-prompt")
      target_link_libraries(oa-boneyards-console-${test_id} PRIVATE oa-formats-png)
    endif()
    target_include_directories(oa-boneyards-console-${test_id} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(oa-boneyards-console-${test_id} PRIVATE
      OA_BY_CLIENT_TEST_DATA="${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data")
    # The new screens are read from the mod folder this build writes
    # (coreprime.ccx beside oamod.yaml). The test is given that folder. The
    # archive target is created later, so the build dependency is added from
    # cmake/OaBoneyards.cmake.
    set(oa_console_mod_arg "")
    if(test_id STREQUAL "gui-screen" OR test_id STREQUAL "matchmaking" OR test_id STREQUAL "locator" OR
       test_id STREQUAL "battle-console" OR test_id STREQUAL "quickplay" OR test_id STREQUAL "registration" OR
       test_id STREQUAL "registration-form" OR test_id STREQUAL "boneyards-navigation" OR
       test_id STREQUAL "gamepad" OR test_id STREQUAL "side-prompt")
      target_link_libraries(oa-boneyards-console-${test_id} PRIVATE oa-formats-hpi)
      set(oa_console_mod_arg "${CMAKE_BINARY_DIR}/coreprime")
    endif()
    if(test_id STREQUAL "boneyards-navigation")
      target_link_libraries(oa-boneyards-console-${test_id} PRIVATE oa-boneyards-screens-replies)
    endif()
    # The language catalogue and the new screens are read from the source tree.
    # The analogue atlas is the committed file, so a tall glyph is measured
    # before the game files are copied.
    if(test_id STREQUAL "translations")
      target_link_libraries(oa-boneyards-console-${test_id}
        PRIVATE oa-formats-hpi oa-formats-tdf)
      target_compile_definitions(oa-boneyards-console-${test_id} PRIVATE
        "OA_BONEYARDS_CATALOGUE=\"${open_annihilation_SOURCE_DIR}/languages/boneyards.tdf\""
        "OA_BONEYARDS_GUI_DIR=\"${open_annihilation_SOURCE_DIR}/data/boneyards/coreprime\""
        "OA_BONEYARDS_ATLAS=\"${OA_FONT_ATLASES_DIR}/boneyards-analogues.bin\"")
    endif()
    # The side prompt is laid out in each language: the catalogue and the
    # analogue atlas come from the source tree too.
    if(test_id STREQUAL "side-prompt")
      target_compile_definitions(oa-boneyards-console-${test_id} PRIVATE
        "OA_BONEYARDS_CATALOGUE=\"${open_annihilation_SOURCE_DIR}/languages/boneyards.tdf\""
        "OA_BONEYARDS_ATLAS=\"${OA_FONT_ATLASES_DIR}/boneyards-analogues.bin\"")
    endif()
    # Sample gadget files for the GUI screen and the Locator sit on disk.
    if(test_id STREQUAL "gui-screen" OR test_id STREQUAL "locator")
      target_compile_definitions(oa-boneyards-console-${test_id} PRIVATE
        OA_GUI_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    endif()
    # Each face drawn in Western and Central European scripts, Greek, Cyrillic
    # and CJK: the bitmap faces, the page faces, and the modern chat line.
    # The update screen's check reads a fixture catalogue and feed and hashes a file.
    if(test_id STREQUAL "update-screen")
      target_link_libraries(oa-boneyards-console-${test_id}
        PRIVATE oa-data-catalogue oa-data-engine-feed oa-base-sha256 oa-test-support)
    endif()
    if(test_id STREQUAL "language-fonts")
      if(NOT TARGET oa-boneyards-html-host-fonts OR NOT TARGET oa-platform-text-font)
        message(FATAL_ERROR "language font checks need the page faces and the chat faces")
      endif()
      target_link_libraries(oa-boneyards-console-${test_id}
        PRIVATE oa-formats-hpi oa-boneyards-html-host-fonts oa-boneyards-platform-fonts
        oa-boneyards-toolkit-chat oa-app-modern-text oa-platform-text-font SDL3::SDL3)
      target_compile_definitions(oa-boneyards-console-${test_id} PRIVATE
        "OA_FONTS_DIR=\"${OA_FONTS_DIR}\""
        "OA_TEXT_FONTS_DIR=\"${OA_TEXT_FONTS_DIR}\""
        "OA_LANGUAGE_ATLAS=\"${OA_FONT_ATLASES_DIR}/boneyards-analogues.bin\"")
      if(NOT OA_TEXT_FONT_FILES)
        message(FATAL_ERROR "the engine's text font list is not available for the language checks")
      endif()
      oa_copy_text_fonts(oa-boneyards-console-${test_id}
        "$<TARGET_FILE_DIR:oa-boneyards-console-${test_id}>")
    endif()
    # Every Help page of the install set in the page faces; with --frames DIR
    # the pages are written as pictures.
    if(test_id STREQUAL "help-pages")
      if(NOT TARGET oa-boneyards-html-host-fonts)
        message(FATAL_ERROR "the Help pages check needs the page faces")
      endif()
      target_link_libraries(oa-boneyards-console-${test_id}
        PRIVATE oa-formats-hpi oa-formats-png oa-boneyards-html-host-fonts oa-boneyards-platform-fonts)
      target_compile_definitions(oa-boneyards-console-${test_id} PRIVATE
        "OA_FONTS_DIR=\"${OA_FONTS_DIR}\""
        "OA_TEXT_FONTS_DIR=\"${OA_TEXT_FONTS_DIR}\"")
    endif()
    add_test(NAME boneyards-console-${test_id}
      COMMAND oa-boneyards-console-${test_id} ${oa_console_mod_arg})
    set_tests_properties(boneyards-console-${test_id} PROPERTIES TIMEOUT 120)
    # Rows that grow for a tall glyph read the atlas shipped in the game's fonts
    # folder. OA_FONT_ATLASES_DIR is left unset so the check uses that file.
    if(test_id STREQUAL "row-fit" AND TARGET oa-game)
      add_dependencies(oa-boneyards-console-${test_id} oa-game)
      target_compile_definitions(oa-boneyards-console-${test_id} PRIVATE
        "OA_BUNDLED_ATLAS=\"$<GENEX_EVAL:$<TARGET_PROPERTY:oa-game,OA_GAME_FILES_DIR>>/fonts/boneyards-analogues.bin\"")
    endif()
    # Every console test but the galaxy graphics' self-contained run reads
    # the BY1117a install OA_GAME_DIR names, and the war-map ones network
    # play's multiplayer install first.
    # The map-loading checks build their own map text. The others read an install.
    if(NOT test_id STREQUAL "galaxy-graphics" AND NOT test_id STREQUAL "map-loading")
      oa_boneyards_install_tests(boneyards-console-${test_id})
    endif()
  endforeach()
  # Without a live server the run returns 77. Whole-suite runs skip that,
  # the same way the other live login does.
  if(TEST boneyards-console-features-live)
    set_tests_properties(boneyards-console-features-live PROPERTIES
      TIMEOUT 180 SKIP_RETURN_CODE 77)
  endif()
  add_test(NAME boneyards-console-galaxy-graphics-data COMMAND oa-boneyards-console-galaxy-graphics --data)
  set_tests_properties(boneyards-console-galaxy-graphics-data PROPERTIES TIMEOUT 120)
  oa_boneyards_install_tests(boneyards-console-galaxy-graphics-data)
endif()
