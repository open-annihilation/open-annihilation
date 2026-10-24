# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Packs brains/classic into classic.oabrain beside the game after it links.
# A cross build does not run the build's oa-tool. The directory may be a
# generator expression, such as the game's OA_GAME_FILES_DIR.
#   oa_pack_classic_brain(<target> <directory>)

function(oa_pack_classic_brain target directory)
  if(CMAKE_CROSSCOMPILING)
    return()
  endif()
  if(NOT TARGET oa-tool)
    message(FATAL_ERROR "oa_pack_classic_brain needs the oa-tool target")
  endif()
  set(classic_source "${PROJECT_SOURCE_DIR}/brains/classic")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}/brains"
    COMMAND $<TARGET_FILE:oa-tool> brain pack --force
            --out "${directory}/brains/classic.oabrain" "${classic_source}"
    VERBATIM)
  add_dependencies(${target} oa-tool)
  if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME sim-brains-classic-package COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/src/sim/brains/tests/check_classic_package.py"
        --tool $<TARGET_FILE:oa-tool>
        --source "${classic_source}"
        --beside "${directory}/brains/classic.oabrain"
        --scratch-root "${CMAKE_CURRENT_BINARY_DIR}/C06-classic-package")
      set_tests_properties(sim-brains-classic-package PROPERTIES TIMEOUT 60)
    endif()
  endif()
endfunction()
