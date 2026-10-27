# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The OA UI kit: one look, one text measure, one layout and one set of controls
# for every OA screen (include/oa/ui/kit/theme.hpp, include/oa/ui/kit/text.hpp,
# include/oa/ui/kit/layout.hpp, include/oa/ui/kit/input.hpp,
# include/oa/ui/kit/looks.hpp, include/oa/ui/kit/canvas.hpp, include/oa/ui/kit/components.hpp,
# include/oa/ui/kit/chrome.hpp, include/oa/ui/kit/controls.hpp,
# include/oa/ui/kit/components_more.hpp, include/oa/ui/kit/first_run.hpp,
# include/oa/ui/kit/keys.hpp), and the
# gallery of every component (include/oa/ui/kit/gallery.hpp). Provisional until
# the kit is declared stable.
add_library(oa-ui-kit src/text.cpp src/wrap.cpp src/layout.cpp src/input.cpp src/canvas.cpp src/components.cpp
  src/chrome.cpp src/controls.cpp src/components_more.cpp src/first_run.cpp src/gallery.cpp
  src/keys.cpp)
# Declared rows: a settings page as a table (include/oa/ui/kit/rows.hpp).
target_sources(oa-ui-kit PRIVATE src/rows.cpp)
add_library(oa::ui::kit ALIAS oa-ui-kit)
target_include_directories(oa-ui-kit PUBLIC include)
target_compile_features(oa-ui-kit PUBLIC cxx_std_20)
# The canvas's smooth form paints with oa-ui-paint in the bundled modern fonts.
target_link_libraries(oa-ui-kit PUBLIC oa-ui-paint oa-platform-text-font oa-ui-frontend-renderer
  oa-present oa-formats-fnt oa-formats-hpi oa-platform-preferences PRIVATE oa-base-text oa-ui-decoded
  oa-options)

if(BUILD_TESTING)
  add_executable(oa-ui-kit-theme-test tests/theme_test.cpp)
  target_link_libraries(oa-ui-kit-theme-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-theme COMMAND oa-ui-kit-theme-test)
  set_tests_properties(ui-kit-theme PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-text-test tests/text_test.cpp)
  target_link_libraries(oa-ui-kit-text-test PRIVATE oa-ui-kit oa-test-support oa-options oa-ui-paint
    oa-platform-text-font oa-base-text)
  # The moved paint wrap checks read the bundled fonts beside the test.
  # This program, the smooth test and the first-run test are written into one
  # folder, so one rule stages that folder's fonts and each of them depends
  # on it. The directory text has to be the same for the rule to be shared.
  set(oa_ui_kit_font_dir "$<TARGET_FILE_DIR:oa-ui-kit-text-test>")
  oa_copy_text_fonts(oa-ui-kit-text-test "${oa_ui_kit_font_dir}")
  add_test(NAME ui-kit-text COMMAND oa-ui-kit-text-test)
  set_tests_properties(ui-kit-text PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-layout-test tests/layout_test.cpp)
  target_link_libraries(oa-ui-kit-layout-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-layout COMMAND oa-ui-kit-layout-test)
  set_tests_properties(ui-kit-layout PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-input-test tests/input_test.cpp)
  target_link_libraries(oa-ui-kit-input-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-input COMMAND oa-ui-kit-input-test)
  set_tests_properties(ui-kit-input PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-components-test tests/components_test.cpp)
  target_link_libraries(oa-ui-kit-components-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-components COMMAND oa-ui-kit-components-test)
  set_tests_properties(ui-kit-components PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-chrome-test tests/chrome_test.cpp)
  target_link_libraries(oa-ui-kit-chrome-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-chrome COMMAND oa-ui-kit-chrome-test)
  set_tests_properties(ui-kit-chrome PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-controls-test tests/controls_test.cpp)
  target_link_libraries(oa-ui-kit-controls-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-controls COMMAND oa-ui-kit-controls-test)
  set_tests_properties(ui-kit-controls PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-tag-icons-test tests/tag_icons_test.cpp)
  target_link_libraries(oa-ui-kit-tag-icons-test PRIVATE oa-ui-kit oa-test-support oa-options
    oa-ui-paint oa-base-sha256)
  add_test(NAME ui-kit-tag-icons COMMAND oa-ui-kit-tag-icons-test)
  set_tests_properties(ui-kit-tag-icons PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-keys-test tests/keys_test.cpp)
  target_link_libraries(oa-ui-kit-keys-test PRIVATE oa-ui-kit oa-test-support oa-options
    oa-ui-paint)
  add_test(NAME ui-kit-keys COMMAND oa-ui-kit-keys-test)
  set_tests_properties(ui-kit-keys PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-notices-test tests/notices_test.cpp)
  target_link_libraries(oa-ui-kit-notices-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-notices COMMAND oa-ui-kit-notices-test)
  set_tests_properties(ui-kit-notices PROPERTIES TIMEOUT 30)

  # The smooth form of the canvas at 1.5x; its text checks read the bundled
  # fonts beside the test.
  add_executable(oa-ui-kit-smooth-test tests/smooth_test.cpp)
  target_link_libraries(oa-ui-kit-smooth-test PRIVATE oa-ui-kit oa-test-support oa-options oa-ui-paint
    oa-platform-text-font)
  oa_copy_text_fonts(oa-ui-kit-smooth-test "${oa_ui_kit_font_dir}")
  add_test(NAME ui-kit-smooth COMMAND oa-ui-kit-smooth-test)
  set_tests_properties(ui-kit-smooth PROPERTIES TIMEOUT 30)

  add_executable(oa-ui-kit-rows-test tests/rows_test.cpp)
  target_link_libraries(oa-ui-kit-rows-test PRIVATE oa-ui-kit oa-test-support oa-options)
  add_test(NAME ui-kit-rows COMMAND oa-ui-kit-rows-test)
  set_tests_properties(ui-kit-rows PROPERTIES TIMEOUT 30)

  # The gallery at Compact, Large and Regular: every role, every state, its
  # places, focus and names, and that no text shows the build's version,
  # which the test is given.
  add_executable(oa-ui-kit-gallery-test tests/gallery_test.cpp)
  target_link_libraries(oa-ui-kit-gallery-test PRIVATE oa-ui-kit oa-test-support oa-options)
  target_compile_definitions(oa-ui-kit-gallery-test PRIVATE
    "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
  # The modern look's case reads the bundled fonts, staged with the text test's.
  oa_copy_text_fonts(oa-ui-kit-gallery-test "${oa_ui_kit_font_dir}")
  target_link_libraries(oa-ui-kit-gallery-test PRIVATE oa-platform-text-font)
  add_test(NAME ui-kit-gallery COMMAND oa-ui-kit-gallery-test)
  set_tests_properties(ui-kit-gallery PROPERTIES TIMEOUT 30)

  # The first-run pieces at Compact, Regular and Large, at 1x and at 150%.
  # The smooth draws read the bundled fonts beside the test.
  add_executable(oa-ui-kit-first-run-test tests/first_run_test.cpp)
  target_link_libraries(oa-ui-kit-first-run-test PRIVATE oa-ui-kit oa-test-support oa-options
    oa-ui-paint oa-platform-text-font)
  oa_copy_text_fonts(oa-ui-kit-first-run-test "${oa_ui_kit_font_dir}")
  add_test(NAME ui-kit-first-run COMMAND oa-ui-kit-first-run-test)
  set_tests_properties(ui-kit-first-run PROPERTIES TIMEOUT 30)
endif()
