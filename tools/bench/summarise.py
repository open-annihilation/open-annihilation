#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Print the frame bench's runs as Markdown tables.

Each PATH is a run's folder (run.sh's OUT, holding bench.txt or log.txt), a
file of a run's lines, or a folder of runs, such as campaign.sh's. A run is
named by its folder or file: OUT/bench.txt is OUT's name. The first table
has a row for each run, by view: its frame rate and 1% low frame rate, the
frame's mean, median, 95th and 99th percentiles, slowest 1% and longest
time, the drawing's mean time, the tick's mean, the match ticks a second
(30 at normal speed), the live units at the end, and a note when the
window was away for some of the span. --stages adds a table of the stage timers' milliseconds a frame, a
column for each run, and --counts one of the scene's counts.

The lines of the bench's first form, which began "P02 " and printed no
median, 99th percentile or slowest 1%, are read too; their missing figures
show as "-".
"""
import argparse
import re
import sys
from pathlib import Path

NUMBER = r"(-?\d+(?:\.\d+)?)"
PREFIX = r"^(?:bench:|P02)"
RESULT = re.compile(
    PREFIX + rf" RESULT label=(\S+) seconds {NUMBER} frames (\d+) fps {NUMBER}"
    rf"(?: low1_fps {NUMBER})? ticks (\d+) \({NUMBER}/s\)")
MEASURE = re.compile(
    PREFIX + rf" MEASURE (.+?)\s+min\s+{NUMBER} avg\s+{NUMBER}(?: p50\s+{NUMBER})? p95\s+{NUMBER}"
    rf"(?: p99\s+{NUMBER})?(?: low1\s+{NUMBER})? max\s+{NUMBER}$")
TICK = re.compile(PREFIX + rf" MEASURE tick\(per tick\)\s+avg\s+{NUMBER}$")
AWAY = re.compile(PREFIX + r" AWAY label=\S+ frames (\d+) of (\d+)")
STAGE = re.compile(PREFIX + rf" STAGE (.+?)\s+{NUMBER}$")
COUNTS = PREFIX + " COUNTS "
FIELDS = ("least", "mean", "p50", "p95", "p99", "low1", "most")
SCENE_COUNTS = ("units", "drawn", "vertices", "terrain", "models", "sprites", "particles", "nano",
                "shadows", "crop_cells", "projectiles", "supersample", "atlas_rows",
                "full_atlas_uploads")


def read_run(path):
    """Returns a run's figures from its lines, or None when it has no RESULT line."""
    run = {"measure": {}, "stage": {}, "counts": {}}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if match := RESULT.match(line):
            label, seconds, frames, fps, low1_fps, ticks, rate = match.groups()
            run.update(label=label, seconds=float(seconds), frames=int(frames), fps=float(fps),
                       low1_fps=float(low1_fps) if low1_fps else None, ticks=int(ticks),
                       tick_rate=float(rate))
        elif match := AWAY.match(line):
            run["away"] = f"window away {match.group(1)} of {match.group(2)} frames"
        elif match := TICK.match(line):
            run["tick"] = float(match.group(1))
        elif match := MEASURE.match(line):
            values = [float(value) if value is not None else None for value in match.groups()[1:]]
            run["measure"][match.group(1).strip()] = dict(zip(FIELDS, values))
        elif match := STAGE.match(line):
            # The nested stages keep their two spaces, so that the table
            # shows them under the stage they belong to.
            run["stage"][match.group(1).rstrip()] = float(match.group(2))
        elif re.match(COUNTS, line):
            words = line.split()[2:]
            for key, value in zip(words, words[1:]):
                if key.isidentifier() and re.fullmatch(r"-?\d+(?:\.\d+)?", value):
                    run["counts"].setdefault(key, float(value))
    return run if "fps" in run else None


def run_files(paths):
    """Returns (name, file) for each run the paths hold."""
    found = []
    for path in paths:
        if path.is_file():
            name = path.parent.name if path.name in ("bench.txt", "log.txt") else path.stem
            found.append((name, path))
        elif (path / "bench.txt").is_file() or (path / "log.txt").is_file():
            found.append((path.name, path / "bench.txt" if (path / "bench.txt").is_file()
                          else path / "log.txt"))
        elif path.is_dir():
            found.extend(run_files(sorted(child for child in path.iterdir() if child.is_dir())))
        else:
            raise SystemExit(f"summarise.py: no run at {path}")
    return found


def shown(value, places=1):
    return "-" if value is None else f"{value:.{places}f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="+", type=Path, metavar="PATH")
    parser.add_argument("--stages", action="store_true", help="add the stage timers' table")
    parser.add_argument("--counts", action="store_true", help="add the scene counts' table")
    arguments = parser.parse_args()
    runs = []
    for name, path in run_files(arguments.paths):
        run = read_run(path)
        if run is None:
            print(f"summarise.py: {path} holds no finished run", file=sys.stderr)
            continue
        runs.append((name, run))
    if not runs:
        raise SystemExit("summarise.py: no finished runs")
    runs.sort(key=lambda entry: (entry[1]["label"], entry[0]))
    print("| view | run | FPS | 1% low FPS | frame avg | p50 | p95 | p99 | 1% low | max | draw avg "
          "| tick | ticks/s | units | note |")
    print("|---|---|" + "---:|" * 12 + "---|")
    for name, run in runs:
        frame = run["measure"].get("frame", {})
        draw = run["measure"].get("draw", {})
        print(f"| {run['label']} | {name} | {run['fps']:.1f} | {shown(run['low1_fps'])} | "
              f"{shown(frame.get('mean'))} | {shown(frame.get('p50'))} | "
              f"{shown(frame.get('p95'))} | {shown(frame.get('p99'))} | "
              f"{shown(frame.get('low1'))} | {shown(frame.get('most'))} | "
              f"{shown(draw.get('mean'))} | {shown(run.get('tick'), 2)} | "
              f"{run['tick_rate']:.1f} | {shown(run['counts'].get('units'), 0)} | "
              f"{run.get('away', '')} |")
    if arguments.stages:
        names = []
        for _, run in runs:
            names.extend(name for name in run["stage"] if name not in names)
        print()
        print("| ms a frame | " + " | ".join(name for name, _ in runs) + " |")
        print("|---|" + "---:|" * len(runs))
        for stage in names:
            print(f"| {stage.replace('  ', '- ', 1) if stage.startswith('  ') else stage} | " +
                  " | ".join(shown(run["stage"].get(stage), 2) for _, run in runs) + " |")
    if arguments.counts:
        print()
        print("| count | " + " | ".join(name for name, _ in runs) + " |")
        print("|---|" + "---:|" * len(runs))
        for key in SCENE_COUNTS:
            print(f"| {key} | " + " | ".join(shown(run["counts"].get(key), 0) for _, run in runs)
                  + " |")


if __name__ == "__main__":
    main()
