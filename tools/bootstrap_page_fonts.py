#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Fetch the page fonts into a dependency folder.

Writes <dir>/fonts and <dir>/font-sources. <dir> is --deps, or local/deps.
Every file is pinned by SHA-256. When those files are already present with
the pinned hashes, nothing is downloaded. This is the engine's entry to the
font bootstrap that lives beside the module; that file is what a checkout
runs directly, with the same --deps.

    python3 tools/bootstrap_page_fonts.py --deps <dir>
"""
import pathlib
import runpy
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
scripts = sorted(ROOT.joinpath("tools").glob("*/bootstrap_fonts.py"))
if len(scripts) != 1:
    sys.exit(f"bootstrap_page_fonts: expected one font bootstrap under tools, found {scripts}")
sys.argv[0] = str(scripts[0])
runpy.run_path(str(scripts[0]), run_name="__main__")
