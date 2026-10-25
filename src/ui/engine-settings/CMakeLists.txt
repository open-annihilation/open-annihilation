# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The Open Annihilation settings: their values, defaults, keys and locks
# (engine_settings.hpp), and the dialog that shows them (engine_settings/dialog.hpp),
# Developer Mode's list of the standard hacks among its sections (src/developer.cpp),
# the one-off notices of Open Annihilation's own in the dialog's look
# (engine_settings/notice.hpp), the prompts that ask with buttons of
# their own in the same look (engine_settings/prompt.hpp), the download
# check (engine_settings/challenge.hpp), the Add registry window
# (engine_settings/add_registry.hpp) and the missing-language notice
# (engine_settings/language_notice.hpp), and System's file-type switches as the file types
# they choose (engine_settings/file_types.hpp).
add_library(oa-ui-engine-settings src/engine_settings.cpp src/dialog.cpp src/dialog_draw.cpp
  src/developer.cpp src/downloads_page.cpp src/updates_page.cpp src/content_page.cpp src/language_page.cpp
  src/language_notice.cpp src/notice.cpp src/prompt.cpp src/challenge.cpp
  src/add_registry.cpp src/registered.cpp src/settings_rows.cpp src/file_types.cpp)
add_library(oa::ui::engine_settings ALIAS oa-ui-engine-settings)
target_include_directories(oa-ui-engine-settings PUBLIC include)
target_compile_features(oa-ui-engine-settings PUBLIC cxx_std_20)
target_link_libraries(oa-ui-engine-settings PUBLIC oa-platform-preferences oa-platform-file-types
  oa-data-languages oa-data-limits oa-data-mod-profile oa-ui-pad-controls oa-present
  oa-ui-frontend-renderer oa-formats-fnt oa-formats-hpi oa-ui-kit PRIVATE oa-sim-session oa-ui-decoded oa-ui-library oa-formats-url oa-options
  oa-base-text oa-platform-display-modes)

if(BUILD_TESTING)
  add_executable(oa-ui-engine-settings-test tests/engine_settings_test.cpp)
  target_link_libraries(oa-ui-engine-settings-test PRIVATE oa-ui-engine-settings oa-options)
  add_test(NAME ui-engine-settings COMMAND oa-ui-engine-settings-test)

  # Prompts of one to three buttons: their layout, keys, pointer and finger.
  add_executable(oa-ui-engine-settings-prompt-test tests/prompt_test.cpp)
  target_link_libraries(oa-ui-engine-settings-prompt-test PRIVATE oa-ui-engine-settings
    oa-test-support oa-options)
  add_test(NAME ui-engine-settings-prompt COMMAND oa-ui-engine-settings-prompt-test)

  # The missing-language notice: its parts, wrap, greyed Download, focus and reach.
  # With --data, the Chinese body is measured in the fonts it is drawn in.
  add_executable(oa-ui-engine-settings-language-notice-test tests/language_notice_test.cpp)
  target_link_libraries(oa-ui-engine-settings-language-notice-test PRIVATE oa-ui-engine-settings
    oa-platform-text-font oa-test-game-data oa-test-support oa-options)
  # The fonts sit beside this program. The directory is its own, so they are
  # not beside the pixel tests, which refuse to draw when fonts are there.
  set_target_properties(oa-ui-engine-settings-language-notice-test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/language-notice")
  oa_copy_text_fonts(oa-ui-engine-settings-language-notice-test
    "$<TARGET_FILE_DIR:oa-ui-engine-settings-language-notice-test>")
  add_test(NAME ui-engine-settings-language-notice COMMAND oa-ui-engine-settings-language-notice-test)
  oa_add_game_data_test(ui-engine-settings-language-notice-data
    oa-ui-engine-settings-language-notice-test --data)

  # The download check: its words, buttons, focus, keys and placement.
  add_executable(oa-ui-engine-settings-challenge-test tests/challenge_test.cpp)
  target_link_libraries(oa-ui-engine-settings-challenge-test PRIVATE oa-ui-engine-settings
    oa-test-support oa-options)
  add_test(NAME ui-engine-settings-challenge COMMAND oa-ui-engine-settings-challenge-test)

  # System's file-type switches: each chooses its own type, and the launch file's switch
  # registers and withdraws that type alone (XDG in a scratch data folder, or the registry
  # under a scratch key of HKEY_CURRENT_USER) and changes only its default opener on macOS
  # (a stand-in for the system's questions).
  add_executable(oa-ui-engine-settings-file-types-test tests/file_types_test.cpp)
  target_link_libraries(oa-ui-engine-settings-file-types-test PRIVATE oa-ui-engine-settings
    oa-platform-file-types oa-test-support oa-options)
  if(WIN32)
    target_link_libraries(oa-ui-engine-settings-file-types-test PRIVATE advapi32 shlwapi)
  endif()
  add_test(NAME ui-engine-settings-file-types COMMAND oa-ui-engine-settings-file-types-test)
  set_tests_properties(ui-engine-settings-file-types PROPERTIES TIMEOUT 60)

  add_executable(oa-ui-engine-settings-dialog-test tests/dialog_test.cpp)
  target_link_libraries(oa-ui-engine-settings-dialog-test PRIVATE oa-ui-engine-settings
    oa-platform-file-types oa-test-game-data oa-options)
  # The test places parts through the dialog's own geometry (src/geometry.hpp).
  target_include_directories(oa-ui-engine-settings-dialog-test PRIVATE src)
  add_test(NAME ui-engine-settings-dialog COMMAND oa-ui-engine-settings-dialog-test)
  # Ten seconds here, 24 minutes as a Debug build under Wine: the regression
  # suite multiplies a set TIMEOUT by its lane's scale (8 there, so 30 minutes),
  # and ctest's default 1500 s, which an unset one gets, it cannot scale.
  set_tests_properties(ui-engine-settings-dialog PROPERTIES TIMEOUT 225)

  # Downloads: the section list, the check, install IDs and the downloaded files.
  add_executable(oa-ui-engine-settings-downloads-test tests/downloads_page_test.cpp)
  target_link_libraries(oa-ui-engine-settings-downloads-test PRIVATE oa-ui-engine-settings
    oa-test-support oa-options)
  target_include_directories(oa-ui-engine-settings-downloads-test PRIVATE src)
  add_test(NAME ui-engine-settings-downloads COMMAND oa-ui-engine-settings-downloads-test)
  # Downloads' Registries part and the Add registry window: rows, subtitles,
  # removals, OK's changes, Escape, the locks in a game, the window's texts
  # and buttons in every mode and phase, its focus, and both at every class.
  add_executable(oa-ui-engine-settings-registries-test tests/registries_page_test.cpp)
  target_link_libraries(oa-ui-engine-settings-registries-test PRIVATE oa-ui-engine-settings
    oa-test-support oa-options)
  target_include_directories(oa-ui-engine-settings-registries-test PRIVATE src)
  add_test(NAME ui-engine-settings-registries COMMAND oa-ui-engine-settings-registries-test)
  # The dialog's fonts from the installed game, and its texts in them.
  oa_add_game_data_test(ui-engine-settings-dialog-data oa-ui-engine-settings-dialog-test --data)

  # Every setting's row as a table of literals: its kind, label, hint lines,
  # stops, levels or choices, and locks.
  add_executable(oa-ui-engine-settings-rows-test tests/rows_test.cpp)
  target_link_libraries(oa-ui-engine-settings-rows-test PRIVATE oa-ui-engine-settings
    oa-test-support oa-options)
  target_include_directories(oa-ui-engine-settings-rows-test PRIVATE src)
  add_test(NAME ui-engine-settings-rows COMMAND oa-ui-engine-settings-rows-test)
  set_tests_properties(ui-engine-settings-rows PROPERTIES TIMEOUT 30)

  # SHA-256 pictures of the dialog, its notices, its prompts and the OA button.
  add_executable(oa-ui-engine-settings-pixels-test tests/pixels_test.cpp)
  target_link_libraries(oa-ui-engine-settings-pixels-test PRIVATE oa-ui-engine-settings
    oa-platform-text-font oa-base-sha256 oa-test-support oa-test-game-data oa-options)
  add_test(NAME ui-engine-settings-pixels COMMAND oa-ui-engine-settings-pixels-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pixels.txt)
  oa_add_game_data_test(ui-engine-settings-pixels-data oa-ui-engine-settings-pixels-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pixels.txt --data)
  set_tests_properties(ui-engine-settings-pixels ui-engine-settings-pixels-data PROPERTIES TIMEOUT 60)

  # The manual's pictures: each section alone, unlocked, Compact at 2x.
  # The test writes them into a scratch folder; run the program with
  # docs/images/settings to refresh the manual.
  add_executable(oa-ui-engine-settings-pictures tests/pictures.cpp)
  target_link_libraries(oa-ui-engine-settings-pictures PRIVATE oa-ui-engine-settings
    oa-formats-png oa-formats-hpi oa-platform-text-font oa-test-game-data)
  # The header draws the app icon from the source tree's branding folder.
  target_compile_definitions(oa-ui-engine-settings-pictures PRIVATE
    OA_BRANDING_DIR="${PROJECT_SOURCE_DIR}/branding")
  oa_add_game_data_test(ui-engine-settings-pictures oa-ui-engine-settings-pictures
    "${CMAKE_CURRENT_BINARY_DIR}/G95-settings-pictures")
  set_tests_properties(ui-engine-settings-pictures PROPERTIES TIMEOUT 60)
endif()
