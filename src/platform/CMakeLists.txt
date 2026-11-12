# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Host services: token locks, clock, threads, heap, performance counters,
# file/log streams, files' times, the log folder, the machine the game runs on and the
# system's memory.
add_library(oa-platform-shims src/allocator.cpp src/crash.cpp src/dataless.cpp src/files.cpp
  src/file_clone.cpp src/file_copy.cpp src/file_time.cpp src/lock.cpp src/log_files.cpp
  src/machine.cpp src/memory_status.cpp src/perf.cpp src/system.cpp src/system_memory.cpp)
add_library(oa::platform::shims ALIAS oa-platform-shims)
target_include_directories(oa-platform-shims PUBLIC include)
target_compile_features(oa-platform-shims PUBLIC cxx_std_20)
target_link_libraries(oa-platform-shims PUBLIC oa-base-threads oa-options)
if(WIN32)
  target_link_libraries(oa-platform-shims PRIVATE psapi)
endif()
# The system's trash (trash.hpp). macOS uses the file manager. Windows asks
# the shell only to recycle, with no question, back to XP (and a failure on
# Windows 9x). Linux uses the freedesktop.org trash, and iOS and Android
# delete the file because they have no trash.
if(WIN32)
  target_sources(oa-platform-shims PRIVATE src/trash_windows.cpp)
  target_link_libraries(oa-platform-shims PRIVATE shell32 ole32 uuid)
elseif(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  enable_language(OBJCXX)
  target_sources(oa-platform-shims PRIVATE src/trash_mac.mm)
elseif(ANDROID OR (APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
  target_sources(oa-platform-shims PRIVATE src/trash_remove.cpp)
else()
  target_sources(oa-platform-shims PRIVATE src/trash_xdg.cpp)
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
  # Copying a file's bytes into a new file, as a move across volumes does: the
  # bytes and a readable file come through, a taken name is kept or replaced as
  # asked, and a copy that fails leaves no file.
  add_executable(oa-platform-file-copy-test tests/file_copy_test.cpp)
  target_link_libraries(oa-platform-file-copy-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-file-copy COMMAND oa-platform-file-copy-test)
  set_tests_properties(platform-file-copy PROPERTIES TIMEOUT 30)
  # A file's time as seconds since 1970 and back: known times, one before 1970 rounded down
  # and a round trip, through a clock with to_sys and one of the Visual C++ shape (100 ns
  # ticks from 1601, no to_sys), then through this system's own file clock and a file.
  add_executable(oa-platform-file-time-test tests/file_time_test.cpp)
  target_link_libraries(oa-platform-file-time-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-file-time COMMAND oa-platform-file-time-test)
  set_tests_properties(platform-file-time PROPERTIES TIMEOUT 30)
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
  # A file in a scratch folder moved to the trash, and gone from the folder.
  # Skips where the system cannot hold the case.
  add_executable(oa-platform-trash-test tests/trash_test.cpp)
  target_link_libraries(oa-platform-trash-test PRIVATE oa-platform-shims oa-test-support)
  add_test(NAME platform-trash COMMAND oa-platform-trash-test)
  set_tests_properties(platform-trash PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
  set_tests_properties(platform-shims PROPERTIES TIMEOUT 60)
  if(OA_SANITIZERS)
    # The crash tests end a child with SIGFPE on purpose; the sanitizer must
    # leave that signal to the program. Leaks are not checked yet. The options
    # are appended to any the caller set, and win over them.
    set_tests_properties(platform-shims PROPERTIES
      ENVIRONMENT_MODIFICATION "ASAN_OPTIONS=string_append::detect_leaks=0:handle_sigfpe=0")
  endif()
endif()

