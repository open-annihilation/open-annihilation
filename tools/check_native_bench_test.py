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
        lines.extend(f"bench: FRAME {frame} frame 1000.00 work 1.00 draw 1.00 ground 0 "
                     f"terrain_packs 0 atlas_resets 0 atlas_rows 0 materials 0 stages{stages}"
                     for frame in range(seconds))
    lines.append("bench: DONE label=check")
    return "\n".join(lines) + "\n"


MATERIAL_KINDS = ("surface", "scenery", "effect", "light", "shadow")
RENDERER_FAMILIES = ("GROUND_INDEX", "POSE", "MATERIAL")


def build_record(**changes):
    """Return a first cache build over eight triangles, with independently rounded phase times."""
    values = dict(frame=1, ordinal=0, previous_attempts=0, was_valid=0, completed=1,
                  reason_mask=125, old_revision=0, revision=41, old_vertices=0, vertices=24,
                  old_card=0, card=1, old_axes="0000000000000000," * 3 + "0000000000000000",
                  axes="3ff0000000000000,0000000000000000,0000000000000000,3ff0000000000000",
                  old_fp="0,0,0", fp="37f,1f80,0", bounds_ms="0.100", world_ms="0.200",
                  card_ms="0.300", wall_ms="0.600")
    values.update(changes)
    return ("bench: GROUND_INDEX_BUILD_FRAME {frame} ordinal {ordinal} "
            "previous_attempts {previous_attempts} was_valid {was_valid} completed {completed} "
            "reason_mask {reason_mask} old_revision {old_revision} revision {revision} "
            "old_vertices {old_vertices} vertices {vertices} old_card {old_card} card {card} "
            "old_axes {old_axes} axes {axes} old_fp {old_fp} fp {fp} "
            "bounds_ms {bounds_ms} world_ms {world_ms} card_ms {card_ms} wall_ms {wall_ms}").format(
                **values)


def renderer_transcript(families=RENDERER_FAMILIES, diagnostics=True):
    """Add complete optional renderer families to an otherwise valid timed transcript."""
    lines = []
    for line in transcript(timers=True).splitlines():
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

    def test_initial_accepts_default_current_and_explicit_versions(self):
        """Off, the current preset without a request, and explicit versions 0..7 identify themselves."""
        for actual, requested in ((4, 0), *((version, version) for version in range(8))):
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
                 (initial_line(4, 6), 4, 4))
        for line, actual, requested in cases:
            with self.subTest(line=line, actual=actual, requested=requested):
                self.assert_rejected(result_for(line + "\n"), bench.check_initial,
                                     rules_version=actual, requested_rules_version=requested)

    def test_initial_refuses_unsupported_versions_even_when_expected(self):
        """An unsupported actual or requested version cannot pass by matching an expectation."""
        for actual, requested in ((8, 0), (8, 8), (4, 8)):
            with self.subTest(actual=actual, requested=requested):
                self.assert_rejected(result_for(initial_line(actual, requested) + "\n"),
                                     bench.check_initial, rules_version=actual,
                                     requested_rules_version=requested)

    def test_complete_timers_off_transcripts_pass(self):
        """Receipt checks preserve the complete three-second, three-frame validation path."""
        for actual, requested in ((0, 0), (4, 0), (4, 4), (6, 6), (7, 7)):
            with self.subTest(actual=actual, requested=requested):
                text = transcript(actual, requested)
                self.assertEqual(bench.check_lines(result_for(text), False, rules_version=actual,
                                                   requested_rules_version=requested),
                                 text.splitlines())

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
            bounds_ms="0.101", world_ms="0.201", card_ms="0.300", wall_ms="0.601"))
        self.assertEqual(bench.check_lines(result_for(text), True), text.splitlines())
        text = renderer_transcript().replace("GROUND_INDEX builds 1 hits 1 triangles 8 wall_ms 0.600",
                                             "GROUND_INDEX builds 1 hits 1 triangles 8 wall_ms 0.601")
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
        missing_first = text.replace(build_record(frame=0, revision=40) + "\n", "")
        self.assert_rejected(result_for(missing_first), bench.check_lines, True)

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

    def test_unsupported_rules_are_rejected_in_complete_transcripts(self):
        """Matching expectations cannot allow an unsupported version through the full checker."""
        self.assert_rejected(result_for(transcript(8, 8)), bench.check_lines, False,
                             rules_version=8, requested_rules_version=8)


if __name__ == "__main__":
    unittest.main()
