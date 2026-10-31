# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Host services: token locks, clock, threads, heap, performance counters,
# file/log streams, the log folder, the machine the game runs on and the
# system's memory.
add_library(oa-platform-shims src/allocator.cpp src/crash.cpp src/dataless.cpp src/files.cpp
  src/file_clone.cpp src/lock.cpp src/log_files.cpp
  src/machine.cpp src/memory_status.cpp src/perf.cpp src/system.cpp src/system_memory.cpp)
add_library(oa::platform::shims ALIAS oa-platform-shims)
target_include_directories(oa-platform-shims PUBLIC include)
target_compile_features(oa-platform-shims PUBLIC cxx_std_20)
target_link_libraries(oa-platform-shims PUBLIC oa-base-threads oa-options)
if(WIN32)
  target_link_libraries(oa-platform-shims PRIVATE psapi)
endif()
# Files iCloud has moved off the Mac (dataless.hpp): macOS only. iOS and
# every other system build the part that reports none and fetches nothing.
if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  enable_language(OBJCXX)
  target_sources(oa-platform-shims PRIVATE src/dataless_mac.mm)
  target_compile_definitions(oa-platform-shims PRIVATE OA_MACOS_DATALESS_FILES=1)
  set_target_properties(oa-platform-shims PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_link_libraries(oa-platform-shims PRIVATE "-framework Foundation")
endif()

if(BUILD_TESTING)
  add_executable(oa-platform-shims-test tests/platform_test.cpp)
  target_link_libraries(oa-platform-shims-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-shims COMMAND oa-platform-shims-test)
  # Replacing a file by a temporary file beside it: a new file, a replaced
  # file, a name outside ASCII, and a folder left untouched when the replace
  # is refused.
  add_executable(oa-platform-replace-file-test tests/replace_file_test.cpp)
  target_link_libraries(oa-platform-replace-file-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-replace-file COMMAND oa-platform-replace-file-test)
  set_tests_properties(platform-replace-file PROPERTIES TIMEOUT 30)
  # A path's UTF-8 spelling and the path a spelling names: UTF-16 and UTF-8
  # both ways, text that is not UTF-8, and a folder whose names no one code
  # page holds, listed, opened and replaced.
  add_executable(oa-platform-path-text-test tests/path_text_test.cpp)
  target_link_libraries(oa-platform-path-text-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-path-text COMMAND oa-platform-path-text-test)
  set_tests_properties(platform-path-text PROPERTIES TIMEOUT 30)
  add_executable(oa-platform-log-files-test tests/log_files_test.cpp)
  target_link_libraries(oa-platform-log-files-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-log-files COMMAND oa-platform-log-files-test)
  add_executable(oa-platform-machine-test tests/machine_test.cpp)
  target_link_libraries(oa-platform-machine-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-machine COMMAND oa-platform-machine-test)
  # This system's memory sampled as the memory guard samples it: the figures
  # printed, checked for sense, and skipped where the system reports none.
  add_executable(oa-platform-system-memory-test tests/system_memory_test.cpp)
  target_link_libraries(oa-platform-system-memory-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-system-memory COMMAND oa-platform-system-memory-test)
  set_tests_properties(platform-system-memory PROPERTIES SKIP_RETURN_CODE 77)
  set_tests_properties(platform-shims PROPERTIES TIMEOUT 60)
  if(OA_SANITIZERS)
    # The crash tests end a child with SIGFPE on purpose; the sanitizer must
    # leave that signal to the program. Leaks are not checked yet. The options
    # are appended to any the caller set, and win over them.
    set_tests_properties(platform-shims PROPERTIES
      ENVIRONMENT_MODIFICATION "ASAN_OPTIONS=string_append::detect_leaks=0:handle_sigfpe=0")
  endif()
endif()

