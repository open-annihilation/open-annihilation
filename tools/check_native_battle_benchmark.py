#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check seeded battle reports, workload modes, renderer independence and refusals."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))


def play(native, game_dir, workdir, name, options=(), report=None):
    """Run a small simulation-only battle and return its process and JSON report."""
    report = report or workdir / f"{name}.json"
    command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
               "--preferences-file", str(workdir / f"{name}.conf"),
               "--battle-benchmark", str(report), "--benchmark-units", "24",
               "--benchmark-warmup", "3", "--match-ticks", "12", "--seed", "7", *options]
    env = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    process = subprocess.run(command, cwd=workdir, env=env, check=False,
                             capture_output=True, text=True, timeout=180)
    value = json.loads(report.read_text(encoding="utf-8")) if report.exists() else None
    return process, value


def require(condition, message, process=None):
    """Stop with the command output when an expected fact does not hold."""
    if condition:
        return
    if process:
        print(process.stdout + process.stderr, end="")
    raise SystemExit(message)


def valid_report(process, report, scenario, enabled, ticks=12):
    """Check completion, population, rules metadata and ordered tick statistics."""
    require(process.returncode == 0 and report is not None, "benchmark failed", process)
    require(report["schema_version"] == 1 and report["completed"] and not report["error"],
            "benchmark did not complete", process)
    require(report["scenario"] == scenario and report["seed"] == 7,
            "benchmark lost its workload or seed")
    require(report["requested_units_per_side"] == 24 and report["initial_units"] == 48,
            "benchmark did not place the requested armies")
    require(report["successful_warmup_ticks"] == 3 and report["successful_ticks"] == ticks,
            "benchmark did not run every requested tick")
    require(report["enhanced_mode"] is enabled and report["enhanced_version"] == int(enabled),
            "benchmark did not record its Enhanced rules")
    require(report["renderer"] == "none", "simulation-only benchmark named a drawn renderer")
    require(len(report["simulation_hash"]) == 64 and len(report["world_digest"]) == 16,
            "benchmark lacks its reproducibility digests")
    require(report["activity_sampling_scope"] == "after_each_successful_warmup_and_measured_tick",
            "benchmark did not state when activity is sampled")
    if scenario in ("idle", "march"):
        require(report["projectile_high_water"] == 0,
                "noncombat workload fired a projectile")
    stats = report["tick_ns"]
    require(stats["count"] == ticks and
            0 < stats["min"] <= stats["p50"] <= stats["p95"] <= stats["p99"] <= stats["max"],
            "benchmark timing statistics are incomplete or unordered")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    native, game_dir = args.native.resolve(), args.game_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="battle-benchmark-", dir=args.scratch_root) as folder:
        workdir = Path(folder)
        for enabled in (False, True):
            mode = "--enhanced-mode" if enabled else "--no-enhanced-mode"
            reference = None
            for index, graphics in enumerate(("off", "full")):
                if index:
                    settings = {
                        "Total Annihilation|SkirmishCommanderDeath": "0",
                        "Total Annihilation|SkirmishMapping": "0",
                        "Total Annihilation|SkirmishLineOfSight": "0",
                        "Total Annihilation|SkirmishLOSType": "0",
                        "Total Annihilation|SkirmishLocation": "0",
                        "Total Annihilation\\Skirmish|Player0Metal": "9000",
                        "Total Annihilation\\Skirmish|Player1Energy": "9000",
                    }
                    profile = workdir / f"idle-{enabled}-{index}.conf"
                    lines = ["open-annihilation-preferences 1"]
                    lines.extend(f'{json.dumps(key)} {json.dumps(value)}' for key, value in settings.items())
                    profile.write_text("\n".join(lines) + "\n", encoding="utf-8")
                process, report = play(native, game_dir, workdir, f"idle-{enabled}-{index}",
                                       (mode, "--benchmark-scenario", "idle",
                                        f"--hardware-acceleration={graphics}"))
                valid_report(process, report, "idle", enabled)
                if reference:
                    require(report["world_digest"] == reference["world_digest"] and
                            report["simulation_hash"] == reference["simulation_hash"],
                            "seeded simulation changes with graphics selection")
                reference = report
        for scenario in ("march", "combat"):
            process, report = play(native, game_dir, workdir, scenario,
                                   ("--enhanced-mode", "--benchmark-scenario", scenario,
                                    "--benchmark-map", "Ashap Plateau", "--match-ticks", "120"))
            valid_report(process, report, scenario, True, ticks=120)
            require(report["activity_observed"], f"{scenario} fixture did no corresponding work")
            if scenario == "march":
                require(report["moved_unit_ticks"] > 0, "march fixture did not move")
            else:
                require(report["projectile_high_water"] > 0 or report["damaged_unit_ticks"] > 0,
                        "combat fixture did not fire or damage units")
        invalid_path = workdir / "capacity.json"
        process, report = play(native, game_dir, workdir, "capacity",
                               ("--benchmark-units", "10000"), invalid_path)
        require(process.returncode != 0 and report is None,
                "unsupported population produced a successful report", process)
        stale_path = workdir / "stale.json"
        stale_path.write_text('{"completed": true}\n', encoding="utf-8")
        process, report = play(native, game_dir, workdir, "missing-map",
                               ("--benchmark-map", "OA benchmark missing map"), stale_path)
        require(process.returncode != 0 and report is not None and
                report["completed"] is False and report["error"] and report["successful_ticks"] == 0,
                "failed setup left a stale successful report", process)
    print("battle benchmark: seeded workloads, mode metadata, graphics independence and failures checked")


if __name__ == "__main__":
    main()
