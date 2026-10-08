# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# OA_WINDOWS_95 builds 32-bit Windows executables that also run on Windows 95,
# and so on every later Windows. Engine code is compiled against the
# declarations of Windows 95; every executable is marked as one Windows 95
# will load (subsystem and system version 4.0); and it links the C library
# Windows 95 ships, msvcrt20.dll, not the one Windows 10 added. A build made
# this way runs on Windows 95 RTM, which ships msvcrt20.dll (and crtdll.dll,
# which lacks the thread functions the engine needs); OSR2 and later ship
# msvcrt40.dll as well, and are covered by the same executable. It needs a
# 32-bit MinGW toolchain whose C library the toolchain files in
# cmake/toolchains switch to msvcrt20, which mingw-w64 links with
# -mcrtdll=msvcrt20: the switch must be made while linking, not only while
# compiling, or the program imports the msvcrt.dll Windows 95 does not have.
# OaOptions.cmake includes this file once oa-options exists.
include_guard(GLOBAL)
option(OA_WINDOWS_95 "Build 32-bit Windows executables that also run on Windows 95" OFF)
if(OA_WINDOWS_95)
  if(NOT MINGW)
    message(FATAL_ERROR "OA_WINDOWS_95 needs a MinGW toolchain for Windows")
  endif()
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "OA_WINDOWS_95 builds 32-bit Windows executables; Windows 95 has no "
      "64-bit edition")
  endif()
  # Windows 95 is 4.00. WINVER and _WIN32_WINNT hold its version, so a call a
  # later Windows added does not compile; _WIN32_WINDOWS holds it too, for the
  # declarations mingw-w64 keeps for the Windows 9x line alone. The one thing
  # the 4.00 declarations leave out that the C++ run-time library needs — the
  # condition variable of the Windows API — cmake/toolchains/windows-95-gthr.hpp
  # declares and oa-platform-xp-runtime defines over the calls 95 does have.
  # SDL_MAIN_HANDLED keeps SDL from supplying the entry point of a Windows
  # program and calling the engine's main through it: on Windows 95 that
  # start-up ends the process before main runs, so nothing is logged, nothing
  # opens and nothing is reported. The program's own main is the entry point
  # instead, reached through the C run-time library's.
  target_compile_definitions(
    oa-options INTERFACE _WIN32_WINDOWS=0x0400 _WIN32_WINNT=0x0400 WINVER=0x0400 SDL_MAIN_HANDLED=1
  )
  # Windows 95 refuses to load a program whose subsystem or system version is
  # newer than it is, so both are stamped 4.0: the subsystem version the
  # loader checks, and the minimum operating system the image names.
  target_link_options(oa-options INTERFACE
    "LINKER:--major-subsystem-version,4" "LINKER:--minor-subsystem-version,0"
    "LINKER:--major-os-version,4" "LINKER:--minor-os-version,0")
  # Every executable links oa-platform-xp-runtime whole, as a Windows XP build
  # does: it defines the functions the C++ run-time library calls that the C
  # library Windows 95 ships does not have — _putenv_s, wcrtomb_s and the _l
  # character, collation and number functions among them — so a program
  # imports none of them.
  target_link_libraries(oa-options INTERFACE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:$<LINK_LIBRARY:WHOLE_ARCHIVE,$<TARGET_NAME_IF_EXISTS:oa-platform-xp-runtime>>>")
endif()
