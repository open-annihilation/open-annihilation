#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Build and launch the current native application from this working tree.
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$repo_dir/local"
caller_dir="$PWD"
build_type="${OA_BUILD_TYPE:-Debug}"
default_build_dir="$repo_dir/build"
if [[ "$build_type" != Debug ]]; then
    default_build_dir="$repo_dir/build-$(printf '%s' "$build_type" | tr '[:upper:]' '[:lower:]')"
fi
build_dir="${OA_BUILD_DIR:-$default_build_dir}"
movie=""
# The installation is --game-dir, else OA_GAME_DIR; without either the game
# uses the folder the player chose before, or asks for one and remembers it.
# --choose-game-dir overrides OA_GAME_DIR but not --game-dir. The build's
# CMake reads OA_GAME_DIR too and refuses a relative path, so it is made
# absolute here first.
case "${OA_GAME_DIR:-}" in ""|/*) ;; *) export OA_GAME_DIR="$caller_dir/$OA_GAME_DIR" ;; esac
game_dir="${OA_GAME_DIR:-}"
option_install=0
choose_game_dir=0
player_args=()

usage() {
    cat <<'USAGE'
Usage: ./run.sh [--game-dir PATH] [--skip-intro] [application options]

Builds current source before launching. No branch switching or stale-build fallback.
Default: build the native game application, open-annihilation, play the intro,
then open the frontend. On macOS it is built as the application bundle
open-annihilation.app, and its executable is started in place.
Gameplay integration remains in progress; unsupported actions report their status.
The game installation is --game-dir, else OA_GAME_DIR; without either the game
uses the Total Annihilation folder chosen before, or asks for it once and
remembers it. The folder that holds the installer of the Total Annihilation
demo (1997) works too: the game recognises the installer by its SHA-256, never
runs it, unpacks its game data (about 20 MB) once into the per-user data
folder and plays that, checking it again on later starts.

  --game-dir PATH     Use this game installation (the folder holding totala1.hpi,
                      or the folder holding the demo's installer)
  --choose-game-dir   Ask for the Total Annihilation folder again and remember it
  --skip-intro        Open the native frontend directly
  --check-navigation Exercise the menu transitions in a bounded check
  --trace-input       Log menu pointer coordinates and button state transitions
  --archive PATH      Mount an explicit archive (repeat for precedence order)
  --movie 1..5         Run only the selected movie of the installation's Data folder
  --frames N          Bound playback and application frames for inspection
  --headless-check    Decode without opening display/audio devices
  --mute              Disable device audio playback
  --snapshot PATH     Save a native frame as a PPM image
  --preferences-file PATH  Use an explicit preferences profile (useful for testing)
  --data-dir PATH     Unpack the demo's game data here instead of the per-user
                      data folder (useful for testing)
  --ios-simulator     Build for the iOS simulator and run there (platforms/ios/run.sh)
  --ios-device        Build for iPhone and iPad devices (platforms/ios/run.sh --device)
  --help              Show this help

Network play:
  --net-loopback-check N  Host and join a match in one process and compare both
                      worlds after N ticks
  --play-demo FILE.tad  Replay a recorded game through the network receive path

A game started for play writes its output to the logs folder in the
per-user folder (see README.md), not to this terminal; checks and other
scripted runs print to the terminal.

Environment: OA_GAME_DIR, OA_BUILD_DIR, OA_BUILD_JOBS (default: 6),
OA_BUILD_TYPE (default: Debug; e.g. RelWithDebInfo builds in build-relwithdebinfo).
SDL is bootstrapped locally if missing.
USAGE
}

require_value() {
    if [[ $# -lt 2 || -z "$2" ]]; then
        printf 'run.sh: %s requires a value\n' "$1" >&2
        exit 2
    fi
}

# The iOS and iPadOS builds have their own script; the remaining arguments go to it.
case "${1:-}" in
    --ios-simulator) shift; exec "$repo_dir/platforms/ios/run.sh" "$@" ;;
    --ios-device) shift; exec "$repo_dir/platforms/ios/run.sh" --device "$@" ;;
esac

while [[ $# -gt 0 ]]; do
    case "$1" in
        --help) usage; exit 0 ;;
        --movie) require_value "$@"; movie="$2"; shift 2 ;;
        --game-dir) require_value "$@"; game_dir="$2"; option_install=1; shift 2 ;;
        --choose-game-dir) choose_game_dir=1; player_args+=("$1"); shift ;;
        --frames|--snapshot|--archive|--preferences-file)
            require_value "$@"
            player_args+=("$1" "$2")
            shift 2 ;;
        --headless-check|--mute|--skip-intro|--check-navigation|--trace-input)
            player_args+=("$1"); shift ;;
        # The application owns its option grammar. Forward new game options
        # unchanged so inspection modes do not need a second parser here.
        *) player_args+=("$1"); shift ;;
    esac
done

case "$movie" in ""|[1-5]) ;; *) printf 'run.sh: --movie must be 1..5\n' >&2; exit 2 ;; esac
if [[ -n "$game_dir" ]]; then
    case "$game_dir" in /*) ;; *) game_dir="$caller_dir/$game_dir" ;; esac
fi
case "$build_dir" in /*) ;; *) build_dir="$repo_dir/$build_dir" ;; esac

# The CMake target to build and the name of the file it builds.
app_target=oa-game
app_file=open-annihilation
launch_args=()
if [[ -n "$movie" ]]; then
    if [[ -z "$game_dir" ]]; then
        printf 'run.sh: --movie plays from an installation; name it with --game-dir or OA_GAME_DIR\n' >&2
        exit 2
    fi
    # Installs mix Data and data, .zrb and .ZRB; do not rely on the file
    # system folding case.
    movie_path=""
    for candidate in "$game_dir/Data/$movie.zrb" "$game_dir/Data/$movie.ZRB" \
                     "$game_dir/data/$movie.zrb" "$game_dir/data/$movie.ZRB"; do
        if [[ -f "$candidate" ]]; then movie_path="$candidate"; break; fi
    done
    if [[ -z "$movie_path" ]]; then
        printf 'run.sh: movie %s is missing from %s/Data\n' "$movie" "$game_dir" >&2
        exit 1
    fi
    app_target=oa-intro
    app_file=oa-intro
    launch_args=("$movie_path")
elif [[ -n "$game_dir" && ( "$option_install" == 1 || "$choose_game_dir" == 0 ) ]]; then
    if [[ -f "$game_dir" ]]; then
        printf 'run.sh: the installation is a file; name the folder that holds it: %s\n' "$game_dir" >&2
        exit 1
    fi
    if [[ ! -d "$game_dir" ]]; then
        printf 'run.sh: installation directory does not exist: %s\n' "$game_dir" >&2
        exit 1
    fi
    launch_args=(--game-dir "$game_dir")
fi
if ! command -v cmake >/dev/null 2>&1; then
    printf 'run.sh: CMake 3.24+ is required.\n' >&2
    exit 1
fi

# The Python 3 that runs the bootstraps, chosen when one is first needed.
# On Windows, python3 may be the placeholder that offers to install Python
# from the Microsoft Store: it is on PATH but fails when run, so each
# command is run once before it is taken.
python=()
choose_python() {
    if [[ -n "${python[0]:-}" ]]; then return 0; fi
    local candidate
    for candidate in "python3" "python" "py -3"; do
        read -r -a python <<<"$candidate"
        if "${python[@]}" -c 'import sys; sys.exit(sys.version_info[0] != 3)' >/dev/null 2>&1; then
            return 0
        fi
    done
    python=()
    printf 'run.sh: building the dependencies under local/deps needs Python 3; none of python3, python or py -3 runs.\n' >&2
    exit 1
}

# SDL installs its CMake package in lib/cmake/SDL3, or in cmake on Windows
# unless it is built with MinGW.
sdl_prefix="$repo_dir/local/deps/sdl-install"
if [[ ! -f "$sdl_prefix/lib/cmake/SDL3/SDL3Config.cmake" && ! -f "$sdl_prefix/cmake/SDL3Config.cmake" ]]; then
    printf 'Building the pinned SDL dependency under local/deps...\n'
    choose_python
    "${python[@]}" "$repo_dir/tools/bootstrap_sdl.py"
fi
# FreeType and the fonts text outside the game's own fonts is drawn with.
freetype_prefixes=("$repo_dir"/local/deps/freetype-install-*/lib/cmake/freetype/freetype-config.cmake)
if [[ ! -f "$repo_dir/local/deps/text-fonts/build-settings.json" || ! -f "${freetype_prefixes[0]}" ]]; then
    printf 'Building the pinned FreeType and text fonts under local/deps...\n'
    choose_python
    "${python[@]}" "$repo_dir/tools/bootstrap_text_fonts.py"
fi
# Page fonts the module copies beside the game. A build with the module off
# does not read them; fetching them here keeps a clean checkout able to configure.
if [[ ! -f "$repo_dir/local/deps/fonts/liberation/LiberationSans-Regular.ttf" ]]; then
    printf 'Fetching the pinned page fonts under local/deps...\n'
    python3 "$repo_dir/tools/bootstrap_page_fonts.py" --deps "$repo_dir/local/deps"
fi
prefix_path="$sdl_prefix"
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then prefix_path="$prefix_path;$CMAKE_PREFIX_PATH"; fi
printf 'Building current native code in %s...\n' "$build_dir"
cmake -S "$repo_dir" -B "$build_dir" "-DCMAKE_BUILD_TYPE=$build_type" \
    -DOA_BUILD_PLATFORM=ON -DOA_BUILD_INTRO_PLAYER=ON \
    "-DCMAKE_PREFIX_PATH=$prefix_path"

# A failed build stops the launcher; it never runs yesterday's executable.
cmake --build "$build_dir" --config "$build_type" --target "$app_target" --parallel "${OA_BUILD_JOBS:-6}"
# On macOS the game is the application bundle open-annihilation.app. Its
# executable is started in place, so the Dock and the application switcher
# show the bundle's name and icon, and the caller's terminal keeps the output.
executable=""
for candidate in "$build_dir/$app_file.app/Contents/MacOS/$app_file" \
                 "$build_dir/$build_type/$app_file.app/Contents/MacOS/$app_file" \
                 "$build_dir/$app_file" "$build_dir/$build_type/$app_file" \
                 "$build_dir/$app_file.exe" "$build_dir/$build_type/$app_file.exe"; do
    if [[ -f "$candidate" && -x "$candidate" ]]; then executable="$candidate"; break; fi
done
if [[ -z "$executable" ]]; then
    printf 'run.sh: native player was not built; check the SDL3 development dependency.\n' >&2
    exit 1
fi
printf 'Launching %s from current source...\n' "$app_file"
# Keep the caller's working directory so relative --snapshot paths stay intuitive.
exec "$executable" ${launch_args[@]+"${launch_args[@]}"} ${player_args[@]+"${player_args[@]}"}
