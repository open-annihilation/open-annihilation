# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Engine-side unit-script calls over the canonical World, on top of the
# script VM and the COB parser.
add_library(oa-sim-unit-script src/unit_script.cpp src/unit_value.cpp)
add_library(oa::sim::unit_script ALIAS oa-sim-unit-script)
target_include_directories(oa-sim-unit-script PUBLIC include)
target_link_libraries(oa-sim-unit-script PUBLIC oa-sim-script-vm oa-core-types oa-formats-cob oa-data-enhanced-mode oa-data-limits oa-data-match-rules PRIVATE oa-options)
if(BUILD_TESTING)
  add_executable(oa-unit-script-test tests/unit_script_test.cpp)
  target_link_libraries(oa-unit-script-test PRIVATE oa-sim-unit-script oa-options)
  add_test(NAME unit-script COMMAND oa-unit-script-test)
endif()
if(BUILD_TESTING)
  add_executable(oa-unit-script-extensions-test tests/script_extensions_test.cpp)
  target_link_libraries(oa-unit-script-extensions-test PRIVATE oa-sim-unit-script oa-test-support oa-options)
  add_test(NAME unit-script-extensions COMMAND oa-unit-script-extensions-test)
endif()
# The extensions under an installed mod's own scripts: the game folder
# OA_MOD_GAME_DIR names, with a mod installed over it, and the reference
# profiles in OA_MOD_PROFILES_DIR. Skipped without them.
if(BUILD_TESTING)
  add_executable(oa-unit-script-extensions-mod-install-test tests/script_extensions_mod_test.cpp)
  target_link_libraries(oa-unit-script-extensions-mod-install-test PRIVATE oa-sim-unit-script
    oa-data-defs-assets oa-data-mod-profile oa-formats-hpi oa-platform-shims oa-test-support oa-options)
  add_test(NAME unit-script-extensions-mod-install COMMAND oa-unit-script-extensions-mod-install-test)
  set_tests_properties(unit-script-extensions-mod-install PROPERTIES SKIP_RETURN_CODE 77)
  set(oa_mod_install_environment)
  if(DEFINED OA_MOD_GAME_DIR)
    list(APPEND oa_mod_install_environment "OA_MOD_GAME_DIR=${OA_MOD_GAME_DIR}")
  endif()
  if(DEFINED OA_MOD_PROFILES_DIR)
    list(APPEND oa_mod_install_environment "OA_MOD_PROFILES_DIR=${OA_MOD_PROFILES_DIR}")
  endif()
  if(oa_mod_install_environment)
    set_tests_properties(unit-script-extensions-mod-install PROPERTIES
      ENVIRONMENT "${oa_mod_install_environment}")
  endif()
endif()
