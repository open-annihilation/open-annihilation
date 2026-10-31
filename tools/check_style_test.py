#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Plant breaches of every check_style.py rule in small trees and require the check to find exactly them.

The names and hex numbers the name rules look for are put together here at
run time from pieces, so that this file itself holds none of them.
"""
import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_style as style  # noqa: E402

failures = []
checked = 0


def text(*pieces):
    """Joins a fixture from pieces."""
    return "".join(pieces)


def check(condition, what):
    """Requires a condition, recording what failed."""
    global checked
    checked += 1
    if not condition:
        failures.append(what)


def write_tree(root, files):
    """Writes files, given as path and text, under root."""
    for name, content in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)


def tree_findings(scratch, label, files):
    """Writes a tree of files under scratch and returns its findings as (path, line, rule, text)."""
    root = scratch / label
    write_tree(root, files)
    _, findings = style.tree_findings(root)
    return {(finding.path, finding.line, finding.rule, finding.text) for finding in findings}


def expect_findings(scratch, label, files, expected, rules=None):
    """Requires a tree's findings of the given rules (all when None) to be exactly those expected."""
    global checked
    found = {finding for finding in tree_findings(scratch, label, files) if rules is None or finding[2] in rules}
    expected = set(expected)
    checked += len(expected | found)
    for finding in sorted(expected - found):
        failures.append(f"{label}: not reported: {finding}")
    for finding in sorted(found - expected):
        failures.append(f"{label}: reported: {finding}")


def run_main(argv):
    """Runs the check's main with arguments and returns its exit status and output."""
    output = io.StringIO()
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
        try:
            status = style.main(argv)
        except SystemExit as stop:
            status = stop.code
    return status, output.getvalue()


def test_fixed_width(scratch):
    """Finds std::-qualified fixed-width types in code only."""
    source = "\n".join([
        "std::int32_t a{};",
        "int32_t b{};",
        "// std::uint8_t in a comment",
        'const char* c = "std::uint16_t";',
        "std::uint_least8_t d{}; std::size_t e{};",
        "std::intptr_t f{};",
    ]) + "\n"
    rule = {"qualified-fixed-width"}
    expect_findings(scratch, "fixed-width", {"src/m/a.cpp": source}, {
        ("src/m/a.cpp", 1, "qualified-fixed-width", "std::int32_t"),
        ("src/m/a.cpp", 5, "qualified-fixed-width", "std::uint_least8_t"),
        ("src/m/a.cpp", 6, "qualified-fixed-width", "std::intptr_t"),
    }, rule)


def test_names(scratch):
    """Finds each kind of offset-derived and placeholder name, and passes bit widths and unnumbered unknowns."""
    field = text("field_", "1c0")
    flags = text("flags_", "241")
    word = text("word_", "3a")
    placeholder = text("FU", "N_", "00a1b2")
    address = text("query_", "00a1", "b2c3")
    unresolved = text("unk_", "0x", "1c")
    unknown_field = text("unkn", "own_3")
    unknown_bit = text("OA_UNIT_FLAG_UNKN", "OWN_BIT5")
    unknown_camel = text("kLabelUnkn", "own1")
    unknown_short = text("un", "k_7")
    source = "\n".join([
        f"int {field}{{}};",
        f"int {flags}{{}};",
        f"int {word}{{}};",
        f"void {placeholder}();",
        f"void {address}();",
        f'const char* note = "{unresolved} in a string";',
        f"// {unresolved} in a comment",
        "int flags32{}; int word16{}; int allegiance_unknown{}; int kProviderUnknown{};",
        f"int {unknown_field}{{}};",
        f"#define {unknown_bit} 0x20u",
        f"int {unknown_camel}{{}};",
        f"int {unknown_short}{{}};",
    ]) + "\n"
    expect_findings(scratch, "names", {"src/m/a.cpp": source}, {
        ("src/m/a.cpp", 1, "name-offset", field),
        ("src/m/a.cpp", 2, "name-offset", flags),
        ("src/m/a.cpp", 3, "name-offset", word),
        ("src/m/a.cpp", 4, "name-placeholder", placeholder),
        ("src/m/a.cpp", 5, "name-address", address),
        ("src/m/a.cpp", 6, "name-unresolved", unresolved),
        ("src/m/a.cpp", 7, "name-unresolved", unresolved),
        ("src/m/a.cpp", 9, "name-unknown", unknown_field),
        ("src/m/a.cpp", 10, "name-unknown", unknown_bit),
        ("src/m/a.cpp", 11, "name-unknown", unknown_camel),
        ("src/m/a.cpp", 12, "name-unknown", unknown_short),
    }, {"name-offset", "name-placeholder", "name-address", "name-unresolved", "name-unknown"})


def test_flags_masks(scratch):
    """Finds hex masks on names ending in flags, but not named bits or other names."""
    source = "\n".join([
        "if (u.flags & 0x10) {}",
        "u.state_flags |= 0x4u;",
        "x = 0x20 & unit->flags;",
        "u.flags &= ~0x10u;",
        "u.flags2 ^= static_cast<uint32_t>(0x1);",
        "if (u.flags & OA_UNIT_FLAG_SELECTED) {}",
        "x = count & 0x10;",
        "if (flags && ready) {}",
        "// flags & 0x10 in a comment",
    ]) + "\n"
    expect_findings(scratch, "flags-masks", {"src/m/a.cpp": source}, {
        ("src/m/a.cpp", 1, "flags-raw-mask", "flags & 0x10"),
        ("src/m/a.cpp", 2, "flags-raw-mask", "state_flags |= 0x4"),
        ("src/m/a.cpp", 3, "flags-raw-mask", "0x20 & unit->flags"),
        ("src/m/a.cpp", 4, "flags-raw-mask", "flags &= ~0x10"),
        ("src/m/a.cpp", 5, "flags-raw-mask", "flags2 ^= static_cast<uint32_t>(0x1"),
    }, {"flags-raw-mask"})


MEMBERS = """enum class Kind : uint8_t { a, b };
using Tick = uint32_t;
struct Plain {
    int32_t a;
    int32_t b{};
    const char* c;
    float d = 0;
    Kind k;
    bool (*fn)(int);
    void (*set)(int) {};
    std::string s;
    int& r;
    static int t;
    uint8_t bits : 3;
    int32_t e[4];
    int x{}, y;
    Tick when;
    unsigned long long big;
    std::vector<int*> list;
    union {
        int32_t in_union;
        float other;
    };
    struct {
        int32_t nested;
    } inner{};
    int f() const { return a; }
};
class Hidden {
    int32_t a;
};
union Either {
    int32_t a;
    float b;
};
struct Built {
    Built() : a(0) {}
    int32_t a;
};
struct Base {
    virtual ~Base() = default;
    int32_t a;
};
"""


def test_members(scratch):
    """Finds scalar, enumeration and pointer members of plain C++ structs without an initializer."""
    files = {"src/m/a.hpp": MEMBERS, "src/m/b.cpp": MEMBERS, "src/m/c.h": MEMBERS}
    expected = set()
    for name in ("src/m/a.hpp", "src/m/b.cpp"):
        expected |= {
            (name, 4, "member-initializer", "a"),
            (name, 6, "member-initializer", "c"),
            (name, 8, "member-initializer", "k"),
            (name, 9, "member-initializer", "fn"),
            (name, 15, "member-initializer", "e"),
            (name, 16, "member-initializer", "y"),
            (name, 17, "member-initializer", "when"),
            (name, 18, "member-initializer", "big"),
            (name, 25, "member-initializer", "nested"),
        }
    expect_findings(scratch, "members", files, expected, {"member-initializer"})


def test_offset_comments(scratch):
    """Finds offset comments everywhere but the file-format directories, pinned records included."""
    lines = [
        "int a{}; // Unit +0x1c",
        "int b{}; // the order's count (+3a)",
        "int c{}; // offset 0x10 of the record",
        "int d{}; // at (width + 0x80) / 2",
        "int e{}; // origin (+128, +32)",
        "int f{}; /* +0c */",
    ]
    source = "\n".join(lines) + "\n"
    pinned = "\n".join([
        "typedef struct Pinned {",
        "    int32_t a; /* +0x00 */",
        "    int32_t b; /* +0x04 */",
        "} Pinned;",
        "OA_ASSERT_OFFSET(Pinned, b, 0x4);",
        "typedef struct Loose {",
        "    int32_t a; /* +0x00 */",
        "} Loose;",
    ]) + "\n"
    files = {"src/game/m/a.cpp": source, "src/formats/m/a.cpp": source, "src/data/persist/a.cpp": source,
             "src/data/unit-definitions/a.cpp": source, "src/core/include/a.h": pinned}
    expect_findings(scratch, "offset-comments", files, {
        ("src/game/m/a.cpp", 1, "offset-comment", "+0x1c"),
        ("src/game/m/a.cpp", 2, "offset-comment", "+3a"),
        ("src/game/m/a.cpp", 3, "offset-comment", "offset 0x10"),
        ("src/game/m/a.cpp", 6, "offset-comment", "+0c"),
        ("src/data/unit-definitions/a.cpp", 1, "offset-comment", "+0x1c"),
        ("src/data/unit-definitions/a.cpp", 2, "offset-comment", "+3a"),
        ("src/data/unit-definitions/a.cpp", 3, "offset-comment", "offset 0x10"),
        ("src/data/unit-definitions/a.cpp", 6, "offset-comment", "+0c"),
        ("src/core/include/a.h", 2, "offset-comment", "+0x00"),
        ("src/core/include/a.h", 3, "offset-comment", "+0x04"),
        ("src/core/include/a.h", 7, "offset-comment", "+0x00"),
    }, {"offset-comment"})


DECLARATIONS = """#pragma once
#include <cstdint>

namespace oa::m {

/// Does a.
int a();
int b();
/// Doc.
template <typename T>
T c(T x);
[[nodiscard]] int d();
/// Doc.
[[nodiscard]] int
e(int value);
int
f(int value);

struct S {
    S();
    S(const S&) = delete;
    S(S&&) = default;
    /// Doc.
    ~S();
    int g() const { return 1; }
    bool operator==(const S&) const;
    int (*fp)(int){};
    int value{};

  private:
    /// Doc.
    void h();
    void i() noexcept;
};

inline int S::g2() { return 2; }
inline const int v = compute(3);
OA_ASSERT_SIZE(S, 8);
extern "C" void j(void);

} // namespace oa::m
"""


def test_doc_blocks(scratch):
    """Finds header functions without a /// line directly above them."""
    files = {"src/m/include/oa/m.hpp": DECLARATIONS, "src/m/src/m.cpp": DECLARATIONS}
    name = "src/m/include/oa/m.hpp"
    expect_findings(scratch, "doc-blocks", files, {
        (name, 8, "doc-block", "b"),
        (name, 12, "doc-block", "d"),
        (name, 16, "doc-block", "f"),
        (name, 20, "doc-block", "S"),
        (name, 25, "doc-block", "g"),
        (name, 26, "doc-block", "operator"),
        (name, 33, "doc-block", "i"),
        (name, 39, "doc-block", "j"),
    }, {"doc-block"})


def test_tiers(scratch):
    """Finds virtual, throw and heap containers in tier directories outside their tests."""
    source = "\n".join([
        "struct Host { virtual void run() = 0; };",
        "void fail() { throw 1; }",
        "std::vector<int> values;",
        "std::string_view name;",
        "std::array<int, 3> fixed{};",
        "// virtual, throw and std::vector in a comment",
    ]) + "\n"
    files = {"src/sim/m/src/a.cpp": source, "src/sim/m/tests/a_test.cpp": source, "src/present/a.cpp": source}
    expect_findings(scratch, "tiers", files, {
        ("src/sim/m/src/a.cpp", 1, "tier-virtual", "virtual"),
        ("src/sim/m/src/a.cpp", 2, "tier-throw", "throw"),
        ("src/sim/m/src/a.cpp", 3, "tier-heap-container", "std::vector"),
    }, {"tier-virtual", "tier-throw", "tier-heap-container"})


def test_test_asserts(scratch):
    """Finds assert() and #undef NDEBUG in tests, and only there."""
    source = "\n".join([
        "#undef NDEBUG",
        "#include <cassert>",
        "void run() { assert(1 + 1 == 2); }",
        "static_assert(sizeof(int) == 4);",
        "void same() { OA_CHECK(true); runner.assert(true); checks::assert (true); }",
        "// assert(false) in a comment",
        "const char* text = \"assert(false)\";",
        "  #  undef   NDEBUG",
        "void spaced() { assert (true); }",
    ]) + "\n"
    files = {
        "src/sim/m/tests/a_test.cpp": source,
        "src/app/b_test.cpp": source,
        "tests/content/c.cpp": source,
        "src/sim/m/src/d.cpp": source,
        "src/app/e_testing.cpp": source,
    }
    expected = set()
    for name in ("src/sim/m/tests/a_test.cpp", "src/app/b_test.cpp", "tests/content/c.cpp"):
        expected |= {
            (name, 1, "test-assert", "#undef NDEBUG"),
            (name, 3, "test-assert", "assert("),
            (name, 8, "test-assert", "#undef NDEBUG"),
            (name, 9, "test-assert", "assert("),
        }
    expect_findings(scratch, "test-asserts", files, expected, {"test-assert"})


def test_path_narrow(scratch):
    """Finds a path written out through the code page outside tests, and leaves its UTF-8 spelling alone."""
    header = "\n".join([
        "struct Options { std::filesystem::path snapshot{}; };",
        "fs::path data_folder();",
    ]) + "\n"
    source = "\n".join([
        "void f(const fs::path& folder, const Options& options) {",
        "    std::string a = folder.string();",
        "    std::string b = entry.path().filename().string();",
        "    std::string c = other.generic_string();",
        "    std::cout << fs::absolute(folder) << '\\n';",
        "    std::string d = options.snapshot.string();",
        "    std::string e = data_folder().string();",
        "    auto stem = folder / \"x\";",
        "    std::string g = stem.string();",
        "    const std::filesystem::path made(argv[1]);",
        "    std::cerr << made.string();",
        "    std::string h = oa::platform::path_to_utf8(folder);",
        "    std::cout << folder.native().size() << text.string() << '\\n';",
        "    // folder.string() in a comment",
        "}",
        "void h(fs::path value) {}",
        "void g(const Json& value) {",
        "    if (value.string() != nullptr) {}",
        "}",
    ]) + "\n"
    files = {"src/app/options.hpp": header, "src/app/a.cpp": source, "src/app/a_test.cpp": source,
             "src/app/tests/b.cpp": source}
    expect_findings(scratch, "path-narrow", files, {
        ("src/app/a.cpp", 2, "path-narrow", "folder.string()"),
        ("src/app/a.cpp", 3, "path-narrow", ".filename().string()"),
        ("src/app/a.cpp", 4, "path-narrow", ".generic_string()"),
        ("src/app/a.cpp", 5, "path-narrow", "<< fs::absolute(folder)"),
        ("src/app/a.cpp", 6, "path-narrow", "snapshot.string()"),
        ("src/app/a.cpp", 7, "path-narrow", "data_folder().string()"),
        ("src/app/a.cpp", 9, "path-narrow", "stem.string()"),
        ("src/app/a.cpp", 11, "path-narrow", "made.string()"),
    }, {"path-narrow"})


def test_lexer(scratch):
    """Separates code from comments, literals and directives."""
    source = "\n".join([
        'const char* raw = R"x(// std::int32_t )x"; std::int16_t after{};',
        "int n = 1'000; std::int8_t sep{};",
        "char q = '\"'; std::int64_t quoted{};",
        "/* std::uint32_t",
        "   std::uint64_t */ std::uintptr_t tail{};",
        "#define WIDE(x) \\",
        "    std::uintmax_t x",
    ]) + "\n"
    expect_findings(scratch, "lexer", {"src/m/a.cpp": source}, {
        ("src/m/a.cpp", 1, "qualified-fixed-width", "std::int16_t"),
        ("src/m/a.cpp", 2, "qualified-fixed-width", "std::int8_t"),
        ("src/m/a.cpp", 3, "qualified-fixed-width", "std::int64_t"),
        ("src/m/a.cpp", 5, "qualified-fixed-width", "std::uintptr_t"),
        ("src/m/a.cpp", 7, "qualified-fixed-width", "std::uintmax_t"),
    }, {"qualified-fixed-width"})
    lines = style.split_lines(source)
    check([line.directive for line in lines] == [False] * 5 + [True, True], "lexer: directive lines")
    check(style.split_lines("/// doc\n//// rule\nint a;\n")[0].doc and not style.split_lines("//// rule\n")[0].doc,
          "lexer: /// lines are doc lines, //// lines are not")


def test_nested_projects(scratch):
    """Leaves out nested projects, third-party code and files that are not sources."""
    source = "std::int32_t a{};\n"
    project = "cmake_minimum_required(VERSION 3.24)\nproject(nested)\nset(OA_ENGINE_DIR .. CACHE PATH \"\")\n"
    expect_findings(scratch, "nested", {"src/m/a.cpp": source, "nested/CMakeLists.txt": project,
                                        "nested/src/a.cpp": source, "src/m/notes.txt": source,
                                        "third_party/lib/lib.h": source}, {
        ("src/m/a.cpp", 1, "qualified-fixed-width", "std::int32_t"),
    })


def test_directories():
    """Gives each file to its nearest CMake directory, else to its first two path components."""
    build = {"src/game/unit-health", "tests/content"}
    for name, owner in (("src/game/unit-health/src/a.cpp", "src/game/unit-health"),
                        ("src/game/unit-health/include/oa/a.hpp", "src/game/unit-health"),
                        ("src/app/runtime.hpp", "src/app"),
                        ("src/ballistics/include/oa/b.hpp", "src/ballistics"),
                        ("src/main.cpp", "src"),
                        ("tests/support/include/oa/t.hpp", "tests/support"),
                        ("tests/content/a_test.cpp", "tests/content")):
        check(style.owner_directory(name, build) == owner, f"directories: {name} owned by {owner}")


def test_ratchet(scratch):
    """Fails on growth and on slack, counted per file; --update writes and lowers, never raising unasked."""
    root = scratch / "ratchet"
    one = "std::int32_t a{};\n"
    two = "std::int32_t a{};\nstd::int32_t b{};\n"
    write_tree(root, {"src/m/CMakeLists.txt": "", "src/m/a.cpp": two})
    status, output = run_main(["--root", str(root)])
    check(status == 2 and "baseline" in output, f"ratchet: a missing baseline is refused (status {status})")
    status, output = run_main(["--root", str(root), "--update"])
    baseline = root / style.DEFAULT_BASELINE
    check(status == 0 and json.loads(baseline.read_text())["counts"] == {"src/m/a.cpp": {"qualified-fixed-width": 2}},
          f"ratchet: --update writes the counts per file when there is no baseline ({output.strip()})")
    status, output = run_main(["--root", str(root)])
    check(status == 0 and "clean, every count equal to the baseline" in output,
          f"ratchet: the recorded tree passes ({output})")
    write_tree(root, {"src/m/a.cpp": two + "std::int32_t c{};\n"})
    status, output = run_main(["--root", str(root)])
    check(status == 1 and "src/m/a.cpp:3: qualified-fixed-width" in output,
          f"ratchet: a count that grows fails and shows its findings ({output})")
    status, output = run_main(["--root", str(root), "--update"])
    check(status == 1 and json.loads(baseline.read_text())["counts"]["src/m/a.cpp"]["qualified-fixed-width"] == 2,
          f"ratchet: --update does not raise a count ({output})")
    write_tree(root, {"src/m/a.cpp": two, "src/m/b.cpp": one})
    status, output = run_main(["--root", str(root)])
    check(status == 1 and "src/m/b.cpp: qualified-fixed-width: 1 finding(s) against a baseline of 0" in output,
          f"ratchet: a new file with a finding fails ({output})")
    write_tree(root, {"src/m/b.cpp": "int32_t b{};\n", "src/m/a.cpp": two + "int a;\nstruct P { int32_t p; };\n"})
    status, output = run_main(["--root", str(root)])
    check(status == 1 and "src/m/a.cpp: member-initializer: 1 finding(s) against a baseline of 0" in output,
          f"ratchet: a rule new to a file fails ({output})")
    write_tree(root, {"src/m/a.cpp": one, "src/m/b.cpp": one})
    status, output = run_main(["--root", str(root)])
    check(status == 1 and "src/m/b.cpp: qualified-fixed-width: 1 finding(s) against a baseline of 0" in output
          and "src/m/a.cpp: qualified-fixed-width: 1 finding(s) against a baseline of 2" in output,
          f"ratchet: a finding fixed in one file makes no room for one in another of the same directory ({output})")
    write_tree(root, {"src/m/b.cpp": "int32_t b{};\n"})
    status, output = run_main(["--root", str(root)])
    check(status == 1 and "src/m/a.cpp: qualified-fixed-width: 1 finding(s) against a baseline of 2" in output
          and "slack" in output and "--update" in output,
          f"ratchet: a count that fell fails until the baseline is lowered ({output})")
    check(json.loads(baseline.read_text())["counts"]["src/m/a.cpp"]["qualified-fixed-width"] == 2,
          "ratchet: a plain run writes nothing")
    status, output = run_main(["--root", str(root), "--update"])
    check(status == 0 and json.loads(baseline.read_text())["counts"] == {"src/m/a.cpp": {"qualified-fixed-width": 1}},
          f"ratchet: --update lowers a count ({output})")
    status, output = run_main(["--root", str(root)])
    check(status == 0, f"ratchet: the lowered baseline passes ({output})")
    (root / "src/m/a.cpp").unlink()
    status, output = run_main(["--root", str(root)])
    check(status == 1 and "src/m/a.cpp: qualified-fixed-width: 0 finding(s) against a baseline of 1" in output,
          f"ratchet: the entry of a file that is gone is slack ({output})")
    status, output = run_main(["--root", str(root), "--update"])
    check(status == 0 and json.loads(baseline.read_text())["counts"] == {},
          f"ratchet: --update removes the entries of files that are gone ({output})")
    write_tree(root, {"src/n/moved.cpp": one})
    status, output = run_main(["--root", str(root), "--update", "--accept-growth"])
    check(status == 0 and "src/n/moved.cpp" in json.loads(baseline.read_text())["counts"],
          f"ratchet: --accept-growth records growth ({output})")
    status, output = run_main(["--root", str(root), "--accept-growth"])
    check(status == 2, f"ratchet: --accept-growth alone is refused ({output})")
    write_tree(root, {"src/m/a.cpp": two})
    status, output = run_main(["--root", str(root), "--report"])
    check(status == 0 and "src/m: 2" in output and "src/n: 1" in output
          and "qualified-fixed-width: 3 in 2 director(ies)" in output,
          f"ratchet: --report prints the counts per directory ({output})")
    status, output = run_main(["--root", str(root), "--list", "--rule", "qualified-fixed-width"])
    check(status == 0 and output.count("qualified-fixed-width") == 3, f"ratchet: --list prints findings ({output})")
    for broken in ('{"version": 2, "counts": {"src/m/a.cpp": {"no-such-rule": 1}}}', '{"version": 1, "counts": {}}',
                   '{"version": 2, "counts": {"src/m/a.cpp": {"qualified-fixed-width": -1}}}',
                   '{"version": 2, "counts": {"src/m/a.cpp": {"qualified-fixed-width": 0}}}',
                   '{"version": 2, "counts": {"src/m/a.cpp": {}}}', "not json"):
        baseline.write_text(broken)
        status, output = run_main(["--root", str(root)])
        check(status == 2, f"ratchet: a broken baseline is refused: {broken} ({output})")


def main():
    """Runs every test and returns the exit status."""
    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        test_fixed_width(scratch)
        test_names(scratch)
        test_flags_masks(scratch)
        test_members(scratch)
        test_offset_comments(scratch)
        test_doc_blocks(scratch)
        test_tiers(scratch)
        test_test_asserts(scratch)
        test_path_narrow(scratch)
        test_lexer(scratch)
        test_nested_projects(scratch)
        test_directories()
        test_ratchet(scratch)
    for failure in failures:
        print(f"check_style_test: {failure}")
    print(f"check_style_test: {checked - len(failures)} of {checked} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
