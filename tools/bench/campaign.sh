#!/usr/bin/env bash
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Runs the frame bench on one or more builds back to back, each view in
# turn, with run.sh, so that the builds meet the same machine, the same
# temperature and the same background: in odd rounds the builds go in the
# order given, in even rounds the other way round. A run whose folder exists
# is skipped, so a campaign that stopped carries on where it stopped. At the
# end summarise.py writes the runs' table to OUTROOT/summary.md and prints it.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
rounds=2
views=()
run_options=()
# The longest a run may take, in seconds, before it is stopped.
run_timeout=600

usage() {
    cat <<'USAGE'
Usage: tools/bench/campaign.sh [options] OUTROOT NAME=APP [NAME=APP...]

  OUTROOT   a folder for the runs: OUTROOT/NAME-VIEW-ROUND for each
  NAME=APP  a build to measure, named NAME in the runs' folders: the
            open-annihilation.app bundle, or the executable

  --runs N            rounds of every view on every build (default 2)
  --view VIEW         a view to run: a view file, or the name of one of this
                      folder's (low-across, turning, far-out, close, idle);
                      give it once for each (default: those five)
  --profile KIND      run.sh's --profile for every run
  --resolution WxH    run.sh's --resolution for every run
  --preference LINE   run.sh's --preference for every run
  -h, --help          show this help

Environment: OA_GAME_DIR, the TA 3.1c installation the game plays (needed).
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --runs) rounds=$2; shift 2 ;;
        --view) views+=("$2"); shift 2 ;;
        --profile|--resolution|--preference) run_options+=("$1" "$2"); shift 2 ;;
        --) shift; break ;;
        -*) printf 'campaign.sh: unknown option %s\n' "$1" >&2; exit 2 ;;
        *) break ;;
    esac
done
if [[ $# -lt 2 ]]; then
    usage >&2
    exit 2
fi
if [[ ! "$rounds" =~ ^[1-9][0-9]*$ ]]; then
    printf 'campaign.sh: --runs takes a number of rounds from 1\n' >&2
    exit 2
fi
root=$1
shift
names=() apps=()
for build in "$@"; do
    if [[ "$build" != *=* || -z "${build%%=*}" || -z "${build#*=}" ]]; then
        printf 'campaign.sh: a build is NAME=APP, not %s\n' "$build" >&2
        exit 2
    fi
    names+=("${build%%=*}")
    apps+=("${build#*=}")
done
[[ ${#views[@]} -eq 0 ]] && views=(low-across turning far-out close idle)
view_files=()
for view in "${views[@]}"; do
    if [[ -f "$view" ]]; then
        view_files+=("$view")
    elif [[ -f "$here/$view.bench" ]]; then
        view_files+=("$here/$view.bench")
    else
        printf 'campaign.sh: no view %s\n' "$view" >&2
        exit 2
    fi
done
runner=()
command -v timeout > /dev/null && runner=(timeout "$run_timeout")
mkdir -p "$root"

for ((round = 1; round <= rounds; ++round)); do
    order=("${!names[@]}")
    if ((round % 2 == 0)); then
        reversed=()
        for ((index = ${#order[@]} - 1; index >= 0; --index)); do
            reversed+=("${order[index]}")
        done
        order=("${reversed[@]}")
    fi
    for view in "${view_files[@]}"; do
        for build in "${order[@]}"; do
            tag="${names[build]}-$(basename -- "$view" .bench)-$round"
            out="$root/$tag"
            if [[ -e "$out" ]]; then
                printf '%s: exists, skipped\n' "$tag"
                continue
            fi
            start=$(date +%s)
            status=0
            "${runner[@]+"${runner[@]}"}" "$here/run.sh" "${run_options[@]+"${run_options[@]}"}" \
                "${apps[build]}" "$view" "$out" > "$root/$tag.run.txt" 2>&1 < /dev/null || status=$?
            mkdir -p "$out"
            mv "$root/$tag.run.txt" "$out/run.txt"
            summary=$(grep -E '^bench: (RESULT|MEASURE frame)' "$out/bench.txt" 2> /dev/null |
                tr -s ' ' | tr '\n' '|' || true)
            printf '%s: %ss, exit %s %s\n' "$tag" "$(($(date +%s) - start))" "$status" "$summary"
        done
    done
done
python3 "$here/summarise.py" "$root" | tee "$root/summary.md"
