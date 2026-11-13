#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check native benchmark receipts and complete transcripts without starting a game."""

import contextlib
import io
from pathlib import Path
import subprocess
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_bench as bench  # noqa: E402


def result_for(text):
    """Return a successful process result containing synthetic benchmark output."""
    return subprocess.CompletedProcess(["open-annihilation", "--bench", "fixture.bench"],
                                       0, text, "")


def initial_line(rules_version=0, requested_rules_version=0):
    """Return the receipt for the rules actually selected and requested."""
    return (f"bench: INITIAL enhanced_mode {int(rules_version != 0)} "
            f"rules_version {rules_version} requested_rules_version {requested_rules_version}")


def spread_line(name, milliseconds):
    """Return a spread whose frames all took the same time."""
    value = f"{milliseconds:.2f}"
    return (f"bench: MEASURE {name} min {value} avg {value} p50 {value} "
            f"p95 {value} p99 {value} low1 {value} max {value}")


def simulation_record(frame, **changes):
    """Return one timed call advancing thirty ticks, independently of frame work."""
    values = dict(frame=frame, before=30 + frame * 30, after=60 + frame * 30,
                  ticks=30, steps=1, sim="1.000", maximum="1.000", slowest=60 + frame * 30)
    values.update(changes)
    return ("bench: SIM_FRAME {frame} sim_ms {sim} tick_before {before} tick_after {after} "
            "ticks {ticks} steps {steps} max_step_ms {maximum} max_step_tick {slowest}").format(**values)


def transcript(rules_version=0, requested_rules_version=0, timers=False):
    """Return a complete run with one 1000 ms frame each second."""
    seconds = 3
    lines = [initial_line(rules_version, requested_rules_version),
             "bench: WARMUP label=check to tick 30",
             f"bench: MEASURING label=check tick=30 seconds={seconds}"]
    for second in range(1, seconds + 1):
        lines.append(f"bench: SEC {second} frames 1 frame 1000.00 sim 1.00 draw 1.00 "
                     f"units 200+200 vertices 1200 tick {30 + second * 30}")
        if second == 1:
            lines.append("bench: SHOT label=check")
    lines.append(f"bench: RESULT label=check seconds {seconds:.2f} frames {seconds} "
                 f"fps 1.00 low1_fps 1.00 ticks {seconds * 30} (30.00/s) game speed 30 of 30")
    lines.extend(spread_line(name, 1000 if name == "frame" else 1)
                 for name in bench.MEASURES)
    lines.append("bench: MEASURE tick(per tick) avg 1.00")
    if timers:
        lines.extend(f"bench: STAGE {name} 1.00" for name in bench.STAGES)
    lines.extend(["bench: COUNTS units 400 (player0 200, others 200) drawn 400 vertices 1200",
                  f"bench: GROUND revisions 0 in {seconds} frames",
                  f"bench: ATLAS terrain_packs 0.00 a frame, atlas_resets 0 in {seconds} frames"])
    if timers:
        lines.append("bench: SLOW frames 1 of 3, the slowest 1%: frame avg 1000.00 "
                     "ground_revisions 0 terrain_packs 0.00 a frame atlas_resets 0 "
                     "atlas_rows 0 a frame full_atlas_uploads 0")
        lines.extend(f"bench: SLOW STAGE {name} 1.00" for name in bench.STAGES)
        stages = " 1.00" * len(bench.STAGES)
        for frame in range(seconds):
            lines.append(f"bench: FRAME {frame} frame 1000.00 work 1.00 draw 1.00 ground 0 "
                         f"terrain_packs 0 atlas_resets 0 atlas_rows 0 materials 0 stages{stages}")
            lines.append(simulation_record(frame))
    lines.append("bench: DONE label=check")
    return "\n".join(lines) + "\n"


MATERIAL_KINDS = ("surface", "scenery", "effect", "light", "shadow")
RENDERER_FAMILIES = ("GROUND_INDEX", "POSE", "MATERIAL")


def build_record(parallel=None, **changes):
    """Return a first cache build over eight triangles, with independently rounded phase times."""
    values = dict(frame=1, ordinal=0, previous_attempts=0, was_valid=0, completed=1,
                  reason_mask=125, old_revision=0, revision=41, old_vertices=0, vertices=24,
                  old_card=0, card=1, old_axes="0000000000000000," * 3 + "0000000000000000",
                  axes="3ff0000000000000,0000000000000000,0000000000000000,3ff0000000000000",
                  old_fp="0,0,0", fp="37f,1f80,0", bounds_ms="0.100", world_ms="0.200",
                  card_ms="0.300", wall_ms="0.600")
    values.update(changes)
    values["parallel_field"] = "" if parallel is None else f" parallel {parallel}"
    return ("bench: GROUND_INDEX_BUILD_FRAME {frame} ordinal {ordinal} "
            "previous_attempts {previous_attempts} was_valid {was_valid} completed {completed} "
            "reason_mask {reason_mask} old_revision {old_revision} revision {revision} "
            "old_vertices {old_vertices} vertices {vertices} old_card {old_card} card {card}{parallel_field} "
            "old_axes {old_axes} axes {axes} old_fp {old_fp} fp {fp} "
            "bounds_ms {bounds_ms} world_ms {world_ms} card_ms {card_ms} wall_ms {wall_ms}").format(
                **values)


def renderer_transcript(families=RENDERER_FAMILIES, diagnostics=True):
    """Add complete optional renderer families to an otherwise valid timed transcript."""
    lines = []
    for line in transcript(timers=True).splitlines():
        if line.startswith("bench: SIM_FRAME "):
            continue
        if line.startswith("bench: FRAME 0 "):
            if "GROUND_INDEX" in families:
                lines.append("bench: GROUND_INDEX builds 1 hits 1 triangles 8 wall_ms 0.600")
            if "POSE" in families:
                lines.append("bench: POSE units 3 pieces 4 vm_triplets 4")
            if "MATERIAL" in families:
                lines.extend(f"bench: MATERIAL {kind} insertions 6 texels {48 * (index + 1)} "
                             f"row_texels {48 * (index + 1)}"
                             for index, kind in enumerate(MATERIAL_KINDS))
        lines.append(line)
        if not line.startswith("bench: FRAME "):
            continue
        frame = int(line.split()[2])
        lines.append(simulation_record(frame))
        if "GROUND_INDEX" in families:
            builds, hits, triangles, wall = ((0, 0, 0, "0.000"), (1, 0, 8, "0.600"),
                                             (0, 1, 0, "0.000"))[frame]
            lines.append(f"bench: GROUND_INDEX_FRAME {frame} builds {builds} hits {hits} "
                         f"triangles {triangles} wall_ms {wall}")
            if diagnostics and frame == 1:
                lines.append(build_record())
        if "POSE" in families:
            units, pieces, triplets = ((0, 0, 0), (2, 4, 4), (1, 0, 0))[frame]
            lines.append(f"bench: POSE_FRAME {frame} units {units} pieces {pieces} "
                         f"vm_triplets {triplets}")
        if "MATERIAL" in families:
            for index, kind in enumerate(MATERIAL_KINDS):
                texels = (frame + 1) * (index + 1) * 8
                rows = texels * 2 if frame < 2 else 0
                lines.append(f"bench: MATERIAL_FRAME {frame} {kind} insertions {frame + 1} "
                             f"texels {texels} row_texels {rows}")
    return "\n".join(lines) + "\n"


def geometry_record(**changes):
    """Return one cold type census with drawable rest-mesh dimensions."""
    values = dict(type=95, present=200, owned=150, x="22.0000", y="26.0000", z="29.0000",
                  footprint_x=2, footprint_z=2, name="ARMPW")
    values.update(changes)
    return ("bench: UNIT_GEOMETRY type {type} present {present} owned {owned} "
            "rest_mesh_x {x} rest_mesh_y {y} rest_mesh_z {z} "
            "footprint_x {footprint_x} footprint_z {footprint_z} name={name}").format(**values)


def detail_record(frame, **changes):
    """Return the frame's automatic model-size reference and actual dot choice."""
    values = dict(frame=frame, automatic=1, average="6.0000", dot="5.0", ratio="1.20",
                  units=180, dots=0)
    values.update(changes)
    return ("bench: UNIT_DETAIL_FRAME {frame} automatic {automatic} average_pixels {average} "
            "dot_pixels {dot} model_ratio {ratio} reference_units {units} dots {dots}").format(**values)


def unit_detail_transcript(geometry=True, detail=True):
    """Add optional cold geometry and complete detail companions to the renderer transcript."""
    lines = []
    for line in renderer_transcript().splitlines():
        lines.append(line)
        if geometry and line.startswith("bench: GROUND revisions "):
            lines.extend((geometry_record(), geometry_record(type=144, present=100, owned=0,
                                                           name="CORAK")))
        if detail and line.startswith("bench: SIM_FRAME "):
            lines.append(detail_record(int(line.split()[2])))
    return "\n".join(lines) + "\n"


class NativeBenchReceiptTest(unittest.TestCase):
    """Receipts identify the actual preset and remain part of strict transcript validation."""

    def assert_rejected(self, result, checker, *args, message=None, **kwargs):
        """Require a failed check and capture the transcript printed by fail()."""
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            if message is None:
                with self.assertRaises(bench.BenchFailure):
                    checker(result, *args, **kwargs)
            else:
                with self.assertRaisesRegex(bench.BenchFailure, message):
                    checker(result, *args, **kwargs)
        self.assertEqual(output.getvalue(), result.stdout + result.stderr)

    def test_unit_detail_families_are_optional_and_project_only_validated_rows(self):
        """Both new families retain strict legacy checks, together or independently."""
        for geometry in (False, True):
            for detail in (False, True):
                with self.subTest(geometry=geometry, detail=detail):
                    text = unit_detail_transcript(geometry, detail)
                    self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
                    self.assertEqual(bench.check_unit_detail_records(result_for(text), text.splitlines()),
                                     renderer_transcript().splitlines())
                    self.assertEqual(bench.check_renderer_records(result_for(text), text.splitlines(),
                                                                  3, True), transcript(timers=True).splitlines())

    def test_unit_detail_accepts_zero_geometry_and_manual_or_unknown_size_choices(self):
        """Empty mesh bounds and manual dots do not imply automatic geometry measurements."""
        text = unit_detail_transcript().replace(geometry_record(), geometry_record(
            x="0.0000", y="0.0000", z="65536.0000", footprint_x=-32768, footprint_z=32767))
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
        for changes in (dict(automatic=0, average="0.0000", units=0, dots=1),
                        dict(average="0.0000", units=0, dots=1),
                        dict(average="0.0000", units=180), dict(average="6.0000", dots=1)):
            with self.subTest(changes=changes):
                changed = text
                for frame in range(3):
                    changed = changed.replace(detail_record(frame), detail_record(frame, **changes))
                self.assertEqual(bench.check_lines(result_for(changed), True), changed.splitlines())

    def test_geometry_rows_require_strict_fields_ranges_and_nonempty_names(self):
        """Malformed metadata cannot disappear through the compatibility projection."""
        text = unit_detail_transcript()
        changes = (dict(type=65536), dict(type=-1), dict(type="095"), dict(present=0),
                   dict(present=4294967296), dict(present=401), dict(owned=201),
                   dict(owned=-1), dict(x="-1.0000"), dict(x="65536.0001"),
                   dict(y="nan"), dict(y="inf"), dict(z="1..0000"), dict(x="1.000"),
                   dict(x="9" * 400 + ".0000"), dict(footprint_x=-32769),
                   dict(footprint_z=32768), dict(footprint_x="2.5"), dict(footprint_x="-0"),
                   dict(footprint_z="02"), dict(name=""),
                   dict(name="ARM PW"))
        for change in changes:
            with self.subTest(change=change):
                self.assert_rejected(result_for(text.replace(geometry_record(), geometry_record(**change))),
                                     bench.check_lines, True, message="unit detail records")
        for row in (geometry_record() + " trailing", geometry_record().replace(" owned 150", "")):
            self.assert_rejected(result_for(text.replace(geometry_record(), row)), bench.check_lines,
                                 True, message="unit detail records")

    def test_geometry_is_one_ordered_unique_cold_block_after_ground(self):
        """Repeated types, moved metadata and a split census are refused."""
        original = unit_detail_transcript().splitlines()
        first = next(i for i, line in enumerate(original) if line.startswith("bench: UNIT_GEOMETRY"))
        malformed = []
        duplicate = original.copy()
        duplicate.insert(first, duplicate[first])
        malformed.append(duplicate)
        reverse = original.copy()
        reverse[first:first + 2] = reversed(reverse[first:first + 2])
        malformed.append(reverse)
        split = original.copy()
        split.insert(first + 1, "bench: ATLAS packed by scenery 0.0 materials 0 texels a frame")
        malformed.append(split)
        for prefix in ("bench: GROUND revisions", "bench: FRAME 0 "):
            moved = original.copy()
            row = moved.pop(first)
            index = next(i for i, line in enumerate(moved) if line.startswith(prefix))
            moved.insert(index, row)
            malformed.append(moved)
        for lines in malformed:
            with self.subTest(lines=lines):
                self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, True,
                                     message="unit detail records")

    def test_detail_rows_require_complete_adjacent_frame_pairing(self):
        """The companion occupies exactly the position after its matching SIM_FRAME."""
        text = unit_detail_transcript()
        malformed = (text.replace(detail_record(1) + "\n", ""),
                     text.replace(detail_record(1), detail_record(1) + "\n" + detail_record(1)),
                     text.replace(detail_record(1), detail_record(0)),
                     text.replace(detail_record(1), detail_record(1) + " trailing"),
                     text.replace(detail_record(1) + "\n", "").replace(
                         "bench: POSE_FRAME 1 ", detail_record(1) + "\nbench: POSE_FRAME 1 "),
                     text.replace(simulation_record(1), detail_record(1) + "\n" + simulation_record(1)),
                     text.replace("bench: FRAME 1 ", "bench: FRAME 2 "))
        for changed in malformed:
            with self.subTest(changed=changed):
                self.assert_rejected(result_for(changed), bench.check_lines, True,
                                     message="unit detail records")
        orphan = transcript().replace("bench: DONE", detail_record(0) + "\nbench: DONE")
        self.assert_rejected(result_for(orphan), bench.check_lines, False, message="unit detail records")

    def test_detail_rows_check_flags_constants_finite_sizes_and_reference_counts(self):
        """The schema keeps native booleans and measured reference invariants distinct."""
        text = unit_detail_transcript()
        changes = (dict(automatic=2), dict(automatic=-1), dict(dots=2), dict(dots="01"),
                   dict(average="nan"), dict(average="inf"), dict(average="-1.0000"),
                   dict(average="9" * 400 + ".0000"), dict(average="6.000"),
                   dict(dot="6.0"), dict(dot="0.0"), dict(dot="nan"), dict(ratio="1.30"),
                   dict(ratio="1.2"), dict(units=4294967296), dict(units=-1),
                   dict(units="180.0"), dict(units="0180"), dict(units=0),
                   dict(automatic=0), dict(automatic=0, units=0))
        for change in changes:
            with self.subTest(change=change):
                self.assert_rejected(result_for(text.replace(detail_record(1), detail_record(1, **change))),
                                     bench.check_lines, True, message="unit detail records")

    def test_timers_off_logged_frames_validate_detail_without_requiring_timers(self):
        """Paused logs retain geometry and detail even though all renderer timings are zero."""
        lines = []
        for line in unit_detail_transcript().splitlines():
            tag = line.split()[1]
            if tag in (*RENDERER_FAMILIES, "STAGE", "SLOW") or tag.startswith("GROUND_INDEX_BUILD_"):
                continue
            if tag == "GROUND_INDEX_FRAME":
                line = f"bench: {tag} {line.split()[2]} builds 0 hits 0 triangles 0 wall_ms 0.000"
            elif tag == "POSE_FRAME":
                line = f"bench: {tag} {line.split()[2]} units 0 pieces 0 vm_triplets 0"
            elif tag == "MATERIAL_FRAME":
                line = f"bench: {tag} {line.split()[2]} {line.split()[3]} insertions 0 texels 0 row_texels 0"
            lines.append(line.replace("label=check", "label=paused"))
        text = "\n".join(lines) + "\n"
        projected = bench.check_renderer_records(result_for(text), lines, 3, False)
        self.assertFalse(any(line.split()[1] in bench.UNIT_DETAIL_TAGS for line in projected))
        broken = text.replace(detail_record(1), detail_record(1, units=0))
        self.assert_rejected(result_for(broken), bench.check_renderer_records,
                             broken.splitlines(), 3, False, message="unit detail records")
        unknown = text.replace("bench: GROUND revisions", "bench: UNIT_DETAIL_EXTRA junk\nbench: GROUND revisions")
        self.assert_rejected(result_for(unknown), bench.check_renderer_records,
                             unknown.splitlines(), 3, False, message="unknown unit metadata tag")

    def test_detail_projection_preserves_unknown_and_malformed_legacy_rejection(self):
        """Validated telemetry cannot conceal unknown tags or alter the existing FRAME contract."""
        text = unit_detail_transcript()
        for changed in (text.replace("bench: GROUND revisions", "bench: UNKNOWN junk\nbench: GROUND revisions"),
                        text.replace("bench: GROUND revisions", "bench: UNIT_DETAIL_EXTRA junk\nbench: GROUND revisions"),
                        text.replace("frame 1000.00 work", "frame nan work", 1)):
            with self.subTest(changed=changed):
                self.assert_rejected(result_for(changed), bench.check_lines, True)

    def test_initial_accepts_default_current_and_explicit_versions(self):
        """Off, the current preset without a request, and explicit supported versions identify themselves."""
        for actual, requested in ((4, 0), *((version, version) for version in (*range(9), 10))):
            with self.subTest(actual=actual, requested=requested):
                bench.check_initial(result_for(initial_line(actual, requested) + "\n"),
                                    rules_version=actual, requested_rules_version=requested)

    def test_initial_requires_one_complete_anchored_receipt(self):
        """Missing, repeated, reordered fields and extra receipt text are refused."""
        line = initial_line()
        malformed = ("", line + "\n" + line, "prefix " + line, line + " trailing",
                     "bench: INITIAL enhanced_mode 0 rules_version 0",
                     "bench: INITIAL rules_version 0 enhanced_mode 0 requested_rules_version 0",
                     "bench: INITIAL enhanced_mode 2 rules_version 0 requested_rules_version 0",
                     "bench: INITIAL enhanced_mode 0 rules_version -1 requested_rules_version 0",
                     "bench: INITIAL enhanced_mode 0 rules_version 0.5 requested_rules_version 0")
        for text in malformed:
            with self.subTest(text=text):
                self.assert_rejected(result_for(text + "\n"), bench.check_initial)

    def test_initial_refuses_wrong_boolean_actual_or_requested_version(self):
        """The enabled bit and both version words must agree with the expected receipt."""
        cases = (("bench: INITIAL enhanced_mode 1 rules_version 0 requested_rules_version 0", 0, 0),
                 ("bench: INITIAL enhanced_mode 0 rules_version 4 requested_rules_version 4", 4, 4),
                 (initial_line(6, 6), 4, 6),
                 (initial_line(4, 0), 4, 4),
                 (initial_line(4, 6), 4, 4),
                 (initial_line(4, 8), 4, 8),
                 (initial_line(8, 7), 8, 7),
                 ("bench: INITIAL enhanced_mode 0 rules_version 8 requested_rules_version 8", 8, 8))
        for line, actual, requested in cases:
            with self.subTest(line=line, actual=actual, requested=requested):
                self.assert_rejected(result_for(line + "\n"), bench.check_initial,
                                     rules_version=actual, requested_rules_version=requested)

    def test_initial_refuses_unsupported_versions_even_when_expected(self):
        """An unsupported actual or requested version cannot pass by matching an expectation."""
        for actual, requested in ((9, 0), (9, 9), (4, 9), (11, 0), (11, 11), (4, 11)):
            with self.subTest(actual=actual, requested=requested):
                self.assert_rejected(result_for(initial_line(actual, requested) + "\n"),
                                     bench.check_initial, rules_version=actual,
                                     requested_rules_version=requested)

    def test_complete_timers_off_transcripts_pass(self):
        """Receipt checks preserve the complete three-second, three-frame validation path."""
        for actual, requested in ((0, 0), (4, 0), (4, 4), (6, 6), (7, 7), (8, 8), (10, 10)):
            with self.subTest(actual=actual, requested=requested):
                text = transcript(actual, requested)
                self.assertEqual(bench.check_lines(result_for(text), False, rules_version=actual,
                                                   requested_rules_version=requested),
                                 text.splitlines())

    def test_complete_experimental_timers_on_transcripts_pass(self):
        """Following and cohort presets retain the timed transcript and simulation checks."""
        for version in (8, 10):
            with self.subTest(version=version):
                text = transcript(version, version, timers=True)
                self.assertEqual(bench.check_lines(result_for(text), True, rules_version=version,
                                                   requested_rules_version=version), text.splitlines())

    def test_unsupported_rules_stop_before_the_battle(self):
        """The exact error and exit code cannot hide a battle that already started."""
        expected = ("bench: FAILED fixture.bench: line 2: rules_version takes 0 or a supported "
                    "Enhanced Mode rules version")
        rejected = result_for(expected + "\n")
        rejected.returncode = 1
        bench.check_rules_rejection(rejected, expected)
        self.assert_rejected(result_for(expected + "\n"), bench.check_rules_rejection, expected)
        missing = result_for("bench: FAILED another reason\n")
        missing.returncode = 1
        self.assert_rejected(missing, bench.check_rules_rejection, expected)
        for progress in ("Offline match world prepared for a map.",
                         "stage: unit 1 queues 1 armpw", initial_line(8, 8),
                         "bench: WARMUP label=check to tick 30",
                         "bench: MEASURING label=check tick=30 seconds=3",
                         "bench: SEC 0 frames 1", "bench: STATIC tick 0",
                         "bench: FRAME 0", "bench: RESULT label=check",
                         "bench: DONE label=check"):
            with self.subTest(progress=progress):
                late = result_for(progress + "\n" + expected + "\n")
                late.returncode = 1
                self.assert_rejected(late, bench.check_rules_rejection, expected)
                for diagnostic in (expected + "\n", expected):
                    stderr_progress = result_for(diagnostic)
                    stderr_progress.returncode = 1
                    stderr_progress.stderr = progress + "\n"
                    self.assert_rejected(stderr_progress, bench.check_rules_rejection, expected)

    def test_complete_transcript_checks_initial_receipt(self):
        """Missing and wrong receipts fail through the full checker as well as its helper."""
        lines = transcript().splitlines()
        self.assert_rejected(result_for("\n".join(lines[1:]) + "\n"), bench.check_lines, False)
        self.assert_rejected(result_for(transcript(4, 0)), bench.check_lines, False)
        lines.insert(1, lines[0])
        self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, False)

    def test_initial_order_and_unknown_categories_are_not_ignored(self):
        """A valid receipt must be first and unknown bench categories cannot disappear."""
        lines = transcript().splitlines()
        lines[0], lines[1] = lines[1], lines[0]
        self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, False)
        for unknown in ("bench: MYSTERY value", "bench: INITIAL_EXTRA enhanced_mode 0"):
            with self.subTest(unknown=unknown):
                lines = transcript().splitlines()
                lines.insert(1, unknown)
                self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, False)

    def test_receipt_does_not_bypass_existing_spread_checks(self):
        """A matching receipt still fails when a timing spread is out of order."""
        text = transcript(6, 6).replace(spread_line("draw", 1),
                                        spread_line("draw", 1).replace("p95 1.00", "p95 0.50"))
        self.assert_rejected(result_for(text), bench.check_lines, False, rules_version=6,
                             requested_rules_version=6, message="spread is out of order")

    def test_simulation_rows_distinguish_catchup_from_one_slow_step(self):
        """The same 90 ms sum can mean five bounded steps or one genuinely slow step."""
        text = transcript(timers=True)
        replacements = (
            simulation_record(0, after=30, ticks=0, steps=0, sim="0.000",
                              maximum="0.000", slowest=0),
            simulation_record(1, before=30, after=35, ticks=5, steps=5, sim="90.000",
                              maximum="30.000", slowest=32),
            simulation_record(2, before=35, after=36, ticks=1, steps=1, sim="90.000",
                              maximum="90.000", slowest=36))
        for frame, row in enumerate(replacements):
            text = text.replace(simulation_record(frame), row)
        text = text.replace("ticks 90 (30.00/s)", "ticks 6 (2.00/s)")
        text = text.replace("work 1.00", "work 1000.00")
        text = text.replace(spread_line("sim(per frame)", 1),
                            "bench: MEASURE sim(per frame) min 0.00 avg 60.00 p50 90.00 "
                            "p95 90.00 p99 90.00 low1 90.00 max 90.00")
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())

    def test_simulation_tick_wrap_and_multitick_calls_preserve_boundaries(self):
        """Wrapped maximum tick zero and hook calls spanning several ticks remain valid."""
        text = transcript(timers=True).replace("tick=30 seconds", "tick=4294967294 seconds")
        rows = (simulation_record(0, before=4294967294, after=4294967295, ticks=1,
                                  slowest=4294967295),
                simulation_record(1, before=4294967295, after=0, ticks=1, slowest=0),
                simulation_record(2, before=0, after=2, ticks=2, slowest=2))
        for frame, row in enumerate(rows):
            text = text.replace(simulation_record(frame), row)
        text = text.replace("ticks 90 (30.00/s)", "ticks 4 (1.33/s)")
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())

    def test_simulation_rows_are_complete_adjacent_and_well_formed(self):
        """A companion family cannot hide missing, duplicate, misplaced or malformed rows."""
        text = renderer_transcript()
        bad = (text.replace(simulation_record(1) + "\n", ""),
               "\n".join(line for line in text.splitlines()
                         if not line.startswith("bench: SIM_FRAME ")) + "\n",
               text.replace(simulation_record(1), simulation_record(1) + "\n" + simulation_record(1)),
               text.replace(simulation_record(1), simulation_record(1).replace("SIM_FRAME 1", "SIM_FRAME 0")),
               text.replace(simulation_record(1), simulation_record(1) + " trailing"),
               text.replace(simulation_record(1), simulation_record(1, sim="nan")),
               text.replace(simulation_record(1), simulation_record(1, maximum="inf")),
               text.replace(simulation_record(1), simulation_record(1, sim="1..0")),
               text.replace(simulation_record(1) + "\n", "").replace(
                   "bench: POSE_FRAME 1 ", simulation_record(1) + "\nbench: POSE_FRAME 1 "))
        for changed in bad:
            with self.subTest(changed=changed):
                self.assert_rejected(result_for(changed), bench.check_lines, True,
                                     message="simulation records")

    def test_simulation_rows_refuse_false_tick_or_timing_claims(self):
        """Tick ranges, zero-step markers and rounded maxima must agree with the measured span."""
        text = transcript(timers=True)
        changes = (dict(before=59), dict(after=91), dict(ticks=29), dict(ticks=4294967296),
                   dict(steps=4294967296), dict(slowest=4294967296), dict(steps=0),
                   dict(maximum="2.000"), dict(maximum="0.000"), dict(slowest=60),
                   dict(slowest=91), dict(sim="2.000", maximum="2.000"),
                   dict(sim="9" * 400))
        for row in changes:
            with self.subTest(row=row):
                changed = text.replace(simulation_record(1), simulation_record(1, **row))
                self.assert_rejected(result_for(changed), bench.check_lines, True,
                                     message="simulation records")
        zero = text
        for frame in range(3):
            zero = zero.replace(simulation_record(frame), simulation_record(
                frame, before=30, after=30, ticks=0, steps=0, sim="0.000", maximum="0.000", slowest=0))
        zero = zero.replace("ticks 90 (30.00/s)", "ticks 0 (0.00/s)")
        zero = zero.replace(spread_line("sim(per frame)", 1), spread_line("sim(per frame)", 0))
        self.assertEqual(bench.check_lines(result_for(zero), True), zero.splitlines())
        for row in (dict(sim="0.001"), dict(maximum="0.001"), dict(slowest=30)):
            base = simulation_record(1, before=30, after=30, ticks=0, steps=0,
                                     sim="0.000", maximum="0.000", slowest=0)
            changed = zero.replace(base, simulation_record(
                1, **(dict(before=30, after=30, ticks=0, steps=0, sim="0.000",
                           maximum="0.000", slowest=0) | row)))
            self.assert_rejected(result_for(changed), bench.check_lines, True,
                                 message="simulation records")

    def test_renderer_families_are_optional_and_preserve_the_legacy_transcript(self):
        """Legacy logs and each complete counter family retain the full strict legacy checks."""
        for families in ((), ("GROUND_INDEX",), ("POSE",), ("MATERIAL",), RENDERER_FAMILIES):
            for diagnostics in (False, True):
                with self.subTest(families=families, diagnostics=diagnostics):
                    text = renderer_transcript(families, diagnostics)
                    result = result_for(text)
                    self.assertEqual(bench.check_lines(result, True), text.splitlines())
                    self.assertEqual(bench.check_renderer_records(
                        result, text.splitlines(), 3, True), transcript(timers=True).splitlines())

    def test_pose_roots_and_same_tick_rig_primes_are_counted_independently(self):
        """Dots roots have no pieces; a later paused model prime adds no second root."""
        text = renderer_transcript(("POSE",)).replace(
            "POSE units 3 pieces 4 vm_triplets 4", "POSE units 3 pieces 8 vm_triplets 6"
        ).replace(
            "POSE_FRAME 0 units 0 pieces 0 vm_triplets 0",
            "POSE_FRAME 0 units 0 pieces 4 vm_triplets 2"
        )
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
        # The existing last row is a root-only capture: units 1, pieces 0.
        self.assertIn("bench: POSE_FRAME 2 units 1 pieces 0 vm_triplets 0", text)
        for before, after in (
            ("POSE_FRAME 0 units 0 pieces 4 vm_triplets 2",
             "POSE_FRAME 0 units 0 pieces 4 vm_triplets 5"),
            ("POSE_FRAME 2 units 1 pieces 0 vm_triplets 0",
             "POSE_FRAME 2 units 1 pieces 0 vm_triplets 1"),
        ):
            with self.subTest(after=after):
                self.assert_rejected(result_for(text.replace(before, after)), bench.check_lines,
                                     True, message="invalid pose frame")

    def test_renderer_summaries_and_frame_rows_must_be_complete_and_ordered(self):
        """A present family has exactly one complete summary and rows for every frame in order."""
        original = renderer_transcript().splitlines()
        prefixes = ("bench: GROUND_INDEX ", "bench: POSE ", "bench: MATERIAL surface ",
                    "bench: GROUND_INDEX_FRAME 1 ", "bench: POSE_FRAME 1 ",
                    "bench: MATERIAL_FRAME 1 effect ")
        for prefix in prefixes:
            index = next(i for i, line in enumerate(original) if line.startswith(prefix))
            for operation in ("remove", "duplicate", "malform"):
                with self.subTest(prefix=prefix, operation=operation):
                    lines = original.copy()
                    if operation == "remove":
                        lines.pop(index)
                    elif operation == "duplicate":
                        lines.insert(index, lines[index])
                    else:
                        lines[index] += " trailing"
                    self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, True)
        for first, second in (("bench: GROUND_INDEX ", "bench: POSE "),
                              ("bench: GROUND_INDEX_FRAME 1 ", "bench: POSE_FRAME 1 "),
                              ("bench: MATERIAL_FRAME 1 surface ", "bench: MATERIAL_FRAME 1 shadow ")):
            lines = original.copy()
            left = next(i for i, line in enumerate(lines) if line.startswith(first))
            right = next(i for i, line in enumerate(lines) if line.startswith(second))
            lines[left], lines[right] = lines[right], lines[left]
            self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, True)

    def test_renderer_records_cannot_precede_the_result_or_escape_timer_checks(self):
        """Deferred records are not accepted during measurement or with stage timers disabled."""
        original = renderer_transcript().splitlines()
        records = [line for line in original if line.startswith((
            "bench: GROUND_INDEX", "bench: POSE", "bench: MATERIAL"))]
        for record in records:
            with self.subTest(record=record):
                lines = original.copy()
                lines.remove(record)
                lines.insert(2, record)
                self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, True)
                untimed = transcript().replace("bench: DONE", record + "\nbench: DONE")
                self.assert_rejected(result_for(untimed), bench.check_lines, False)

    def test_renderer_unknown_tags_frame_indices_numbers_and_totals_are_rejected(self):
        """Known families cannot hide unknown tags, malformed numbers or inconsistent sums."""
        text = renderer_transcript()
        replacements = (("bench: POSE units", "bench: POSE_EXTRA units"),
                        ("bench: POSE_FRAME 2 ", "bench: POSE_FRAME 3 "),
                        ("bench: POSE_FRAME 1 ", "bench: POSE_FRAME 0 "),
                        ("bench: MATERIAL effect ", "bench: MATERIAL unknown "),
                        ("bench: POSE units 3", "bench: POSE units 4"),
                        ("GROUND_INDEX builds 1 hits 1 triangles 8", "GROUND_INDEX builds 2 hits 1 triangles 8"),
                        ("GROUND_INDEX builds 1 hits 1 triangles 8 wall_ms 0.600", "GROUND_INDEX builds 1 hits 1 triangles 8 wall_ms 0.900"),
                        ("MATERIAL surface insertions 6 texels 48", "MATERIAL surface insertions 6 texels 49"),
                        ("POSE_FRAME 1 units 2", "POSE_FRAME 1 units 2.5"),
                        ("POSE_FRAME 1 units 2", "POSE_FRAME 1 units -2"),
                        ("wall_ms 0.600", "wall_ms 1..0"),
                        ("wall_ms 0.600", "wall_ms nan"),
                        ("wall_ms 0.600", "wall_ms inf"))
        for before, after in replacements:
            with self.subTest(after=after):
                self.assert_rejected(result_for(text.replace(before, after, 1)), bench.check_lines, True)

    def test_ground_build_records_check_keys_masks_ordinals_and_subspans(self):
        """A build diagnostic agrees with its exact miss predicate and enclosing frame counters."""
        text = renderer_transcript()
        malformed = (dict(frame=0), dict(ordinal=1), dict(was_valid=2), dict(completed=2),
                     dict(reason_mask=0), dict(reason_mask=61), dict(reason_mask=253),
                     dict(vertices=25), dict(vertices="24.0"), dict(card=2),
                     dict(axes="3ff0000000000000,0,0,3ff0000000000000"),
                     dict(fp="37f,1f80"), dict(fp="1234567890abcdef0,0,0"),
                     dict(bounds_ms="0.900"), dict(wall_ms="0.900"),
                     dict(wall_ms="-0.100"), dict(world_ms="1..0"))
        for changes in malformed:
            with self.subTest(changes=changes):
                self.assert_rejected(result_for(text.replace(build_record(), build_record(**changes))),
                                     bench.check_lines, True)
        duplicate = text.replace(build_record(), build_record() + "\n" + build_record())
        self.assert_rejected(result_for(duplicate), bench.check_lines, True)
        impossible_history = build_record(
            was_valid=1, old_revision=40, old_vertices=24, old_card=1, reason_mask=4,
            old_axes="3ff0000000000000,0000000000000000,0000000000000000,3ff0000000000000",
            old_fp="37f,1f80,0")
        self.assert_rejected(result_for(text.replace(build_record(), impossible_history)),
                             bench.check_lines, True)
        orphan = renderer_transcript(("POSE",)).replace("bench: POSE_FRAME 1 ",
                                                       build_record() + "\nbench: POSE_FRAME 1 ")
        self.assert_rejected(result_for(orphan), bench.check_lines, True)

    def test_ground_build_rounding_and_cache_local_first_attempts_are_valid(self):
        """Rounded phase clocks and a later cache's first attempt remain valid diagnostics."""
        text = renderer_transcript().replace(build_record(), build_record(
            bounds_ms="0.101", world_ms="0.200", card_ms="0.300", wall_ms="0.600"))
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
        # The log has no Scene identity. Another frame may build a new
        # cache whose own attempt count starts from zero again.
        text = renderer_transcript().replace(
            "GROUND_INDEX builds 1 hits 1 triangles 8 wall_ms 0.600",
            "GROUND_INDEX builds 2 hits 1 triangles 16 wall_ms 1.200").replace(
                "bench: GROUND_INDEX_FRAME 0 builds 0 hits 0 triangles 0 wall_ms 0.000",
                "bench: GROUND_INDEX_FRAME 0 builds 1 hits 0 triangles 8 wall_ms 0.600\n" +
                build_record(frame=0, revision=40))
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
        rounded = text.replace("GROUND_INDEX builds 2 hits 1 triangles 16 wall_ms 1.200",
                               "GROUND_INDEX builds 2 hits 1 triangles 16 wall_ms 1.201")
        self.assertEqual(bench.check_lines(result_for(rounded), True), rounded.splitlines())
        missing_first = text.replace(build_record(frame=0, revision=40) + "\n", "")
        self.assert_rejected(result_for(missing_first), bench.check_lines, True)

    def test_ground_build_serial_marker_preserves_phase_partition(self):
        """Both absent and explicit serial markers require additive constructor spans."""
        for parallel in (None, 0):
            with self.subTest(parallel=parallel):
                text = renderer_transcript().replace(build_record(), build_record(parallel=parallel))
                self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
                changed = text.replace(build_record(parallel=parallel), build_record(
                    parallel=parallel, world_ms="0.400", card_ms="0.400"))
                self.assert_rejected(result_for(changed), bench.check_lines, True,
                                     message="BUILD subspans do not partition wall time")

    def test_ground_build_parallel_marker_allows_overlap_and_dispatch_overhead(self):
        """Independent constructor clocks fit inside the total, with its rounding tolerance."""
        for world, card in (("0.400", "0.300"), ("0.300", "0.400"),
                            ("0.502", "0.300")):
            with self.subTest(world=world, card=card):
                text = renderer_transcript().replace(build_record(), build_record(
                    parallel=1, world_ms=world, card_ms=card))
                self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
        text = renderer_transcript().replace(build_record(), build_record(parallel=1))
        malformed = (dict(world_ms="0.503"), dict(card_ms="0.503"),
                     dict(card=0, reason_mask=77,
                          axes="0000000000000000,0000000000000000,0000000000000000,0000000000000000"),
                     dict(ordinal=1), dict(reason_mask=61), dict(vertices=25))
        for changes in malformed:
            with self.subTest(changes=changes):
                changed = text.replace(build_record(parallel=1), build_record(parallel=1, **changes))
                self.assert_rejected(result_for(changed), bench.check_lines, True)

    def test_ground_build_parallel_marker_and_spans_are_strict(self):
        """Malformed markers and nonfinite or negative spans never weaken record validation."""
        text = renderer_transcript()
        for parallel in ("", -1, 2, "01", "1.0", "1 parallel 1"):
            with self.subTest(parallel=parallel):
                changed = text.replace(build_record(), build_record(parallel=parallel))
                self.assert_rejected(result_for(changed), bench.check_lines, True)
        moved = build_record().replace("bounds_ms", "parallel 1 bounds_ms")
        self.assert_rejected(result_for(text.replace(build_record(), moved)), bench.check_lines, True)
        overflow = "9" * 400
        for field in ("bounds_ms", "world_ms", "card_ms", "wall_ms"):
            for value in ("-0.100", "nan", "inf", "1..0", overflow):
                with self.subTest(field=field, value=value):
                    changed = text.replace(build_record(), build_record(parallel=1, **{field: value}))
                    self.assert_rejected(result_for(changed), bench.check_lines, True,
                                         message="BUILD spans are not finite nonnegative times"
                                         if value == overflow else None)

    def test_ground_build_empty_uncached_incomplete_and_zero_phase_records_are_valid(self):
        """Successful empty/uncached indices and incomplete bounds attempts remain distinct."""
        cases = ((dict(completed=0, bounds_ms="0.600", world_ms="0.000", card_ms="0.000"), 8,
                  "0.600"),
                 (dict(vertices=0, reason_mask=117), 0, "0.600"),
                 (dict(revision=0, reason_mask=123), 8, "0.600"),
                 (dict(card=0, reason_mask=77,
                       axes="0000000000000000,0000000000000000,0000000000000000,0000000000000000"),
                  8, "0.600"),
                 (dict(bounds_ms="0.000", world_ms="0.000", card_ms="0.000", wall_ms="0.000"),
                  8, "0.000"))
        for changes, triangles, wall in cases:
            with self.subTest(changes=changes):
                text = renderer_transcript().replace(build_record(), build_record(**changes))
                text = text.replace("triangles 8 wall_ms 0.600", f"triangles {triangles} wall_ms {wall}")
                self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())

    def test_ground_build_records_and_dropped_attempts_match_the_bounded_prefix(self):
        """Eight retained diagnostics plus one dropped record account for nine attempts."""
        rows = [build_record(ordinal=ordinal, revision=41 + ordinal) for ordinal in range(8)]
        dropped = "bench: GROUND_INDEX_BUILD_DROPPED_FRAME 1 count 1"
        text = renderer_transcript().replace(build_record(), "\n".join([*rows, dropped]))
        text = text.replace("GROUND_INDEX builds 1 hits 1 triangles 8 wall_ms 0.600",
                            "GROUND_INDEX builds 9 hits 1 triangles 72 wall_ms 5.400")
        text = text.replace("GROUND_INDEX_FRAME 1 builds 1 hits 0 triangles 8 wall_ms 0.600",
                            "GROUND_INDEX_FRAME 1 builds 9 hits 0 triangles 72 wall_ms 5.400")
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
        malformed = (text.replace(dropped, ""),
                     text.replace(dropped, dropped + "\n" + dropped),
                     text.replace(dropped, dropped.replace("count 1", "count 0")),
                     text.replace(dropped, dropped.replace("count 1", "count 2")),
                     text.replace(dropped, dropped.replace("FRAME 1", "FRAME 2")),
                     text.replace(rows[-1] + "\n", "").replace("count 1", "count 2"),
                     text.replace(rows[-1], rows[-1].replace("ordinal 7", "ordinal 8")),
                     text.replace(dropped, dropped + " trailing"))
        for changed in malformed:
            with self.subTest(changed=changed):
                self.assert_rejected(result_for(changed), bench.check_lines, True)

    def test_renderer_summaries_cannot_move_across_slow_or_frame_rows(self):
        """The counter summaries follow SLOW and precede the frame log."""
        original = renderer_transcript().splitlines()
        summary = next(line for line in original if line.startswith("bench: GROUND_INDEX "))
        for prefix in ("bench: SLOW frames", "bench: FRAME 1 "):
            lines = original.copy()
            lines.remove(summary)
            index = next(i for i, line in enumerate(lines) if line.startswith(prefix))
            lines.insert(index, summary)
            self.assert_rejected(result_for("\n".join(lines) + "\n"), bench.check_lines, True)

    def test_timers_off_frame_logs_allow_only_unmeasured_renderer_rows(self):
        """Paused frame-log validation accepts complete zero rows without stage summaries."""
        lines = []
        for line in renderer_transcript().splitlines():
            tag = line.split()[1]
            if tag in (*RENDERER_FAMILIES, "STAGE", "SLOW") or tag.startswith("GROUND_INDEX_BUILD_"):
                continue
            if tag == "GROUND_INDEX_FRAME":
                line = f"bench: {tag} {line.split()[2]} builds 0 hits 0 triangles 0 wall_ms 0.000"
            elif tag == "POSE_FRAME":
                line = f"bench: {tag} {line.split()[2]} units 0 pieces 0 vm_triplets 0"
            elif tag == "MATERIAL_FRAME":
                line = f"bench: {tag} {line.split()[2]} {line.split()[3]} insertions 0 texels 0 row_texels 0"
            lines.append(line)
        text = "\n".join(lines) + "\n"
        legacy = [line for line in lines if line.split()[1] not in (
            "GROUND_INDEX_FRAME", "POSE_FRAME", "MATERIAL_FRAME")]
        self.assertEqual(bench.check_renderer_records(result_for(text), lines, 3, False), legacy)
        changes = (("GROUND_INDEX_FRAME 1 builds 0 hits 0", "GROUND_INDEX_FRAME 1 builds 0 hits 1"),
                   ("POSE_FRAME 1 units 0", "POSE_FRAME 1 units 1"),
                   ("MATERIAL_FRAME 1 surface insertions 0 texels 0", "MATERIAL_FRAME 1 surface insertions 1 texels 1"),
                   ("bench: POSE_FRAME 1 ", build_record() + "\nbench: POSE_FRAME 1 "),
                   ("bench: FRAME 0 ", "bench: POSE units 0 pieces 0 vm_triplets 0\nbench: FRAME 0 "))
        for before, after in changes:
            with self.subTest(after=after):
                changed = text.replace(before, after, 1)
                self.assert_rejected(result_for(changed), bench.check_renderer_records,
                                     changed.splitlines(), 3, False)

    def test_unsupported_rules_are_rejected_in_complete_transcripts(self):
        """Matching expectations cannot allow an unsupported version through the full checker."""
        for version in (9, 11):
            with self.subTest(version=version):
                self.assert_rejected(result_for(transcript(version, version)), bench.check_lines,
                                     False, rules_version=version,
                                     requested_rules_version=version)


if __name__ == "__main__":
    unittest.main()
