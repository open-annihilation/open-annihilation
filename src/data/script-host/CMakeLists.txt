# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Lua 5.4.9 for the script host. The sources in third_party/lua are compiled
# unchanged, as C, in one translation unit, with the engine's floating-point
# options and with warnings off.

# The ^ operator calls this library, so it has to follow the Lua archive on a
# link line. H05 adds its number text here as well. libm supplies sqrt and
# frexp; a Linux link drops an earlier libm once those references appear.
add_library(oa-data-script-host-numbers STATIC
  src/deterministic_math.cpp
  src/number_text.cpp)
add_library(oa::data::script_host_numbers ALIAS oa-data-script-host-numbers)
target_include_directories(oa-data-script-host-numbers PUBLIC include)
target_compile_features(oa-data-script-host-numbers PUBLIC cxx_std_20)
target_link_libraries(oa-data-script-host-numbers PRIVATE oa-base-game-math oa-options)
if(NOT MSVC)
  target_link_libraries(oa-data-script-host-numbers PRIVATE m)
endif()

add_library(oa-data-script-host-lua STATIC src/lua_build.c)
add_library(oa::data::script_host_lua ALIAS oa-data-script-host-lua)
target_include_directories(oa-data-script-host-lua SYSTEM PUBLIC
  "${PROJECT_SOURCE_DIR}/third_party/lua")
if(MSVC)
  # /W0 turns every warning off. oa-options is left out: an option given after
  # the warning level it sets would not replace that level. /fp:strict is set
  # on this target, which is what oa-options would have carried, so each
  # floating-point operation rounds as written.
  target_compile_options(oa-data-script-host-lua PRIVATE /W0 /fp:strict)
else()
  target_link_libraries(oa-data-script-host-lua PRIVATE oa-options)
  target_compile_options(oa-data-script-host-lua PRIVATE -w)
endif()
# A C file otherwise uses the system C library's formatting. On Windows XP
# that library has no conversion for a long long and rounds decimal text
# differently from the formatting the engine's C++ files already use.
target_compile_definitions(oa-data-script-host-lua PRIVATE
  $<$<BOOL:${MINGW}>:__USE_MINGW_ANSI_STDIO=1>)
# floor, frexp and the other C maths calls the interpreter still makes.
# Visual Studio's C library provides them; every other target takes them
# from libm. The ^ operator is not one of them.
if(NOT MSVC)
  target_link_libraries(oa-data-script-host-lua PUBLIC m)
endif()
target_link_libraries(oa-data-script-host-lua PUBLIC oa-data-script-host-numbers)

add_library(oa-data-script-host
  src/sandbox.cpp
  src/libraries.cpp
  src/patterns.cpp
  src/string_format.cpp
  src/require.cpp
  src/math_library.cpp
  src/fixed_library.cpp
  src/sorted.cpp
  src/memory_sources.cpp)
add_library(oa::data::script_host ALIAS oa-data-script-host)
target_include_directories(oa-data-script-host PUBLIC include)
target_compile_features(oa-data-script-host PUBLIC cxx_std_20)
target_link_libraries(oa-data-script-host
  PUBLIC oa-data-script-host-lua
  PRIVATE oa-options oa-formats-tdf oa-base-game-math)

if(BUILD_TESTING)
  # A text chunk, the open libraries and their numbers, one table order from
  # three states, and a refused precompiled chunk.
  add_executable(oa-data-script-host-lua-test tests/lua_build_test.cpp)
  target_link_libraries(oa-data-script-host-lua-test PRIVATE
    oa-data-script-host-lua oa-test-support oa-options)
  add_test(NAME data-script-host-lua COMMAND oa-data-script-host-lua-test)
  set_tests_properties(data-script-host-lua PROPERTIES TIMEOUT 30)

  # Libraries, require, errors, profiles and a phase that does not run a chunk.
  add_executable(oa-data-script-host-sandbox-test tests/sandbox_test.cpp)
  target_link_libraries(oa-data-script-host-sandbox-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-sandbox COMMAND oa-data-script-host-sandbox-test)
  set_tests_properties(data-script-host-sandbox PROPERTIES TIMEOUT 60)

  # Counted patterns against Lua's own results, and a match that spends the allowance.
  add_executable(oa-data-script-host-patterns-test tests/patterns_test.cpp)
  target_link_libraries(oa-data-script-host-patterns-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-patterns COMMAND oa-data-script-host-patterns-test)
  set_tests_properties(data-script-host-patterns PROPERTIES TIMEOUT 60)

  # Library work charged to the allowance, against Lua's own results.
  add_executable(oa-data-script-host-c-work-test tests/c_work_test.cpp)
  target_link_libraries(oa-data-script-host-c-work-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-c-work COMMAND oa-data-script-host-c-work-test)
  set_tests_properties(data-script-host-c-work PROPERTIES TIMEOUT 180)

  # Every route that would make a string longer than 64 KiB.
  add_executable(oa-data-script-host-string-length-test tests/string_length_test.cpp)
  target_link_libraries(oa-data-script-host-string-length-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-string-length COMMAND oa-data-script-host-string-length-test)
  set_tests_properties(data-script-host-string-length PROPERTIES TIMEOUT 60)

  # Instruction and memory allowances, including one run at the default budget.
  add_executable(oa-data-script-host-budgets-test tests/budgets_test.cpp)
  target_link_libraries(oa-data-script-host-budgets-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-budgets COMMAND oa-data-script-host-budgets-test)
  set_tests_properties(data-script-host-budgets PROPERTIES TIMEOUT 120)

  # Real-time backstop: a short limit, a huge instruction allowance.
  add_executable(oa-data-script-host-time-limit-test tests/time_limit_test.cpp)
  target_link_libraries(oa-data-script-host-time-limit-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-time-limit COMMAND oa-data-script-host-time-limit-test)
  set_tests_properties(data-script-host-time-limit PROPERTIES TIMEOUT 60)

  # Engine maths, the seeded generator and the math library's argument rules.
  add_executable(oa-data-script-host-maths-test tests/deterministic_math_test.cpp)
  target_link_libraries(oa-data-script-host-maths-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-maths COMMAND oa-data-script-host-maths-test)
  set_tests_properties(data-script-host-maths PROPERTIES TIMEOUT 60)

  # Sorted pairs, the refusal of unordered keys, and the host's name helpers.
  add_executable(oa-data-script-host-iteration-test tests/iteration_test.cpp)
  target_link_libraries(oa-data-script-host-iteration-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-iteration COMMAND oa-data-script-host-iteration-test)
  set_tests_properties(data-script-host-iteration PROPERTIES TIMEOUT 30)

  # One number spelling, no place in memory, and no binary pack.
  add_executable(oa-data-script-host-text-test tests/text_test.cpp)
  target_link_libraries(oa-data-script-host-text-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-text COMMAND oa-data-script-host-text-test)
  set_tests_properties(data-script-host-text PROPERTIES TIMEOUT 30)

  # A per-tick instruction bank, its real-time backstop, and print lines handed out.
  add_executable(oa-data-script-host-bank-test tests/bank_test.cpp)
  target_link_libraries(oa-data-script-host-bank-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-bank COMMAND oa-data-script-host-bank-test)
  set_tests_properties(data-script-host-bank PROPERTIES TIMEOUT 120)

  # Package files served from memory, matched byte for byte.
  add_executable(oa-data-script-host-memory-sources-test tests/memory_sources_test.cpp)
  target_link_libraries(oa-data-script-host-memory-sources-test PRIVATE
    oa-data-script-host oa-test-support oa-options)
  add_test(NAME data-script-host-memory-sources COMMAND oa-data-script-host-memory-sources-test)
  set_tests_properties(data-script-host-memory-sources PROPERTIES TIMEOUT 30)

  # Fixed counts against an independent table, through C++ and through a sandbox.
  add_executable(oa-data-script-host-fixed-test tests/fixed_test.cpp)
  target_link_libraries(oa-data-script-host-fixed-test PRIVATE
    oa-data-script-host oa-formats-tdf oa-test-support oa-options)
  add_test(NAME data-script-host-fixed COMMAND oa-data-script-host-fixed-test)
  set_tests_properties(data-script-host-fixed PROPERTIES TIMEOUT 60)
endif()
