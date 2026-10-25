#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Report narrowing and sign warnings on lines changed since a merge base.

Compiles, with Clang and -fsyntax-only, each C or C++ source that changed
between the working tree and the merge base of REF and HEAD. Uncommitted
edits count; files Git does not track do not. Warnings are kept only when
they name one of the three flags below and fall on a line the change added
or modified, in a changed file. Code under third_party/ and local/ is never
compiled or reported.

The three flags stand in for the narrowing Visual Studio's compiler reports
(C4244, C4267) and for GCC's -Wsign-compare. They are off, or easy to miss,
in the engine's ordinary Clang build, so this pass is meant to be run before
a push. It does not build the engine and it does not write into the
repository or into a build folder it was only asked to read.

Options:
  --base REF       required (except with --self-test). The diff is the
                   working tree against `git merge-base REF HEAD`.
  --build DIR      use DIR/compile_commands.json when that file exists.
                   When it does not, configure a scratch tree instead.
  --scratch DIR    where to configure that tree. The default is a folder
                   under the system temporary directory, named from the
                   repository path. Configured once, and only when the
                   compile database is missing.
  --whole-files    report every warning of the three flags in a changed
                   file, not only on changed lines.
  --jobs N         syntax-only compiles at a time (default 6).
  --self-test      check the pure parts on built-in text. Needs no
                   compiler, repository, network or game data.

Exit status:
  0  no finding
  1  one or more findings
  2  the check cannot run (no Clang, git, CMake or the compile database)

Limits:
  Only the three flags in FLAGS. Other -Wconversion members are out.
  A changed source with no compile-database entry is listed as skipped
  and is not a finding. A deleted file is not compiled. A warning in a
  header is reported when that header changed and the line did too
  (or the whole header, with --whole-files). Fix a finding with an
  explicit cast or a wider type, not by turning the warning off.
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# The warnings this pass reports. One constant, so the self-test can
# require the set the brief names.
FLAGS = (
    "-Wimplicit-int-float-conversion",
    "-Wshorten-64-to-32",
    "-Wsign-compare",
)

SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".m", ".mm")
HEADER_SUFFIXES = (".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")
EXCLUDED_PREFIXES = ("third_party/", "local/")

HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
DIAG_RE = re.compile(r"^(.*):(\d+):(\d+): (warning|note|error): (.*)$")
FLAG_RE = re.compile(r"\[(-W[^\]]+)\]\s*$")

DEFAULT_JOBS = 6


def excluded(path):
    """A path under third_party/ or local/ is outside this check."""
    text = path.replace("\\", "/").lstrip("./")
    return text.startswith(EXCLUDED_PREFIXES)


def source_file(path):
    return path.endswith(SOURCE_SUFFIXES) and not excluded(path)


def header_file(path):
    return path.endswith(HEADER_SUFFIXES) and not excluded(path)


def interesting(path):
    return source_file(path) or header_file(path)


def parse_hunks(text):
    """Changed lines of each file in a `git diff -U0` patch.

    Returns (lines, present) where lines maps a relative path to the set of
    new-file line numbers the patch adds, and present is the set of paths
    that still exist afterwards (a deleted file is absent). A rename is
    recorded under the new path. A hunk whose new-side count is 0 adds no
    lines. Paths under third_party/ and local/ are left out.
    """
    lines = {}
    present = set()
    current = None
    alive = False

    def take(path):
        nonlocal current, alive
        current = path.replace("\\", "/")
        alive = bool(current) and current != "/dev/null" and interesting(current)
        if alive:
            present.add(current)
            lines.setdefault(current, set())

    for raw in text.splitlines():
        if raw.startswith("diff --git "):
            current = None
            alive = False
            parts = raw.split(" ")
            if len(parts) >= 4 and parts[-1].startswith("b/"):
                take(parts[-1][2:])
            continue
        if raw.startswith("rename to "):
            take(raw[len("rename to "):])
            continue
        if raw.startswith("deleted file "):
            if current in present:
                present.discard(current)
                lines.pop(current, None)
            alive = False
            continue
        if raw.startswith("+++ "):
            path = raw[4:].strip()
            if path.startswith("b/"):
                path = path[2:]
            if path == "/dev/null":
                if current in present:
                    present.discard(current)
                    lines.pop(current, None)
                alive = False
            else:
                take(path)
            continue
        if not alive:
            continue
        match = HUNK_RE.match(raw)
        if not match:
            continue
        start = int(match.group(3))
        count = int(match.group(4)) if match.group(4) is not None else 1
        if count == 0:
            continue
        # A new file's first hunk is `@@ -0,0 +1,N @@`.
        if start == 0:
            start = 1
        lines[current].update(range(start, start + count))
    return lines, present


def parse_diagnostic(line):
    """One compiler diagnostic, or None when the line is not one.

    The path may contain spaces. The kind is warning, note or error.
    """
    match = DIAG_RE.match(line.rstrip("\r\n"))
    if not match:
        return None
    flag = None
    flagged = FLAG_RE.search(match.group(5))
    if flagged:
        flag = flagged.group(1)
    return {
        "path": match.group(1).replace("\\", "/"),
        "line": int(match.group(2)),
        "column": int(match.group(3)),
        "kind": match.group(4),
        "flag": flag,
        "message": match.group(5),
        "text": line.rstrip("\r\n"),
    }


def keep_finding(diagnostic, changed_lines, changed_files, whole):
    """Whether a parsed diagnostic is a finding of this pass."""
    if diagnostic is None or diagnostic["kind"] != "warning":
        return False
    if diagnostic["flag"] not in FLAGS:
        return False
    path = diagnostic["path"]
    if excluded(path):
        return False
    if path not in changed_files:
        return False
    if whole:
        return True
    return diagnostic["line"] in changed_lines.get(path, ())


def rewrite_arguments(arguments):
    """Drop -o FILE, -c and -Werror, and add -fsyntax-only plus FLAGS.

    -I, -D, -std= and -isysroot (and every other flag) stay. The input
    file stays, because it is not one of the dropped options.
    """
    # -MD/-MF would write dependency files. -o would write an object.
    # Neither belongs in a syntax-only pass, and neither may land in the
    # repository or in a build folder this check was only asked to read.
    drop_with_value = {"-o", "-MF", "-MT", "-MQ"}
    rewritten = []
    skip_next = False
    for argument in arguments:
        if skip_next:
            skip_next = False
            continue
        if argument in drop_with_value:
            skip_next = True
            continue
        if argument == "-c" or argument == "-Werror" or argument.startswith("-Werror="):
            continue
        if argument in ("-MD", "-MMD", "-MG") or argument.startswith("-MF"):
            continue
        rewritten.append(argument)
    rewritten.append("-fsyntax-only")
    rewritten.append("-fno-color-diagnostics")
    rewritten.extend(FLAGS)
    return rewritten


def compiler_of(arguments):
    """The compiler argv of a compile-database entry, unwrapping ccache."""
    if not arguments:
        return ""
    program = Path(arguments[0]).name
    if program == "ccache" and len(arguments) > 1:
        return arguments[1]
    return arguments[0]


def relativize(path, root):
    """A path relative to the repository, with forward slashes."""
    text = path.replace("\\", "/")
    root_text = str(root).replace("\\", "/")
    if text == root_text:
        return ""
    prefix = root_text.rstrip("/") + "/"
    if text.startswith(prefix):
        return text[len(prefix):]
    return text


def default_scratch(root):
    safe = re.sub(r"[^A-Za-z0-9._-]", "_", str(root))
    return Path(tempfile.gettempdir()) / ("check-conversions-" + safe)


def load_database(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    by_file = {}
    for entry in data:
        file_name = entry.get("file", "")
        command = entry.get("command")
        arguments = entry.get("arguments")
        if arguments is None and command:
            arguments = shlex.split(command, posix=(os.name != "nt"))
        if not arguments:
            continue
        directory = entry.get("directory") or ""
        by_file.setdefault(file_name.replace("\\", "/"), []).append((arguments, directory))
    return by_file


def configure_scratch(root, scratch):
    """Configure a scratch tree so it writes compile_commands.json.

    Does not build. Writes only under scratch.
    """
    scratch.mkdir(parents=True, exist_ok=True)
    prefix = root / "local" / "deps" / "sdl-install"
    command = [
        "cmake", "-S", str(root), "-B", str(scratch), "-G", "Ninja",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_BUILD_TYPE=Check",
        "-DCMAKE_PREFIX_PATH=" + str(prefix),
    ]
    game_dir = os.environ.get("OA_GAME_DIR")
    if game_dir:
        command.append("-DOA_GAME_DIR=" + game_dir)
    completed = subprocess.run(
        command, cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if completed.returncode != 0:
        detail = (completed.stderr or "").strip().splitlines()
        tail = detail[-1] if detail else "cmake failed"
        fail("could not configure a scratch tree in " + str(scratch) + ": " + tail)
    database = scratch / "compile_commands.json"
    if not database.is_file():
        fail("configure wrote no compile_commands.json in " + str(scratch))
    return database


def fail(message):
    print("check_conversions: " + message, file=sys.stderr)
    sys.exit(2)


def git_output(root, args):
    completed = subprocess.run(
        ["git", *args], cwd=root, capture_output=True, text=True)
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "git failed").strip()
        fail(detail)
    return completed.stdout


def clang_version_says_clang(compiler):
    try:
        completed = subprocess.run(
            [compiler, "--version"], capture_output=True, text=True)
    except OSError:
        return False
    text = (completed.stdout or "") + (completed.stderr or "")
    return completed.returncode == 0 and "clang" in text.lower()


def ensure_clang(compilers):
    for compiler in compilers:
        if not clang_version_says_clang(compiler):
            print(
                "check_conversions: needs Clang; " + compiler + " is not",
                file=sys.stderr)
            sys.exit(2)


def compile_one(job):
    arguments, directory = job
    completed = subprocess.run(
        arguments, cwd=directory or None, capture_output=True, text=True)
    return (completed.stdout or "") + (completed.stderr or "")


def findings_of(output, root, changed_lines, changed_files, whole):
    found = []
    for raw in output.splitlines():
        diagnostic = parse_diagnostic(raw)
        if diagnostic is None:
            continue
        diagnostic["path"] = relativize(diagnostic["path"], root)
        if not keep_finding(diagnostic, changed_lines, changed_files, whole):
            continue
        # Clang's own spelling, with a repository-relative path.
        found.append(
            "{path}:{line}:{column}: warning: {message}".format(
                path=diagnostic["path"],
                line=diagnostic["line"],
                column=diagnostic["column"],
                message=diagnostic["message"],
            ))
    return found


def summary_line(count, changed_sources, changed_files, whole, ref):
    scope = changed_files if whole else changed_sources
    kind = "changed files" if whole else "changed sources"
    return (
        "check_conversions: {n} on changed lines in {m} {kind} since {ref}"
        .format(n=count, m=len(scope), kind=kind, ref=ref))


def run(root, base, build, scratch, whole, jobs):
    merge_base = git_output(root, ["merge-base", base, "HEAD"]).strip()
    patch = git_output(root, ["diff", "-U0", "--find-renames", merge_base])
    changed_lines, present = parse_hunks(patch)
    changed_sources = sorted(path for path in present if source_file(path))
    changed_files = set(present)
    if not changed_sources:
        print(summary_line(0, changed_sources, changed_files, whole, base))
        return 0

    database_path = None
    if build is not None:
        candidate = Path(build) / "compile_commands.json"
        if candidate.is_file():
            database_path = candidate
    if database_path is None:
        scratch_dir = Path(scratch) if scratch else default_scratch(root)
        database_path = scratch_dir / "compile_commands.json"
        if not database_path.is_file():
            database_path = configure_scratch(root, scratch_dir)

    by_file = load_database(database_path)
    relative_index = {}
    for file_name, commands in by_file.items():
        relative_index.setdefault(relativize(file_name, root), []).extend(commands)

    planned = []
    skipped = []
    compilers = set()
    for path in changed_sources:
        commands = relative_index.get(path, [])
        if not commands:
            skipped.append(path)
            continue
        for arguments, directory in commands:
            compilers.add(compiler_of(arguments))
            planned.append((rewrite_arguments(arguments), directory))
    ensure_clang(compilers)

    outputs = []
    if planned:
        workers = max(1, jobs)
        with ThreadPoolExecutor(max_workers=workers) as pool:
            outputs = list(pool.map(compile_one, planned))

    found = []
    for output in outputs:
        found.extend(findings_of(
            output, root, changed_lines, changed_files, whole))
    unique = sorted(set(found))
    for line in unique:
        print(line)
    for path in skipped:
        print("check_conversions: skipped " + path + " (no compile command)")
    print(summary_line(len(unique), changed_sources, changed_files, whole, base))
    return 1 if unique else 0


def check(condition, label, failures):
    if condition:
        return 1
    failures.append(label)
    return 0


def self_test():
    """The pure parts, on text built into this file. No compiler."""
    failures = []
    passed = 0

    new_file = "\n".join([
        "diff --git a/src/new.cpp b/src/new.cpp",
        "new file mode 100644",
        "--- /dev/null",
        "+++ b/src/new.cpp",
        "@@ -0,0 +1,2 @@",
        "+int a;",
        "+int b;",
        "",
    ])
    lines, present = parse_hunks(new_file)
    passed += check(
        present == {"src/new.cpp"} and lines["src/new.cpp"] == {1, 2},
        "new file", failures)

    renamed = "\n".join([
        "diff --git a/src/old.cpp b/src/renamed.cpp",
        "similarity index 90%",
        "rename from src/old.cpp",
        "rename to src/renamed.cpp",
        "--- a/src/old.cpp",
        "+++ b/src/renamed.cpp",
        "@@ -3 +3 @@",
        "-int before;",
        "+int after;",
        "",
    ])
    lines, present = parse_hunks(renamed)
    passed += check(
        "src/old.cpp" not in present and lines.get("src/renamed.cpp") == {3},
        "renamed file", failures)

    deleted = "\n".join([
        "diff --git a/src/gone.cpp b/src/gone.cpp",
        "deleted file mode 100644",
        "--- a/src/gone.cpp",
        "+++ /dev/null",
        "@@ -1 +0,0 @@",
        "-int gone;",
        "",
    ])
    lines, present = parse_hunks(deleted)
    passed += check(
        "src/gone.cpp" not in present and "src/gone.cpp" not in lines,
        "deleted file", failures)

    empty_hunk = "\n".join([
        "diff --git a/src/empty.cpp b/src/empty.cpp",
        "--- a/src/empty.cpp",
        "+++ b/src/empty.cpp",
        "@@ -4,0 +5,0 @@",
        "",
    ])
    lines, present = parse_hunks(empty_hunk)
    passed += check(
        present == {"src/empty.cpp"} and lines["src/empty.cpp"] == set(),
        "hunk of count 0", failures)

    several = "\n".join([
        "diff --git a/src/many.cpp b/src/many.cpp",
        "--- a/src/many.cpp",
        "+++ b/src/many.cpp",
        "@@ -10,2 +10,4 @@",
        "-int a;",
        "-int b;",
        "+int c;",
        "+int d;",
        "+int e;",
        "+int f;",
        "",
    ])
    lines, present = parse_hunks(several)
    passed += check(
        lines["src/many.cpp"] == {10, 11, 12, 13},
        "hunk of several lines", failures)

    warning = parse_diagnostic(
        "src/many.cpp:10:12: warning: implicit conversion [-Wshorten-64-to-32]")
    passed += check(
        warning and warning["kind"] == "warning" and warning["line"] == 10
        and warning["flag"] == "-Wshorten-64-to-32",
        "a warning", failures)

    note = parse_diagnostic("src/many.cpp:9:5: note: previous declaration is here")
    passed += check(note and note["kind"] == "note" and note["flag"] is None,
                     "a note", failures)

    error = parse_diagnostic("src/many.cpp:2:3: error: expected ';'")
    passed += check(error and error["kind"] == "error", "an error", failures)

    spaced = parse_diagnostic(
        "src/my file.cpp:4:1: warning: comparison [-Wsign-compare]")
    passed += check(
        spaced and spaced["path"] == "src/my file.cpp" and spaced["line"] == 4
        and spaced["flag"] == "-Wsign-compare",
        "a path with spaces", failures)

    changed_lines = {"src/many.cpp": {10}}
    changed_files = {"src/many.cpp"}
    on_change = {
        "path": "src/many.cpp", "line": 10, "kind": "warning",
        "flag": "-Wshorten-64-to-32",
    }
    off_change = dict(on_change, line=11)
    third = dict(on_change, path="third_party/src/many.cpp")
    passed += check(keep_finding(on_change, changed_lines, changed_files, False),
                     "finding on a changed line kept", failures)
    passed += check(not keep_finding(off_change, changed_lines, changed_files, False),
                     "finding on an unchanged line dropped", failures)
    passed += check(
        not keep_finding(third, {"third_party/src/many.cpp": {10}},
                         {"third_party/src/many.cpp"}, False),
        "under third_party/ dropped", failures)
    passed += check(keep_finding(off_change, changed_lines, changed_files, True),
                     "--whole-files keeping the unchanged one", failures)

    rewritten = rewrite_arguments([
        "clang++", "-I", "include", "-DNAME", "-std=c++20",
        "-isysroot", "/Library/SDK", "-Werror", "-c", "src/many.cpp",
        "-o", "x.o",
    ])
    passed += check(
        "-o" not in rewritten and "x.o" not in rewritten
        and "-c" not in rewritten and "-Werror" not in rewritten
        and "-I" in rewritten and "include" in rewritten
        and "-DNAME" in rewritten and "-std=c++20" in rewritten
        and "-isysroot" in rewritten and "/Library/SDK" in rewritten
        and "src/many.cpp" in rewritten and "-fsyntax-only" in rewritten,
        "argument rewrite", failures)

    passed += check(
        FLAGS == (
            "-Wimplicit-int-float-conversion",
            "-Wshorten-64-to-32",
            "-Wsign-compare",
        ),
        "flag constant", failures)

    if failures:
        for label in failures:
            print("check_conversions self-test: FAIL " + label, file=sys.stderr)
        return 1
    print("check_conversions self-test: {n} cases passed".format(n=passed))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", help="diff the working tree against the merge base of REF and HEAD")
    parser.add_argument("--build", help="read DIR/compile_commands.json when it exists")
    parser.add_argument("--scratch", help="scratch configure directory")
    parser.add_argument("--whole-files", action="store_true",
                        help="report every warning in a changed file")
    parser.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                        help="syntax-only compiles at a time (default 6)")
    parser.add_argument("--self-test", action="store_true",
                        help="check the pure parts on built-in text")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.base:
        fail("--base REF is required")
    if args.jobs < 1:
        fail("--jobs must be at least 1")
    root = Path(__file__).resolve().parents[1]
    return run(root, args.base, args.build, args.scratch, args.whole_files, args.jobs)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
