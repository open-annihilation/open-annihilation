# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The facts of one game, as plain owned data a brain reads. Nothing here
# opens a match or a script.
add_library(oa-data-brain-facts src/brain_facts.cpp)
add_library(oa::data::brain_facts ALIAS oa-data-brain-facts)
target_include_directories(oa-data-brain-facts PUBLIC include)
target_compile_features(oa-data-brain-facts PUBLIC cxx_std_20)
target_link_libraries(oa-data-brain-facts PUBLIC oa-data-mod-profile oa-formats-oamod
  oa-core-types PRIVATE oa-options)

if(BUILD_TESTING)
  # mod_facts_of for no profile, the base-game id and a ridge profile, and a
  # default GameFacts.
  add_executable(oa-data-brain-facts-test tests/brain_facts_test.cpp)
  target_link_libraries(oa-data-brain-facts-test PRIVATE oa-data-brain-facts oa-test-support)
  add_test(NAME data-brain-facts COMMAND oa-data-brain-facts-test)
  set_tests_properties(data-brain-facts PROPERTIES TIMEOUT 30)
endif()
