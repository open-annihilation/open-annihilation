# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Versioned Enhanced Mode presets layered over the chosen game's rules.
add_library(oa-data-enhanced-mode src/enhanced_mode.cpp)
add_library(oa::data::enhanced_mode ALIAS oa-data-enhanced-mode)
target_include_directories(oa-data-enhanced-mode PUBLIC include)
target_link_libraries(oa-data-enhanced-mode PUBLIC oa-data-mod-profile PRIVATE oa-options)
target_compile_features(oa-data-enhanced-mode PUBLIC cxx_std_20)

if(BUILD_TESTING)
  add_executable(oa-data-enhanced-mode-test tests/enhanced_mode_test.cpp)
  target_link_libraries(oa-data-enhanced-mode-test PRIVATE oa-data-enhanced-mode oa-test-support oa-options)
  add_test(NAME data-enhanced-mode COMMAND oa-data-enhanced-mode-test)
endif()
