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


def transcript(rules_version=0, requested_rules_version=0):
    """Return a complete timers-off run with one 1000 ms frame each second."""
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
    lines.extend(["bench: MEASURE tick(per tick) avg 1.00",
                  "bench: COUNTS units 400 (player0 200, others 200) drawn 400 vertices 1200",
                  f"bench: GROUND revisions 0 in {seconds} frames",
                  f"bench: ATLAS terrain_packs 0.00 a frame, atlas_resets 0 in {seconds} frames",
                  "bench: DONE label=check"])
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
        """Off, the current preset without a request, and explicit 4/6 identify themselves."""
        for actual, requested in ((0, 0), (4, 0), (4, 4), (6, 6)):
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
        for actual, requested in ((7, 0), (7, 7), (4, 7)):
            with self.subTest(actual=actual, requested=requested):
                self.assert_rejected(result_for(initial_line(actual, requested) + "\n"),
                                     bench.check_initial, rules_version=actual,
                                     requested_rules_version=requested)

    def test_complete_timers_off_transcripts_pass(self):
        """Receipt checks preserve the complete three-second, three-frame validation path."""
        for actual, requested in ((0, 0), (4, 0), (4, 4), (6, 6)):
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


if __name__ == "__main__":
    unittest.main()
