# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The automation endpoint's part of oa-game (README.md): the automation
# protocol's codec, the endpoint's options and token, and its extension,
# oa-app-automation, which the top-level CMakeLists.txt registers with
# oa_add_extension in every build.

# The automation protocol's frames. Their JSON is oa-formats-json, linked
# publicly so every target that reached it through this library still does.
add_library(oa-app-automation-protocol STATIC protocol.cpp)
add_library(oa::app::automation_protocol ALIAS oa-app-automation-protocol)
target_include_directories(oa-app-automation-protocol PUBLIC include)
target_compile_features(oa-app-automation-protocol PUBLIC cxx_std_20)
target_link_libraries(oa-app-automation-protocol PUBLIC oa-formats-json
  PRIVATE ZLIB::ZLIB oa-options)

# The endpoint's options and their checks, and its token, which their
# check runs without a running game. A launch's status block turns the status
# stream on: network play reads the launch and keeps the block, and its
# options do not link back.
add_library(oa-app-automation-options STATIC options.cpp token.cpp)
add_library(oa::app::automation_options ALIAS oa-app-automation-options)
target_include_directories(oa-app-automation-options PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# oa/app/app.hpp, which names the run's options, needs the headers of the
# libraries it uses.
target_link_libraries(oa-app-automation-options PUBLIC oa-app-headers oa-app-command-line
  oa-ui-frontend-renderer oa-ui-frontend-state oa-sim-scenario oa-ui-screen-registry
  PRIVATE oa-options oa-platform-random oa-app-netgame-options)

# The device events of an input request, read from its JSON, which their
# check reads without a running game.
add_library(oa-app-automation-input STATIC input_events.cpp)
add_library(oa::app::automation_input ALIAS oa-app-automation-input)
target_include_directories(oa-app-automation-input PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oa-app-automation-input PUBLIC oa-app-automation-protocol PRIVATE oa-options)

if(TARGET oa-game)
  # A window the automation check drives: one button and the idle clock on
  # the main menu, listed by the endpoint when --fixture-window is given. It
  # is a test extension, so only a build configured with
  # -DOA_RECORD_EXTENSION_HOOKS=ON, which combines the test extensions of
  # tests/extension, combines it; the game players get combines none.
  if(OA_RECORD_EXTENSION_HOOKS)
    add_library(oa-app-automation-window-fixture STATIC window_fixture.cpp)
    add_library(oa::app::automation_window_fixture ALIAS oa-app-automation-window-fixture)
    target_compile_features(oa-app-automation-window-fixture PRIVATE cxx_std_20)
    # oa/app/app.hpp, which names the main menu, needs the headers of the
    # libraries it uses.
    target_link_libraries(oa-app-automation-window-fixture PRIVATE oa-app-headers
      oa-app-command-line oa-ui-frontend-renderer oa-ui-frontend-state oa-sim-scenario
      oa-ui-screen-registry oa-options)
    oa_add_extension(oa-app-automation-window-fixture INIT oa_extension_init_window_fixture)
  endif()
  # What the endpoint reports of a match and of the battle room, and the
  # events it learns by comparing what the game holds from frame to frame,
  # read from the canonical records and the multiplayer screens' lobby
  # alone, so that their check runs without a running game.
  add_library(oa-app-automation-reports STATIC reports.cpp events.cpp)
  add_library(oa::app::automation_reports ALIAS oa-app-automation-reports)
  target_include_directories(oa-app-automation-reports PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-app-automation-reports
    PUBLIC oa-app-automation-protocol oa-core-types
    PRIVATE oa-ui-frontend-multiplayer oa-sim-messages oa-sim-scenario oa-options)
  # Status lines: the same events, on standard output or in a file, with no
  # socket. The check links this library and not the endpoint.
  add_library(oa-app-automation-status STATIC status.cpp)
  add_library(oa::app::automation_status ALIAS oa-app-automation-status)
  target_include_directories(oa-app-automation-status PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-app-automation-status
    PUBLIC oa-app-automation-reports
    PRIVATE oa-platform-shims oa-app-automation-options)
  # The endpoint and the extension's hooks. It reads the game through the
  # check host and the automation host (check_host.hpp, automation_host.hpp)
  # and reaches no Runtime member.
  add_library(oa-app-automation STATIC extension.cpp endpoint.cpp requests.cpp controls.cpp
    input.cpp frames.cpp serve_reports.cpp)
  add_library(oa::app::automation ALIAS oa-app-automation)
  target_link_libraries(oa-app-automation PRIVATE oa-app-automation-protocol
    oa-app-automation-options oa-app-automation-input oa-app-automation-reports
    oa-app-automation-status oa-app-headers
    oa-app-game-directory oa-netgame oa-ui-frontend-multiplayer oa-ui-frontend-renderer
    oa-core-types SDL3::SDL3 oa-options oa-platform-shims)
  # The frame request: PNG files of the frames the game presents.
  target_link_libraries(oa-app-automation PRIVATE oa-formats-png)
  # hello names the engine's version.
  set_property(SOURCE requests.cpp APPEND
    PROPERTY COMPILE_DEFINITIONS "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")
endif()

if(BUILD_TESTING)
  # The codec over the test vectors kept beside it (tests/vectors, which
  # make_vectors.py there writes), and over malformed input.
  add_executable(oa-app-automation-protocol-test tests/protocol_test.cpp)
  target_link_libraries(oa-app-automation-protocol-test PRIVATE oa-app-automation-protocol
    oa-test-support oa-options)
  add_test(NAME automation-protocol COMMAND oa-app-automation-protocol-test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
  # The endpoint's options through oa-game's command line, the engine
  # options a run with the endpoint refuses, and its token.
  add_executable(oa-app-automation-options-test tests/options_test.cpp)
  target_link_libraries(oa-app-automation-options-test PRIVATE oa-app-automation-options
    oa-app-netgame-options oa-app-game-options oa-test-support oa-options)
  add_test(NAME automation-options COMMAND oa-app-automation-options-test)
  # The device events of input requests, and those that cannot be read.
  add_executable(oa-app-automation-input-test tests/input_test.cpp)
  target_link_libraries(oa-app-automation-input-test PRIVATE oa-app-automation-input
    oa-test-support oa-options)
  add_test(NAME automation-input COMMAND oa-app-automation-input-test)
  if(TARGET oa-app-automation-reports)
    # The match and room answers and the events, over records and a lobby
    # the check builds: what each writes, and that none changes the game.
    add_executable(oa-app-automation-reports-test tests/reports_test.cpp)
    target_link_libraries(oa-app-automation-reports-test PRIVATE oa-app-automation-reports
      oa-app-automation-protocol oa-ui-frontend-multiplayer oa-sim-scenario oa-test-support
      oa-options)
    add_test(NAME automation-reports COMMAND oa-app-automation-reports-test)
  endif()
  if(TARGET oa-app-automation-status)
    # Status lines: hello, one event, one exit, and how a match's seats ended.
    add_executable(oa-app-automation-status-test tests/status_test.cpp)
    target_link_libraries(oa-app-automation-status-test PRIVATE oa-app-automation-status
      oa-app-automation-options oa-platform-shims oa-test-support oa-options)
    add_test(NAME automation-status COMMAND oa-app-automation-status-test)
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    # The vectors are the ones make_vectors.py writes.
    add_test(NAME automation-protocol-vectors COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/make_vectors.py" --check)
    set_tests_properties(automation-protocol-vectors PROPERTIES TIMEOUT 60)
  endif()
endif()

# The endpoint in a running game, over the installation OA_GAME_DIR names.
if(BUILD_TESTING AND TARGET oa-game AND IS_DIRECTORY "${OA_GAME_DIR}")
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    set(oa_automation_checks "${CMAKE_CURRENT_BINARY_DIR}/native-checks")
    file(MAKE_DIRECTORY "${oa_automation_checks}")
    # The game started with --fark on the dummy drivers, driven through the
    # endpoint on the main menu: its hello, screen, preferences and quit, a
    # wrong token, a second client and a connection that speaks no frames;
    # then Load Game's message box over its dialog.
    add_test(NAME native-automation-menus COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_automation.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}")
    set_tests_properties(native-automation-menus PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # Settings driven by its controls' names, in a 640x480 window and in a
    # 1920x1080 one: the OA button opens it, a section, a switch's half and
    # a drop-down's item are clicked at their listed rectangles, and OK and
    # Cancel keep and drop what was chosen.
    add_test(NAME native-automation-settings COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_automation_settings.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}")
    set_tests_properties(native-automation-settings PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # Each control of Settings, the Library and the key editor pressed at the
    # centre of where it is drawn, at every Interface size and Window size
    # Settings offers, in the classic look and the modern one: it takes the
    # press there, a drop-down opens and a switch's half is chosen.
    add_test(NAME native-automation-hit-areas COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_automation_hit_areas.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}")
    set_tests_properties(native-automation-hit-areas PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # Input through the endpoint: a typed address reaching the TCP/IP
    # dialog's field, an arrow key the endpoint holds scrolling a
    # skirmish's camera, as the keyboard's does, and the match saved under a
    # typed name that Load Game then lists.
    # A build with the address sanitizer keeps the memory it frees, so the
    # game's memory is not judged there while input waits.
    set(oa_automation_input_sanitized)
    if(OA_SANITIZERS)
      set(oa_automation_input_sanitized --sanitized)
    endif()
    add_test(NAME native-automation-input COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_automation_input.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}" ${oa_automation_input_sanitized})
    set_tests_properties(native-automation-input PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # The frames the game presents on the main menu, in a window the canvas
    # fills and in one it is drawn larger in: whole, as PNG files, as
    # regions and as hashes, against the frame the game composed.
    add_test(NAME native-automation-frame COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_automation_frame.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}")
    set_tests_properties(native-automation-frame PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A recorded network game played back in the main loop with --fark and a
    # client reading the match, its digest, the room and every event at
    # every frame plays the same ticks as without the endpoint.
    add_test(NAME native-automation-digest COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_automation_digest.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}")
    set_tests_properties(native-automation-digest PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # The MCP server (tools/mcp), started as an MCP client starts it, starts
    # the game on the dummy drivers and drives it through the endpoint: from
    # the main menu to Single Player and back, with a picture and the screen
    # events, and quits it.
    add_test(NAME native-mcp-server COMMAND ${Python3_EXECUTABLE}
      "${PROJECT_SOURCE_DIR}/tools/check_native_mcp_server.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}")
    set_tests_properties(native-mcp-server PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    # A skirmish with --status-stdout --quit-after-game: hello, a screen, the
    # match starting, game_over with each player's result, and exit, and
    # nothing else on standard output. The process exits 0.
    add_test(NAME native-status-stdout COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_status.py"
      --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
      --scratch-root "${oa_automation_checks}")
    set_tests_properties(native-status-stdout PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    # An extension's window: the endpoint lists its controls, a click on its
    # button reaches it, and its clock reads the fixed clock's step. Only a
    # build that combines the window fixture runs it.
    if(OA_RECORD_EXTENSION_HOOKS)
      add_test(NAME native-automation-extension-windows COMMAND ${Python3_EXECUTABLE}
        "${PROJECT_SOURCE_DIR}/tools/check_native_automation_windows.py"
        --native $<TARGET_FILE:oa-game> --game-dir "${OA_GAME_DIR}"
        --scratch-root "${oa_automation_checks}")
      set_tests_properties(native-automation-extension-windows PROPERTIES
        TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT})
    endif()
  endif()
endif()
