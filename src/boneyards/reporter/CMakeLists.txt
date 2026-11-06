# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The match report the Boneyards keeps of a network game: the score, player
# and settings tables it assembles from the Player records, and their
# uploads through hooks and to the reporting library. The Boneyards'
# extension drives it through the report hooks of network play's extension
# API (src/netgame/app/boneyards_report.cpp).
add_library(oa-boneyards-reporter STATIC src/reporter.cpp)
add_library(oa::boneyards::reporter ALIAS oa-boneyards-reporter)
target_include_directories(oa-boneyards-reporter PUBLIC include)
target_compile_features(oa-boneyards-reporter PUBLIC cxx_std_20)
target_link_libraries(oa-boneyards-reporter PUBLIC oa-core-types
  PRIVATE oa-data-campaign oa-sim-simulation-state oa-ui-frontend-multiplayer oa-options)

if(BUILD_TESTING)
  add_executable(oa-boneyards-reporter-test tests/reporter_test.cpp)
  target_link_libraries(oa-boneyards-reporter-test
    PRIVATE oa-boneyards-reporter oa-data-campaign oa-ui-frontend-multiplayer oa-options)
  add_test(NAME boneyards-reporter COMMAND oa-boneyards-reporter-test)
  set_tests_properties(boneyards-reporter PROPERTIES TIMEOUT 60)
endif()
