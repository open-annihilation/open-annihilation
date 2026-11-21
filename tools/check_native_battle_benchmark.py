#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check seeded battle reports, workload modes, staged battles, renderer independence and refusals."""

import argparse
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import native_offline

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
TICK_BUDGET_NS = 33_333_333
MAXIMUM_UINT64 = (1 << 64) - 1


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


def valid_timing(stats, count, label, allow_zero=True):
    """Check ranks, cumulative cost and strict budget counts without assuming clock resolution."""
    require(isinstance(stats, dict), f"{label} timing is not an object")
    keys = ("count", "min", "p50", "p95", "p99", "max", "total", "over_budget")
    require(all(type(stats.get(key)) is int and 0 <= stats[key] <= MAXIMUM_UINT64
                for key in keys), f"{label} timing has invalid integer facts")
    require(type(stats.get("total_saturated")) is bool,
            f"{label} timing lacks explicit total saturation")
    require(stats["count"] == count and
            0 <= stats["min"] <= stats["p50"] <= stats["p95"] <= stats["p99"] <= stats["max"],
            f"{label} timing statistics are incomplete or unordered")
    require(type(stats.get("mean")) in (int, float) and math.isfinite(stats["mean"]),
            f"{label} timing has no finite mean")
    if count == 0:
        require(all(stats[key] == 0 for key in keys) and stats["mean"] == 0 and
                not stats["total_saturated"], f"{label} empty timing is not zero-filled")
        return
    require(allow_zero or stats["min"] > 0, f"{label} wall timing is empty")
    require(stats["min"] <= stats["mean"] <= stats["max"] and
            stats["over_budget"] <= count, f"{label} timing facts contradict their bounds")
    if stats["total_saturated"]:
        require(stats["total"] == MAXIMUM_UINT64,
                f"{label} saturated timing total did not stop at uint64 maximum")
    else:
        require(count * stats["min"] <= stats["total"] <= count * stats["max"] and
                math.isclose(stats["mean"], stats["total"] / count, rel_tol=1e-12, abs_tol=1e-6),
                f"{label} cumulative timing contradicts its mean or range")
        require(stats["total"] >= stats["over_budget"] * (TICK_BUDGET_NS + 1),
                f"{label} long ticks exceed their cumulative cost")
    if stats["max"] <= TICK_BUDGET_NS:
        require(stats["over_budget"] == 0, f"{label} counted a tick at or below the budget")
    else:
        require(stats["over_budget"] > 0, f"{label} omitted its long maximum tick")
    if stats["min"] > TICK_BUDGET_NS:
        require(stats["over_budget"] == count, f"{label} omitted ticks above its budget")
    else:
        require(stats["over_budget"] < count, f"{label} counted every tick as above budget")


def valid_timing_report(report):
    """Keep wall elapsed timing distinct from optional calling-thread CPU observations."""
    require(report["tick_budget_ns"] == TICK_BUDGET_NS, "benchmark changed its tick budget")
    count = report["successful_ticks"]
    valid_timing(report["tick_ns"], count, "wall", allow_zero=False)
    available = report["thread_cpu_available"]
    require(type(available) is bool, "benchmark CPU availability is not explicit")
    clock = report["thread_cpu_clock"]
    status = report["thread_cpu_status"]
    if available:
        require(status == "available" and clock in ("GetThreadTimes", "CLOCK_THREAD_CPUTIME_ID"),
                "benchmark CPU timing lacks a supported clock")
        # Coarse thread clocks can report zero, and sampling overhead means CPU
        # and wall intervals need not obey a strict CPU <= wall comparison.
        valid_timing(report["tick_thread_cpu_ns"], count, "thread CPU")
    else:
        require(report["tick_thread_cpu_ns"] is None and status in ("unsupported", "read_failed"),
                "unavailable CPU timing was filled with substitute samples")
        require((status == "unsupported" and clock == "unsupported") or
                (status == "read_failed" and clock in ("GetThreadTimes", "CLOCK_THREAD_CPUTIME_ID")),
                "benchmark CPU unavailability lacks its clock reason")
    for phase, timing in report["simulation_phase_ns"].items():
        valid_timing(timing, count, f"{phase} phase")


def valid_report(process, report, scenario, enabled, ticks=12, version=None, feature_spec=""):
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
    names = report.get("enhanced_features")
    require(isinstance(names, list) and all(isinstance(name, str) for name in names),
            "benchmark did not name the features it played")
    require(report.get("enhanced_features_spec") == feature_spec,
            "benchmark did not record the feature specification")
    require(report.get("enhanced_features_" + "override") is (feature_spec != ""),
            "benchmark did not record whether the feature flag chose the set")
    require(type(report.get("data_faults")) is int and report["data_faults"] >= 0,
            "benchmark did not count its data faults")
    if not enabled and feature_spec == "":
        require(names == [], "a match without Enhanced Mode named features")
    staged = report.get("staged_battle")
    if isinstance(staged, dict):
        require(staged.get("extra") == {}, "staged battle did not leave its extra object empty")
    require(report["renderer"] == "none", "simulation-only benchmark named a drawn renderer")
    require(len(report["simulation_hash"]) == 64 and len(report["world_digest"]) == 16,
            "benchmark lacks its reproducibility digests")
    require(report["activity_sampling_scope"] == "after_each_successful_warmup_and_measured_tick",
            "benchmark did not state when activity is sampled")
    if scenario in ("idle", "march", "group-march"):
        require(report["projectile_high_water"] == 0,
                "noncombat workload fired a projectile")
    valid_timing_report(report)


# A small staged battle on Ashap Plateau, whose fixed start positions put the
# ARM commander (player 0) at 3504,304 and the CORE commander (player 1) at
# 848,3696: twelve troops beside each, each army patrolling to the other
# commander, CORE a computer player. The measure line lies across ARM's path.
STAGED_SETTINGS = {
    "open-annihilation.first-run": "done",
    "open-annihilation.unit-limit": "250",
    "Total Annihilation\\Skirmish|Player0Controller": "1",
    "Total Annihilation\\Skirmish|Player0Side": "0",
    "Total Annihilation\\Skirmish|Player1Controller": "2",
    "Total Annihilation\\Skirmish|Player1Side": "1",
    "Total Annihilation|NumSkirmishPlayers": "2",
    "Total Annihilation|SkirmishMap": "Ashap Plateau",
    "Total Annihilation|SkirmishLocation": "1",
}
STAGED_LINE = "ahead=2213,383,3787,1617"
STAGED_TICKS = 900


def staged_stage():
    """The stage text: two groups of twelve troops with their commanders, Fire at Will, patrols."""
    lines = ["group arm", "group-add 1"]
    lines += [f"unit 0 ARMPW {dx} {dz}" for dx in (-144, -96, -48, 0) for dz in (64, 112, 160)]
    lines += ["group core", "group-add 251"]
    lines += [f"unit 1 CORAK {dx} {dz}" for dx in (0, 48, 96, 144) for dz in (-64, -112, -160)]
    lines += ["group", "fire-order arm 2", "fire-order core 2",
              "patrol arm 848 3696", "patrol core 3504 304"]
    return "\n".join(lines) + "\n"


def play_staged(native, game_dir, workdir, name, options=()):
    """Run the stage scenario with its own preferences and return its process and report."""
    preferences = workdir / f"{name}.conf"
    lines = native_offline.preferences_text().splitlines()
    lines.extend(f"{json.dumps(key)} {json.dumps(value)}" for key, value in STAGED_SETTINGS.items())
    preferences.write_text("\n".join(lines) + "\n", encoding="utf-8")
    stage = workdir / "staged.stage"
    stage.write_text(staged_stage(), encoding="utf-8")
    report = workdir / f"{name}.json"
    command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
               "--preferences-file", str(preferences), "--battle-benchmark", str(report),
               "--benchmark-scenario", "stage", "--stage", str(stage), "--benchmark-warmup", "0",
               "--match-ticks", str(STAGED_TICKS), "--seed", "7", *options]
    env = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    process = subprocess.run(command, cwd=workdir, env=env, check=False,
                             capture_output=True, text=True, timeout=300)
    value = json.loads(report.read_text(encoding="utf-8")) if report.exists() else None
    return process, value


def staged_battles(native, game_dir, workdir):
    """Check the stage scenario's report, its repeatability and its refusals."""
    reports = []
    for run in (1, 2):
        process, report = play_staged(native, game_dir, workdir, f"staged-{run}",
                                      ("--enhanced-mode", "--benchmark-rules-version", "6",
                                       "--benchmark-crossing", STAGED_LINE))
        require(process.returncode == 0 and report is not None and report["completed"],
                "staged battle failed", process)
        require(report["scenario"] == "stage" and report["map"] == "Ashap Plateau" and
                report["requested_units_per_side"] == 0 and report["initial_units"] == 26 and
                report["successful_ticks"] == STAGED_TICKS and report["enhanced_version"] == 6,
                "staged battle lost its map, population, ticks or rules", process)
        valid_timing_report(report)
        staged = report["staged_battle"]
        require(isinstance(staged, dict) and staged["window_ticks"] == 300 and
                [line["name"] for line in staged["lines"]] == ["ahead"],
                "staged battle lacks its window or measure line")
        players = staged["players"]
        require([player["player"] for player in players] == [0, 1] and
                [player["computer"] for player in players] == [False, True],
                "staged battle lost its players or its computer player")
        for player in players:
            require(player["tracked_units"] == 13 and player["goal_units"] == 13,
                    "staged battle did not follow every staged unit to its patrol")
            require(player["window_live_unit_ticks"] <= 13 * 300 and
                    player["window_stationary_unit_ticks"] <= player["window_live_unit_ticks"],
                    "stationary counts exceed the window")
            require(len(player["crossers"]) == 1, "staged battle lost a line's crossers")
        require(players[0]["mean_progress_pixels"] > 100 and players[0]["advanced_units"] > 0,
                "the staged ARM army made no progress toward its patrol")
        require(players[0]["crossers"][0] > 0, "no ARM unit crossed the line across its path")
        audit = staged["ownership_audit"]
        require(audit["audits"] == STAGED_TICKS // 150 + 1 and audit["audited_units"] > 0 and
                audit["ownership_errors"] == 0 and audit["overlap_cells"] == 0 and
                audit["first_error_tick"] is None,
                "staged battle's ownership audits did not all run clean")
        reports.append(report)
    require(reports[0]["world_digest"] == reports[1]["world_digest"] and
            reports[0]["staged_battle"] == reports[1]["staged_battle"],
            "a repeated staged battle measured differently")
    process, report = play_staged(native, game_dir, workdir, "staged-bad-line",
                                  ("--enhanced-mode", "--benchmark-crossing", "bad=1,2,3"))
    require(process.returncode != 0 and report is not None and not report["completed"] and
            "--benchmark-crossing" in report["error"] and report["successful_ticks"] == 0,
            "a malformed measure line was accepted", process)
    for refused, message in (
            ((), "--benchmark-scenario stage needs --stage"),
            (("--stage", "staged.stage", "--benchmark-units", "24"),
             "--benchmark-scenario stage cannot be used with --benchmark-units or --benchmark-map")):
        command = [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
                   "--battle-benchmark", str(workdir / "refused.json"),
                   "--benchmark-scenario", "stage", *refused]
        process = subprocess.run(command, cwd=workdir, check=False, capture_output=True, text=True,
                                 timeout=120)
        require(process.returncode != 0 and message in process.stdout + process.stderr,
                f"the stage scenario did not refuse: {message}", process)


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
                    lines = native_offline.preferences_text().splitlines()
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
        group_references = {}
        work_keys = ("shared_route_requests", "shared_route_hits", "shared_route_fields",
                     "shared_route_cells", "shared_route_waits", "shared_route_invalidations",
                     "full_route_searches", "target_queries", "target_candidates",
                     "target_full_scans",
                     "local_flow_probes", "local_flow_moving_blockers",
                     "local_flow_steering_adjustments", "local_flow_yielded_ticks",
                     "local_flow_blocked_ticks", "local_flow_deferred_replans")
        for version in (1, 2, 2, 3, 3, 4, 4, 5, 5, 10, 10, 11, 11, 13, 13):
            name = f"group-{version}-{'repeat' if version in group_references else 'first'}"
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
                    f"v{version} group movement did not share route construction")
            group_reference = group_references.get(version)
            if group_reference:
                require(report["world_digest"] == group_reference["world_digest"] and
                        report["simulation_hash"] == group_reference["simulation_hash"] and
                        report["goal_progress"] == group_reference["goal_progress"] and
                        all(report[key] == group_reference[key] for key in work_keys),
                        f"seeded v{version} group movement or work counts changed on repeat")
            group_references[version] = report
        process, report = play(native, game_dir, workdir, "version-with-mode-off",
                               ("--no-enhanced-mode", "--benchmark-rules-version", "1"))
        require(process.returncode != 0 and report is not None and not report["completed"],
                "an explicit Enhanced preset silently enabled the mode", process)
        valid_timing_report(report)
        invalid_path = workdir / "capacity.json"
        process, report = play(native, game_dir, workdir, "capacity",
                               ("--no-enhanced-mode", "--benchmark-units", "10000"), invalid_path)
        require(process.returncode != 0 and report is not None and
                not report["completed"] and report["successful_ticks"] == 0,
                "classic mode accepted an Enhanced population", process)
        valid_timing_report(report)
        stale_path = workdir / "stale.json"
        stale_path.write_text('{"completed": true}\n', encoding="utf-8")
        process, report = play(native, game_dir, workdir, "missing-map",
                               ("--benchmark-map", "OA benchmark missing map"), stale_path)
        require(process.returncode != 0 and report is not None and
                report["completed"] is False and report["error"] and report["successful_ticks"] == 0,
                "failed setup left a stale successful report", process)
        valid_timing_report(report)
        # Version 13 without cardinal queue escape is version 10: 22 names, and the
        # rules hold is version 1 because the feature flag chose the set.
        version_10 = [
            "capacity", "local_flow", "routes_fine_regions", "indexed_searches",
            "route_corner_advance", "swept_ground_steps", "patrol_goal_retention",
            "ai_strike_retarget", "squad_position_index", "route_queue",
            "group_destination_repair", "distributed_arrivals", "moving_corner_advance",
            "local_throat_guide", "credited_fallback_search", "bounded_strike_recruits",
            "ai_attack_retention", "ai_rally_retention", "local_following",
            "occupancy_free_planning", "cohort_targets", "steering_projection_reuse",
        ]
        spec = "v13,-cardinal_queue_escape"
        process, report = play(native, game_dir, workdir, "features-v10",
                               ("--enhanced-mode", "--benchmark-features", spec))
        valid_report(process, report, "combat", True, version=1, feature_spec=spec)
        require(report["enhanced_features"] == version_10,
                "v13 without cardinal queue escape did not play version 10's features", process)
        print("benchmark-features %s enhanced_features_override true enhanced_version %s names %d" % (
            spec, report["enhanced_version"], len(report["enhanced_features"])))
        print(" ".join(report["enhanced_features"]))
        staged_battles(native, game_dir, workdir)
    print("battle benchmark: seeded workloads, group flow, preset versions, graphics independence, "
          "staged battles and failures checked")


if __name__ == "__main__":
    main()
