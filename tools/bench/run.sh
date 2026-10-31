#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# One run of the frame bench (--bench, docs/command-line.md#--bench) in a
# real window: the game plays the view file's staged skirmish, measures it
# and prints its "bench:" lines. Every run gets fresh preferences (this
# folder's preferences.conf), a user folder, a data folder and a log folder
# inside OUT, so the player's own settings are never read or written. The
# window is 1512x917 unless --resolution says otherwise, on the Full tier,
# with 4x anti-aliasing, muted and without the intro.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
profile=none
resolution=1512x917
extra_preferences=()

usage() {
    cat <<'USAGE'
Usage: tools/bench/run.sh [options] APP VIEW OUT

  APP    the game: open-annihilation.app on macOS, or the open-annihilation
         executable
  VIEW   a view file, such as tools/bench/turning.bench
  OUT    a new folder for the run: bench.txt (the bench's lines), log.txt,
         stdout.txt, shot.png and any profile

  --profile KIND      none (default); on macOS time (xctrace's Time Profiler,
                      20 s), metal (its Metal System Trace, 10 s) or sample
                      (sample, 10 s), started as the measured span begins
  --resolution WxH    the window's size (default 1512x917)
  --preference LINE   one more preferences line, such as
                      '"open-annihilation.wreck-glow.intensity" "0"'; give it
                      once for each
  -h, --help          show this help

Environment: OA_GAME_DIR, the TA 3.1c installation the game plays (needed).
On macOS the window is captured to shot.png when the bench prints SHOT;
the first run compiles windowid.swift into OA_BENCH_CACHE (default
$TMPDIR/oa-bench).
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --profile) profile=$2; shift 2 ;;
        --resolution) resolution=$2; shift 2 ;;
        --preference) extra_preferences+=("$2"); shift 2 ;;
        --) shift; break ;;
        -*) printf 'run.sh: unknown option %s\n' "$1" >&2; exit 2 ;;
        *) break ;;
    esac
done
if [[ $# -ne 3 ]]; then
    usage >&2
    exit 2
fi
app=$1 view=$2 out=$3
: "${OA_GAME_DIR:?run.sh: set OA_GAME_DIR to the TA 3.1c installation}"
case "$profile" in
    none|time|metal|sample) ;;
    *) printf 'run.sh: --profile takes none, time, metal or sample\n' >&2; exit 2 ;;
esac
if [[ -d "$app" ]]; then
    game="$app/Contents/MacOS/open-annihilation"
else
    game="$app"
fi
[[ -x "$game" ]] || { printf 'run.sh: %s is not the game\n' "$app" >&2; exit 2; }
[[ -f "$view" ]] || { printf 'run.sh: no view file %s\n' "$view" >&2; exit 2; }
if [[ -e "$out" ]]; then
    printf 'run.sh: %s already exists\n' "$out" >&2
    exit 2
fi
view="$(cd -- "$(dirname -- "$view")" && pwd)/$(basename -- "$view")"
mkdir -p "$out/user" "$out/data" "$out/logs"
out="$(cd -- "$out" && pwd)"
cp "$here/preferences.conf" "$out/user/preferences.conf"
for line in "${extra_preferences[@]+"${extra_preferences[@]}"}"; do
    printf '%s\n' "$line" >> "$out/user/preferences.conf"
done

# The window's number, for screencapture: the helper is compiled once.
windowid=""
if [[ "$(uname -s)" == Darwin ]] && command -v swiftc > /dev/null; then
    cache="${OA_BENCH_CACHE:-${TMPDIR:-/tmp}/oa-bench}"
    mkdir -p "$cache"
    windowid="$cache/windowid"
    if [[ ! -x "$windowid" || "$here/windowid.swift" -nt "$windowid" ]]; then
        swiftc -O -o "$windowid.new" "$here/windowid.swift" > "$cache/windowid.log" 2>&1 &&
            mv "$windowid.new" "$windowid" || windowid=""
    fi
fi

# The bench needs the system's own video driver.
unset SDL_VIDEO_DRIVER SDL_RENDER_DRIVER
"$game" --game-dir "$OA_GAME_DIR" --skip-intro --mute --bench "$view" \
    --preferences-file "$out/user/preferences.conf" --data-dir "$out/data" \
    --log-dir "$out/logs" --hardware-acceleration=full --resolution "$resolution" \
    > "$out/stdout.txt" 2>&1 &
pid=$!
printf 'run.sh: %s on %s, pid %s\n' "$(basename -- "$view")" "$app" "$pid"

# The bench's lines are followed in the game's log as it writes them.
log=""
profiled=0 shot=0
while kill -0 "$pid" 2> /dev/null; do
    if [[ -z "$log" ]]; then
        log=$(ls "$out"/logs/*.log 2> /dev/null | head -1 || true)
    fi
    if [[ -n "$log" ]]; then
        if [[ $profiled == 0 ]] && grep -q '^bench: MEASURING' "$log"; then
            profiled=1
            case "$profile" in
                time)
                    xcrun xctrace record --template 'Time Profiler' --attach "$pid" \
                        --time-limit 20s --output "$out/time.trace" > "$out/xctrace.txt" 2>&1 &
                    ;;
                metal)
                    xcrun xctrace record --template 'Metal System Trace' --attach "$pid" \
                        --time-limit 10s --output "$out/metal.trace" > "$out/xctrace.txt" 2>&1 &
                    ;;
                sample)
                    (sleep 2; sample "$pid" 10 -file "$out/sample.txt" > /dev/null 2>&1) &
                    ;;
            esac
        fi
        if [[ $shot == 0 ]] && grep -q '^bench: SHOT' "$log"; then
            shot=1
            if [[ -n "$windowid" ]]; then
                id=$("$windowid" "$pid" || true)
                if [[ -n "$id" && "$id" != 0 ]]; then
                    screencapture -x -o -l"$id" "$out/shot.png" || true
                fi
            fi
        fi
    fi
    sleep 0.2
done
status=0
wait "$pid" || status=$?
wait
[[ -z "$log" ]] && log=$(ls "$out"/logs/*.log 2> /dev/null | head -1 || true)
if [[ -n "$log" ]]; then
    cp "$log" "$out/log.txt"
    grep '^bench: ' "$log" > "$out/bench.txt" || true
fi
grep -E '^bench: (RESULT|MEASURE frame)' "$out/bench.txt" 2> /dev/null || true
printf 'run.sh: the game exited with %s\n' "$status"
exit "$status"
