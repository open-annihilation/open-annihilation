# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The Library's model: every package the player can get or has installed,
# built from plain inputs, with its tabs, filters, search, selection, details
# and download queue lines, the pending expansion choice (content.cpp), and
# its words (library.hpp, text.hpp). It draws nothing and reads no file,
# clock or network. The update question asked
# before an update that may stop saved games loading (update_question.hpp),
# the questions about the mod the game plays: SWITCH OFF's, PLAY NOW's
# refusal and ROLL BACK's (mod_questions.hpp), UNINSTALL's rules, its
# question and its notices (uninstall.hpp), and the order of a mutator's
# prerequisites, the download plan and GET WITH's question (prerequisites.cpp).
add_library(oa-ui-library src/library.cpp src/search.cpp src/text.cpp
  src/content.cpp src/game_icon.cpp src/update_question.cpp src/mod_questions.cpp
  src/uninstall.cpp src/prerequisites.cpp)
# The Library's screens, made of the UI kit's components: their layout at
# the three size classes, their drawing and their input (screen.hpp).
target_sources(oa-ui-library PRIVATE src/layout.cpp src/draw.cpp src/input.cpp)
add_library(oa::ui::library ALIAS oa-ui-library)
target_include_directories(oa-ui-library PUBLIC include)
target_compile_features(oa-ui-library PUBLIC cxx_std_20)
target_link_libraries(oa-ui-library PUBLIC oa-formats-oamod oa-base-sha256 oa-ui-kit
  oa-data-mutators PRIVATE
  oa-base-text oa-data-mod-profile oa-data-content-bundles oa-data-languages oa-formats-oamut
  oa-formats-oabrain oa-options)

if(BUILD_TESTING)
  # Entries and their states, blocking, registries, tabs, each tab's filter,
  # New, the filters' counts, tags, brains, the Active tab, the order, selection,
  # buttons, facts, status lines, notes, queue lines and every text.
  add_executable(oa-ui-library-test tests/library_test.cpp)
  target_link_libraries(oa-ui-library-test PRIVATE oa-ui-library oa-data-languages
    oa-data-mod-profile oa-test-support oa-options)
  add_test(NAME ui-library COMMAND oa-ui-library-test)
  # The base game chooser and locked-pill checks walk every short sequence of
  # presses (about 115,000), a second here and minutes in a Windows debug build.
  set_tests_properties(ui-library PROPERTIES TIMEOUT 120)

  # The update question: when it is asked (never for a built-in registry's
  # update, one that keeps the rules, or a language or map pack), its words
  # for a new revision and a new version, UPDATE ALL's and its buttons; and
  # the questions about the mod the game plays, their words and buttons.
  add_executable(oa-ui-library-update-question-test tests/update_question_test.cpp)
  target_link_libraries(oa-ui-library-update-question-test PRIVATE oa-ui-library oa-ui-kit
    oa-test-support oa-options)
  add_test(NAME ui-library-update-question COMMAND oa-ui-library-update-question-test)
  set_tests_properties(ui-library-update-question PROPERTIES TIMEOUT 30)

  # UNINSTALL: on for every installed package, off for a map pack in a game
  # and while queued, what it leads to for the mod being played, the
  # language pack in use and any other package, its question, its notices
  # and the note that says why it is off.
  add_executable(oa-ui-library-uninstall-test tests/uninstall_test.cpp)
  target_link_libraries(oa-ui-library-uninstall-test PRIVATE oa-ui-library oa-ui-kit
    oa-test-support oa-options)
  add_test(NAME ui-library-uninstall COMMAND oa-ui-library-uninstall-test)
  set_tests_properties(ui-library-uninstall PROPERTIES TIMEOUT 30)

  # The order of mutator prerequisites: the closure, the dependants and
  # whether a list keeps every prerequisite above what needs it; and the
  # download plan, GET WITH and its question.
  add_executable(oa-ui-library-prerequisites-test tests/prerequisites_test.cpp)
  target_link_libraries(oa-ui-library-prerequisites-test PRIVATE oa-ui-library oa-test-support
    oa-options)
  add_test(NAME ui-library-prerequisites COMMAND oa-ui-library-prerequisites-test)
  set_tests_properties(ui-library-prerequisites PROPERTIES TIMEOUT 30)

  # The search: what ranks first, several words, a missing word, folding,
  # tag prefixes, the cap and ties.
  add_executable(oa-ui-library-search-test tests/search_test.cpp)
  target_link_libraries(oa-ui-library-search-test PRIVATE oa-ui-library oa-test-support oa-options)
  add_test(NAME ui-library-search COMMAND oa-ui-library-search-test)
  set_tests_properties(ui-library-search PROPERTIES TIMEOUT 30)

  # The screens' layout at Compact, Regular and Large and at three windows:
  # parts inside the frame, no two controls meeting, every action a control,
  # Tab's order and the arrows, the names, the texts fitting and the Rules
  # fact whole, and what the keys, the controller and the pointer change.
  add_executable(oa-ui-library-layout-test tests/layout_test.cpp)
  target_link_libraries(oa-ui-library-layout-test PRIVATE oa-ui-library oa-ui-kit oa-test-support
    oa-options)
  add_test(NAME ui-library-layout COMMAND oa-ui-library-layout-test)
  set_tests_properties(ui-library-layout PROPERTIES TIMEOUT 60)

  # The pictures of the rows in view, and a few past each edge: a list of
  # forty, scrolled, asks for each of those once, the selected row first,
  # and a picture that arrives is on its row.
  add_executable(oa-ui-library-pictures-test tests/pictures_test.cpp)
  target_link_libraries(oa-ui-library-pictures-test PRIVATE oa-ui-library oa-ui-kit oa-test-support
    oa-options)
  add_test(NAME ui-library-pictures COMMAND oa-ui-library-pictures-test)
  set_tests_properties(ui-library-pictures PROPERTIES TIMEOUT 30)

  # Each class drawn twice in the installed game's fonts, and the Active and
  # Updates tabs, byte for byte the same and nothing outside the window;
  # with --frames DIR it writes the pictures for review.
  add_executable(oa-ui-library-draw-test tests/draw_test.cpp)
  target_link_libraries(oa-ui-library-draw-test PRIVATE oa-ui-library oa-ui-kit oa-formats-png
    oa-formats-hpi oa-platform-text-font oa-present oa-test-game-data oa-test-support oa-options)
  # The modern fonts draw the characters the game's fonts lack, as beside the game.
  oa_copy_text_fonts(oa-ui-library-draw-test "$<TARGET_FILE_DIR:oa-ui-library-draw-test>")
  # The Registries picture draws the app icon from the source tree's branding
  # folder, which a build tree outside the sources cannot find by climbing.
  target_compile_definitions(oa-ui-library-draw-test PRIVATE
    OA_BRANDING_DIR="${PROJECT_SOURCE_DIR}/branding")
  oa_add_game_data_test(ui-library-draw-data oa-ui-library-draw-test --data)
  set_tests_properties(ui-library-draw-data PROPERTIES TIMEOUT 60)
endif()
