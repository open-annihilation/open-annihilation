# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Packs brains/classic into classic.oabrain beside the game after it links.
# The directory may be a generator expression, such as the game's
# OA_GAME_FILES_DIR.
#
# A cross build cannot run the oa-tool it compiles. OA_HOST_OA_TOOL names
# the oa-tool of a host build. When it names an executable, the cross build
# runs that tool with the same arguments and writes the same path, so the
# package is the bytes a host build packs. When it is empty, the cross build
# leaves Classic unpacked and says so.
#   oa_pack_classic_brain(<target> <directory>)

set(OA_HOST_OA_TOOL "$ENV{OA_HOST_OA_TOOL}" CACHE FILEPATH
  "Host oa-tool that packs Classic beside the game when cross-compiling; empty leaves it unpacked")
# set() never replaces an existing entry. A build tree configured before the
# variable was exported holds an empty one: take the environment's value.
if(OA_HOST_OA_TOOL STREQUAL "" AND NOT "$ENV{OA_HOST_OA_TOOL}" STREQUAL "")
  set_property(CACHE OA_HOST_OA_TOOL PROPERTY VALUE "$ENV{OA_HOST_OA_TOOL}")
endif()

function(oa_pack_classic_brain target directory)
  set(classic_source "${PROJECT_SOURCE_DIR}/brains/classic")
  if(CMAKE_CROSSCOMPILING)
    if(OA_HOST_OA_TOOL STREQUAL "")
      message(STATUS "Classic is not packed: this is a cross build and OA_HOST_OA_TOOL is empty, "
        "so there is no host oa-tool to pack it with")
      return()
    endif()
    if(NOT IS_ABSOLUTE "${OA_HOST_OA_TOOL}")
      message(FATAL_ERROR "OA_HOST_OA_TOOL (\"${OA_HOST_OA_TOOL}\") is a relative path; pass the absolute "
        "path of the host build's oa-tool")
    endif()
    if(NOT EXISTS "${OA_HOST_OA_TOOL}" OR IS_DIRECTORY "${OA_HOST_OA_TOOL}")
      message(FATAL_ERROR "OA_HOST_OA_TOOL (\"${OA_HOST_OA_TOOL}\") does not name an executable")
    endif()
    message(STATUS "Classic is packed with the host oa-tool ${OA_HOST_OA_TOOL}")
    set(pack_tool "${OA_HOST_OA_TOOL}")
  else()
    if(NOT TARGET oa-tool)
      message(FATAL_ERROR "oa_pack_classic_brain needs the oa-tool target")
    endif()
    set(pack_tool "$<TARGET_FILE:oa-tool>")
  endif()
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}/brains"
    COMMAND "${pack_tool}" brain pack --force
            --out "${directory}/brains/classic.oabrain" "${classic_source}"
    VERBATIM)
  if(CMAKE_CROSSCOMPILING)
    return()
  endif()
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
