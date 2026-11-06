# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The MCP server's part of oa-game (README.md): the protocol, the --mcp
# and --mcp-http options, and the extension oa-app-mcp, which the top-level
# CMakeLists.txt registers with oa_add_extension after the automation endpoint.

# The protocol, with no game and no stream: a session answers one message
# at a time, and a line buffer frames a byte stream. oa-formats-json is
# public so a test that parses an answer still finds it.
add_library(oa-app-mcp-protocol STATIC session.cpp schema.cpp)
add_library(oa::app::mcp_protocol ALIAS oa-app-mcp-protocol)
target_include_directories(oa-app-mcp-protocol PUBLIC include
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(oa-app-mcp-protocol PUBLIC cxx_std_20)
target_link_libraries(oa-app-mcp-protocol PUBLIC oa-formats-json PRIVATE oa-options)

# Streamable HTTP on loopback. The listener is opened only by HttpServer::open.
# The token is the automation endpoint's, compared the same way.
add_library(oa-app-mcp-http STATIC http.cpp)
add_library(oa::app::mcp_http ALIAS oa-app-mcp-http)
target_include_directories(oa-app-mcp-http PUBLIC include)
target_compile_features(oa-app-mcp-http PUBLIC cxx_std_20)
target_link_libraries(oa-app-mcp-http PUBLIC oa-app-mcp-protocol oa-netgame
  PRIVATE oa-app-automation-options oa-platform-shims)

# --mcp, --mcp-http and --mcp-clock, checked without a running game. Network play's
# options are read only to refuse a launch on standard input and
# --list-content; the automation endpoint's options, to refuse
# --status-stdout. Nothing here opens a socket.
add_library(oa-app-mcp-options STATIC options.cpp)
add_library(oa::app::mcp_options ALIAS oa-app-mcp-options)
target_include_directories(oa-app-mcp-options PUBLIC include)
target_compile_features(oa-app-mcp-options PUBLIC cxx_std_20)
# oa/app/app.hpp, which names the run's options, needs the headers of the
# libraries it uses.
target_link_libraries(oa-app-mcp-options PUBLIC oa-app-headers oa-app-command-line
  oa-ui-frontend-renderer oa-ui-frontend-state oa-sim-scenario oa-ui-screen-registry
  PRIVATE oa-app-mcp-http oa-app-automation-options oa-app-netgame-options oa-options)

# game.speed, game.pause, the camera tools, the clock tools, orders and
# selection, and the queue that applies them at after_pump. No runtime: the
# guard test links this and supplies its own host, so it never links
# play_host or the game.
add_library(oa-app-mcp-play STATIC actions.cpp orders.cpp)
add_library(oa::app::mcp_play ALIAS oa-app-mcp-play)
target_include_directories(oa-app-mcp-play PUBLIC include)
target_compile_features(oa-app-mcp-play PUBLIC cxx_std_20)
target_link_libraries(oa-app-mcp-play PUBLIC oa-app-mcp-protocol oa-app-headers)

if(TARGET oa-game)
  # The extension: standard input and output, loopback HTTP, and the tools.
  # It reads the running match through the automation host and the match
  # view, and drives the screen through the shared automation core.
  add_library(oa-app-mcp STATIC extension.cpp stdio.cpp tools.cpp ui.cpp units.cpp map.cpp
    event_feed.cpp event_screen.cpp)
  add_library(oa::app::mcp ALIAS oa-app-mcp)
  target_include_directories(oa-app-mcp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_features(oa-app-mcp PRIVATE cxx_std_20)
  target_link_libraries(oa-app-mcp PRIVATE oa-app-mcp-protocol oa-app-mcp-play
    oa-app-mcp-options oa-app-mcp-http oa-app-automation-reports oa-app-automation-core
    oa-app-headers oa-formats-png oa-platform-shims oa-core-types SDL3::SDL3 oa-options)
  # initialize reports the engine's version.
  set_property(SOURCE extension.cpp APPEND
    PROPERTY COMPILE_DEFINITIONS "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
endif()

if(BUILD_TESTING)
  add_executable(oa-app-mcp-protocol-test tests/protocol_test.cpp)
  target_link_libraries(oa-app-mcp-protocol-test PRIVATE oa-app-mcp-protocol oa-test-support
    oa-options)
  add_test(NAME mcp-protocol COMMAND oa-app-mcp-protocol-test)

  # The unit tools with a stand-in match view, so the test does not link the game.
  # The match view's header is the app headers' include; linking that interface
  # reaches it without naming another module's directory.
  add_executable(oa-app-mcp-units-test tests/units_test.cpp units.cpp)
  target_include_directories(oa-app-mcp-units-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-app-mcp-units-test PRIVATE oa-app-mcp-protocol oa-test-support
    oa-app-headers)
  add_test(NAME mcp-units COMMAND oa-app-mcp-units-test)

  # The map tools with a stand-in match view, so the test does not link the game.
  add_executable(oa-app-mcp-map-test tests/map_test.cpp map.cpp)
  target_include_directories(oa-app-mcp-map-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-app-mcp-map-test PRIVATE oa-app-mcp-protocol oa-test-support
    oa-app-headers oa-formats-png)
  add_test(NAME mcp-map COMMAND oa-app-mcp-map-test)

  add_executable(oa-app-mcp-options-test tests/options_test.cpp)
  target_link_libraries(oa-app-mcp-options-test PRIVATE oa-app-mcp-options oa-app-mcp-http
    oa-app-automation-options oa-app-netgame-options oa-app-game-options oa-test-support
    oa-options)
  add_test(NAME mcp-options COMMAND oa-app-mcp-options-test)

  add_executable(oa-app-mcp-http-test tests/http_test.cpp)
  target_link_libraries(oa-app-mcp-http-test PRIVATE oa-app-mcp-http oa-test-support oa-options)
  add_test(NAME mcp-http COMMAND oa-app-mcp-http-test)

  # A replay refuses game.speed, game.pause, an order and the clock, and may
  # still move the view. The queue applies a real match's change at the poll
  # after after_pump, and the answer names the tick.
  add_executable(oa-app-mcp-play-test tests/play_test.cpp)
  target_link_libraries(oa-app-mcp-play-test PRIVATE oa-app-mcp-play oa-test-support oa-options)
  add_test(NAME mcp-play COMMAND oa-app-mcp-play-test)
endif()

# game.info and game.status, with a stand-in match. The test compiles the
# tools and supplies the hosts, so it does not link the game.
if(BUILD_TESTING AND TARGET oa-app-automation-core)
  add_executable(oa-app-mcp-status-test tests/status_test.cpp tools.cpp)
  target_include_directories(oa-app-mcp-status-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-app-mcp-status-test PRIVATE oa-app-mcp-play oa-app-mcp-protocol
    oa-app-automation-core oa-ui-frontend-multiplayer oa-sim-scenario oa-test-support
    oa-app-headers oa-options)
  add_test(NAME mcp-status COMMAND oa-app-mcp-status-test)
endif()

# The event tools, with notes handed in and no running game. The feed's header
# names the automation watch's record, so the test is built with that library.
# It does not link the screen watch itself.
if(BUILD_TESTING AND TARGET oa-app-automation-core)
  add_executable(oa-app-mcp-events-test tests/events_test.cpp event_feed.cpp)
  target_include_directories(oa-app-mcp-events-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-app-mcp-events-test PRIVATE oa-app-mcp-protocol oa-app-automation-core
    oa-test-support oa-app-headers)
  add_test(NAME mcp-events COMMAND oa-app-mcp-events-test)

  # A masked field's text stays out of ui.tree, ui.screen, ui.wait and the
  # screen a click answers with. The tree reports the field's length.
  add_executable(oa-app-mcp-password-test tests/password_test.cpp)
  target_link_libraries(oa-app-mcp-password-test PRIVATE oa-app-automation-core
    oa-netgame oa-test-support oa-app-headers)
  add_test(NAME mcp-password COMMAND oa-app-mcp-password-test)
endif()

# The server in a running game, over the installation OA_GAME_DIR names.
if(BUILD_TESTING AND TARGET oa-game AND IS_DIRECTORY "${OA_GAME_DIR}")
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(oa_mcp_checks "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
    file(MAKE_DIRECTORY "${oa_mcp_checks}")
    # --mcp on the dummy drivers: initialize, the tool list, game.status and
    # game.quit, and the process then exits 0.
    add_test(NAME native-mcp-stdio COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_stdio.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-stdio PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # --mcp-http on the dummy drivers: initialize, game.status and game.quit,
    # and the process then exits 0. No --mcp, so standard input stays the game's.
    add_test(NAME native-mcp-http COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_http.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-http PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish on the dummy drivers, then SIGTERM. The process leaves
    # within 10 seconds, without waiting on the exit question. --mcp is the
    # standard streams, not --mcp-http, so the leave is the dummy driver.
    # SIGTERM is the system's quit on Unix.
    if(UNIX)
      add_test(NAME native-mcp-sigterm COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_sigterm.py"
        --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
        --scratch-root "${oa_mcp_checks}")
      set_tests_properties(native-mcp-sigterm PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    endif()
    # The main menu, single player, the skirmish setup, the Library, Settings
    # and the main menu strip, each driven by a control's name.
    add_test(NAME native-mcp-ui COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_ui.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-ui PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish, reached through --fark, then game.speed and game.pause on
    # --mcp. game.status reports the speed and the pause.
    add_test(NAME native-mcp-play COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_play.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-play PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish, then camera.set, camera.center and camera.zoom. camera.get
    # reports the place the view's limits held, and a screenshot follows it.
    add_test(NAME native-mcp-camera COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_camera.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-camera PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # The same skirmish twice, with --mcp-clock stepped. Each run sets the
    # speed and steps the same ticks. The two saved-state hashes must match.
    add_test(NAME native-mcp-clock COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_clock.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-clock PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish with the clock held. The local commander is selected as soon
    # as it is listed, before any step, and the tick is still 0.
    add_test(NAME native-mcp-commander COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_commander.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-commander PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish on the dummy drivers: units.list, a factory queue after
    # --give-orders, units.get, units.orders and units.types, then game.quit.
    add_test(NAME native-mcp-units COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_units.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-units PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish on the dummy drivers, without --give-orders: select, a
    # standing order, a solar collector, a move, an attack, and a refusal
    # for a unit this machine does not play.
    add_test(NAME native-mcp-orders COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_orders.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-orders PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish on the dummy drivers, without --give-orders: subscribe, move
    # the commander, and read a unit_idle and an order_done for that move.
    add_test(NAME native-mcp-events COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_events.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-events PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish on the dummy drivers: map.info, the sight and explored grids
    # against the minimap's fog, map.radar with no identity, then game.quit.
    add_test(NAME native-mcp-map COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_map.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-map PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish on the dummy drivers with --mcp-clock stepped: game.info and
    # game.status, the unit limit, the local economy, a clock that advances,
    # and a computer player's economy left unread.
    add_test(NAME native-mcp-status COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_status.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_mcp_checks}")
    set_tests_properties(native-mcp-status PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
  endif()
endif()
