#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Add SPDX copyright and licence headers to the engine's files, or check that every file has its terms.

Every file the engine owns (tools/engine_files.py: the files Git tracks,
and new ones it does not ignore, less the nested projects and third-party
directories) states who holds its copyright and under which licence it is
distributed, in one of three ways. A file's copyright line names the
project's authors and points to COPYRIGHT_FILE at the tree's root, which says
who they are, so that the holder is stated once per repository rather than
in every file:

  - a header of its own: two comment lines in the style of its language at
    the top of the file (after a '#!' line, a Python coding line or a
    Dockerfile parser directive, which must come first), the SPDX
    FileCopyrightText tag with COPYRIGHT_TEXT and the SPDX
    License-Identifier tag with LICENCE_EXPRESSION (header_lines writes
    them);
  - an annotation in the root REUSE.toml, for a file that cannot carry a
    header (an image, JSON, data, the documents);
  - being a licence text or a third-party notice, which states its own terms:
    a file named LICENSE, LICENCE or COPYING (with any '.' or '-' suffix),
    or anything under a root licenses/ or LICENSES/ directory.

A run adds the header to each file that has none and can hold one: a file
whose comment style its name gives (COMMENT_BY_NAME, COMMENT_BY_SUFFIX, or a
'#!' line). Only a file that cannot hold one counts through REUSE.toml; one
that has neither a style nor an annotation is reported. A second run changes
nothing. --check changes nothing and fails when the root holds no
COPYRIGHT_FILE, when a file states no terms, when it holds only one of the
two SPDX lines, when its header names another licence than
LICENCE_EXPRESSION or another copyright than COPYRIGHT_TEXT (unless
OTHER_TERMS lists it), when REUSE.toml cannot be
read, when one of its paths without a wildcard names no file (stale after a
move; a pattern with one is a rule for files to come) or when a path
matches a file of a nested project, whose terms are its own.

REUSE.toml follows the REUSE specification's format (version 1): each
[[annotations]] table has 'path' (a pattern or a list of them, relative to
the root: '*' matches within one directory, '**' across directories, and
'\\' escapes the next character), 'SPDX-FileCopyrightText' and
'SPDX-License-Identifier'.

Exit status: 0 when every file states its terms (or, without --check, now
does); 1 when a file does not and cannot be given a header, or --check
finds a problem; 2 when the tree cannot be read, or Python is older than
3.11 (MIN_PYTHON), which the tool needs to read REUSE.toml.
"""
import argparse
import fnmatch
import platform
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:
    tomllib = None  # Python before 3.11; main() says so

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_files import engine_files, nested_project_directories, listed_files  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
# The first Python whose standard library reads TOML (tomllib).
MIN_PYTHON = (3, 11)
# The copyright line every header gives: the project's authors, and the file
# at the tree's root that names the holders and the licence. The licence
# expression is the engine's (README.md, LICENSE).
COPYRIGHT_HOLDER = "The Open Annihilation Authors"
COPYRIGHT_FILE = "COPYRIGHT"
COPYRIGHT_TEXT = f"{COPYRIGHT_HOLDER}; see {COPYRIGHT_FILE}"
LICENCE_EXPRESSION = "GPL-3.0-only"
# The engine's files whose own header states other terms, relative to the
# root: code the engine keeps under its authors' licence outside a
# third-party directory. None yet.
OTHER_TERMS = frozenset()
# The tags, spelt in two parts so that tools which read SPDX tags from files
# do not take these strings for this file's own.
COPYRIGHT_TAG = "SPDX-" "FileCopyrightText"
LICENCE_TAG = "SPDX-" "License-Identifier"
# How many lines at the top of a file a header may lie in.
HEADER_LINES = 12
# What may close a header line after its value: a block comment's end.
COMMENT_CLOSE = "*/"
# Bound on a file whose header is read or written.
MAX_FILE_BYTES = 16 << 20
ANNOTATIONS_FILE = "REUSE.toml"
ANNOTATIONS_VERSION = 1
MAX_ANNOTATIONS_BYTES = 1 << 20
# Licence texts, and the root directories of third-party notices.
LICENCE_TEXT_RE = re.compile(r"^(?:LICEN[CS]E|COPYING)(?:[-.].*)?$")
NOTICE_DIRECTORIES = ("licenses/", "LICENSES/")
# The comment that opens each header line, by file suffix, then by file name
# (fnmatch patterns, matched in order).
LINE_COMMENT = "//"
HASH_COMMENT = "#"
COMMENT_BY_SUFFIX = {
    **dict.fromkeys((".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".m", ".mm", ".rc",
                     ".glsl"),
                    LINE_COMMENT),
    **dict.fromkeys((".py", ".sh", ".cmake", ".yml", ".yaml", ".toml", ".specs"), HASH_COMMENT),
}
COMMENT_BY_NAME = (
    ("CMakeLists.txt", HASH_COMMENT),
    ("requirements*.txt", HASH_COMMENT),  # pip requirements
    ("*ignorelist.txt", HASH_COMMENT),    # sanitizer special-case lists
    ("Dockerfile", HASH_COMMENT),
    ("*.Dockerfile", HASH_COMMENT),
    (".gitignore", HASH_COMMENT),
    (".gitattributes", HASH_COMMENT),
    (".clang-format", HASH_COMMENT),
    (".clang-tidy", HASH_COMMENT),
    ("*.rc.in", LINE_COMMENT),          # Windows resource script templates
)
SHEBANG = "#!"
# Lines that must stay first: a Python coding declaration (on line 1 or 2)
# and Dockerfile parser directives.
CODING_RE = re.compile(r"^[ \t\f]*#.*?coding[:=][ \t]*[-\w.]+")
PARSER_DIRECTIVE_RE = re.compile(r"^#\s*(?:syntax|escape|check)\s*=", re.IGNORECASE)
BYTE_ORDER_MARK = "\ufeff"


@dataclass
class Annotation:
    """One [[annotations]] table of REUSE.toml."""
    patterns: list = field(default_factory=list)   # the path patterns, as written
    expressions: list = field(default_factory=list)  # each pattern compiled
    copyright: list = field(default_factory=list)
    licence: str = ""


class AnnotationError(Exception):
    """A REUSE.toml that cannot be read."""


def pattern_expression(pattern):
    """Compiles a REUSE.toml path pattern.

    @param pattern the pattern: '*' matches within one directory, '**' across directories, '\\' escapes
    @return the compiled expression, matching whole paths
    """
    parts = []
    i = 0
    while i < len(pattern):
        c = pattern[i]
        if c == "\\" and i + 1 < len(pattern):
            parts.append(re.escape(pattern[i + 1]))
            i += 2
        elif pattern.startswith("**", i):
            parts.append(".*")
            i += 2
        elif c == "*":
            parts.append("[^/]*")
            i += 1
        else:
            parts.append(re.escape(c))
            i += 1
    return re.compile("".join(parts) + r"\Z")


def has_wildcard(pattern):
    """Tells whether a REUSE.toml path pattern holds a wildcard, or names one file.

    @param pattern the pattern
    @return true when it holds a '*' that no '\\' escapes
    """
    return bool(re.search(r"(?<!\\)(?:\\\\)*\*", pattern))


def read_annotations(root):
    """Reads the annotations of the root REUSE.toml.

    @param root the tree's root
    @return the annotations; none when the file is missing
    """
    path = root / ANNOTATIONS_FILE
    if not path.is_file():
        return []
    try:
        if path.stat().st_size > MAX_ANNOTATIONS_BYTES:
            raise AnnotationError(f"{ANNOTATIONS_FILE}: larger than {MAX_ANNOTATIONS_BYTES} bytes")
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise AnnotationError(f"{ANNOTATIONS_FILE}: {error}") from error
    if data.get("version") != ANNOTATIONS_VERSION:
        raise AnnotationError(f"{ANNOTATIONS_FILE}: version is not {ANNOTATIONS_VERSION}")
    tables = data.get("annotations", [])
    if not isinstance(tables, list):
        raise AnnotationError(f"{ANNOTATIONS_FILE}: annotations is not a list of tables")
    annotations = []
    for number, table in enumerate(tables, 1):
        where = f"{ANNOTATIONS_FILE}: annotation {number}"
        patterns = table.get("path") if isinstance(table, dict) else None
        patterns = [patterns] if isinstance(patterns, str) else patterns
        copyright_text = table.get(COPYRIGHT_TAG) if isinstance(table, dict) else None
        copyright_text = [copyright_text] if isinstance(copyright_text, str) else copyright_text
        licence = table.get(LICENCE_TAG) if isinstance(table, dict) else None
        if not patterns or not all(isinstance(pattern, str) and pattern for pattern in patterns):
            raise AnnotationError(f"{where}: no path")
        if not copyright_text or not all(isinstance(text, str) and text for text in copyright_text):
            raise AnnotationError(f"{where}: no {COPYRIGHT_TAG}")
        if not isinstance(licence, str) or not licence:
            raise AnnotationError(f"{where}: no {LICENCE_TAG}")
        annotations.append(Annotation(patterns, [pattern_expression(pattern) for pattern in patterns],
                                      copyright_text, licence))
    return annotations


def is_licence_text(name):
    """Tells whether a file is a licence text or a third-party notice, which states its own terms.

    @param name path relative to the root
    @return true for LICENSE, LICENCE and COPYING files and the root notice directories
    """
    return bool(LICENCE_TEXT_RE.match(name.rsplit("/", 1)[-1])) or name.startswith(NOTICE_DIRECTORIES)


def comment_style(name, first_line):
    """Finds the comment that opens a header line in a file.

    @param name path relative to the root
    @param first_line the file's first line
    @return the comment, or None when the file's language is not known
    """
    base = name.rsplit("/", 1)[-1]
    for pattern, comment in COMMENT_BY_NAME:
        if fnmatch.fnmatchcase(base, pattern):
            return comment
    suffix = Path(base).suffix
    if suffix in COMMENT_BY_SUFFIX:
        return COMMENT_BY_SUFFIX[suffix]
    if not suffix and first_line.startswith(SHEBANG):
        return HASH_COMMENT
    return None


def header_tags(text):
    """Reads the SPDX tags in the first HEADER_LINES lines of a file.

    @param text the file
    @return (the copyright texts, the licence expressions), each as written after its tag
    """
    head = text.split("\n", HEADER_LINES)[:HEADER_LINES]
    found = ([], [])
    for line in head:
        for values, tag in zip(found, (COPYRIGHT_TAG, LICENCE_TAG)):
            at = line.find(f"{tag}:")
            if at >= 0:
                value = line[at + len(tag) + 1:].strip()
                values.append(value.removesuffix(COMMENT_CLOSE).strip())
    return found


def is_project_copyright(copyright_text):
    """Tells whether an SPDX copyright text is the one every engine file gives.

    @param copyright_text the text after the tag, with the spaces around it removed
    @return true when it is COPYRIGHT_TEXT, which points to the root's COPYRIGHT_FILE
    """
    return copyright_text == COPYRIGHT_TEXT


def header_lines(comment):
    """Writes the two header lines in a comment style.

    @param comment the comment that opens each line
    @return the lines, without line ends
    """
    return [f"{comment} {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}", f"{comment} {LICENCE_TAG}: {LICENCE_EXPRESSION}"]


def with_header(text, comment):
    """Adds the header to a file, after the lines that must stay first.

    @param text the file
    @param comment the comment that opens each header line
    @return the file with the header
    """
    bom = BYTE_ORDER_MARK if text.startswith(BYTE_ORDER_MARK) else ""
    text = text[len(bom):]
    ending = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(ending)
    trailing = len(lines) > 1 and lines[-1] == ""
    if trailing or lines == [""]:
        lines.pop()
    start = 0
    if lines and lines[0].startswith(SHEBANG):
        start = 1
    if start < len(lines) and start < 2 and comment == HASH_COMMENT and CODING_RE.match(lines[start]):
        start += 1
    while start < len(lines) and PARSER_DIRECTIVE_RE.match(lines[start]):
        start += 1
    added = header_lines(comment)
    if start < len(lines) and lines[start].strip():
        added.append("")
    lines[start:start] = added
    return bom + ending.join(lines) + ending


@dataclass
class Survey:
    """What a run found in the tree."""
    headers_added: list = field(default_factory=list)
    problems: list = field(default_factory=list)
    counts: dict = field(default_factory=dict)  # how many files state their terms each way


def survey(root, write, other_terms=OTHER_TERMS):
    """Checks every engine file for its terms, adding headers when asked.

    @param root the tree's root
    @param write true to add the missing headers
    @param other_terms the files whose header may state other terms than the engine's
    @return what was found, and done
    """
    result = Survey(counts={"header": 0, "annotation": 0, "licence text": 0})
    if not (root / COPYRIGHT_FILE).is_file():
        result.problems.append(f"{COPYRIGHT_FILE}: missing; every header points to it for the copyright holder")
    try:
        annotations = read_annotations(root)
    except AnnotationError as error:
        result.problems.append(str(error))
        annotations = []
    names = engine_files(root)
    matched = [[False] * len(annotation.patterns) for annotation in annotations]
    for name in names:
        annotated = False
        for number, annotation in enumerate(annotations):
            for index, expression in enumerate(annotation.expressions):
                if expression.match(name):
                    matched[number][index] = True
                    annotated = True
        if is_licence_text(name):
            result.counts["licence text"] += 1
            continue
        path = root / name
        if path.stat().st_size > MAX_FILE_BYTES:
            result.problems.append(f"{name}: larger than {MAX_FILE_BYTES} bytes")
            continue
        data = path.read_bytes()
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = None
        copyrights, licences = header_tags(text) if text is not None else ([], [])
        if copyrights and licences:
            result.counts["header"] += 1
            if name in other_terms:
                continue
            if any(licence != LICENCE_EXPRESSION for licence in licences):
                result.problems.append(f"{name}: its header names {' and '.join(licences)}, not "
                                       f"{LICENCE_EXPRESSION}; list a file with other terms in OTHER_TERMS")
            if not any(is_project_copyright(copyright_text) for copyright_text in copyrights):
                result.problems.append(f"{name}: its header's copyright is not '{COPYRIGHT_TEXT}'; "
                                       f"list a file with other terms in OTHER_TERMS")
            continue
        if copyrights or licences:
            missing = LICENCE_TAG if copyrights else COPYRIGHT_TAG
            result.problems.append(f"{name}: its header has no {missing} line")
            continue
        comment = comment_style(name, text.split("\n", 1)[0]) if text is not None else None
        if comment is None:
            if annotated:
                result.counts["annotation"] += 1
            else:
                result.problems.append(f"{name}: states no copyright or licence; annotate it in "
                                       f"{ANNOTATIONS_FILE}, or teach tools/spdx_headers.py its comment style")
            continue
        result.headers_added.append(name)
        result.counts["header"] += 1
        if write:
            path.write_bytes(with_header(text, comment).encode("utf-8"))
    for annotation, found in zip(annotations, matched):
        for pattern, hit in zip(annotation.patterns, found):
            if not hit and not has_wildcard(pattern):
                result.problems.append(f"{ANNOTATIONS_FILE}: path \"{pattern}\" names no file of the engine")
    projects = nested_project_directories(root)
    if projects and annotations:
        for name in listed_files(root):
            if name.startswith(projects) and any(expression.match(name) for annotation in annotations
                                                 for expression in annotation.expressions):
                result.problems.append(f"{ANNOTATIONS_FILE}: a path matches {name}, a file of a nested project, "
                                       f"whose terms are its own")
                break
    return result


def run(root, check):
    """Adds the missing headers, or with check only reports them.

    @param root the tree's root
    @param check true to change nothing
    @return exit status
    """
    try:
        result = survey(root, write=not check)
    except OSError as error:
        print(f"spdx_headers: {error}", file=sys.stderr)
        return 2
    for name in result.headers_added:
        print(f"{name}: {'has no SPDX header' if check else 'SPDX header added'}")
    for problem in result.problems:
        print(problem)
    counts = ", ".join(f"{count} by {way}" for way, count in result.counts.items())
    if check:
        failed = bool(result.headers_added or result.problems)
        print(f"spdx_headers: {'terms missing' if failed else 'every file states its terms'} ({counts})")
        if result.headers_added:
            print("spdx_headers: python3 tools/spdx_headers.py adds the missing headers")
        return 1 if failed else 0
    print(f"spdx_headers: added {len(result.headers_added)} header(s) ({counts})")
    return 1 if result.problems else 0


SELF_TEST_HEADERS = (
    # (name, file, expected file with the header)
    ("a.cpp", "// A file.\n#include <x>\n",
     f"// {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\n// {LICENCE_TAG}: {LICENCE_EXPRESSION}\n\n// A file.\n#include <x>\n"),
    ("s.py", "#!/usr/bin/env python3\n\"\"\"Doc.\"\"\"\n",
     f"#!/usr/bin/env python3\n# {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\n# {LICENCE_TAG}: {LICENCE_EXPRESSION}\n\n"
     "\"\"\"Doc.\"\"\"\n"),
    ("c.py", "#!/usr/bin/env python3\n# -*- coding: utf-8 -*-\nx = 1\n",
     f"#!/usr/bin/env python3\n# -*- coding: utf-8 -*-\n# {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\n"
     f"# {LICENCE_TAG}: {LICENCE_EXPRESSION}\n\nx = 1\n"),
    ("Dockerfile", "# syntax=docker/dockerfile:1\nFROM x\n",
     f"# syntax=docker/dockerfile:1\n# {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\n# {LICENCE_TAG}: {LICENCE_EXPRESSION}\n\n"
     "FROM x\n"),
    ("CMakeLists.txt", "\ncmake_minimum_required(VERSION 3.24)",
     f"# {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\n# {LICENCE_TAG}: {LICENCE_EXPRESSION}\n\ncmake_minimum_required(VERSION 3.24)\n"),
    ("w.h", "#pragma once\r\nint x;\r\n",
     f"// {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\r\n// {LICENCE_TAG}: {LICENCE_EXPRESSION}\r\n\r\n#pragma once\r\nint x;\r\n"),
    ("e.sh", "",
     f"# {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\n# {LICENCE_TAG}: {LICENCE_EXPRESSION}\n"),
    ("bom.cpp", f"{BYTE_ORDER_MARK}int x;\n",
     f"{BYTE_ORDER_MARK}// {COPYRIGHT_TAG}: {COPYRIGHT_TEXT}\n// {LICENCE_TAG}: {LICENCE_EXPRESSION}\n\nint x;\n"),
)


def self_test():
    """Checks the header, pattern and survey rules on known cases and a small tree; returns the exit status."""
    import tempfile
    failures = []
    for name, text, expected in SELF_TEST_HEADERS:
        comment = comment_style(name, text.split("\n", 1)[0])
        got = with_header(text, comment) if comment else None
        if got != expected:
            failures.append(f"{name}: got {got!r}, expected {expected!r}")
        elif header_tags(got) != ([COPYRIGHT_TEXT], [LICENCE_EXPRESSION]):
            failures.append(f"{name}: the added header reads back as {header_tags(got)}")
    for name, style in (("tools/run", None), ("docs/a.md", None), ("x/.gitignore", HASH_COMMENT),
                        ("cmake/sanitizer-ignorelist.txt", HASH_COMMENT), ("a/b.mm", LINE_COMMENT),
                        ("src/app/game.rc.in", LINE_COMMENT), ("a/b.rc", LINE_COMMENT), ("a/Info.plist.in", None)):
        if comment_style(name, "") != style:
            failures.append(f"{name}: comment style {comment_style(name, '')!r}, expected {style!r}")
    if comment_style("tools/run", "#!/bin/sh") != HASH_COMMENT:
        failures.append("a script without a suffix was not given '#' comments")
    for pattern, name, expected in (("*.md", "README.md", True), ("*.md", "docs/a.md", False),
                                    ("docs/**", "docs/a/b.png", True), ("src/**.md", "src/README.md", True),
                                    ("src/**.md", "src/a/README.md", True), ("src/**.md", "src/a.mdx", False),
                                    ("a\\*b", "a*b", True), ("a\\*b", "axb", False)):
        if bool(pattern_expression(pattern).match(name)) != expected:
            failures.append(f"pattern {pattern!r} on {name!r} did not give {expected}")
    for pattern, expected in (("docs/a.md", False), ("docs/*.md", True), ("a\\*b", False), ("a\\\\*b", True)):
        if has_wildcard(pattern) != expected:
            failures.append(f"pattern {pattern!r}: wildcard {not expected}, expected {expected}")
    for name, expected in (("LICENSE", True), ("docs/COPYING.LESSER", True), ("licenses/zlib.txt", True),
                           ("src/licenses/x.cpp", False), ("src/licence.cpp", False), ("LICENSE-MIT", True)):
        if is_licence_text(name) != expected:
            failures.append(f"{name}: licence text {not expected}, expected {expected}")
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        files = {
            "src/a.cpp": "int a;\n",
            "src/b.hpp": f"// {COPYRIGHT_TAG}: x\n// {LICENCE_TAG}: MIT\n#pragma once\n",
            "src/spaced.cpp": f"//  {COPYRIGHT_TAG}:  {COPYRIGHT_TEXT}  \n// {LICENCE_TAG}: {LICENCE_EXPRESSION}\n",
            "src/named.cpp": f"// {COPYRIGHT_TAG}: 2026 A. Person\n// {LICENCE_TAG}: {LICENCE_EXPRESSION}\n",
            "src/block.c": f"/* {COPYRIGHT_TAG}: {COPYRIGHT_TEXT} */\n/* {LICENCE_TAG}: {LICENCE_EXPRESSION} */\n",
            "src/near.cpp": f"// {COPYRIGHT_TAG}: {COPYRIGHT_HOLDER}\n// {LICENCE_TAG}: {LICENCE_EXPRESSION}\n",
            "src/half.cpp": f"// {LICENCE_TAG}: MIT\nint h;\n",
            "docs/a.md": "# A\n",
            "docs/draw.py": "print(1)\n",
            "docs/b.png": "\x89PNG",
            "data/table.bin": "\x00\x01",
            "LICENSE": "text\n",
            "nested/CMakeLists.txt": "project(n)\nset(x ${OA_ENGINE_DIR})\n",
            "nested/n.md": "# N\n",
            ANNOTATIONS_FILE: f"version = 1\n[[annotations]]\npath = [\"docs/**\", \"*.json\", \"docs/gone.txt\"]\n"
                              f"{COPYRIGHT_TAG} = \"x\"\n{LICENCE_TAG} = \"MIT\"\n",
        }
        for name, text in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(text, encoding="utf-8")
        found = survey(root, write=True)
        if found.headers_added != ["REUSE.toml", "docs/draw.py", "src/a.cpp"]:
            failures.append(f"the tree gained headers in {found.headers_added}")
        expected_problems = [
            f"{COPYRIGHT_FILE}: missing",
            "data/table.bin: states no copyright",
            f"src/b.hpp: its header names MIT, not {LICENCE_EXPRESSION}",
            f"src/b.hpp: its header's copyright is not '{COPYRIGHT_TEXT}'",
            f"src/half.cpp: its header has no {COPYRIGHT_TAG} line",
            f"src/named.cpp: its header's copyright is not '{COPYRIGHT_TEXT}'",
            f"src/near.cpp: its header's copyright is not '{COPYRIGHT_TEXT}'",
            f"{ANNOTATIONS_FILE}: path \"docs/gone.txt\" names no file",
        ]
        for expected in expected_problems:
            if not any(problem.startswith(expected) for problem in found.problems):
                failures.append(f"the tree did not report '{expected}': {found.problems}")
        if len(found.problems) != len(expected_problems):
            failures.append(f"the tree reported {found.problems}")
        again = survey(root, write=True)
        if again.headers_added:
            failures.append(f"a second run added headers to {again.headers_added}")
        (root / COPYRIGHT_FILE).write_text("The holder.\n", encoding="utf-8")
        if any(problem.startswith(f"{COPYRIGHT_FILE}: missing") for problem in survey(root, write=False).problems):
            failures.append(f"a tree with {COPYRIGHT_FILE} is reported as missing it")
        if any(problem.startswith("src/b.hpp") for problem in survey(root, False, {"src/b.hpp"}).problems):
            failures.append("a file listed with other terms was reported")
        (root / ANNOTATIONS_FILE).write_text(
            (root / ANNOTATIONS_FILE).read_text(encoding="utf-8").replace("\"*.json\"", "\"**.md\""),
            encoding="utf-8")
        if not any("nested project" in problem for problem in survey(root, write=False).problems):
            failures.append("an annotation over a nested project was not reported")
    for failure in failures:
        print(f"spdx_headers self-test: {failure}")
    if failures:
        return 1
    print("spdx_headers self-test: passed")
    return 0


def main(argv=None):
    """Parses the command line and adds headers, checks the tree or tests itself; returns the exit status."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT, help="the tree (default: %(default)s)")
    parser.add_argument("--check", action="store_true", help="change nothing; fail when a file states no terms")
    parser.add_argument("--self-test", action="store_true", help="check the tool's own rules")
    args = parser.parse_args(argv)
    if tomllib is None:
        print(f"spdx_headers: needs Python {'.'.join(map(str, MIN_PYTHON))} or newer to read {ANNOTATIONS_FILE}, "
              f"and this is Python {platform.python_version()}", file=sys.stderr)
        return 2
    if args.self_test:
        return self_test()
    root = args.root.resolve()
    if not root.is_dir():
        print(f"spdx_headers: no such tree: {root}", file=sys.stderr)
        return 2
    return run(root, args.check)


if __name__ == "__main__":
    sys.exit(main())
