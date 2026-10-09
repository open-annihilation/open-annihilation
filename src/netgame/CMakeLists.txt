# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Network play's transport and wire formats: the condenser and its frames,
# records, bitstream and unit-state codecs, the DirectPlay 4 core protocol
# over TCP/IP, TA's session layer, the player slots and the sockets
# (socket/socket_host.cpp, the one source that calls the system's socket
# functions). The modules below build the match, the multiplayer frontend
# states and the network console commands on it.
add_library(oa-netgame
  network.cpp
  wire/records.cpp
  wire/bitstream.cpp
  wire/unit_state.cpp
  wire/frame.cpp
  wire/condenser.cpp
  wire/content_hash.cpp
  wire/private_channel.cpp
  wire/recorder_session.cpp
  wire/unicode_chat.cpp
  wire/dplay.cpp
  dplay/protocol.cpp
  dplay/engine.cpp
  session/session.cpp
  players/player_slots.cpp)
add_library(oa::netgame ALIAS oa-netgame)
target_link_libraries(oa-netgame PRIVATE oa-base-game-math oa-base-text oa-present)
target_include_directories(oa-netgame PUBLIC include)
target_link_libraries(oa-netgame PUBLIC oa-base-bytes oa-formats-sqsh oa-options oa-core-types
  PRIVATE oa-base-game-loop)
# The sockets, and the system library they need on Windows.
add_subdirectory(socket)
if(WIN32)
  if(OA_WINDOWS_95)
    # Windows 95 ships Winsock 1.1. Winsock 2 arrived with an update to it and
    # with Windows 98, so ws2_32.dll is not on a Windows 95 CD at all.
    target_link_libraries(oa-netgame PUBLIC wsock32)
  else()
    target_link_libraries(oa-netgame PUBLIC ws2_32)
  endif()
endif()

# The frame commands: condenser frames decoded, re-encoded, stored and
# encoded.
add_executable(oa-netgame-tool tools/net_tool.cpp)
target_link_libraries(oa-netgame-tool PRIVATE oa-netgame oa-options)
# A DirectPlay peer run by hand: hosts or joins a session and prints what it
# receives.
add_executable(oa-netgame-dplay-peer tools/dplay_peer.cpp)
target_link_libraries(oa-netgame-dplay-peer PRIVATE oa-netgame)

if(BUILD_TESTING)
  add_executable(oa-netgame-test tests/network_test.cpp)
  target_link_libraries(oa-netgame-test PRIVATE oa-netgame)
  add_test(NAME network COMMAND oa-netgame-test)
  set_tests_properties(network PROPERTIES TIMEOUT 30)

  add_executable(oa-netgame-wire-test tests/net_wire_test.cpp)
  target_link_libraries(oa-netgame-wire-test PRIVATE oa-netgame oa-formats-tad)
  add_test(NAME net-wire COMMAND oa-netgame-wire-test)
  set_tests_properties(net-wire PROPERTIES TIMEOUT 60)

  add_executable(oa-netgame-dplay-test tests/dplay_protocol_test.cpp)
  target_link_libraries(oa-netgame-dplay-test PRIVATE oa-netgame)
  add_test(NAME net-dplay-protocol COMMAND oa-netgame-dplay-test)
  set_tests_properties(net-dplay-protocol PROPERTIES TIMEOUT 30)

  add_executable(oa-netgame-loopback-test tests/dplay_loopback_test.cpp)
  target_link_libraries(oa-netgame-loopback-test PRIVATE oa-base-text oa-netgame)
  add_test(NAME net-dplay-loopback COMMAND oa-netgame-loopback-test)
  set_tests_properties(net-dplay-loopback PROPERTIES TIMEOUT 60)

  add_executable(oa-netgame-socket-search-test tests/socket_search_test.cpp)
  target_link_libraries(oa-netgame-socket-search-test PRIVATE oa-netgame)
  add_test(NAME net-socket-search COMMAND oa-netgame-socket-search-test)
  set_tests_properties(net-socket-search PROPERTIES TIMEOUT 30)

  # Malformed and mutated messages, frames, records and recordings; .tad
  # recordings given as arguments, or directories of them, join the seeds.
  add_executable(oa-netgame-wire-malformed-test tests/wire_malformed_test.cpp)
  target_link_libraries(oa-netgame-wire-malformed-test PRIVATE oa-netgame oa-formats-tad oa-test-support)
  add_test(NAME net-wire-malformed COMMAND oa-netgame-wire-malformed-test)
  set_tests_properties(net-wire-malformed PROPERTIES TIMEOUT 120)

  # The wire forms a mod profile's network rules add.
  add_executable(oa-netgame-wire-rules-test tests/wire_rules_test.cpp)
  target_link_libraries(oa-netgame-wire-rules-test PRIVATE oa-netgame)
  add_test(NAME net-wire-rules COMMAND oa-netgame-wire-rules-test)
  set_tests_properties(net-wire-rules PROPERTIES TIMEOUT 30)

  # Unicode chat's announcement, forms and parts.
  add_executable(oa-netgame-unicode-chat-test tests/unicode_chat_test.cpp)
  target_link_libraries(oa-netgame-unicode-chat-test PRIVATE oa-netgame)
  add_test(NAME net-unicode-chat COMMAND oa-netgame-unicode-chat-test)
  set_tests_properties(net-unicode-chat PROPERTIES TIMEOUT 30)

  # The condenser's datagram envelope, pinned for every platform.
  add_executable(oa-netgame-condenser-golden-test tests/condenser_golden_test.cpp)
  target_link_libraries(oa-netgame-condenser-golden-test PRIVATE oa-netgame)
  add_test(NAME net-condenser-golden COMMAND oa-netgame-condenser-golden-test)
  set_tests_properties(net-condenser-golden PROPERTIES TIMEOUT 30)
endif()

if(WIN32)
  set(oa_netgame_executables oa-netgame-tool oa-netgame-dplay-peer)
  if(BUILD_TESTING)
    list(APPEND oa_netgame_executables oa-netgame-test)
  endif()
  foreach(target IN LISTS oa_netgame_executables)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DDLLS=$<TARGET_RUNTIME_DLLS:${target}>"
        "-DDEST=$<TARGET_FILE_DIR:${target}>" -P "${PROJECT_SOURCE_DIR}/tools/copy_runtime.cmake")
  endforeach()
endif()

add_subdirectory(replication)
add_subdirectory(match)
add_subdirectory(frontend)
add_subdirectory(console)
add_subdirectory(sync)
add_subdirectory(messages)
