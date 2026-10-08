# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# oa-options, the compile options every target that compiles engine code
# links. The top-level project includes this file, and so does each module
# that can also be configured on its own, so both builds compile that module
# with the same options. The top-level OA_SANITIZERS option adds its flags to
# this target in the top-level CMakeLists.txt.
include_guard(GLOBAL)
if(NOT TARGET oa-options)
  add_library(oa-options INTERFACE)
  add_library(oa::options ALIAS oa-options)
  if(MSVC)
    # No warning that a type was padded to the alignment alignas asks for,
    # which is what alignas is for. The sources are UTF-8, and so are their
    # strings, Chinese among them, whatever the machine's code page.
    target_compile_options(oa-options INTERFACE /W4 /permissive- /wd4324 /utf-8)
  else()
    target_compile_options(oa-options INTERFACE -Wall -Wextra -Wpedantic)
  endif()
  # OA_WARNINGS_AS_ERRORS turns every warning those options raise into an
  # error, in the targets that link oa-options; SDL and the third-party
  # sources keep their own warnings. CI turns it on for every compiler.
  option(OA_WARNINGS_AS_ERRORS "Fail the build on any compiler warning in engine code" OFF)
  if(OA_WARNINGS_AS_ERRORS)
    if(MSVC)
      # The linker's warnings too, in the programs and libraries that link
      # oa-options.
      target_compile_options(oa-options INTERFACE /WX)
      target_link_options(oa-options INTERFACE /WX)
    else()
      target_compile_options(oa-options INTERFACE -Werror)
    endif()
  endif()
  # Each basic floating-point operation is rounded as written: never fused
  # into a multiply-add, which GCC and Clang do by default wherever the CPU
  # has the instruction (arm64 has it; baseline x86-64 does not), and never
  # reassociated or otherwise changed by value-changing optimisations.
  # Simulation state, trace digests and save files depend on this, so the
  # options apply to the whole tree rather than to a list of simulation
  # modules. -ffp-contract=off comes after -fno-fast-math because some Clang
  # versions reset contraction to their default on -fno-fast-math.
  #
  # Maths library functions such as atan2, sin, cos and hypot, and the
  # precision of long double, differ between platforms; these options do not
  # control them. The simulation therefore uses neither: it takes its
  # arctangents, sines, cosines and lengths from src/base/game-math, which
  # computes them with integer arithmetic (see its README.md).
  if(MSVC)
    target_compile_options(oa-options INTERFACE /fp:strict)
  else()
    target_compile_options(oa-options INTERFACE -fno-fast-math -ffp-contract=off)
  endif()
  # A 32-bit x86 build rounds each float operation to a 24-bit and each
  # double operation to a 53-bit significand as it is computed, as x86-64
  # does, instead of carrying intermediate results at a wider precision
  # until they are stored. Its processor floor is the Pentium III and the
  # Athlon XP: nothing needs SSE2.
  # - Doubles are computed on the processor's older floating-point unit,
  #   which every executable sets to a double's precision before main()
  #   runs, and every thread the engine starts sets again
  #   (src/base/float-precision, taken whole into each executable).
  # - Floats: OA_X86_FLOAT=sse (the default) computes them with SSE, which
  #   rounds each to a float; fpu computes them on the same unit as the
  #   doubles, at a double's precision, which changes the simulation's
  #   results.
  # A build that takes /fp:strict rounds both as written already.
  if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT MSVC AND
     CMAKE_SYSTEM_PROCESSOR MATCHES "^([iI][3-6]86|[xX]86|[xX]86_64|AMD64|amd64)$")
    set(OA_X86_FLOAT "sse" CACHE STRING "How a 32-bit x86 build computes floats: sse or fpu")
    set_property(CACHE OA_X86_FLOAT PROPERTY STRINGS sse fpu)
    if(OA_X86_FLOAT STREQUAL "sse")
      target_compile_options(oa-options INTERFACE -msse -mfpmath=sse)
    elseif(NOT OA_X86_FLOAT STREQUAL "fpu")
      message(FATAL_ERROR "OA_X86_FLOAT is '${OA_X86_FLOAT}'; it must be sse or fpu")
    endif()
    target_link_libraries(oa-options INTERFACE
      "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:$<LINK_LIBRARY:WHOLE_ARCHIVE,$<TARGET_NAME_IF_EXISTS:oa-base-float-precision>>>")
  endif()
  # Every executable of a MinGW build takes its steady clock from the
  # performance counter, whole, before the linker reaches the C++ run-time
  # library, whose own may read the time of day (src/platform/steady-clock).
  if(MINGW)
    target_link_libraries(oa-options INTERFACE
      "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:$<LINK_LIBRARY:WHOLE_ARCHIVE,$<TARGET_NAME_IF_EXISTS:oa-platform-steady-clock>>>")
  endif()
  # A 32-bit POSIX build uses 64-bit file offsets and file serial numbers,
  # so it can examine every file and folder a file system holds, however
  # large the file or its serial number.
  if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT WIN32)
    target_compile_definitions(oa-options INTERFACE _FILE_OFFSET_BITS=64)
  endif()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/OaWindowsXp.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/OaWindows95.cmake")
