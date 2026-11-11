#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that open-annihilation calls the hooks of its extension tables.

Runs an open-annihilation built with the recorder test extensions
(-DOA_RECORD_EXTENSION_HOOKS=ON), which the game lists after network play's
extension, and reads the counts each run leaves in its --record-hooks file.
Every run must record at least the calls RUNS lists for it; a hook recorded
with an enumerator ("frame pump") names that call only. The follower, the
extension built on the recorder, counts its own calls as "follower.<hook>":
across the runs of a set it must be called for every hook the recorder is
called for, but the Runtime member only the recorder has, and at each point
where the combined table calls the two in a set order it counts
"follower.order <hook>" when the order held and "follower.misordered <hook>"
when it did not, which fails the run. --runs picks the runs:

  options  the option hooks, without game data: --help, the -r switch
           (whose refusal is network play's), an unknown option, a game
           directory that does not exist and a follower that fills
           frontend_game beside network play, which stops the start;
  game     the rest, over the installation OA_GAME_DIR names: a headless
           skirmish, the headless --check-navigation run (whose speed keys
           reach speed_changed and whose menus app_mode_set), a headless run
           the recorder takes to drive the check host without a window
           (--record-check-host), and interactive main menu frames,
           --check-multiplayer-menu (network play's walk of the multiplayer
           screens, after which the recorder drives a round of the main menu
           through the check host), --check-match-dialogs
           (whose close requests reach close_requested and whose GAME slider
           speed_changed), a --generate-script run whose recording no
           extension replays (open_recording), a run whose recorder registers
           a Settings page the registry refuses, which stops the start
           (--record-refused-page), and a run the recorder ends through
           ScreenServices::quit (--record-quit) with status 3, through SDL's
           dummy drivers. When OA_GAME_DIR is unset,
           empty or names no directory it exits with --skip-code
           (OA_GAME_DATA_SKIP_CODE), which ctest reports as skipped, or fails
           when OA_REQUIRE_GAME_DATA=1.

HOOKS must name the hooks src/app/include/oa/app/extension.hpp declares, in its order.
With every run of both sets, each of them is reached except those UNREACHED
lists, which only a shared match or a run these sets do not make reaches,
and those NETWORK_PLAY_ONLY lists, which one extension at most may fill and
network play fills, so neither test extension does; so are match_event's
left, results_reported and watching_kept, which these runs do not ask for.
A new hook must be added to the recorder and the follower (or, when one
extension at most may fill it and network play does, to
NETWORK_PLAY_ONLY), to HOOKS and to a run here or to UNREACHED.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Wall-time limit per game run; a loaded machine stretches headless runs.
RUN_TIMEOUT_SECONDS = 900
# SDL's drivers for runs that open the game's window without a display.
DUMMY_DRIVERS = {"SDL_VIDEO_DRIVER": "dummy", "SDL_AUDIO_DRIVER": "dummy", "SDL_RENDER_DRIVER": "software"}

EXTENSION_HEADER = Path(__file__).resolve().parents[2] / "src" / "app" / "include" / "oa" / "app" / "extension.hpp"
# A hook's declaration in the table: a named pointer to a function whose
# parameters may hold one more (check_console's), default-initialised.
HOOK_RE = re.compile(r"\(\*(\w+)\)\s*\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)\s*\{\}\s*;")

# Every hook of src/app/include/oa/app/extension.hpp, as the recorder names them.
HOOKS = [
    "take_option", "check_options", "switch_handler", "text", "startup", "register_screens",
    "register_settings_pages", "ready", "frontend_entry", "frontend_states", "run_mode", "start_scene", "shutdown", "select_multiplayer",
    "frontend_game", "keep_stored_password", "check_multiplayer_menu", "state", "frame",
    "simulation_step", "outcome_ready", "match_game", "match_event", "disconnect_text",
    "give_resources", "message_hooks", "player_gone", "console_host", "check_console", "draw_loading",
    "draw_match_hud", "draw_match_overlay", "pause_changed", "load_progress", "team_panel_host",
    "close_requested", "return_label", "speed_changed", "app_mode_set", "open_recording", "release_runtime",
    "replay_frame", "replay_hold", "replay_play", "replay_pause", "replay_set_speed", "replay_step_speed",
    "replay_seek", "replay_state",
]
# The hooks one extension at most may fill, which network play fills and
# neither test extension does.
NETWORK_PLAY_ONLY = {"frontend_game", "frontend_states"}
# What the recorder alone records.
FOLLOWER_UNFILLED = {"runtime_member"}
# The follower's usage note, which --help prints in place of the engine's.
FOLLOWER_NOTE = "The recorder test extensions record every hook's calls."
# The environment variable that has the follower fill frontend_game too.
DOUBLE_FRONTEND_GAME = "OA_RECORDER_FOLLOWER_FRONTEND_GAME"
# The preferences file each game run starts from. The Kit fixture, the
# build's third test extension (kit_fixture.cpp), draws over the
# battlefield's top left while its chip setting is on, where the navigation
# check reads the message log; these runs keep it off.
PREFERENCES = 'open-annihilation-preferences 1\n"kit-fixture.show-chip" "0"\n'
# Hooks no run here reaches, and why.
UNREACHED = {
    "disconnect_text": "the end-of-game screen asks for it only after a shared match",
    # A windowed --play-demo reaches replay_frame. replay_hold runs on a frame
    # whose match clock does not step during that playback, and the speed keys
    # reach replay_step_speed. These runs do not watch a recording.
    "replay_frame": "these runs do not watch a recording; a windowed --play-demo reaches it",
    "replay_hold": "these runs do not watch a recording; a frame that does not step during one reaches it",
    "replay_step_speed": "these runs do not watch a recording; the speed keys during one reach it",
    "replay_play": "no engine code calls them until the playback bar",
    "replay_pause": "no engine code calls them until the playback bar",
    "replay_set_speed": "no engine code calls them until the playback bar",
    "replay_seek": "no engine code calls them until the playback bar",
    "replay_state": "no engine code calls them until the playback bar",
}


class Run:
    """One start of open-annihilation: its arguments, whether it needs the game, and what it must record.

    expected names calls recorded at least once; at_least maps a call to
    the fewest times it must be recorded, for a hook every run reaches
    once through --record-hooks itself. output is a text, or a list of
    texts, the run's output must hold; environment adds variables to the
    run's environment; files maps the names of files the run reads to the
    text written into them in the scratch directory first.
    """

    def __init__(self, name, arguments, expected, *, at_least=None, game=False, status=0, output="",
                 dummy=False, environment=None, files=None):
        self.name = name
        self.arguments = arguments
        self.expected = expected
        self.at_least = at_least or {}
        self.game = game
        self.status = status
        self.output = output
        self.dummy = dummy
        self.environment = environment or {}
        self.files = files or {}


# Every run passes --record-hooks first, which take_option takes, asking
# the follower first; a run that checks take_option passes another option
# the engine does not know.
RUNS = {
    "options": [
        # The follower's usage note is asked for first and answers, so the
        # recorder is not asked for one.
        Run("help", ["--help"],
            ["text usage_checks", "text usage_runs", "text usage_switches", "follower.text",
             "follower.order take_option"],
            output=["usage: open-annihilation", FOLLOWER_NOTE]),
        # -r prints its line and ends through write_startup_line, which exits 0,
        # as --help and --version do.
        Run("register-switch", ["-r"], ["switch_handler", "text register_switch"],
            status=0, output="-r registers the game as a DirectPlay application"),
        Run("unknown-option", ["--not-an-option"], [], at_least={"take_option": 2}, status=1,
            output="unknown option: --not-an-option"),
        Run("missing-game-dir", ["--game-dir", "{scratch}/no-such-game"],
            ["switch_handler", "check_options"], status=1, output="game directory does not exist"),
        Run("double-frontend-game", ["--help"], [], status=1,
            output="the extensions oa-app-netgame and oa-extension-recorder-follower both fill "
                   "frontend_game, which one extension at most may fill",
            environment={DOUBLE_FRONTEND_GAME: "1"}),
    ],
    "game": [
        Run("headless-skirmish",
            ["--game-dir", "{game}", "--skip-intro", "--mute", "--headless-check", "--match-ticks", "60"],
            ["startup", "runtime_member", "register_screens", "register_settings_pages", "ready",
             "frontend_entry", "run_mode start",
             "run_mode headless_first", "run_mode headless", "match_game", "match_event torn_down",
             "draw_loading", "draw_match_hud", "draw_match_overlay", "state", "load_progress",
             "team_panel_host", "return_label", "keep_stored_password", "release_runtime",
             "follower.order startup", "follower.order run_mode", "follower.order return_label",
             "follower.order release_runtime"],
            game=True),
        Run("headless-navigation",
            ["--game-dir", "{game}", "--skip-intro", "--mute", "--headless-check", "--check-navigation"],
            ["console_host", "check_console", "give_resources", "message_hooks", "simulation_step",
             "outcome_ready", "player_gone", "match_event finished", "match_event results_released",
             "pause_changed on", "pause_changed off", "speed_changed", "app_mode_set"],
            game=True),
        # The recorder takes the headless run and drives the check host's
        # entries that need no window; its close request reaches
        # close_requested.
        Run("check-host-headless",
            ["--game-dir", "{game}", "--skip-intro", "--mute", "--headless-check", "--record-check-host"],
            ["run_mode headless", "close_requested", "follower.order run_mode"], game=True,
            output="recorder: check host, headless:"),
        Run("main-menu-frames", ["--game-dir", "{game}", "--skip-intro", "--mute", "--frames", "30"],
            ["start_scene", "frame pump", "frame after_pump", "frame presented", "shutdown", "state",
             "follower.order shutdown"], game=True, dummy=True),
        Run("multiplayer-menu", ["--game-dir", "{game}", "--skip-intro", "--mute", "--check-multiplayer-menu"],
            ["check_multiplayer_menu", "select_multiplayer"], game=True, dummy=True,
            output=["recorder: --check-multiplayer-menu", "recorder: check host, windowed:"]),
        Run("match-dialogs", ["--game-dir", "{game}", "--skip-intro", "--mute", "--check-match-dialogs"],
            ["close_requested", "return_label", "speed_changed"], game=True,
            dummy=True, output="match close check:"),
        # A director script's recording is offered to the extensions, the
        # follower first; neither replays it, so the run stops.
        Run("generate-script",
            ["--game-dir", "{game}", "--generate-script", "{scratch}/none.rec"],
            ["open_recording", "follower.order open_recording"], game=True, status=1,
            output="no extension of this build replays none.rec", files={"none.rec": "no recording\n"}),
        # The recorder registers a Settings page under an id the registry
        # refuses, which stops the start once every extension's
        # register_settings_pages has run.
        Run("refused-page",
            ["--game-dir", "{game}", "--skip-intro", "--mute", "--headless-check", "--record-refused-page"],
            ["register_settings_pages", "follower.register_settings_pages"], game=True, status=1,
            output="settings page registration rejected: an id is malformed"),
        # The recorder uses the screen services it keeps on the fifth frame
        # and ends the run through quit on the tenth, long before 60.
        Run("quit", ["--game-dir", "{game}", "--skip-intro", "--mute", "--frames", "60", "--record-quit", "3"],
            ["start_scene", "frame after_pump", "shutdown"], game=True, dummy=True, status=3,
            output=["recorder: stop_sounds, play_sound_alternate and run_frontend", "recorder: quit 3"]),
    ],
}


def declared_hooks():
    """The hooks struct Extension declares in src/app/include/oa/app/extension.hpp, in order."""
    text = EXTENSION_HEADER.read_text(encoding="utf-8")
    start = text.index("struct Extension {")
    return HOOK_RE.findall(text[start:text.index("\n};", start)])


def read_record(path):
    """The counts a --record-hooks file holds, by hook and enumerator."""
    counts = {}
    if not path.is_file():
        return counts
    for line in path.read_text(encoding="utf-8").splitlines():
        key, _, count = line.rpartition(" ")
        if key and count.isdigit():
            counts[key] = int(count)
    return counts


def recorded(counts, expected):
    """How many calls of expected, a hook or a hook and enumerator, counts hold."""
    return sum(count for key, count in counts.items() if key == expected or key.startswith(expected + " "))


def run_game(game, run, scratch, game_dir):
    """Starts open-annihilation for a run; returns the failures it found and the hooks it recorded."""
    record = scratch / f"{run.name}.hooks"
    preferences = scratch / f"{run.name}.conf"
    for name, text in run.files.items():
        (scratch / name).write_text(text, encoding="utf-8")
    arguments = [argument.format(game=game_dir, scratch=scratch) for argument in run.arguments]
    command = [*RUNNER, str(game), "--record-hooks", str(record), *arguments]
    if run.game:
        preferences.write_text(PREFERENCES, encoding="utf-8")
        command += ["--preferences-file", str(preferences)]
    environment = dict(os.environ)
    if run.dummy:
        environment.update(DUMMY_DRIVERS)
    environment.update(run.environment)
    result = subprocess.run(command, cwd=scratch, env=environment, capture_output=True, text=True,
                            errors="replace", timeout=RUN_TIMEOUT_SECONDS, check=False)
    output = result.stdout + result.stderr
    failures = []
    if result.returncode != run.status:
        failures.append(f"exit status {result.returncode}, expected {run.status}")
    for text in run.output if isinstance(run.output, list) else [run.output]:
        if text and text not in output:
            failures.append(f"no '{text}' in its output")
    counts = read_record(record)
    for expected in run.expected:
        if recorded(counts, expected) == 0:
            failures.append(f"'{expected}' was not recorded")
    for expected, fewest in run.at_least.items():
        calls = recorded(counts, expected)
        if calls < fewest:
            failures.append(f"'{expected}' was recorded {calls} times, expected at least {fewest}")
    for key in sorted(counts):
        if key.startswith("follower.misordered"):
            failures.append(f"the follower was called out of order: '{key}'")
    if failures:
        tail = "\n".join(output.splitlines()[-20:])
        failures.append(f"its output ended:\n{tail}")
    return failures, counts


def main():
    """Runs the chosen runs and reports what they did not record; returns the exit status."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--game", type=Path, required=True, help="open-annihilation built with the recorder")
    parser.add_argument("--runs", choices=sorted(RUNS), required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--skip-code", type=int,
                        help="exit status of a skipped game run (OA_GAME_DATA_SKIP_CODE); needed by --runs game")
    args = parser.parse_args()
    if args.runs == "game" and args.skip_code is None:
        parser.error("--runs game needs --skip-code")
    # Path("") is the current directory, so an empty OA_GAME_DIR is checked first.
    game_dir = os.environ.get("OA_GAME_DIR", "")
    if args.runs == "game" and (not game_dir or not Path(game_dir).is_dir()):
        if os.environ.get("OA_REQUIRE_GAME_DATA") == "1":
            print("check_hooks: OA_REQUIRE_GAME_DATA is set but OA_GAME_DIR names no installation")
            return 1
        print("check_hooks: skipped: OA_GAME_DIR names no installation")
        return args.skip_code
    if declared_hooks() != HOOKS:
        print(f"check_hooks: extension.hpp declares {declared_hooks()}; HOOKS and the recorder must follow it")
        return 1
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    failed = False
    reached = set()
    with tempfile.TemporaryDirectory(prefix=f"hooks-{args.runs}-", dir=args.scratch_root) as temporary:
        scratch = Path(temporary)
        for run in RUNS[args.runs]:
            failures, counts = run_game(args.game.resolve(), run, scratch, game_dir)
            reached.update(key.split(" ", 1)[0] for key in counts)
            for failure in failures:
                print(f"check_hooks: {run.name}: {failure}")
            failed = failed or bool(failures)
            if not failures:
                print(f"check_hooks: {run.name}: recorded {len(run.expected) + len(run.at_least)} expected "
                      f"calls among {len(counts)} kinds")
    # The follower is called wherever the recorder is, for every hook it fills.
    prefix = "follower."
    followed = {key[len(prefix):] for key in reached if key.startswith(prefix)}
    recorder_reached = {key for key in reached if not key.startswith(prefix)}
    for hook in sorted(recorder_reached - followed - FOLLOWER_UNFILLED):
        print(f"check_hooks: the runs reached the recorder's {hook} but not the follower's")
        failed = True
    if args.runs == "game":
        # The option runs reach the option hooks; together they reach the rest.
        options = {key.split(" ", 1)[0] for run in RUNS["options"] for key in [*run.expected, *run.at_least]}
        missing = sorted(set(HOOKS) - reached - options - set(UNREACHED) - NETWORK_PLAY_ONLY)
        for hook in missing:
            print(f"check_hooks: no run reached {hook}; add a run that does, or list it in UNREACHED")
        failed = failed or bool(missing)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
