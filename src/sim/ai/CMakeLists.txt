# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Computer player: decisions over the canonical World behind ComputerHost.
add_library(oa-sim-ai src/computer_player.cpp src/computer_knowledge.cpp src/computer_report.cpp)
add_library(oa::sim::ai ALIAS oa-sim-ai)
target_include_directories(oa-sim-ai PUBLIC include)
target_compile_features(oa-sim-ai PUBLIC cxx_std_20)
target_link_libraries(oa-sim-ai
  PUBLIC oa-core-types oa-sim-detection oa-data-limits oa-data-match-rules
  PRIVATE oa-sim-simulation-state oa-base-game-math oa-sim-unit-movement oa-sim-unit-health
    oa-data-enhanced-mode)
if(TARGET oa-options)
  target_link_libraries(oa-sim-ai PRIVATE oa-options)
endif()

if(BUILD_TESTING)
  add_executable(oa-ai-computer-player-test tests/computer_player_test.cpp)
  target_link_libraries(oa-ai-computer-player-test PRIVATE oa-sim-ai oa-sim-combat-state oa-sim-unit-movement
    oa-formats-hpi oa-test-game-data)
  add_test(NAME ai-computer-player COMMAND oa-ai-computer-player-test)
  oa_add_game_data_test(ai-computer-player-data oa-ai-computer-player-test --data)
endif()
