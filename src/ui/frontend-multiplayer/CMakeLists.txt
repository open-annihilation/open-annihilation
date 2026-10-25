# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Multiplayer connect, lobby and battleroom screens.
add_library(oa-ui-frontend-multiplayer
  src/panel.cpp
  src/lobby_net.cpp
  src/battleroom.cpp
  src/room_mutators.cpp
  src/room_limits.cpp
  src/team_rules.cpp
  src/unit_sync.cpp
  src/restrict.cpp
  src/connect.cpp
  src/dialogs.cpp
  src/map_list.cpp
  src/screens.cpp
  src/presence_card.cpp
  src/get_map.cpp
  src/host_mutators.cpp
  src/mutator_room.cpp)
add_library(oa::ui::frontend_multiplayer ALIAS oa-ui-frontend-multiplayer)
target_include_directories(oa-ui-frontend-multiplayer PUBLIC include)
target_link_libraries(oa-ui-frontend-multiplayer
  PUBLIC oa-core-types oa-ui-gui-layout oa-ui-gui-input oa-netgame oa-ui-screen-registry oa-options
    oa-data-campaign oa-data-match-rules oa-ui-kit oa-data-content-bundles oa-ui-frontend-state
  PRIVATE oa-platform-shims oa-base-text oa-base-bytes oa-ui-frontend-renderer oa-ui-decoded oa-formats-hpi oa-data-defs-assets oa-formats-tnt oa-data-unit-definitions
    oa-formats-tdf
    oa-data-campaign-assets oa-data-defs oa-data-languages oa-netgame-sync oa-present oa-present-model
    oa-base-game-loop oa-sim-messages oa-sim-mission-units oa-data-mod-profile
    oa-base-sha256 oa-data-map-pack oa-ui-library)

if(BUILD_TESTING)
  add_executable(oa-ui-frontend-multiplayer-lobby-test tests/lobby_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-lobby-test PRIVATE oa-ui-frontend-multiplayer oa-formats-hpi
    oa-formats-gaf oa-data-defs oa-data-languages oa-data-mod-profile oa-test-game-data)
  add_test(NAME ui-multiplayer-lobby COMMAND oa-ui-frontend-multiplayer-lobby-test)
  # The lounge and game list over the GUI files of the install OA_GAME_DIR
  # names, and the screens over its store.
  add_test(NAME ui-multiplayer-lobby-data COMMAND oa-ui-frontend-multiplayer-lobby-test --data)
  set_tests_properties(ui-multiplayer-lobby ui-multiplayer-lobby-data PROPERTIES TIMEOUT 60)
  add_executable(oa-ui-frontend-multiplayer-screens-test tests/screens_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-screens-test PRIVATE oa-ui-frontend-multiplayer oa-formats-hpi
    oa-formats-tnt oa-ui-frontend-renderer oa-present oa-data-unit-definitions oa-data-defs-assets
    oa-data-languages oa-test-game-data)
  add_test(NAME ui-multiplayer-screens COMMAND oa-ui-frontend-multiplayer-screens-test)
  set_tests_properties(ui-multiplayer-screens PROPERTIES TIMEOUT 120)
  # The presence card's words, its hover card and place by the badge, and
  # when it opens and closes, over hand-made battle rooms and a synthetic font.
  add_executable(oa-ui-frontend-multiplayer-presence-card-test tests/presence_card_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-presence-card-test PRIVATE
    oa-ui-frontend-multiplayer oa-ui-kit oa-netgame oa-data-languages oa-test-support)
  add_test(NAME ui-multiplayer-presence-card COMMAND oa-ui-frontend-multiplayer-presence-card-test)
  set_tests_properties(ui-multiplayer-presence-card PROPERTIES TIMEOUT 30)
  # The battle room's mutator gate, the host's list parts and the unit-sync restart.
  add_executable(oa-ui-frontend-multiplayer-room-mutators-test tests/room_mutators_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-room-mutators-test PRIVATE
    oa-ui-frontend-multiplayer oa-netgame oa-data-languages)
  add_test(NAME ui-multiplayer-room-mutators COMMAND oa-ui-frontend-multiplayer-room-mutators-test)
  set_tests_properties(ui-multiplayer-room-mutators PROPERTIES TIMEOUT 30)
  # A room past 3.1c's weapon types or sides: who is refused, who holds
  # START, and the lines written to the log. No game data.
  add_executable(oa-ui-frontend-multiplayer-room-limits-test tests/room_limits_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-room-limits-test PRIVATE
    oa-ui-frontend-multiplayer oa-netgame oa-data-languages)
  add_test(NAME ui-multiplayer-room-limits COMMAND oa-ui-frontend-multiplayer-room-limits-test)
  set_tests_properties(ui-multiplayer-room-limits PROPERTIES TIMEOUT 30)
  # GET MAP: every state and line over hand-made records, packs and offers;
  # with --data, the battle room's side over the installed GUI files: what a
  # joiner reads of the host's map, the host's pack through
  # multiplayer_bind_map_pack, and a pack map installed while the battle
  # room shows.
  add_executable(oa-ui-frontend-multiplayer-get-map-test tests/get_map_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-get-map-test PRIVATE
    oa-ui-frontend-multiplayer oa-netgame oa-ui-frontend-renderer oa-formats-hpi oa-test-game-data)
  add_test(NAME ui-multiplayer-get-map COMMAND oa-ui-frontend-multiplayer-get-map-test)
  add_test(NAME ui-multiplayer-get-map-data COMMAND oa-ui-frontend-multiplayer-get-map-test --data)
  set_tests_properties(ui-multiplayer-get-map ui-multiplayer-get-map-data PROPERTIES TIMEOUT 30)
  # The host's mutator lines and where the block sits above Select Map.
  # No game data.
  add_executable(oa-ui-frontend-multiplayer-mutator-room-test tests/mutator_room_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-mutator-room-test PRIVATE
    oa-ui-frontend-multiplayer oa-data-languages)
  add_test(NAME ui-multiplayer-mutator-room COMMAND oa-ui-frontend-multiplayer-mutator-room-test)
  set_tests_properties(ui-multiplayer-mutator-room PROPERTIES TIMEOUT 30)
  oa_game_data_tests(ui-multiplayer-get-map-data)
  # Whether a guest can fetch one mutator of the host's list: every state
  # over one hand-made mutator, the 16-digit prefix and a release the
  # catalogue lists at another SHA-256.
  add_executable(oa-ui-frontend-multiplayer-host-mutators-test tests/host_mutators_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-host-mutators-test PRIVATE
    oa-ui-frontend-multiplayer oa-netgame)
  add_test(NAME ui-multiplayer-host-mutators COMMAND oa-ui-frontend-multiplayer-host-mutators-test)
  set_tests_properties(ui-multiplayer-host-mutators PROPERTIES TIMEOUT 30)
  add_executable(oa-ui-frontend-multiplayer-team-rules-test tests/team_rules_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-team-rules-test PRIVATE oa-ui-frontend-multiplayer)
  add_test(NAME ui-multiplayer-team-rules COMMAND oa-ui-frontend-multiplayer-team-rules-test)
  add_executable(oa-ui-frontend-multiplayer-lobby-rules-test tests/lobby_rules_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-lobby-rules-test PRIVATE oa-ui-frontend-multiplayer
    oa-formats-hpi oa-test-game-data)
  add_test(NAME ui-multiplayer-lobby-rules COMMAND oa-ui-frontend-multiplayer-lobby-rules-test)
  # The panel cases over the GUI files of the install OA_GAME_DIR names.
  add_test(NAME ui-multiplayer-lobby-rules-data COMMAND oa-ui-frontend-multiplayer-lobby-rules-test --data)
  oa_game_data_tests(ui-multiplayer-lobby-rules-data)
  oa_game_data_tests(ui-multiplayer-lobby-data ui-multiplayer-screens)
  # A map installed while the battle room shows: the list is rebuilt after
  # the screen callback has returned, and the new map is still read.
  add_executable(oa-ui-frontend-multiplayer-map-install-test tests/map_install_test.cpp)
  target_link_libraries(oa-ui-frontend-multiplayer-map-install-test PRIVATE
    oa-ui-frontend-multiplayer oa-formats-hpi oa-test-support)
  add_test(NAME ui-multiplayer-map-install COMMAND oa-ui-frontend-multiplayer-map-install-test)
  set_tests_properties(ui-multiplayer-map-install PROPERTIES TIMEOUT 30)
endif()
