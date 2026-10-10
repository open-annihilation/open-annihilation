# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Network play's part of oa-game: its state for each runtime and the work it
# does on it (NetworkPlay, network_play.hpp, defined by the runtime_*.cpp
# sources here), the hooks of its extension table, the launch
# options and 3.1c game switches it takes, the checks that run the game
# networked, and the API an extension built on network play links
# (oa-app-netgame-sdk). The top-level CMakeLists.txt registers oa-app-netgame
# with oa_add_extension.

# The 3.1c command line's network switches, which write the launch block.
add_library(oa-app-netgame-launch STATIC launch_switches.cpp)
add_library(oa::app::netgame_launch ALIAS oa-app-netgame-launch)
target_include_directories(oa-app-netgame-launch PUBLIC include)
target_compile_features(oa-app-netgame-launch PUBLIC cxx_std_20)
target_link_libraries(oa-app-netgame-launch PUBLIC oa-app-command-line oa-ui-frontend-multiplayer
  PRIVATE oa-base-text oa-options)

# The long options and 3.1c network switches, which the option test parses
# without a running app.
add_library(oa-app-netgame-options STATIC net_options.cpp)
add_library(oa::app::netgame_options ALIAS oa-app-netgame-options)
target_include_directories(oa-app-netgame-options PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oa-app-netgame-options PUBLIC oa::app::headers oa-app-netgame-launch oa-netgame-frontend
  oa-app-command-line oa-ui-frontend-renderer oa-ui-frontend-state oa-sim-scenario oa-ui-screen-registry
  oa-core-types
  PRIVATE oa-options)

# The network rules a mod profile gives network play, which their test
# reads without a running app.
add_library(oa-app-netgame-wire-rules STATIC wire_rules_binding.cpp)
add_library(oa::app::netgame_wire_rules ALIAS oa-app-netgame-wire-rules)
target_include_directories(oa-app-netgame-wire-rules PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oa-app-netgame-wire-rules PUBLIC oa-data-mod-profile oa-netgame)

# The traffic readouts drawn over the battlefield through the engine's match
# overlay, which their test draws without a running app.
add_library(oa-app-netgame-traffic-overlay STATIC traffic_overlay.cpp)
add_library(oa::app::netgame_traffic_overlay ALIAS oa-app-netgame-traffic-overlay)
target_include_directories(oa-app-netgame-traffic-overlay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oa-app-netgame-traffic-overlay PUBLIC oa::app::headers oa-netgame oa-core-types
  PRIVATE oa-netgame-match oa-present-world-renderer oa-ui-console)

# The handler a request to end the program runs, as the game keeps it
# installed through its application modes and matches, which its test runs
# without a running app.
add_library(oa-app-netgame-close-handlers STATIC close_handlers.cpp)
add_library(oa::app::netgame_close_handlers ALIAS oa-app-netgame-close-handlers)
target_include_directories(oa-app-netgame-close-handlers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oa-app-netgame-close-handlers PUBLIC oa-core-types
  PRIVATE oa-ui-frontend-state oa-ui-frontend-multiplayer)

# The launch block as the running game binds it (where a launch lands,
# whether one is active, the multiplayer screens' link, the extension API's
# hooks) and the lines posted to a launched battle, which their test runs
# without a running app. It implements network play's API for the
# extensions built on it (oa/app/netgame/extension_api.hpp).
add_library(oa-app-netgame-launch-binding STATIC launch_binding.cpp battle_lines.cpp)
add_library(oa::app::netgame_launch_binding ALIAS oa-app-netgame-launch-binding)
target_include_directories(oa-app-netgame-launch-binding PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" include)
target_link_libraries(oa-app-netgame-launch-binding
  PUBLIC oa-app-netgame-close-handlers oa-app-netgame-launch oa-ui-frontend-multiplayer oa-core-types)

# The sources that define NetworkPlay, Runtime's friend, and the hooks that reach it.
set(oa_app_netgame_sources
  runtime_console_network.cpp
  runtime_net.cpp
  runtime_demo.cpp
  extension.cpp
  runtime_netgame.cpp
  runtime_report.cpp
  runtime_session_cheats.cpp)

# The recorder's report line names the engine's version.
set_property(SOURCE runtime_net.cpp APPEND
  PROPERTY COMPILE_DEFINITIONS "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")

if(TARGET oa-game)
  # multiplayer_menu_check.cpp, a self-check, drives the game through the
  # engine's check host and reaches no Runtime member.
  if(OA_SELF_CHECKS)
    list(APPEND oa_app_netgame_sources multiplayer_menu_check.cpp)
  endif()
  add_library(oa-app-netgame STATIC ${oa_app_netgame_sources})
  add_library(oa::app::netgame ALIAS oa-app-netgame)
  target_include_directories(oa-app-netgame PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  # The engine's extension SDK: its hooks, and the Runtime NetworkPlay
  # reaches, seen as oa-game's own sources see it (the engine applies an
  # extension's OA_RUNTIME_EXTENSION_MEMBERS to both).
  target_link_libraries(oa-app-netgame
    PUBLIC oa-app-netgame-options oa-app-netgame-traffic-overlay oa-app-netgame-close-handlers
      oa-app-netgame-wire-rules
      oa-app-netgame-launch-binding oa-ui-frontend-multiplayer oa-netgame-match-runtime oa-session-demo
      oa-netgame-console oa-netgame-messages
    PRIVATE oa::extension-sdk oa-app-view-rules oa-base-text oa-data-languages oa-present)
  # Network play's API for the extensions built on it: an extension library
  # that links it links network play's extension too, so the engine lists
  # that extension after network play's.
  add_library(oa-app-netgame-sdk INTERFACE)
  add_library(oa::app::netgame_sdk ALIAS oa-app-netgame-sdk)
  target_link_libraries(oa-app-netgame-sdk INTERFACE oa-app-netgame-launch-binding oa-app-netgame)
endif()

if(BUILD_TESTING)
  add_executable(oa-netgame-launch-switches-test tests/launch_switches_test.cpp)
  target_link_libraries(oa-netgame-launch-switches-test PRIVATE oa-app-netgame-launch oa-options)
  add_test(NAME net-launch-switches COMMAND oa-netgame-launch-switches-test)
  add_executable(oa-netgame-options-test tests/options_test.cpp)
  target_link_libraries(oa-netgame-options-test PRIVATE oa-app-netgame-options oa-app-game-options oa-options)
  add_test(NAME netgame-game-options COMMAND oa-netgame-options-test)
  add_executable(oa-netgame-close-handlers-test tests/close_handlers_test.cpp)
  target_link_libraries(oa-netgame-close-handlers-test PRIVATE oa-app-netgame-launch-binding oa-ui-frontend-state)
  add_test(NAME netgame-close-handlers COMMAND oa-netgame-close-handlers-test)
  add_executable(oa-netgame-launch-binding-test tests/launch_binding_test.cpp)
  target_link_libraries(oa-netgame-launch-binding-test PRIVATE oa-app-netgame-launch-binding
    oa-netgame-test-launch)
  add_test(NAME netgame-launch-binding COMMAND oa-netgame-launch-binding-test)
  set_tests_properties(netgame-close-handlers netgame-launch-binding PROPERTIES TIMEOUT 60)
  # The network rules a profile gives; the reference profiles in the folder
  # OA_MOD_PROFILES_DIR names are read when it is set.
  add_executable(oa-netgame-wire-rules-binding-test tests/wire_rules_test.cpp)
  target_link_libraries(oa-netgame-wire-rules-binding-test PRIVATE oa-app-netgame-wire-rules oa-platform-shims)
  add_test(NAME netgame-wire-rules COMMAND oa-netgame-wire-rules-binding-test)
  if(DEFINED OA_MOD_PROFILES_DIR)
    set_tests_properties(netgame-wire-rules PROPERTIES
      ENVIRONMENT "OA_MOD_PROFILES_DIR=${OA_MOD_PROFILES_DIR}")
  endif()
  add_executable(oa-netgame-traffic-overlay-test tests/traffic_overlay_test.cpp)
  target_link_libraries(oa-netgame-traffic-overlay-test PRIVATE oa-app-netgame-traffic-overlay oa-netgame-match
    oa-ui-console)
  add_test(NAME netgame-traffic-overlay COMMAND oa-netgame-traffic-overlay-test)
  # The private Runtime names these sources use stay within
  # tools/runtime-surface-baseline.json, which only shrinks as they move
  # behind hooks and declared headers.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME netgame-runtime-surface COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_runtime_surface.py"
      ${oa_app_netgame_sources})
    set_tests_properties(netgame-runtime-surface PROPERTIES TIMEOUT 60
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()

# The checks that run the game networked, over the installation OA_GAME_DIR
# names.
set(oa_netgame_checks "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
if(BUILD_TESTING AND TARGET oa-game AND IS_DIRECTORY "${OA_GAME_DIR}")
  file(MAKE_DIRECTORY "${oa_netgame_checks}")
  # The host's player gives every unit it has; the installation's
  # commanders are in the Commander category, so its commander stays.
  add_test(NAME native-net-loopback COMMAND oa-game --game-dir "${OA_GAME_DIR}"
    --skip-intro --mute --headless-check --net-loopback-check 300
    --preferences-file "${oa_netgame_checks}/net-loopback.conf")
  set_tests_properties(native-net-loopback PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    FAIL_REGULAR_EXPRESSION "its commander went too")
  add_test(NAME native-net-loopback-watcher COMMAND oa-game --game-dir "${OA_GAME_DIR}"
    --skip-intro --mute --headless-check --net-loopback-check 300 --net-loopback-watcher
    --preferences-file "${oa_netgame_checks}/net-loopback-watcher.conf")
  set_tests_properties(native-net-loopback-watcher PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
  # With Simplified Chinese shown, whose pack turns Unicode chat on, each
  # machine's setup block says it reads chat in UTF-8.
  add_test(NAME native-net-loopback-unicode-chat COMMAND oa-game --game-dir "${OA_GAME_DIR}"
    --skip-intro --mute --headless-check --net-loopback-check 300
    --preferences-file "${oa_netgame_checks}/net-loopback-unicode-chat.conf" Chinese)
  set_tests_properties(native-net-loopback-unicode-chat PROPERTIES
    TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    PASS_REGULAR_EXPRESSION "Unicode chat on, setup blocks saying UTF-8 [1-9][0-9]* / [1-9]"
    FAIL_REGULAR_EXPRESSION "its commander went too|check failed|FAILED")
  # Under a profile that presents the recorder and turns ui.whiteboard on
  # over the installed game, the host's whiteboard marks reach the joiner
  # while the host allies it, with the marker echoed to its log, and not
  # once the alliance is broken.
  file(WRITE "${oa_netgame_checks}/whiteboard.oamod.yaml"
    "oamod: 1\n"
    "id: loopback-whiteboard\n"
    "name: Loopback whiteboard\n"
    "version: \"1.0\"\n"
    "requires: {base: ta-3.1c, catalogue: 1}\n"
    "author: {name: unknown}\n"
    "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n"
    "hacks:\n"
    "  recorder.ta-demo-recorder: true\n"
    "  ui.whiteboard: true\n")
  add_test(NAME native-net-loopback-whiteboard COMMAND oa-game --game-dir "${OA_GAME_DIR}"
    --skip-intro --mute --headless-check --net-loopback-check 300
    --mod "${oa_netgame_checks}/whiteboard.oamod.yaml"
    --preferences-file "${oa_netgame_checks}/net-loopback-whiteboard.conf")
  set_tests_properties(native-net-loopback-whiteboard PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
  # The host seats a computer player, which builds for 6000 ticks, is given
  # a squad and strikes the joiner's commander; with the watcher, the
  # joiner watches it strike the host's.
  add_test(NAME native-net-loopback-computer COMMAND oa-game --game-dir "${OA_GAME_DIR}"
    --skip-intro --mute --headless-check --net-loopback-check 6000 --net-loopback-computer
    --preferences-file "${oa_netgame_checks}/net-loopback-computer.conf")
  set_tests_properties(native-net-loopback-computer PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
  add_test(NAME native-net-loopback-computer-watcher COMMAND oa-game --game-dir "${OA_GAME_DIR}"
    --skip-intro --mute --headless-check --net-loopback-check 6000 --net-loopback-computer
    --net-loopback-watcher
    --preferences-file "${oa_netgame_checks}/net-loopback-computer-watcher.conf")
  set_tests_properties(native-net-loopback-computer-watcher PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
  # The engine's recording hook (open_recording) declines bytes that are no
  # recording and refuses a recording's truncated header; with --play-demo
  # the check would also replay that recording through the replay hooks.
  add_test(NAME native-recording-hook COMMAND oa-game --game-dir "${OA_GAME_DIR}"
    --skip-intro --mute --check-recording-hook
    --preferences-file "${oa_netgame_checks}/recording-hook.conf")
  set_tests_properties(native-recording-hook PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
  # A joiner whose host is not found, through the main loop: "Host not
  # found.  Exiting..." shows, and 4 s later the game leaves and the run ends
  # with no reason and exit status 0.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME native-host-not-found COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_host_not_found.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_netgame_checks}")
    set_tests_properties(native-host-not-found PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # Games started with --host and --join, each a program of its own over
    # 127.0.0.1: the host's battle room, a join by address with both names
    # listed on both machines, a wrong password refused and the right one
    # joining the game named.
    add_test(NAME native-net-options COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_net_options.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_netgame_checks}")
    set_tests_properties(native-net-options PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
  endif()
endif()
