# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The lobby's unit sync and map checks: unit content checksums and the map
# content hash the players compare before a game starts.
add_library(oa-netgame-sync src/unit_checksum.cpp src/map_hash.cpp)
add_library(oa::netgame::sync ALIAS oa-netgame-sync)
target_include_directories(oa-netgame-sync PUBLIC include)
target_compile_features(oa-netgame-sync PUBLIC cxx_std_20)
target_link_libraries(oa-netgame-sync PUBLIC oa-data-unit-definitions oa-data-campaign oa-formats-tdf
  PRIVATE oa-base-bytes oa-data-defs oa-formats-hpi)

if(BUILD_TESTING)
  add_executable(oa-netgame-sync-test tests/sync_test.cpp)
  target_link_libraries(oa-netgame-sync-test PRIVATE oa-netgame-sync oa-data-campaign-assets oa-netgame-test-installs)
  add_test(NAME net-sync COMMAND oa-netgame-sync-test)
  # The content hash of a map of the install OA_GAME_DIR names.
  add_test(NAME net-sync-data COMMAND oa-netgame-sync-test --data)
  oa_game_data_tests(net-sync-data)
endif()
