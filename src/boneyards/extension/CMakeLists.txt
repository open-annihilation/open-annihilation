# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The launch, the options and the launch check. The extension library itself
# is created from cmake/OaBoneyards.cmake once every library it links exists,
# and registered there. These three are what the tests below link. The launch
# and options libraries' tests run without a running app.
set(OA_BONEYARDS_EXTENSION_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" PARENT_SCOPE)
if(TARGET oa-boneyards-console-app)
  add_library(oa-boneyards-extension-launch STATIC
    boneyards_data.cpp boneyards_launch.cpp boneyards_report.cpp boneyards_exit_prompt.cpp
    boneyards_tunnel_route.cpp)
  add_library(oa::boneyards::extension_launch ALIAS oa-boneyards-extension-launch)
  target_include_directories(oa-boneyards-extension-launch PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-boneyards-extension-launch
    PUBLIC oa-app-netgame-launch-binding oa-boneyards-console oa-boneyards-console-app oa-boneyards-client
      oa-boneyards-tunnel
      oa-netgame-frontend oa-boneyards-reporter oa-ui-frontend-multiplayer oa-ui-frontend-state
      oa-ui-screen-registry oa-core-types oa-data-mod-profile
    PRIVATE oa-data-replays oa-options)
  add_library(oa-boneyards-extension-options STATIC boneyards_options.cpp)
  add_library(oa::boneyards::extension_options ALIAS oa-boneyards-extension-options)
  target_include_directories(oa-boneyards-extension-options PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-boneyards-extension-options
    PUBLIC oa::app::headers oa-app-command-line oa-ui-frontend-renderer oa-ui-frontend-state oa-sim-scenario
      oa-ui-screen-registry oa-core-types
    PRIVATE oa-app-netgame-launch-binding oa-options)
endif()
if(OA_SELF_CHECKS AND TARGET oa-game AND TARGET oa-boneyards-extension-launch)
  # --check-boneyards-launch, which plays a battle the Boneyards launches
  # from an overlay of the running game. A release build leaves it out, with
  # the engine's own self-checks.
  add_library(oa-boneyards-extension-launch-check STATIC boneyards_launch_check.cpp)
  add_library(oa::boneyards::extension_launch_check ALIAS oa-boneyards-extension-launch-check)
  target_include_directories(oa-boneyards-extension-launch-check PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-boneyards-extension-launch-check
    PUBLIC oa-boneyards-extension-launch oa-ui-screen-registry
    PRIVATE oa-boneyards-console-private oa::app::headers oa-ui-frontend-renderer oa-ui-frontend-state
      oa-sim-scenario oa-boneyards-client oa-ui-frontend-multiplayer oa-sim-match-runtime SDL3::SDL3
      oa-options oa-platform-shims)
endif()

if(BUILD_TESTING)
  # The expansion folder is found by the engine's loose-file lookup. The
  # fixture spells the folder and the record in another case.
  add_executable(oa-boneyards-extension-expansion-folder-test
    tests/expansion_folder_test.cpp expansion_folder.cpp)
  target_include_directories(oa-boneyards-extension-expansion-folder-test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-boneyards-extension-expansion-folder-test
    PRIVATE oa-app-game-directory oa-data-content-bundles oa-formats-hpi
      oa-platform-preferences oa-test-support oa-options)
  add_test(NAME boneyards-expansion-folder
    COMMAND oa-boneyards-extension-expansion-folder-test)
  set_tests_properties(boneyards-expansion-folder PROPERTIES TIMEOUT 60)
  # The entry's decisions before the package: the online features, the
  # update question, Try again, the carried request and the quiet runs.
  add_executable(oa-boneyards-extension-entry-test tests/boneyards_entry_test.cpp)
  # turn_install_id_on lives in oa-app-content. The test checks that Turn
  # on clears only the picked registry's off key.
  target_link_libraries(oa-boneyards-extension-entry-test
    PRIVATE oa::app::headers oa-app-content oa-test-support oa-options)
  add_test(NAME boneyards-entry-flow COMMAND oa-boneyards-extension-entry-test)
  set_tests_properties(boneyards-entry-flow PROPERTIES TIMEOUT 60)
  # A made-up map in a fixture archive beside this test, read through the
  # same fallback the extension installs after the game's files miss.
  add_executable(oa-boneyards-shipped-archive-test
    tests/shipped_archive_test.cpp shipped_archive.cpp)
  target_include_directories(oa-boneyards-shipped-archive-test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(oa-boneyards-shipped-archive-test
    PRIVATE oa-boneyards-client oa-boneyards-rover-detail oa-formats-hpi
      oa-platform-shims oa-test-support oa-options)
  add_test(NAME boneyards-shipped-archive COMMAND oa-boneyards-shipped-archive-test)
  set_tests_properties(boneyards-shipped-archive PROPERTIES TIMEOUT 60)
  if(TARGET oa-boneyards-extension-options)
    add_executable(oa-boneyards-extension-options-test tests/boneyards_options_test.cpp)
    target_link_libraries(oa-boneyards-extension-options-test
      PRIVATE oa-boneyards-extension-options oa-netgame-test-launch oa-app-netgame-launch-binding oa-app-game-options oa-options)
    add_test(NAME boneyards-options COMMAND oa-boneyards-extension-options-test)
    set_tests_properties(boneyards-options PROPERTIES TIMEOUT 60)
  endif()
  if(TARGET oa-boneyards-extension-launch)
    add_executable(oa-boneyards-extension-launch-test tests/boneyards_launch_test.cpp)
    target_link_libraries(oa-boneyards-extension-launch-test
      PRIVATE oa-boneyards-extension-launch oa-netgame-test-launch oa-session-demo oa-formats-oademo oa-options
      oa::app::headers)
    add_test(NAME boneyards-launch COMMAND oa-boneyards-extension-launch-test)
    add_executable(oa-boneyards-exit-prompt-test tests/boneyards_exit_prompt_test.cpp)
    target_link_libraries(oa-boneyards-exit-prompt-test
      PRIVATE oa-boneyards-extension-launch oa-netgame-test-launch oa-ui-frontend-state oa-options)
    add_test(NAME boneyards-exit-prompt COMMAND oa-boneyards-exit-prompt-test)
    set_tests_properties(boneyards-launch boneyards-exit-prompt PROPERTIES TIMEOUT 60)
  endif()
endif()

# The installs the checks run on: the 3.1c install OA_GAME_DIR names, and
# the multiplayer install OA_LAUNCHER_GAME_DIR names, each only when set.
# Each Boneyards check mounts the mod folder this build writes, so the new
# screens are read through the game's files.
set(oa_boneyards_game_dir "${OA_GAME_DIR}")
set(oa_launcher_game_dir "${OA_LAUNCHER_GAME_DIR}")
set(oa_boneyards_checks "${CMAKE_BINARY_DIR}/native-checks")
if(BUILD_TESTING AND TARGET oa-game AND IS_DIRECTORY "${oa_boneyards_game_dir}")
  file(MAKE_DIRECTORY "${oa_boneyards_checks}")
  find_package(Python3 COMPONENTS Interpreter QUIET)
  # Enter Boneyards reaches frontend state 7 and the Boneyards desktop
  # draws the frame; nothing logs in.
  add_test(NAME native-boneyards-entry COMMAND oa-game --game-dir "${oa_boneyards_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
    --mute --enter-boneyards 90
    --snapshot "${oa_boneyards_checks}/boneyards-entry.ppm"
    --preferences-file "${oa_boneyards_checks}/boneyards-entry.conf")
  set_tests_properties(native-boneyards-entry PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    WORKING_DIRECTORY "${oa_boneyards_checks}")
  # Profile APPLY stores a unit limit and the engine's unit limit preference
  # reads it back. Nothing is drawn and nobody logs in.
  add_test(NAME native-boneyards-unit-limit COMMAND oa-game --game-dir "${oa_boneyards_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
    --mute --check-boneyards-unit-limit 333
    --preferences-file "${oa_boneyards_checks}/boneyards-unit-limit.conf")
  set_tests_properties(native-boneyards-unit-limit PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    WORKING_DIRECTORY "${oa_boneyards_checks}")
  # Pointer clicks on the strip's SINGLE, MULTI and EXIT, and on the old
  # MULTI column. SINGLE opens single player, MULTI opens SELECT CONNECTION
  # and EXIT ends the run. The run has no window.
  add_test(NAME native-boneyards-strip-buttons COMMAND oa-game
    --game-dir "${oa_boneyards_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
    --mute --check-strip-buttons
    --preferences-file "${oa_boneyards_checks}/boneyards-strip-buttons.conf")
  set_tests_properties(native-boneyards-strip-buttons PROPERTIES
    TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    WORKING_DIRECTORY "${oa_boneyards_checks}"
    PASS_REGULAR_EXPRESSION
      "strip buttons check: SINGLE opens single player, MULTI opens SELECT CONNECTION, and EXIT ends the run"
    ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
  # The status strip on the presented main menu, once with Basic asked and
  # once with Full. Each run keeps its own preferences file. The dummy
  # video driver has no card renderer, so the processor presents the frame;
  # the strip still has to be on it.
  foreach(oa_overlay_tier basic full)
    add_test(NAME native-boneyards-overlay-${oa_overlay_tier} COMMAND oa-game
      --game-dir "${oa_boneyards_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --skip-intro --resolution 1280x960
      --hardware-acceleration=${oa_overlay_tier}
      --frames 1200 --check-boneyards-overlay ${oa_overlay_tier}
      --preferences-file "${oa_boneyards_checks}/boneyards-overlay-${oa_overlay_tier}.conf")
    set_tests_properties(native-boneyards-overlay-${oa_overlay_tier} PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      PASS_REGULAR_EXPRESSION
        "boneyards overlay check: ${oa_overlay_tier}: the strip is on the presented frame"
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
  endforeach()
  # "-y" has the main menu's first update enter the Boneyards (dispatcher
  # step 0x10b), as Enter Boneyards does without its network check: frontend
  # state 7 and the Boneyards desktop, with no key pressed; nothing logs in.
  add_test(NAME native-boneyards-entry-switch COMMAND oa-game --game-dir "${oa_boneyards_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
    --mute -y --enter-boneyards 90
    --snapshot "${oa_boneyards_checks}/boneyards-entry-switch.ppm"
    --preferences-file "${oa_boneyards_checks}/boneyards-entry-switch.conf")
  set_tests_properties(native-boneyards-entry-switch PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    WORKING_DIRECTORY "${oa_boneyards_checks}")
  # "-y" in the windowed loop. The welcome is already finished, so the update
  # check would run, and the data folder is empty, so no stored feed is in
  # hand. The option is not a headless check and not unattended. The login
  # opens with no input, and leaving it ends the run.
  file(MAKE_DIRECTORY "${oa_boneyards_checks}/boneyards-shortcut-data"
    "${oa_boneyards_checks}/boneyards-shortcut-user")
  file(WRITE "${oa_boneyards_checks}/boneyards-shortcut.conf"
    "open-annihilation-preferences 1\n\"open-annihilation.first-run\" \"done\"\n")
  add_test(NAME native-boneyards-shortcut COMMAND oa-game
    --game-dir "${oa_boneyards_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
    --mute --skip-intro -y --check-boneyards-shortcut
    --preferences-file "${oa_boneyards_checks}/boneyards-shortcut.conf"
    --data-dir "${oa_boneyards_checks}/boneyards-shortcut-data"
    --user-folder "${oa_boneyards_checks}/boneyards-shortcut-user")
  set_tests_properties(native-boneyards-shortcut PROPERTIES
    TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    WORKING_DIRECTORY "${oa_boneyards_checks}"
    PASS_REGULAR_EXPRESSION
      "boneyards shortcut check: -y entered the login and left with no reason"
    FAIL_REGULAR_EXPRESSION "You were rejected from the game"
    ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
  # A made-up mod is on. The entry asks, Switch off restarts, and the next
  # run has no mod and the entry under way. Each run leaves no pending entry,
  # no remembered mod and no mod selected, so the check can pass twice.
  file(MAKE_DIRECTORY "${oa_boneyards_checks}/boneyards-mod-entry")
  add_test(NAME native-boneyards-mod-entry COMMAND oa-game
    --game-dir "${oa_boneyards_game_dir}"
    --skip-intro --mute --check-boneyards-mod-entry
    --preferences-file "${oa_boneyards_checks}/boneyards-mod-entry/preferences.conf"
    --user-folder "${oa_boneyards_checks}/boneyards-mod-entry/Open Annihilation")
  set_tests_properties(native-boneyards-mod-entry PROPERTIES
    TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    WORKING_DIRECTORY "${oa_boneyards_checks}/boneyards-mod-entry"
    PASS_REGULAR_EXPRESSION
      "mod entry check: the mod was switched off and the entry is under way"
    ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
  # A battle the Boneyards launches, played by two copies of the game over
  # TCP/IP on this machine, each from its replay of made-up Rover replies:
  # the login, the launch_game record the console takes, the frontend's pass
  # into the battle room, the battle, the reports to the Boneyards (settings,
  # players, scores and chat) and the return to its debriefing room. The two
  # copies host and search on a DirectPlay port the check picks.
  if(Python3_Interpreter_FOUND)
    # A quiet entry over a fixture registry that lists the expansion as one
    # package of the archive and its folder. install fetches it once, with
    # both online checks off and left off, and opens the Boneyards with its
    # art and sounds in use. update is offered a newer game and goes on;
    # an equal record fetches nothing, and a changed package is installed
    # once over the copy, keeping the war map. Each run wipes its own work
    # folder.
    add_test(NAME native-boneyards-entry-install COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_entry.py"
      --game "$<TARGET_FILE:oa-game>" --tool "$<TARGET_FILE:oa-tool>"
      --game-dir "${oa_boneyards_game_dir}" --mode install
      --work "${oa_boneyards_checks}/boneyards-entry-install")
    set_tests_properties(native-boneyards-entry-install PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    add_test(NAME native-boneyards-entry-update COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_entry.py"
      --game "$<TARGET_FILE:oa-game>" --tool "$<TARGET_FILE:oa-tool>"
      --game-dir "${oa_boneyards_game_dir}" --mode update
      --work "${oa_boneyards_checks}/boneyards-entry-update")
    set_tests_properties(native-boneyards-entry-update PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    # A registry on this machine that requires an install ID. The quiet half
    # logs the question and stops. Cancel changes nothing. Turn on faces a
    # player over the loopback MCP endpoint, with no frame limit, clears the
    # off key, fetches the package once and opens the Boneyards. Each run
    # wipes its own work folder.
    add_test(NAME native-boneyards-entry-install-id COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_entry.py"
      --game "$<TARGET_FILE:oa-game>" --tool "$<TARGET_FILE:oa-tool>"
      --game-dir "${oa_boneyards_game_dir}" --mode install-id
      --work "${oa_boneyards_checks}/boneyards-entry-install-id")
    set_tests_properties(native-boneyards-entry-install-id PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    # The way in on a mod's main menu, with two made-up mods the check
    # writes and the Boneyards installed as the expansion: on the base
    # game's art the strip, which takes a click on its art over the mod's
    # hidden EXIT; on art of the mod's own the base game's layout and the
    # strip's ENTER plate, which starts the Boneyards entry.
    # Each run wipes its own work folder.
    add_test(NAME native-boneyards-menu-modes COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_menu_modes.py"
      --game "$<TARGET_FILE:oa-game>" --game-dir "${oa_boneyards_game_dir}"
      --coreprime "${CMAKE_BINARY_DIR}/coreprime"
      --bundle-table "${open_annihilation_SOURCE_DIR}/src/data/content-bundles/src/bundle_table.inc"
      --work "${oa_boneyards_checks}/boneyards-menu-modes")
    set_tests_properties(native-boneyards-menu-modes PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      PASS_REGULAR_EXPRESSION
        "boneyards menu modes: the strip on the base game's art, the ENTER plate on art of the mod's own"
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
    # The strip's keyboard and the main menu's order, with the Boneyards
    # installed. Tab reaches the order before the strip, in strip mode and
    # in button mode, and while the ring shows an arrow and Return press the
    # stop the arrow ringed. After the services question closes, Tab moves
    # on, to Credits after Enter. A layout that moves Accept and Press opens
    # More maps… and the update pill from those keys; Return, unbound, does
    # not. Each run wipes its own work folder.
    add_test(NAME native-boneyards-menu-keys COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_menu_keys.py"
      --game "$<TARGET_FILE:oa-game>" --tool "$<TARGET_FILE:oa-tool>"
      --game-dir "${oa_boneyards_game_dir}"
      --coreprime "${CMAKE_BINARY_DIR}/coreprime"
      --work "${oa_boneyards_checks}/boneyards-menu-keys")
    set_tests_properties(native-boneyards-menu-keys PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      PASS_REGULAR_EXPRESSION
        "boneyards menu keys: the strip and the plate agree with the order, and a moved Accept opens More maps and the update pill"
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
    add_test(NAME native-boneyards-launch COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_launch.py"
      --game "$<TARGET_FILE:oa-game>" --game-dir "${oa_boneyards_game_dir}"
      --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --replays "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
      --work "${oa_boneyards_checks}/boneyards-launch" --timeout 400)
    set_tests_properties(native-boneyards-launch PROPERTIES TIMEOUT 450 COST 100
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
    # The same battle, with the replay's launch_game carrying an overlay that
    # points at a relay on 127.0.0.1. The check proves the two games exchanged
    # nothing on a direct socket.
    if(TARGET oa-boneyards-test-relay)
      add_test(NAME native-boneyards-launch-overlay COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_launch.py"
        --game "$<TARGET_FILE:oa-game>" --game-dir "${oa_boneyards_game_dir}"
        --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --replays "${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        --work "${oa_boneyards_checks}/boneyards-launch-overlay" --timeout 400
        --overlay --relay "$<TARGET_FILE:oa-boneyards-test-relay>")
      set_tests_properties(native-boneyards-launch-overlay PROPERTIES TIMEOUT 450 COST 100
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
    endif()
  endif()
  if(IS_DIRECTORY "${oa_launcher_game_dir}")
    # A login replayed from a file of Rover replies reaches the dossier and
    # the galactic map of that install; nothing is dialled.
    add_test(NAME native-boneyards-login-replay COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --check-boneyards-login
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_post_login.replay"
      --preferences-file "${oa_boneyards_checks}/boneyards-login-replay.conf")
    set_tests_properties(native-boneyards-login-replay PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    # Phase 3 screens clicked through from a replay. The Beta Tester's
    # preferences offer the in-game registration form; an ordinary account's
    # do not. Each run keeps the folder beside its preferences file, so the
    # two accounts do not share saved files when the suite runs them together.
    # Nothing is dialled. Without live credentials the live console run is a
    # separate check and skips.
    set(oa_features_beta_dir "${oa_boneyards_checks}/features-beta")
    set(oa_features_player_dir "${oa_boneyards_checks}/features-player")
    file(MAKE_DIRECTORY "${oa_features_beta_dir}" "${oa_features_player_dir}")
    file(WRITE "${oa_features_beta_dir}/preferences.conf"
      "open-annihilation-preferences 1\n\"Boneyards|BetaLoginUX\" \"1\"\n")
    file(WRITE "${oa_features_player_dir}/preferences.conf"
      "open-annihilation-preferences 1\n")
    add_test(NAME native-boneyards-features-beta COMMAND oa-game
      --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --frames 400000 --check-boneyards-features beta
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_features_beta.replay"
      --preferences-file "${oa_features_beta_dir}/preferences.conf")
    add_test(NAME native-boneyards-features-player COMMAND oa-game
      --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --frames 400000 --check-boneyards-features player
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_features_player.replay"
      --preferences-file "${oa_features_player_dir}/preferences.conf")
    set_tests_properties(native-boneyards-features-beta PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_features_beta_dir}"
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software;OA_BY_USER=;OA_BY_PASSWORD=")
    set_tests_properties(native-boneyards-features-player PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_features_player_dir}"
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software;OA_BY_USER=;OA_BY_PASSWORD=")
    # A replayed login while the window stays inactive. The guest name is
    # filled and LOGIN is clicked the way a program driving the game clicks,
    # and the login finishes before the loop is allowed to wait. The engine's
    # own inactive-loop check is separate and does not open this login.
    set(oa_login_inactive_dir "${oa_boneyards_checks}/login-inactive")
    file(MAKE_DIRECTORY "${oa_login_inactive_dir}")
    add_test(NAME native-boneyards-login-inactive COMMAND oa-game
      --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --skip-intro --mute --check-running-while-inactive
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_post_login.replay"
      --preferences-file "${oa_login_inactive_dir}/login-inactive.conf")
    set_tests_properties(native-boneyards-login-inactive PROPERTIES
      TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_login_inactive_dir}"
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software;OA_BY_USER=;OA_BY_PASSWORD=")
    # The same replayed login typed into the login console after Enter
    # Boneyards: the dossier closes the login console and the green
    # slides off alpha's 25-planet galactic map, whose viewer must then
    # show at least 20 planet sprites; a press on a planet must then show
    # it in the planet summary beside Recon (the final frame is kept as
    # boneyards-post-login.ppm); no chat console opens. GREEN_SLIDE,
    # BIGBUTTON and MORE must resolve to WAVs in the install.
    add_test(NAME native-boneyards-post-login COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_post_login.replay"
      --snapshot "${oa_boneyards_checks}/boneyards-post-login.ppm"
      --preferences-file "${oa_boneyards_checks}/boneyards-post-login.conf")
    set_tests_properties(native-boneyards-post-login PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    # The same replay, driven from outside the game: "-y" enters, the login
    # console's controls are listed, a made-up name and password are typed
    # and LOGIN is pressed down and released across a pulse. The console
    # closes once the dossier arrives.
    if(Python3_Interpreter_FOUND)
      add_test(NAME native-boneyards-driver-login COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_driver_login.py"
        --native "$<TARGET_FILE:oa-game>" --game-dir "${oa_launcher_game_dir}"
        --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_post_login.replay"
        --scratch-root "${oa_boneyards_checks}")
      set_tests_properties(native-boneyards-driver-login PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_boneyards_checks}"
        ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=;SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The side prompt and matchmaking, driven by keys. Login may still be
      # clicked. Choosing the side, and Activate, are ui.key only.
      add_test(NAME native-boneyards-keys-modal COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_keys_modal.py"
        --native "$<TARGET_FILE:oa-game>" --game-dir "${oa_launcher_game_dir}"
        --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --side-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/side_prompt.replay"
        --beta-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_features_beta.replay"
        --match-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/matchmaking.replay"
        --scratch-root "${oa_boneyards_checks}")
      set_tests_properties(native-boneyards-keys-modal PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_boneyards_checks}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Battle, QuickPlay and the Locator by keys. Consoles still open with
      # a click. JOIN is focused and not pressed. Right on the host form's
      # skill row chooses Seasoned, and Down from that row reaches the players
      # row. FIND BATTLE is run and its offer is not accepted, and Return in
      # the Locator's name searches.
      add_test(NAME native-boneyards-keys-battle COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_keys_battle.py"
        --native "$<TARGET_FILE:oa-game>" --game-dir "${oa_launcher_game_dir}"
        --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --beta-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_features_beta.replay"
        --quickplay-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/quickplay.replay"
        --scratch-root "${oa_boneyards_checks}")
      set_tests_properties(native-boneyards-keys-battle PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_boneyards_checks}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Help and a page link, then a chat line, by keys. Login and the planet
      # may still be clicked. Each launch writes its own preferences.
      add_test(NAME native-boneyards-keys-contents COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_keys_contents.py"
        --native "$<TARGET_FILE:oa-game>" --game-dir "${oa_launcher_game_dir}"
        --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --contents-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_contents.replay"
        --chat-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_chat.replay"
        --scratch-root "${oa_boneyards_checks}")
      set_tests_properties(native-boneyards-keys-contents PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_boneyards_checks}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # Every lobby console, driven through MCP against four replays; an
      # ordinary account opens the Locator and QuickPlay, and the Beta
      # Tester's also opens Matchmaking. Each launch logs in as a registered
      # player. The client drops OA_BY_USER and
      # OA_BY_PASSWORD itself.
      add_test(NAME native-mcp-lobby COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_mcp_lobby.py"
        --native "$<TARGET_FILE:oa-game>"
        --game-dir "${oa_launcher_game_dir}"
        --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --chat-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_chat.replay"
        --side-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/side_prompt.replay"
        --contents-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_contents.replay"
        --beta-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_features_beta.replay"
        --scratch-root "${oa_boneyards_checks}")
      set_tests_properties(native-mcp-lobby PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_boneyards_checks}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
      # The war map by keyboard. Login may still be clicked. The planet is
      # chosen with F6, the arrows and Return.
      add_test(NAME native-boneyards-keys-war-map COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_keys_war_map.py"
        --native "$<TARGET_FILE:oa-game>" --game-dir "${oa_launcher_game_dir}"
        --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_planets.replay"
        --scratch-root "${oa_boneyards_checks}")
      set_tests_properties(native-boneyards-keys-war-map PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_boneyards_checks}"
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
    endif()
    # On that map, contested planet 1801 is hovered and pressed through
    # SDL input and its census (synthetic in alpha_planets.replay) must
    # arrive, after which the planet summary must read its war-map name,
    # Contested, the census's Battles and Forming counts and each side's
    # wins; then HELP opens its console and Escape hands the map back
    # (boneyards-pick-pick.ppm, boneyards-pick-help.ppm, and the map
    # after the release as boneyards-pick.ppm).
    add_test(NAME native-boneyards-pick-replay COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_planets.replay"
      --boneyards-pick 1801 --boneyards-open help
      --snapshot "${oa_boneyards_checks}/boneyards-pick.ppm"
      --preferences-file "${oa_boneyards_checks}/boneyards-pick.conf")
    set_tests_properties(native-boneyards-pick-replay PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    # RECON on planet 1801 instead: once the census its open asks for
    # has arrived, the console must name the planet as the war map does,
    # show its map's gravity and the census population, and scroll its
    # panorama (boneyards-recon-recon.ppm while it shows).
    add_test(NAME native-boneyards-recon-replay COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_planets.replay"
      --boneyards-pick 1801 --boneyards-open recon
      --snapshot "${oa_boneyards_checks}/boneyards-recon.ppm"
      --preferences-file "${oa_boneyards_checks}/boneyards-recon.conf")
    set_tests_properties(native-boneyards-recon-replay PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    # The same pick opens the chat console, where +locate goes out as
    # Rover's locate request and the replay's synthetic user_located answer must
    # print its line in the chat log (boneyards-locate-say.ppm).
    add_test(NAME native-boneyards-chat-locate COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_planets.replay"
      --boneyards-pick 1801 --boneyards-open chat --boneyards-say "+locate Ghost_Commander"
      --snapshot "${oa_boneyards_checks}/boneyards-locate.ppm"
      --preferences-file "${oa_boneyards_checks}/boneyards-locate.conf")
    set_tests_properties(native-boneyards-chat-locate PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    # CHAT on planet 1801 of the synthetic alpha_chat.replay, driven as a
    # player would: the news ticker must scroll the replay's ticker line
    # (boneyards-chat-ticker.ppm); pick Slag, press CHAT, and the console
    # must enter a planetary default and list the replay's frequencies
    # (in type order, the entered one circled) and the frequency's
    # members, with the entry, the members' lines from the replay and the page
    # the entry brings in its log (boneyards-chat-chat.ppm); the typed
    # line must reach Rover as a say to the frequency and come back; a
    # click on the second player row,
    # Ghost_Commander's, must show the player pane filled from his
    # profile in the replay (Rover's profile request, the activity light
    # running until it arrives; boneyards-chat-chat-player.ppm); a double click on the other
    # planetary default must switch to it (boneyards-chat-chat-switched.ppm);
    # CREATE must open the create-frequency pane, where the typed name
    # turns GO on and GO asks Rover for the frequency, and the replay's
    # Forward Base must be entered (boneyards-chat-chat-create.ppm,
    # boneyards-chat-chat-created.ppm); Release must leave it and hand the
    # map back.
    add_test(NAME native-boneyards-chat-replay COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_chat.replay"
      --boneyards-chat 1801 --boneyards-say "hello from oa" --boneyards-create "Forward Base"
      --snapshot "${oa_boneyards_checks}/boneyards-chat.ppm"
      --preferences-file "${oa_boneyards_checks}/boneyards-chat.conf")
    set_tests_properties(native-boneyards-chat-replay PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    # PROFILE opens its console, whose first record reads the install's
    # Profile.def through Rover: the account tab must show that file's
    # Login description, and the player's own record, asked for with
    # Rover's profile request and the activity light running, must arrive and stop
    # the light; the console must then show its seven category rows with
    # Login selected, the start of that description under them and its
    # status line down (boneyards-profile-profile.ppm).
    add_test(NAME native-boneyards-profile COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_planets.replay"
      --boneyards-open profile
      --snapshot "${oa_boneyards_checks}/boneyards-profile.ppm"
      --preferences-file "${oa_boneyards_checks}/boneyards-profile.conf")
    set_tests_properties(native-boneyards-profile PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    # NEWS, LADDERS and HELP open their consoles on the synthetic
    # alpha_contents.replay: the contents list must fill and the first
    # headline's page replace the waiting page with the activity light
    # dark. News reads its contents file from the replay's news server
    # and Ladders its first page from the ladder server, each downloaded
    # through Rover's file download to a temporary file that must be deleted
    # once read (boneyards-news-news.ppm, boneyards-ladders-ladders.ppm,
    # boneyards-help-help.ppm).
    foreach(oa_contents_console IN ITEMS news ladders help)
      add_test(NAME native-boneyards-${oa_contents_console} COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
        --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
        --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_contents.replay"
        --boneyards-open ${oa_contents_console}
        --snapshot "${oa_boneyards_checks}/boneyards-${oa_contents_console}.ppm"
        --preferences-file "${oa_boneyards_checks}/boneyards-${oa_contents_console}.conf")
      set_tests_properties(native-boneyards-${oa_contents_console} PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
        WORKING_DIRECTORY "${oa_boneyards_checks}"
        ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
    endforeach()
    # CHAT on planet 1801 of the synthetic alpha_chat.replay: once one of
    # Slag's two planetary defaults is entered, a line typed and sent
    # with Enter must reach Rover as a say to it and come back to the log
    # (the replay echoes it), and Escape must leave the frequency
    # (boneyards-chat-say-chat.ppm, boneyards-chat-say-say.ppm).
    add_test(NAME native-boneyards-chat-say COMMAND oa-game --game-dir "${oa_launcher_game_dir}" --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --mute --enter-boneyards 400 --boneyards-login --boneyards-planets 25
      --boneyards-replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_chat.replay"
      --boneyards-pick 1801 --boneyards-open chat --boneyards-say "Sure, on my way."
      --snapshot "${oa_boneyards_checks}/boneyards-chat-say.ppm"
      --preferences-file "${oa_boneyards_checks}/boneyards-chat-say.conf")
    set_tests_properties(native-boneyards-chat-say PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      WORKING_DIRECTORY "${oa_boneyards_checks}"
      ENVIRONMENT "OA_BY_USER=;OA_BY_PASSWORD=")
  endif()
  # The expansion's files, linked into a scratch user folder, are what the
  # overlay and a help page read. The game folder is this install with the
  # expansion's root file and folder left out. The check clears that scratch
  # folder at the start of every run.
  if(Python3_Interpreter_FOUND)
    add_test(NAME native-boneyards-expansion COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_native_boneyards_expansion.py"
      --game "$<TARGET_FILE:oa-game>"
      --source-game-dir "${oa_boneyards_game_dir}"
      --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
      --bundle-table "${open_annihilation_SOURCE_DIR}/src/data/content-bundles/src/bundle_table.inc"
      --replay "${open_annihilation_SOURCE_DIR}/src/boneyards/client/tests/data/alpha_contents.replay"
      --work "${oa_boneyards_checks}/X02-boneyards-expansion")
    set_tests_properties(native-boneyards-expansion PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
      ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software")
  endif()
endif()

# The multiplayer install the Boneyards launches from, which OA_LAUNCHER_GAME_DIR
# names, passes the folder dialog's check, and MULTI clicked through SDL
# input opens network play's screens on it, with the Boneyards' part of the
# multiplayer menu check after network play's. REGISTER opens the registration
# screen, which is in the mod folder this build writes.
if(BUILD_TESTING AND TARGET oa-game AND IS_DIRECTORY "${oa_launcher_game_dir}")
  if(TARGET oa-game-directory-test)
    add_test(NAME game-directory-launcher-install COMMAND oa-game-directory-test --install)
    set_tests_properties(game-directory-launcher-install PROPERTIES
      ENVIRONMENT "OA_GAME_DIR=${oa_launcher_game_dir}")
  endif()
  set(oa_multiplayer_check_dir "${oa_boneyards_checks}/multiplayer-menu-launcher")
  file(MAKE_DIRECTORY "${oa_multiplayer_check_dir}")
  add_test(NAME native-multiplayer-menu-launcher COMMAND oa-game --game-dir "${oa_launcher_game_dir}"
    --mod-dir "${CMAKE_BINARY_DIR}/coreprime"
    --skip-intro --mute --check-multiplayer-menu
    --preferences-file "${oa_multiplayer_check_dir}/multiplayer-menu.conf")
  set_tests_properties(native-multiplayer-menu-launcher PROPERTIES TIMEOUT ${OA_NATIVE_CHECK_TIMEOUT}
    WORKING_DIRECTORY "${oa_multiplayer_check_dir}"
    ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy;SDL_RENDER_DRIVER=software;MallocScribble=1")
endif()

# Each Boneyards check gets a temporary folder of its own: the service's
# downloaded files are named by the first number not yet taken in that
# folder, so checks running side by side would otherwise meet each other's
# files.
get_property(oa_boneyards_app_tests DIRECTORY PROPERTY TESTS)
foreach(oa_test IN LISTS oa_boneyards_app_tests)
  if(oa_test MATCHES "^native-boneyards-")
    set(oa_test_temp "${CMAKE_CURRENT_BINARY_DIR}/native-checks/${oa_test}-temp")
    file(MAKE_DIRECTORY "${oa_test_temp}")
    set_property(TEST ${oa_test} APPEND PROPERTY ENVIRONMENT
      "TMPDIR=${oa_test_temp}" "TMP=${oa_test_temp}" "TEMP=${oa_test_temp}")
  endif()
endforeach()
