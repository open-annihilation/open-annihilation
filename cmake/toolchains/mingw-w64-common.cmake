# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The part the mingw-w64 toolchain files beside this one share; each names its
# target and includes it. It is not a toolchain file of its own. It finds the
# cross-compiler and its tools by the target triple, searches the
# dependencies built for the target (zlib, SDL3 and any a project adding this
# one needs) under OA_WINDOWS_DEPS, one install prefix per subdirectory, and
# links the run-time libraries statically, so that executables run on a
# stock Windows install without mingw DLLs.
#
# OA_WINDOWS_DEPS defaults to local/deps/<name>, where tools/build_windows.sh
# builds them: windows for x86-64, windows-i686 for 32-bit x86, each with -xp
# added for a build whose executables also run on Windows XP
# (-DOA_WINDOWS_XP=ON, cmake/OaWindowsXp.cmake), or -95 for one that also
# runs on Windows 95 (-DOA_WINDOWS_95=ON, cmake/OaWindows95.cmake).
#
# The including file sets oa_mingw_triple (the target triple, which names
# the tools), oa_mingw_name (the name above, without -xp),
# oa_mingw_target_flags (compiler options for every source of the target)
# and oa_mingw_flags_if_accepted (options added only where the compiler
# takes them).

if(NOT DEFINED oa_mingw_triple)
  message(FATAL_ERROR "${CMAKE_CURRENT_LIST_FILE} is shared by the toolchain files beside it; "
    "name one of those as CMAKE_TOOLCHAIN_FILE")
endif()

set(OA_MINGW_TRIPLE "${oa_mingw_triple}" CACHE STRING "Target triple of the mingw-w64 toolchain")
if(NOT OA_MINGW_TRIPLE STREQUAL oa_mingw_triple)
  message(FATAL_ERROR "OA_MINGW_TRIPLE is ${OA_MINGW_TRIPLE}, but this toolchain file builds for "
    "${oa_mingw_triple}; name cmake/toolchains/${OA_MINGW_TRIPLE}.cmake as CMAKE_TOOLCHAIN_FILE instead")
endif()
find_program(CMAKE_C_COMPILER NAMES ${OA_MINGW_TRIPLE}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${OA_MINGW_TRIPLE}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${OA_MINGW_TRIPLE}-windres)
find_program(CMAKE_AR NAMES ${OA_MINGW_TRIPLE}-gcc-ar ${OA_MINGW_TRIPLE}-ar)
find_program(CMAKE_RANLIB NAMES ${OA_MINGW_TRIPLE}-gcc-ranlib ${OA_MINGW_TRIPLE}-ranlib)

if(NOT DEFINED OA_WINDOWS_DEPS)
  if(DEFINED ENV{OA_WINDOWS_DEPS})
    set(OA_WINDOWS_DEPS "$ENV{OA_WINDOWS_DEPS}")
  else()
    set(_oa_mingw_deps_name "${oa_mingw_name}")
    if(OA_WINDOWS_XP)
      string(APPEND _oa_mingw_deps_name "-xp")
    elseif(OA_WINDOWS_95)
      string(APPEND _oa_mingw_deps_name "-95")
    endif()
    get_filename_component(OA_WINDOWS_DEPS "${CMAKE_CURRENT_LIST_DIR}/../../local/deps/${_oa_mingw_deps_name}" ABSOLUTE)
  endif()
endif()
set(OA_WINDOWS_DEPS "${OA_WINDOWS_DEPS}" CACHE PATH "Install prefixes of dependencies built for the Windows target")

# Every subdirectory of the dependency root is an install prefix.
set(CMAKE_FIND_ROOT_PATH "")
if(IS_DIRECTORY "${OA_WINDOWS_DEPS}")
  file(GLOB _oa_windows_prefixes LIST_DIRECTORIES true "${OA_WINDOWS_DEPS}/*")
  foreach(_prefix IN LISTS _oa_windows_prefixes)
    if(IS_DIRECTORY "${_prefix}")
      list(APPEND CMAKE_FIND_ROOT_PATH "${_prefix}")
      # A cross-compiler whose driver injects search directories of its own,
      # as a Nix-wrapped one does, leaves the find commands re-rooting those
      # rather than the prefixes above; naming each prefix as well makes the
      # dependency it holds be found either way, and costs nothing where the
      # driver injects none.
      list(APPEND CMAKE_PREFIX_PATH "${_prefix}")
    endif()
  endforeach()
endif()
# Host tools (Python, the compilers) come from the host; headers and libraries
# only from the target prefixes.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Prefer the static archives so oa-tool and the test drivers are single files.
set(ZLIB_USE_STATIC_LIBS ON)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")

# Sets `result` to 1 when the compiler, given the options that follow,
# compiles against the headers of the C library Windows 10 added, to 0 when
# against those of the one every Windows release includes, and to an empty
# string when the compiler cannot be run.
function(_oa_mingw_newer_c_library result)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "#include <_mingw.h>"
    COMMAND "${CMAKE_C_COMPILER}" ${ARGN} -E -dM -x c -
    OUTPUT_VARIABLE macros RESULT_VARIABLE status ERROR_QUIET)
  if(NOT status EQUAL 0)
    set(${result} "" PARENT_SCOPE)
  elseif(macros MATCHES "#define _UCRT[ \t\r\n]")
    set(${result} 1 PARENT_SCOPE)
  else()
    set(${result} 0 PARENT_SCOPE)
  endif()
endfunction()

# The options every compile and link of the target takes, worked out when a
# project names this toolchain. The checks CMake compiles while it configures
# take their project's options, so they work nothing out again.
get_property(_oa_mingw_in_try_compile GLOBAL PROPERTY IN_TRY_COMPILE)
if(NOT _oa_mingw_in_try_compile)
  set(_oa_mingw_flags ${oa_mingw_target_flags})
  set(_oa_mingw_cxx_flags "")
  foreach(_oa_mingw_flag IN LISTS oa_mingw_flags_if_accepted)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo ""
      COMMAND "${CMAKE_C_COMPILER}" ${_oa_mingw_flag} -E -x c -
      RESULT_VARIABLE _oa_mingw_status OUTPUT_QUIET ERROR_QUIET)
    if(_oa_mingw_status EQUAL 0)
      list(APPEND _oa_mingw_flags ${_oa_mingw_flag})
    endif()
  endforeach()
  # A build for Windows XP links the C library every Windows release includes,
  # not the one Windows 10 added. A cross-compiler that links the newer one by
  # default is switched to the older one (-mcrtdll=msvcrt-os). Its run-time
  # libraries were built against the newer one's headers, and three things
  # keep them working with the older one's:
  # - mbstate_t, time_t and so struct timespec keep the sizes those libraries
  #   were built with (-D__LARGE_MBSTATE_T -D_TIME_BITS=64), where the older
  #   headers would make them smaller;
  # - windows-xp-c-library.specs links mingw-w64's extension library and the
  #   C library as a group, since each calls the other;
  # - windows-xp-c-library.hpp, included before every C++ source, declares
  #   two functions the C++ library's <cstdlib> names and the older headers
  #   do not declare.
  if(OA_WINDOWS_XP)
    _oa_mingw_newer_c_library(_oa_mingw_newer)
    if(_oa_mingw_newer STREQUAL "")
      message(FATAL_ERROR "${CMAKE_C_COMPILER} could not be run to tell which C library it links")
    elseif(_oa_mingw_newer)
      _oa_mingw_newer_c_library(_oa_mingw_still_newer -mcrtdll=msvcrt-os)
      if(NOT _oa_mingw_still_newer STREQUAL "0")
        message(FATAL_ERROR "OA_WINDOWS_XP needs the C library every Windows release includes; "
          "${CMAKE_C_COMPILER} links the one Windows 10 added and cannot be switched to the other "
          "(-mcrtdll=msvcrt-os). Use a cross-compiler that links the older library.")
      endif()
      list(APPEND _oa_mingw_flags -mcrtdll=msvcrt-os -D__LARGE_MBSTATE_T -D_TIME_BITS=64
        "-specs=\"${CMAKE_CURRENT_LIST_DIR}/windows-xp-c-library.specs\"")
      set(_oa_mingw_cxx_flags "-include \"${CMAKE_CURRENT_LIST_DIR}/windows-xp-c-library.hpp\"")
    endif()
  endif()
  # A build for Windows 95 links the C library Windows 95 ships, msvcrt20.dll,
  # which OSR2 and every later Windows ship too. The switch must be on the
  # link line as well as the compile line: with it only on the compile line
  # the linker still links msvcrt.dll, which Windows 95 does not have.
  if(OA_WINDOWS_95 AND NOT OA_WINDOWS_XP)
    list(APPEND _oa_mingw_flags -mcrtdll=msvcrt20)
    # The C++ run-time library's thread support is told that the version
    # compiled for has the condition variable of the Windows API, which
    # Windows 95 does not, and oa-platform-xp-runtime defines the calls it is
    # built from over the ones Windows 95 does have.
    set(_oa_mingw_cxx_flags "-include \"${CMAKE_CURRENT_LIST_DIR}/windows-95-gthr.hpp\"")
    # CMake does not pass the compiler flags on a link line, so the switch is
    # made on each of those too: a program linked without it imports
    # msvcrt.dll, which Windows 95 does not have.
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -mcrtdll=msvcrt20")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " -mcrtdll=msvcrt20")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " -mcrtdll=msvcrt20")
  endif()
  list(JOIN _oa_mingw_flags " " _oa_mingw_flags)
  set(CMAKE_C_FLAGS_INIT "${_oa_mingw_flags}")
  string(STRIP "${_oa_mingw_flags} ${_oa_mingw_cxx_flags}" CMAKE_CXX_FLAGS_INIT)
endif()
