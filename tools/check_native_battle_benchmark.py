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


def valid_report(process, report, scenario, enabled, ticks=12, version=None):
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
    require(report["enhanced_mode"] is enabled and
            (report["enhanced_version"] > 0 if enabled else report["enhanced_version"] == 0),
            "benchmark did not record its Enhanced rules")
    if version is not None:
        require(report["enhanced_version"] == version, "benchmark selected the wrong rules version")
    require(report["renderer"] == "none", "simulation-only benchmark named a drawn renderer")
    require(len(report["simulation_hash"]) == 64 and len(report["world_digest"]) == 16,
            "benchmark lacks its reproducibility digests")
    require(report["activity_sampling_scope"] == "after_each_successful_warmup_and_measured_tick",
            "benchmark did not state when activity is sampled")
    if scenario in ("idle", "march", "group-march"):
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
        group_reference = None
        work_keys = ("shared_route_requests", "shared_route_hits", "shared_route_fields",
                     "shared_route_cells", "shared_route_waits", "shared_route_invalidations",
                     "local_flow_probes", "local_flow_moving_blockers",
                     "local_flow_steering_adjustments", "local_flow_yielded_ticks",
                     "local_flow_blocked_ticks", "local_flow_deferred_replans")
        for version in (1, 2, 2):
            name = f"group-{version}-{'repeat' if group_reference else 'first'}"
            process, report = play(native, game_dir, workdir, name,
                                   ("--enhanced-mode", "--benchmark-rules-version", str(version),
                                    "--benchmark-scenario", "group-march", "--benchmark-map",
                                    "Ashap Plateau", "--match-ticks", "120"))
            valid_report(process, report, "group-march", True, ticks=120, version=version)
            require(report["goal_metrics_available"] and len(report["goal_progress"]) == 2,
                    "group movement lacks per-army progress")
            require(report["damaged_unit_ticks"] == 0, "hold-fire group movement damaged a unit")
            require(report["routing_metrics_scope"] == "cumulative_setup_warmup_and_measured",
                    "route work did not state its sampling scope")
            for side in report["goal_progress"]:
                require(side["goal_units"] == 24 and side["live_units"] == 24,
                        "group movement lost an assigned unit")
                require(side["moved_units"] >= 2 and side["progressed_units"] >= 2,
                        "group activity could be explained by one moving or jittering unit")
                require(sum(third["goal_units"] for third in side["distance_thirds"]) == 24,
                        "group movement lost its trip-distance fairness bins")
            if version == 1:
                require(report["shared_route_hits"] == 0 and report["local_flow_probes"] == 0,
                        "v1 acquired the new routing or local-flow behavior")
                continue
            require(report["shared_route_hits"] > 0 and 0 < report["shared_route_fields"] < 48,
                    "v2 group movement did not share route construction")
            if group_reference:
                require(report["world_digest"] == group_reference["world_digest"] and
                        report["simulation_hash"] == group_reference["simulation_hash"] and
                        report["goal_progress"] == group_reference["goal_progress"] and
                        all(report[key] == group_reference[key] for key in work_keys),
                        "seeded v2 group movement or work counts changed on repeat")
            group_reference = report
        process, report = play(native, game_dir, workdir, "version-with-mode-off",
                               ("--no-enhanced-mode", "--benchmark-rules-version", "1"))
        require(process.returncode != 0 and report is not None and not report["completed"],
                "an explicit Enhanced preset silently enabled the mode", process)
        invalid_path = workdir / "capacity.json"
        process, report = play(native, game_dir, workdir, "capacity",
                               ("--no-enhanced-mode", "--benchmark-units", "10000"), invalid_path)
        require(process.returncode != 0 and report is not None and
                not report["completed"] and report["successful_ticks"] == 0,
                "classic mode accepted an Enhanced population", process)
        stale_path = workdir / "stale.json"
        stale_path.write_text('{"completed": true}\n', encoding="utf-8")
        process, report = play(native, game_dir, workdir, "missing-map",
                               ("--benchmark-map", "OA benchmark missing map"), stale_path)
        require(process.returncode != 0 and report is not None and
                report["completed"] is False and report["error"] and report["successful_ticks"] == 0,
                "failed setup left a stale successful report", process)
    print("battle benchmark: seeded workloads, group flow, preset versions, graphics independence and failures checked")


if __name__ == "__main__":
    main()
