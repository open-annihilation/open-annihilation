# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Writes the header that names the engine's build, run as a script on every
# build: cmake -DVERSION=<x.y.z> -DSOURCE_DIR=<dir> -DOUTPUT=<header>
# -P OaEngineBuild.cmake. The build is the version and, where SOURCE_DIR is
# a Git checkout, the commit it holds, as <version>+<commit>; elsewhere the
# version alone. OUTPUT is rewritten only when its text changes, so that a
# build of the same commit compiles nothing again.
if(NOT VERSION OR NOT SOURCE_DIR OR NOT OUTPUT)
  message(FATAL_ERROR
    "OaEngineBuild.cmake needs -DVERSION=<x.y.z> -DSOURCE_DIR=<dir> -DOUTPUT=<header>")
endif()
set(build "${VERSION}")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE commit
    RESULT_VARIABLE result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(result EQUAL 0 AND commit MATCHES "^[0-9a-f]+$")
    set(build "${VERSION}+${commit}")
  endif()
endif()
file(CONFIGURE OUTPUT "${OUTPUT}" @ONLY CONTENT [[
// Written by cmake/OaEngineBuild.cmake on every build; edit nothing here.
#pragma once

/// The engine's build: its version and the commit it was built from, which
/// the renderer records' strikes and records are written under.
#define OA_ENGINE_BUILD "@build@"
]])
