# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Total Annihilation folders found where Steam, Heroic, Lutris or Bottles put
# them, from the libraries and lists those programs keep, read within fixed
# bounds (game_installs.hpp).
add_library(oa-platform-game-installs src/game_installs.cpp src/library_files.cpp)
add_library(oa::platform::game_installs ALIAS oa-platform-game-installs)
target_include_directories(oa-platform-game-installs PUBLIC include)
target_compile_features(oa-platform-game-installs PUBLIC cxx_std_20)
target_link_libraries(oa-platform-game-installs PRIVATE oa-platform-shims oa-options)

if(BUILD_TESTING)
  # Synthetic home folders built in a temporary folder, and the readers of
  # each library file, malformed ones included.
  add_executable(oa-platform-game-installs-test tests/game_installs_test.cpp)
  target_link_libraries(oa-platform-game-installs-test PRIVATE oa-platform-game-installs
    oa-test-support oa-options)
  add_test(NAME platform-game-installs COMMAND oa-platform-game-installs-test)
endif()
