#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the engine keeps its layout: modules, links, layers, names and graphics links.

A module is a directory under src/ whose CMakeLists.txt defines targets
(src/<group>/<module>, or src/<group> itself for a group-level module such
as src/core); its group is the first directory under src/. The check reads
the build files and sources under --root and the targets a configuration
defined (--targets, written by cmake/OaLayout.cmake), and fails on:

  reach        a build file that names a directory outside its own with ..
               (add_subdirectory, an include directory or a source), or a
               target's include directory or source in another module
  foreign      a target linked from another directory's CMakeLists.txt,
               whether the configuration made the link or a build file
               under src/ names it in a branch this platform does not take
               (target_link_libraries of a target another directory's
               build file adds)
  include      an #include that leaves its module by a relative path, a
               public header's relative #include of a file outside its
               module's include/ (which the targets that export the header
               do not carry), or an #include that names another module's
               header when no link of the including target (or of the
               header's own target) reaches a target that exports it
  layer        a link from a module library to a library of a later layer
               (the order is core; base; platform and formats, which may
               not use each other; data; sim, which may not use platform;
               present, audio and media; ui, netgame and session; app;
               boneyards, which may use every earlier layer and which no
               other layer may use),
               unless
               tools/layout-baseline.json lists it
  stale        a baseline entry for a link that no longer exists, which
               must be removed: the baseline may only shrink
  name         a module library not named oa-<group>-<module>[-part]
               (oa-<group>[-part] in a group-level module); any library of
               the tree (the build files at the root and under cmake/, src/
               and tests/, every platform's branch included) not named
               oa-<group>[-<part>] or without the ALIAS
               oa::<group>[::<part>], the part's hyphens as underscores; or
               a public header outside include/oa/<group>/<module>/ and
               include/oa/<group>/<module>.* (include/oa/<group>/ in a
               group-level module)
  graphics     a target of the tree, test or tool included, that links a
               graphics API: a library or framework GRAPHICS_LIBRARIES
               names (d3d9, d3d11, d3d12, dxgi, opengl32, vulkan-1, GL,
               EGL, vulkan, Metal, QuartzCore and the like), by name, as
               -l<name> or -l:<file>, as a file or framework path, or as
               one of the OpenGL:: and Vulkan:: imported targets. It is
               found where the configuration made the link, where any
               build file at the root or under cmake/, src/ and tests/
               makes it, in any branch (calls over several lines,
               generator expressions, the variables the build files and
               CMake's OpenGL and Vulkan packages set, link options and
               target and directory link properties included), and where
               a source under src/, tests/ or tools/ asks the linker for
               it outside a comment (comment(lib, ...) or
               comment(linker, "/DEFAULTLIB:...")). A variable a link
               names is read with every value any build file gives it, in
               any function or scope, so a link's variable needs a name of
               its own. The engine reaches a graphics API only at run
               time, through the objects SDL made or the system loader,
               so that no package imports one; what SDL's own target
               brings with it is SDL's and is not read

Tests (executables) and tools may link anything, but must still link what
they include, and link no graphics API. Code outside src/ is not checked,
apart from the names of its libraries and its graphics links.

--self-test checks a small built-in tree. Exit status is 1 on any finding
and 2 when the targets file or the baseline cannot be read.
"""
import argparse
import contextlib
import io
import json
import os
import posixpath
import re
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASELINE = ROOT / "tools" / "layout-baseline.json"
LAYERS = {"core": 1, "base": 2, "platform": 3, "formats": 3, "data": 4, "sim": 5, "present": 6, "audio": 6,
          "media": 6, "ui": 7, "netgame": 7, "session": 7, "app": 8, "boneyards": 9}
# Pairs of groups in one layer that may not use each other.
APART = {("platform", "formats"), ("formats", "platform")}
# Groups a group may not use although they come earlier.
FORBIDDEN = {("sim", "platform")}
# Groups whose directory is itself a module (src/core, not src/core/<module>).
GROUP_LEVEL = {"core", "platform", "present", "audio", "media", "netgame", "app"}
LIBRARY_TYPES = {"STATIC_LIBRARY", "SHARED_LIBRARY", "MODULE_LIBRARY", "OBJECT_LIBRARY", "INTERFACE_LIBRARY"}
# Targets that are build infrastructure rather than modules.
INFRASTRUCTURE = {"oa-options", "oa-extension-sdk", "oa-test-game-data"}
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".mm", ".m"}
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(["<])([^">\n]+)[">]', re.M)
COMMAND_RE = re.compile(r"(?m)^[ \t]*(add_subdirectory|target_include_directories|add_library|add_executable|"
                        r"target_sources)[ \t]*\(([^)]*)\)")
ALIAS_RE = re.compile(r"add_library\(\s*(\S+)\s+ALIAS\s+(\S+)\s*\)")
DEFINE_RE = re.compile(r"(?m)^[ \t]*(?:add_library|add_executable)[ \t]*\(\s*([^\s)]+)")
LIBRARY_RE = re.compile(r"(?m)^[ \t]*add_library[ \t]*\(\s*([^\s)]+)([^)]*)\)")
# The folders under the root whose build files define the tree's libraries,
# besides the root's own CMakeLists.txt.
BUILD_FILE_FOLDERS = ("cmake", "src", "tests")
LINK_RE = re.compile(r"(?m)^[ \t]*target_link_libraries[ \t]*\(\s*([^\s)]+)")
# The largest baseline or targets file read; the tree's are a few kilobytes
# and a few megabytes.
MAX_BASELINE_BYTES = 1 << 20
MAX_TARGETS_BYTES = 64 << 20

# The graphics libraries no target of the tree links, lower case, as the
# linker names them: the Windows import libraries, the Linux and other Unix
# shared libraries, and the Apple frameworks. A link to any of them would
# make every start load the driver, or keep the program from starting where
# the library is missing (an older Windows, a Linux without the library).
GRAPHICS_LIBRARIES = frozenset({
    "d3d9", "d3d10", "d3d10_1", "d3d11", "d3d12", "dxgi", "ddraw", "d2d1", "dcomp", "opengl32", "glu32", "vulkan-1",
    "gl", "glx", "egl", "glesv1_cm", "glesv2", "glu", "opengl", "vulkan", "gbm", "drm",
    "metal", "metalkit", "metalfx", "metalperformanceshaders", "quartzcore", "agl",
})
# The shader compiler's import library, with or without its version.
GRAPHICS_LIBRARY_RE = re.compile(r"d3dcompiler(?:_\d+)?")
# The namespaces of the imported targets CMake's OpenGL and Vulkan packages define.
GRAPHICS_NAMESPACES = ("opengl::", "vulkan::")
# The variables CMake's OpenGL and Vulkan packages set to the libraries they find.
GRAPHICS_VARIABLE_RE = re.compile(r"(?:opengl|vulkan|egl|gles\w*)_\w*librar(?:y|ies)", re.IGNORECASE)
# A file name's library suffix: a shared library and its version, an archive,
# an import library, a framework.
LIBRARY_SUFFIX_RE = re.compile(r"\.(?:so(?:\.\d+)*|a|dylib|tbd|lib|dll|framework)$", re.IGNORECASE)
# A Windows linker option (/SUBSYSTEM:WINDOWS, /NODEFAULTLIB:name), which names no library to link.
LINKER_OPTION_RE = re.compile(r"/[A-Z_]+(?::.*)?")
# The Windows linker option that names a library to link.
DEFAULT_LIBRARY_OPTION = "/DEFAULTLIB:"
# The Unix linker option that names a library by its file name (-l:libGL.so.1).
EXACT_LIBRARY_OPTION = "-l:"
# Linker options whose next word is a folder to search, not a library to link.
PATH_OPTIONS = frozenset({"-L", "-F", "-rpath", "-rpath-link", "--rpath", "--rpath-link"})
# Compiler options that hand the next word to the linker, and so stand between
# a search-path option and its folder.
LINKER_PASS_OPTIONS = frozenset({"-Wl", "-Xlinker"})
# Prefixes CMake reads off a link option before handing it on.
LINK_OPTION_PREFIXES = ("LINKER:", "SHELL:")
# A source's request that the linker add a library: #pragma comment(lib, "name")
# and #pragma comment(linker, "/DEFAULTLIB:name"), and their __pragma and
# _Pragma forms. The text is the name, or the linker's options.
PRAGMA_LIBRARY_RE = re.compile(r'\bcomment\s*\(\s*(?:lib|linker)\s*,\s*\\?"([^"\\]+)')
# The comments of a C, C++ or Objective-C source, and the string and character
# literals that may hold what looks like one.
SOURCE_COMMENT_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/', re.S)
# The words of target_link_libraries that are not libraries.
LINK_KEYWORDS = frozenset({"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE",
                           "LINK_INTERFACE_LIBRARIES", "debug", "optimized", "general"})
# The words of target_link_options and add_link_options that are not options.
LINK_OPTION_KEYWORDS = frozenset({"BEFORE", "PUBLIC", "PRIVATE", "INTERFACE"})
# The target properties that hold what a target links.
LINK_PROPERTY_RE = re.compile(r"(?:INTERFACE_)?LINK_(?:LIBRARIES|OPTIONS)(?:_DIRECT)?|LINK_FLAGS(?:_\w+)?|"
                              r"STATIC_LIBRARY_OPTIONS|IMPORTED_(?:LOCATION|IMPLIB|LIBNAME)(?:_\w+)?|"
                              r"IMPORTED_LINK_INTERFACE_LIBRARIES(?:_\w+)?")
# The variables that hold link options or libraries for every target.
LINK_VARIABLE_RE = re.compile(r"CMAKE_\w*(?:LINKER_FLAGS|STANDARD_LIBRARIES)\w*")
# Generator expressions that give 0 or 1, or compare, and so never name a library.
GENEX_CONDITIONS = frozenset({
    "BOOL", "AND", "OR", "NOT", "STREQUAL", "EQUAL", "IN_LIST", "VERSION_LESS", "VERSION_GREATER",
    "VERSION_EQUAL", "VERSION_LESS_EQUAL", "VERSION_GREATER_EQUAL", "PATH_EQUAL", "TARGET_EXISTS", "CONFIG",
    "PLATFORM_ID", "COMPILE_LANGUAGE", "LINK_LANGUAGE", "COMPILE_LANG_AND_ID", "LINK_LANG_AND_ID",
    "COMPILE_FEATURES", "TARGET_POLICY", "C_COMPILER_ID", "CXX_COMPILER_ID", "OBJC_COMPILER_ID",
    "OBJCXX_COMPILER_ID", "C_COMPILER_VERSION", "CXX_COMPILER_VERSION", "OBJC_COMPILER_VERSION",
    "OBJCXX_COMPILER_VERSION",
})
# Generator expressions whose first argument is not a value they give: $<IF:condition,a,b>,
# $<LINK_LIBRARY:feature,libraries>, $<LINK_GROUP:feature,libraries>, $<TARGET_GENEX_EVAL:target,text>.
GENEX_SKIP_FIRST = frozenset({"IF", "LINK_LIBRARY", "LINK_GROUP", "TARGET_GENEX_EVAL"})
# The arguments of find_library that are not the names it looks for.
FIND_LIBRARY_KEYWORDS = frozenset({
    "NAMES_PER_DIR", "HINTS", "PATHS", "PATH_SUFFIXES", "VALIDATOR", "DOC", "REQUIRED", "OPTIONAL", "NO_CACHE",
    "REGISTRY_VIEW", "ENV", "NO_DEFAULT_PATH", "NO_PACKAGE_ROOT_PATH", "NO_CMAKE_PATH",
    "NO_CMAKE_ENVIRONMENT_PATH", "NO_SYSTEM_ENVIRONMENT_PATH", "NO_CMAKE_SYSTEM_PATH",
    "NO_CMAKE_INSTALL_PREFIX", "CMAKE_FIND_ROOT_PATH_BOTH", "ONLY_CMAKE_FIND_ROOT_PATH",
    "NO_CMAKE_FIND_ROOT_PATH",
})
# A variable reference, innermost first: ${name}, $CACHE{name}, $ENV{name}.
VARIABLE_REFERENCE_RE = re.compile(r"\$(CACHE|ENV)?\{([^${}]*)\}")
# How many variables deep a value is expanded through the variables it names;
# each variable is expanded once, so this only bounds the recursion.
MAX_EXPANSION_DEPTH = 64
# How many times a text is expanded again for references its expansion forms
# (${prefix_${kind}}).
MAX_NESTING = 8
# The depth an expansion reports when it left out no variable being expanded.
NO_CUT = sys.maxsize
# A CMake command's name, and the characters that end an unquoted argument.
COMMAND_NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
BRACKET_OPEN_RE = re.compile(r"\[(=*)\[")
UNQUOTED_END = frozenset(' \t\r\n()#"')
# The words of set() after which its arguments are no longer the variable's values.
SET_STOP_WORDS = frozenset({"CACHE", "PARENT_SCOPE"})
# What a link made through a directory's properties is made for.
DIRECTORY_TARGETS = "every target of the directory"
# What the graphics finding tells the reader to do instead.
GRAPHICS_ADVICE = "reach the API at run time through the objects SDL made or the system loader instead"
# What a graphics finding made through a variable adds: variables are read
# across the whole tree, not scope by scope.
GRAPHICS_VARIABLE_NOTE = ("a variable a link names is read with every value any build file, function or branch "
                          "gives it, so a link's variable needs a name of its own")


class InputError(Exception):
    """A targets file or baseline that cannot be read."""


def read_bounded(path, limit, what):
    """Read a UTF-8 text file of at most limit bytes."""
    path = Path(path)
    try:
        size = path.stat().st_size
        if size > limit:
            raise InputError(f"{what} {path} is {size} bytes, more than the {limit} this check reads")
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        raise InputError(f"cannot read {what} {path}: {error.strerror}") from error


def strip_genex(value):
    """Return the target or path an entry names, or None for other generator expressions."""
    match = re.fullmatch(r"\$<(?:BUILD_INTERFACE|LINK_ONLY):(.*)>", value)
    if match:
        return match.group(1)
    if value.startswith("$<"):
        return None
    return value


def split_list(value):
    """Split a CMake list, keeping generator expressions whole and dropping empty items."""
    return [item for item in split_top(value, ";") if item]


def split_top(text, separators):
    """Split text at the separators that lie outside its generator expressions."""
    parts = []
    depth = 0
    current = []
    for index, char in enumerate(text):
        if char == "<" and text[index - 1:index] == "$":
            depth += 1
        elif char == ">" and depth:
            depth -= 1
        if char in separators and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(char)
    parts.append("".join(current))
    return parts


def genex_pieces(text):
    """Split text into its literal pieces ("text", piece) and generator expressions ("genex", inside $<...>)."""
    pieces = []
    start = index = 0
    while index < len(text):
        if not text.startswith("$<", index):
            index += 1
            continue
        if start < index:
            pieces.append(("text", text[start:index]))
        depth = 0
        end = index
        while end < len(text):
            if text.startswith("$<", end):
                depth += 1
                end += 2
                continue
            if text[end] == ">":
                depth -= 1
                if depth == 0:
                    break
            end += 1
        pieces.append(("genex", text[index + 2:end]))
        start = index = end + 1
    if start < len(text):
        pieces.append(("text", text[start:]))
    return pieces


def entry_values(text):
    """Return the texts a link entry can stand for, each generator expression replaced by the values it can give.

    A condition's value ($<$<PLATFORM_ID:Windows>:d3d9>) counts whatever the
    condition, so that a link made for one platform is found on every one.
    """
    values = []
    for kind, piece in genex_pieces(text):
        if kind == "text":
            values.append(piece)
            continue
        head = split_top(piece, ":")[0]
        if len(head) == len(piece):
            continue  # $<NAME> with no arguments: $<CONFIG>, $<PLATFORM_ID>
        arguments = piece[len(head) + 1:]
        if head.startswith("$<") or head in ("0", "1"):
            values.extend(entry_values(arguments))  # $<condition:value>
            continue
        name = head.upper()
        if name in GENEX_CONDITIONS:
            continue
        parts = split_top(arguments, ",")
        if name in GENEX_SKIP_FIRST:
            parts = parts[1:]
        for part in parts:
            values.extend(entry_values(part))
    return values


def library_stem(word):
    """Return the library one link word names, lower case, or None for a word that names none.

    -lGL, -l:libGL.so.1, libGL.so.1, /usr/lib/libGL.so, opengl32.lib,
    /DEFAULTLIB:opengl32 and Metal.framework give gl, gl, gl, gl, opengl32,
    opengl32 and metal; an imported target (OpenGL::GL) is returned whole.
    """
    word = word.strip().strip("\"'")
    if not word:
        return None
    if word.upper().startswith(DEFAULT_LIBRARY_OPTION):
        word = word[len(DEFAULT_LIBRARY_OPTION):]
    elif word.startswith(EXACT_LIBRARY_OPTION) and len(word) > len(EXACT_LIBRARY_OPTION):
        word = word[len(EXACT_LIBRARY_OPTION):]
    elif word.startswith("-l") and len(word) > 2:
        word = word[2:]
    elif word.startswith("-") or LINKER_OPTION_RE.fullmatch(word):
        return None
    if "::" in word:
        return word.lower()
    stem = LIBRARY_SUFFIX_RE.sub("", re.split(r"[\\/]", word)[-1])
    if stem.lower().startswith("lib") and len(stem) > len("lib"):
        stem = stem[len("lib"):]
    return stem.lower() or None


def is_graphics_library(stem):
    """Whether a library stem (library_stem) names a graphics API."""
    if "::" in stem:
        return stem.startswith(GRAPHICS_NAMESPACES)
    return stem in GRAPHICS_LIBRARIES or GRAPHICS_LIBRARY_RE.fullmatch(stem) is not None


def graphics_names(entry):
    """Return the graphics libraries one link entry names, lower case, in the order they appear.

    The entry is one item of a link list or a link option, generator
    expressions included; its words are split at spaces and commas, so that
    "-framework Metal" and LINKER:-framework,Metal name Metal. The folder
    after a search-path option (-L, -F, -rpath) names no library, so
    -Wl,-rpath,/opt/vendor/GL names none.
    """
    names = []
    for value in entry_values(entry):
        for item in value.split(";"):
            item = item.strip()
            for prefix in LINK_OPTION_PREFIXES:
                if item.startswith(prefix):
                    item = item[len(prefix):]
            folder_follows = False
            for word in re.split(r"[\s,]+", item):
                if word in PATH_OPTIONS:
                    folder_follows = True
                    continue
                if word in LINKER_PASS_OPTIONS:
                    continue
                if folder_follows:
                    folder_follows = False
                    continue
                stem = library_stem(word)
                if stem and is_graphics_library(stem) and stem not in names:
                    names.append(stem)
    return names


def strip_source_comments(text):
    """Return a C, C++ or Objective-C source without its comments, each line where it was.

    String and character literals are kept whole, so the _Pragma form of a
    request to the linker stays and a // inside a string ends nothing.
    """
    def replace(match):
        token = match.group(0)
        if token.startswith("//"):
            return ""
        if token.startswith("/*"):
            return " " + "\n" * token.count("\n")
        return token

    return SOURCE_COMMENT_RE.sub(replace, text)


def skip_comment(text, index, line):
    """Skip the CMake comment that starts at text[index] ('#'); return the index after it and the line there.

    A bracket comment (#[[ ... ]]) may span lines; a line comment ends before
    its newline.
    """
    bracket = BRACKET_OPEN_RE.match(text, index + 1)
    if bracket:
        close = "]" + bracket.group(1) + "]"
        end = text.find(close, bracket.end())
        end = len(text) if end < 0 else end + len(close)
        return end, line + text.count("\n", index, end)
    end = text.find("\n", index)
    return (len(text) if end < 0 else end), line


def read_arguments(text, index, line):
    """Read a command's arguments from just after its '('.

    Returns [(argument, line)], the index after the closing ')' and the line
    there.
    """
    arguments = []
    depth = 0
    size = len(text)
    while index < size:
        char = text[index]
        if char == "\n":
            line += 1
            index += 1
            continue
        if char in " \t\r":
            index += 1
            continue
        if char == "#":
            index, line = skip_comment(text, index, line)
            continue
        if char == "(":
            depth += 1
            index += 1
            continue
        if char == ")":
            index += 1
            if depth == 0:
                return arguments, index, line
            depth -= 1
            continue
        bracket = BRACKET_OPEN_RE.match(text, index)
        if char == '"':
            end = index + 1
            while end < size and text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            value = text[index + 1:min(end, size)].replace('\\"', '"')
            following = end + 1
        elif bracket:
            close = "]" + bracket.group(1) + "]"
            end = text.find(close, bracket.end())
            end = size if end < 0 else end
            value = text[bracket.end():end]
            following = end + len(close)
        else:
            end = index
            while end < size and text[end] not in UNQUOTED_END:
                end += 2 if text[end] == "\\" else 1
            if end == index:
                index += 1
                continue
            value = text[index:end]
            following = end
        arguments.append((value, line))
        line += text.count("\n", index, min(following, size))
        index = following
    return arguments, index, line


def cmake_commands(text):
    """Yield (command, [(argument, line)], line) for every command a CMake file calls, in every branch.

    Comments are dropped, quoted and bracket arguments are kept whole, and a
    call is read over as many lines as it spans. The command is lower case,
    as CMake does not tell case apart in command names.
    """
    index, line, size = 0, 1, len(text)
    while index < size:
        char = text[index]
        if char == "\n":
            line += 1
            index += 1
            continue
        if char == "#":
            index, line = skip_comment(text, index, line)
            continue
        match = COMMAND_NAME_RE.match(text, index)
        if not match:
            index += 1
            continue
        after = match.end()
        while after < size and text[after] in " \t":
            after += 1
        if after >= size or text[after] != "(":
            index = match.end()
            continue
        arguments, index, end_line = read_arguments(text, after + 1, line)
        yield match.group(0).lower(), arguments, line
        line = end_line


def set_values(arguments):
    """Return the values a set() call gives its variable: its arguments after the name, up to CACHE or PARENT_SCOPE.

    What follows CACHE (the type, the doc string, FORCE) is not a value.
    """
    values = arguments[1:]
    for index, (value, _) in enumerate(values):
        if value in SET_STOP_WORDS:
            return values[:index]
    return values


def collect_variables(commands):
    """Map each variable the build files set to every value they give it, in every branch.

    Reads set(), list(APPEND|PREPEND|INSERT), string(APPEND|PREPEND),
    find_library() (the names it looks for) and foreach() (the items it runs
    over).
    """
    variables = defaultdict(list)
    for command, arguments, _ in commands:
        values = [value for value, _ in arguments]
        if command == "set" and values:
            variables[values[0]].extend(value for value, _ in set_values(arguments))
        elif command in ("list", "string") and len(values) >= 2 and values[0] in ("APPEND", "PREPEND", "INSERT"):
            variables[values[1]].extend(values[2:])
        elif command == "find_library" and len(values) >= 2:
            rest = values[1:]
            if "NAMES" in rest:
                rest = rest[rest.index("NAMES") + 1:]
                names = []
                for value in rest:
                    if value in FIND_LIBRARY_KEYWORDS:
                        break
                    names.append(value)
            else:
                names = rest[:1]
            variables[values[0]].extend(names)
        elif command == "foreach" and len(values) >= 2 and values[1] != "RANGE":
            lists = False
            for value in values[1:]:
                if value in ("IN", "ITEMS", "LISTS", "ZIP_LISTS"):
                    lists = value in ("LISTS", "ZIP_LISTS")
                    continue
                variables[values[0]].append("${" + value + "}" if lists else value)
    return variables


class VariableExpander:
    """Replaces variable references with every value the build files give each variable (collect_variables).

    Each variable is expanded once and its expansion kept, so a variable set
    many times over its own value (set(x ${x} ...) in each platform's branch)
    costs as much as its values, not a power of them. A reference to a
    variable whose expansion is under way gives nothing: its values are
    already in that expansion. An expansion that left out such a variable
    further out is kept only until that variable's expansion is done.
    $ENV{...} and variables no build file sets expand to nothing.
    """

    def __init__(self, variables):
        self.variables = variables
        # name -> (text, graphics variables it uses), for expansions that left nothing out.
        self.expanded = {}
        # name -> (text, graphics variables, depth of the outermost variable it left out).
        self.partial = {}
        # name -> depth, for the variables whose expansion is under way.
        self.active = {}

    def expand(self, text, graphics_variables):
        """Return text with its variable references expanded, joined as lists.

        Adds to graphics_variables the names of the variables CMake's OpenGL
        and Vulkan packages set that the text uses, directly or through
        another variable.
        """
        return self._text(text, graphics_variables)[0]

    def _text(self, text, graphics_variables):
        """Expand text; return it and the depth of the outermost variable under way that it left out."""
        outermost = NO_CUT

        def replace(match):
            nonlocal outermost
            if match.group(1) == "ENV":
                return ""
            value, used, cut = self._variable(match.group(2))
            graphics_variables.update(used)
            outermost = min(outermost, cut)
            return value

        for _ in range(MAX_NESTING):
            expanded = VARIABLE_REFERENCE_RE.sub(replace, text)
            if expanded == text:
                break
            text = expanded
        return text, outermost

    def _variable(self, name):
        """Return one variable's expansion, the graphics variables it uses and the depth of what it left out."""
        used = {name} if GRAPHICS_VARIABLE_RE.fullmatch(name) else set()
        if name in self.expanded:
            text, kept = self.expanded[name]
            return text, kept, NO_CUT
        if name in self.active:
            return "", used, self.active[name]
        if name in self.partial:
            return self.partial[name]
        if name not in self.variables:
            return "", used, NO_CUT
        depth = len(self.active)
        if depth >= MAX_EXPANSION_DEPTH:
            return "", used, 0
        self.active[name] = depth
        text, cut = self._text(";".join(self.variables[name]), used)
        del self.active[name]
        # What was cut short at this depth or deeper lacks this variable's values.
        self.partial = {key: kept for key, kept in self.partial.items() if kept[2] < depth}
        if cut >= depth:
            self.expanded[name] = (text, frozenset(used))
            return text, used, NO_CUT
        self.partial[name] = (text, frozenset(used), cut)
        return text, used, cut


def link_entries(command, arguments):
    """Return (what links, [(entry, line)]) for a command that makes a link, or None for any other command.

    what is the target or targets the link is made for, as written, "every
    target" for directory-wide links and link variables, or DIRECTORY_TARGETS
    for a directory's link properties.
    """
    values = [value for value, _ in arguments]
    if command == "target_link_libraries" and values:
        return values[0], [(value, line) for value, line in arguments[1:] if value not in LINK_KEYWORDS]
    if command == "target_link_options" and values:
        return values[0], [(value, line) for value, line in arguments[1:] if value not in LINK_OPTION_KEYWORDS]
    if command in ("link_libraries", "add_link_options"):
        return "every target", [(value, line) for value, line in arguments if value not in LINK_OPTION_KEYWORDS]
    if command in ("set_target_properties", "set_directory_properties") and "PROPERTIES" in values:
        split = values.index("PROPERTIES")
        pairs = arguments[split + 1:]
        entries = [pairs[index + 1] for index in range(0, len(pairs) - 1, 2)
                   if LINK_PROPERTY_RE.fullmatch(pairs[index][0])]
        what = " ".join(values[:split]) if command == "set_target_properties" else DIRECTORY_TARGETS
        return what, entries
    if command == "set_property" and values[:1] in (["TARGET"], ["DIRECTORY"]) and "PROPERTY" in values:
        split = values.index("PROPERTY")
        if split + 1 < len(values) and LINK_PROPERTY_RE.fullmatch(values[split + 1]):
            if values[0] == "DIRECTORY":
                return DIRECTORY_TARGETS, arguments[split + 2:]
            targets = [value for value in values[1:split] if value not in ("APPEND", "APPEND_STRING")]
            return " ".join(targets), arguments[split + 2:]
        return None
    if command == "set" and values and LINK_VARIABLE_RE.fullmatch(values[0]):
        return "every target", set_values(arguments)
    if command in ("list", "string") and len(values) >= 2 and values[0] in ("APPEND", "PREPEND") \
            and LINK_VARIABLE_RE.fullmatch(values[1]):
        return "every target", arguments[2:]
    return None


def longest_prefix(path, prefixes):
    """Return the longest of prefixes that is path or a directory holding it."""
    best = None
    for prefix in prefixes:
        if path == prefix or path.startswith(prefix + "/"):
            if best is None or len(prefix) > len(best):
                best = prefix
    return best


def module_names(module):
    """Return (group, module or None) of a module directory."""
    parts = module.split("/")
    return parts[1], ("-".join(parts[2:]) if len(parts) > 2 else None)


def layer_allowed(source_group, target_group):
    """Whether a module of one group may link a module of another."""
    if source_group == target_group:
        return True
    if (source_group, target_group) in FORBIDDEN or (source_group, target_group) in APART:
        return False
    return LAYERS[target_group] <= LAYERS[source_group]


class Targets:
    """The targets a configuration defined, read from its layout-targets file."""

    def __init__(self, path, root):
        self.root = root
        self.props = defaultdict(dict)
        self.aliases = {}
        for line in read_bounded(path, MAX_TARGETS_BYTES, "the targets file").splitlines():
            parts = line.split("\t", 2)
            if len(parts) != 3:
                continue
            if parts[0] == "ALIAS":
                self.aliases[parts[1]] = parts[2]
                continue
            self.props[parts[0]][parts[1]] = parts[2]
        self.names = set(self.props)

    def get(self, target, name):
        return self.props[target].get(name, "")

    def rel(self, path):
        """Return a path relative to the root, or None outside it."""
        try:
            return Path(path).resolve().relative_to(self.root).as_posix()
        except ValueError:
            return None

    def links(self, target, interface=False, usage=False):
        """Return the targets one target links (its interface links, when asked).

        usage leaves out $<LINK_ONLY:...> entries, which bring no include
        directories.
        """
        value = self.get(target, "INTERFACE_LINK_LIBRARIES" if interface else "LINK_LIBRARIES")
        found = []
        for item in split_list(value):
            if item.startswith("::@"):
                continue
            if usage and item.startswith("$<LINK_ONLY:"):
                continue
            name = strip_genex(item)
            if name is None:
                continue
            name = self.aliases.get(name, name)
            if name in self.names:
                found.append(name)
        return found

    def foreign_links(self, target):
        """Whether a link of a target was made from another directory (CMake marks it ::@(...))."""
        return "::@(" in self.get(target, "LINK_LIBRARIES") + self.get(target, "INTERFACE_LINK_LIBRARIES")

    def closure(self, target):
        """Return every target whose include directories a target sees through its links."""
        seen = set()
        stack = list(self.links(target, usage=True)) + list(self.links(target, interface=True, usage=True))
        while stack:
            name = stack.pop()
            if name in seen:
                continue
            seen.add(name)
            stack.extend(self.links(name, interface=True, usage=True))
        return seen

    def include_dirs(self, target, interface):
        """Return a target's own include directories that lie inside the root."""
        value = self.get(target, "INTERFACE_INCLUDE_DIRECTORIES" if interface else "INCLUDE_DIRECTORIES")
        found = []
        for item in split_list(value):
            path = strip_genex(item)
            if path is not None:
                relative = self.rel(path)
                if relative is not None:
                    found.append(relative)
        return found

    def sources(self, target):
        """Return a target's sources that lie inside the root."""
        found = []
        source_dir = self.get(target, "SOURCE_DIR")
        for item in split_list(self.get(target, "SOURCES")):
            path = strip_genex(item)
            if path is None:
                continue
            relative = self.rel(path if os.path.isabs(path) else os.path.join(source_dir, path))
            if relative is not None and not self.generated(relative):
                found.append(relative)
        return found

    def generated(self, relative):
        """Whether a path lies in a build tree inside the root, such as a file CMake generates there.

        A build tree is a top-level folder that holds a CMakeCache.txt.
        """
        top = relative.split("/", 1)[0]
        return "/" in relative and (self.root / top / "CMakeCache.txt").is_file()


class Checker:
    """Runs every rule over one tree and collects findings."""

    def __init__(self, root, targets_path, baseline):
        self.root = Path(root).resolve()
        self.findings = []
        self.targets = Targets(targets_path, self.root)
        self.baseline = baseline
        self.engine = {name for name in self.targets.names if self._inside(self.targets.get(name, "SOURCE_DIR"))}
        self.module_of_target = {name: self._module_of_target(name) for name in self.engine}
        self.modules = sorted({module for module in self.module_of_target.values() if module})
        self.files = self._files()

    def _inside(self, directory):
        """Whether a target's directory lies in the engine tree (the configuration may hold more)."""
        return self.targets.rel(directory) is not None

    def _defined_in_tree(self, name):
        """Whether the root's build file or one under BUILD_FILE_FOLDERS defines a target."""
        directory = self.targets.rel(self.targets.get(name, "SOURCE_DIR"))
        return directory == "." or (directory is not None and directory.split("/", 1)[0] in BUILD_FILE_FOLDERS)

    def _module_of_target(self, name):
        source_dir = self.targets.rel(self.targets.get(name, "SOURCE_DIR"))
        if source_dir and source_dir.startswith("src/"):
            return source_dir
        # A target the root defines belongs to the module that holds all its
        # sources (oa-game: src/app), or, with no sources, all its include
        # directories, if one does.
        folders = {posixpath.dirname(path) for path in self.targets.sources(name)}
        if not folders:
            folders = set(self.targets.include_dirs(name, True))
        if not folders or not all(folder.startswith("src/") for folder in folders):
            return None
        parts = posixpath.commonpath(sorted(folders)).split("/")
        if len(parts) >= 2 and parts[1] in GROUP_LEVEL:
            return "/".join(parts[:2])
        return "/".join(parts[:3]) if len(parts) >= 3 else None

    def _files(self, top="src"):
        files = []
        for folder, subfolders, names in os.walk(self.root / top):
            subfolders[:] = [name for name in subfolders if not name.startswith(".")]
            for name in names:
                files.append((Path(folder) / name).relative_to(self.root).as_posix())
        return sorted(files)

    def build_files(self):
        """Return the tree's build files: the root's CMakeLists.txt and those under BUILD_FILE_FOLDERS."""
        found = ["CMakeLists.txt"] if (self.root / "CMakeLists.txt").is_file() else []
        for top in BUILD_FILE_FOLDERS:
            found.extend(path for path in self._files(top)
                         if Path(path).name == "CMakeLists.txt" or path.endswith(".cmake"))
        return found

    def report(self, rule, where, message):
        """Record a finding once (a header several targets reach is read once for each)."""
        if (rule, where, message) not in self.findings:
            self.findings.append((rule, where, message))

    def module_of(self, path):
        return longest_prefix(path, self.modules)

    # -- rules ----------------------------------------------------------------

    def check_build_files(self):
        """Report build files under src/ that name directories outside their own with .., or that
        link a target another directory's build file adds.

        The links are read from the text, every branch included, so a link made only on another
        platform is found on this one too.
        """
        texts = {}
        for path in self.files:
            if Path(path).name != "CMakeLists.txt" and not path.endswith(".cmake"):
                continue
            text = re.sub(r"#[^\n]*", "", (self.root / path).read_text(encoding="utf-8", errors="replace"))
            texts[path] = text
            for match in COMMAND_RE.finditer(text):
                for argument in match.group(2).split():
                    value = re.sub(r"^\$\{CMAKE_CURRENT_(?:SOURCE|LIST)_DIR\}/?", "", argument.strip('"'))
                    if ".." in value.split("/"):
                        line = text[:match.start()].count("\n") + 1
                        self.report("reach", f"{path}:{line}", f"{match.group(1)} names {argument}")
        adders = defaultdict(set)
        for path, text in texts.items():
            for match in DEFINE_RE.finditer(text):
                adders[match.group(1)].add(path)
        for path, text in texts.items():
            folder = posixpath.dirname(path)
            for match in LINK_RE.finditer(text):
                name = match.group(1)
                where = sorted(adders.get(name, ()))
                if where and all(posixpath.dirname(adder) != folder for adder in where):
                    line = text[:match.start()].count("\n") + 1
                    self.report("foreign", f"{path}:{line}", f"links {name}, which {', '.join(where)} adds")

    def check_targets(self):
        """Report foreign links and include directories or sources of other modules."""
        targets = self.targets
        for name in sorted(self.engine):
            module = self.module_of_target[name]
            if module is None:
                continue
            if targets.foreign_links(name):
                self.report("foreign", name, "is linked from another directory's CMakeLists.txt")
            for directory in targets.include_dirs(name, False) + targets.include_dirs(name, True):
                if directory.startswith("src/") and self.module_of(directory) != module:
                    self.report("reach", name, f"has the include directory {directory} of another module")
            for source in targets.sources(name):
                if source.startswith("src/") and self.module_of(source) != module:
                    self.report("reach", name, f"compiles {source} of another module")

    def include_index(self):
        """Map include paths to files, and each exported header to the targets that export it."""
        index = {}
        owners = defaultdict(set)
        exporters = defaultdict(set)
        for name in self.engine:
            for directory in set(self.targets.include_dirs(name, True)):
                exporters[directory].add(name)
        for directory in sorted(exporters):
            base = self.root / directory
            if not base.is_dir():
                continue
            for folder, _, names in os.walk(base):
                for file_name in names:
                    full = Path(folder) / file_name
                    key = full.relative_to(base).as_posix()
                    path = full.relative_to(self.root).as_posix()
                    index.setdefault(key, path)
                    owners[path].update(exporters[directory])
        return index, owners

    def resolve(self, source, delimiter, name, index):
        """Resolve an #include: next to the including file first, then through the include index."""
        if delimiter == '"':
            candidate = posixpath.normpath(posixpath.join(posixpath.dirname(source), name))
            if (self.root / candidate).is_file():
                return candidate, True
        found = index.get(name)
        return (found, False) if found else (None, False)

    def check_includes(self):
        """Report includes that leave a module by a relative path or that no link reaches."""
        index, owners = self.include_index()
        closures = {}

        def reach(target):
            if target not in closures:
                closures[target] = self.targets.closure(target) | {target}
            return closures[target]

        def check_file(path, module, allowed, where, seen):
            if path in seen or not (self.root / path).is_file():
                return
            seen.add(path)
            text = (self.root / path).read_text(encoding="utf-8", errors="replace")
            for match in INCLUDE_RE.finditer(text):
                delimiter, name = match.group(1), match.group(2)
                resolved, relative = self.resolve(path, delimiter, name, index)
                if resolved is None:
                    continue
                other = self.module_of(resolved)
                line = text[:match.start()].count("\n") + 1
                if relative and ".." in name.split("/") and other != module:
                    self.report("include", f"{path}:{line}", f"reaches {resolved} of {other} by a relative path")
                    continue
                public = f"{self.module_of(path)}/include/"
                if relative and path.startswith(public) and not resolved.startswith(public):
                    self.report("include", f"{path}:{line}", f"is a public header that reaches {resolved}, outside "
                                                             f"{public}, by a relative path")
                    continue
                if other == module or other is None:
                    if relative and resolved.startswith("src/") and resolved not in owners:
                        check_file(resolved, module, allowed, where, seen)
                    continue
                if not owners.get(resolved, set()) & allowed:
                    self.report("include", f"{path}:{line}",
                                f"includes {name} of {other}, which no link of {where} reaches")

        for name in sorted(self.engine):
            module = self.module_of_target[name]
            if module is None:
                continue
            allowed = reach(name)
            seen = set()
            for source in self.targets.sources(name):
                if Path(source).suffix in SOURCE_SUFFIXES:
                    check_file(source, module, allowed, name, seen)
        # A public header answers for its own includes through the links of its
        # module's targets and of the targets that export it.
        by_module = defaultdict(set)
        for name, module in self.module_of_target.items():
            if module:
                by_module[module].add(name)
        for header, names in sorted(owners.items()):
            module = self.module_of(header)
            if module is None or Path(header).suffix not in SOURCE_SUFFIXES:
                continue
            allowed = set()
            for owner in names | by_module[module]:
                allowed |= reach(owner)
            check_file(header, module, allowed, f"the targets of {module}", set())

    def check_layers(self):
        """Report links from module libraries against the layer order."""
        known = {(entry["from"], entry["to"]) for entry in self.baseline.get("layer_exceptions", [])}
        used = set()
        for name in sorted(self.engine):
            module = self.module_of_target[name]
            if module is None or self.targets.get(name, "TYPE") not in LIBRARY_TYPES:
                continue
            group, _ = module_names(module)
            if group not in LAYERS:
                self.report("layer", name, f"is in {module}, whose group {group} has no layer")
                continue
            for dep in sorted(set(self.targets.links(name)) | set(self.targets.links(name, interface=True))):
                dep_module = self.module_of_target.get(dep)
                if dep_module is None or dep in INFRASTRUCTURE:
                    continue
                dep_group, _ = module_names(dep_module)
                if dep_group not in LAYERS or layer_allowed(group, dep_group):
                    continue
                if (name, dep) in known:
                    used.add((name, dep))
                    continue
                self.report("layer", name, f"({group}) links {dep} ({dep_group}), against the layer order")
        for source, target in sorted(known - used):
            self.report("stale", f"{source} -> {target}", "no longer links against the layer order; remove the "
                                                          "baseline entry")

    def check_names(self):
        """Report libraries and public headers that break the naming rule."""
        declared = defaultdict(set)
        defined = {}
        for path in self.build_files():
            text = re.sub(r"#[^\n]*", "", (self.root / path).read_text(encoding="utf-8", errors="replace"))
            for match in ALIAS_RE.finditer(text):
                declared[match.group(2)].add(match.group(1))
            for match in LIBRARY_RE.finditer(text):
                rest = match.group(2).split()
                if "ALIAS" in rest or "IMPORTED" in rest or "$" in match.group(1):
                    continue
                defined.setdefault(match.group(1), f"{path}:{text[:match.start()].count(chr(10)) + 1}")
        libraries = {name for name in self.engine if self.targets.get(name, "TYPE") in LIBRARY_TYPES
                     and self._defined_in_tree(name)}
        for name in sorted(libraries | set(defined)):
            module = self.module_of_target.get(name)
            source_dir = self.targets.rel(self.targets.get(name, "SOURCE_DIR")) if name in self.engine else None
            if module is not None and name not in INFRASTRUCTURE and source_dir and source_dir.startswith("src/"):
                group, sub = module_names(module)
                stem = f"oa-{group}" + (f"-{sub}" if sub else "")
                if name != stem and not name.startswith(stem + "-"):
                    self.report("name", name, f"is in {module}; its name should be {stem} or {stem}-<part>")
                    continue
            if not re.fullmatch(r"oa(-[a-z0-9]+)+", name):
                self.report("name", name, "is not named oa-<group>[-<part>]")
                continue
            rest = name[len("oa-"):]
            alias = "oa::" + (rest.replace("-", "::", 1).replace("-", "_") if "-" in rest else rest)
            if alias not in declared.get(name, set()):
                self.report("name", name, f"has no ALIAS {alias}")
        for path in self.files:
            module = self.module_of(path)
            if module is None or not path.startswith(module + "/include/"):
                continue
            group, sub = module_names(module)
            include = path[len(module) + len("/include/"):]
            prefix = f"oa/{group}" if sub is None else f"oa/{group}/{sub.replace('-', '_')}"
            good = include.startswith(prefix + "/") or (sub is not None and posixpath.splitext(include)[0] == prefix)
            # The fold keeps the include paths the sources already use
            # (oa/boneyards/..., oa/rover/..., the test installs and the
            # pages' font header) instead of moving 131k lines of includes.
            if not good and group == "boneyards":
                stem = posixpath.splitext(include)[0]
                good = (include.startswith("oa/boneyards/") or stem == "oa/boneyards"
                        or include.startswith("oa/rover/") or stem == "oa/rover"
                        or include.startswith("oa/boneyards_test/")
                        or include == "oa/platform/fonts.hpp")
            if not good:
                self.report("name", path, f"is included as {include}; headers of {module} belong under {prefix}")

    def check_graphics(self):
        """Report every target that links a graphics API, wherever the link is made.

        Reads the links the configuration made, every link the build files
        make in any branch, and the libraries sources ask the linker for, so a
        link made for one platform fails on every platform.
        """
        for name in sorted(self.engine):
            for link_property in ("LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES"):
                for entry in split_list(self.targets.get(name, link_property)):
                    names = [] if entry.startswith("::@") else graphics_names(entry)
                    if names:
                        self.report("graphics", name, f"links {entry} ({', '.join(names)}); {GRAPHICS_ADVICE}")
        commands = {}
        for path in self.build_files():
            commands[path] = list(cmake_commands((self.root / path).read_text(encoding="utf-8", errors="replace")))
        # One map of every variable of every build file, function and scope:
        # a link's variable is read with every value any of them gives it.
        expander = VariableExpander(collect_variables(command for listed in commands.values() for command in listed))
        for path, listed in commands.items():
            for command, arguments, _ in listed:
                link = link_entries(command, arguments)
                if link is None:
                    continue
                what, entries = link
                what = expander.expand(what, set()) or what
                for entry, line in entries:
                    graphics_variables = set()
                    expanded = expander.expand(entry, graphics_variables)
                    names = sorted(graphics_variables)
                    for item in split_list(expanded):
                        names.extend(found for found in graphics_names(item) if found not in names)
                    if names:
                        note = f"; {GRAPHICS_VARIABLE_NOTE}" if VARIABLE_REFERENCE_RE.search(entry) else ""
                        self.report("graphics", f"{path}:{line}",
                                    f"{what} links {entry.strip()} ({', '.join(names)}); {GRAPHICS_ADVICE}{note}")
        for top in ("src", "tests", "tools"):
            for path in self._files(top):
                if Path(path).suffix not in SOURCE_SUFFIXES:
                    continue
                text = strip_source_comments((self.root / path).read_text(encoding="utf-8", errors="replace"))
                for match in PRAGMA_LIBRARY_RE.finditer(text):
                    names = graphics_names(match.group(1))
                    if names:
                        line = text[:match.start()].count("\n") + 1
                        self.report("graphics", f"{path}:{line}",
                                    f"asks the linker for {match.group(1)} ({', '.join(names)}); {GRAPHICS_ADVICE}")

    def run(self):
        self.check_build_files()
        self.check_targets()
        self.check_includes()
        self.check_layers()
        self.check_names()
        self.check_graphics()
        return self.findings


def load_baseline(path):
    """Read the baseline of known layer exceptions; a missing file is an empty baseline."""
    if not path or not Path(path).is_file():
        return {}
    text = read_bounded(path, MAX_BASELINE_BYTES, "the baseline")
    try:
        baseline = json.loads(text)
    except json.JSONDecodeError as error:
        raise InputError(f"the baseline {path} is not JSON: {error.msg} at line {error.lineno}") from error
    entries = baseline.get("layer_exceptions", []) if isinstance(baseline, dict) else None
    if not isinstance(entries, list) or not all(isinstance(entry, dict) and isinstance(entry.get("from"), str)
                                                and isinstance(entry.get("to"), str) for entry in entries):
        raise InputError(f"the baseline {path} needs a layer_exceptions list of {{\"from\", \"to\"}} entries")
    return baseline


def check(root, targets, baseline_path):
    """Check one configured tree; print the findings and return the exit status."""
    try:
        checker = Checker(root, targets, load_baseline(baseline_path))
    except InputError as error:
        print(f"check_layout: {error}", file=sys.stderr)
        return 2
    findings = checker.run()
    for rule, where, message in findings:
        print(f"{where}: {rule}: {message}")
    if findings:
        print(f"check_layout: {len(findings)} findings in {len(checker.modules)} modules", file=sys.stderr)
        return 1
    print(f"check_layout: {len(checker.modules)} modules and {len(checker.engine)} targets keep the layout")
    return 0


# ---- self-test --------------------------------------------------------------------


SELF_TEST_FILES = {
    "src/core/CMakeLists.txt": "add_library(oa-core-types INTERFACE)\nadd_library(oa::core::types ALIAS oa-core-types)\n",
    "src/core/include/oa/core/types.h": "#pragma once\n",
    "src/base/game-math/CMakeLists.txt":
        "add_library(oa-base-game-math src/math.cpp)\nadd_library(oa::base::game_math ALIAS oa-base-game-math)\n",
    "src/base/game-math/include/oa/base/game_math.hpp": '#pragma once\n#include "oa/core/types.h"\n',
    "src/base/game-math/src/math.cpp": '#include "oa/base/game_math.hpp"\n#include "oa/sim/unit.hpp"\n',
    "src/base/game-math/src/table.inc": "\n",
    "src/sim/unit/CMakeLists.txt":
        "add_library(oa-sim-unit src/unit.cpp)\nadd_library(oa::sim::unit ALIAS oa-sim-unit)\n"
        "add_subdirectory(../../base/game-math math)\n",
    "src/sim/unit/include/oa/sim/unit.hpp": '#pragma once\n#include "oa/base/game_math.hpp"\n',
    "src/sim/unit/include/oa/misplaced.hpp": "#pragma once\n",
    "src/sim/unit/src/unit.cpp": '#include "oa/sim/unit.hpp"\n#include "../../../base/game-math/src/table.inc"\n',
    "src/sim/unit/socket/CMakeLists.txt":
        "target_sources(oa-sim-unit PRIVATE socket.cpp)\nif(WIN32)\n  target_link_libraries(oa-sim-unit PUBLIC ws2_32)\n"
        "endif()\n",
    "src/platform/CMakeLists.txt": "add_library(oa-platform-shims src/files.cpp)\n",
    "src/platform/include/oa/platform/files.hpp": '#pragma once\n#include "../../../src/detail.hpp"\n',
    "src/platform/src/detail.hpp": "#pragma once\n",
    "src/platform/src/files.cpp": '#include "oa/platform/files.hpp"\n',
    "src/ui/hud/CMakeLists.txt": "add_library(hud src/hud.cpp)\nadd_library(oa::ui::hud ALIAS hud)\n",
    "src/ui/hud/src/hud.cpp": '#include "oa/sim/unit.hpp"\n',
    "CMakeLists.txt": "add_library(oa-test-support INTERFACE)\nadd_library(oa::test::support ALIAS oa-test-support)\n"
                      "add_library(oa-test-launch STATIC launch.cpp)\n# add_library(oa::test::launch ALIAS oa-test-launch)\n"
                      "if(APPLE)\n  target_link_libraries(oa-test-launch PRIVATE\n"
                      "    \"/System/Library/Frameworks/Metal.framework\")\nendif()\n",
    "cmake/Options.cmake": "add_library(oa-options INTERFACE)\nadd_library(oa::options ALIAS oa-options)\n"
                           "add_library(SDL3::SDL3 SHARED IMPORTED)\n",
    "tests/extension/CMakeLists.txt": "if(WIN32)\n  add_library(oa-extension-windows STATIC a.cpp)\nendif()\n"
                                      "add_library(recorder STATIC b.cpp)\nadd_library(oa::recorder ALIAS recorder)\n",
    "tests/extension/a.cpp":
        'static int value;\n#pragma comment(lib, "d3d11.lib")\n#pragma comment(lib, "user32.lib")\n'
        '// never #pragma comment(lib, "d3d9.lib")\n/* nor #pragma comment(lib, "dxgi.lib")\n*/\n'
        '#pragma comment(linker, "/SUBSYSTEM:WINDOWS /DEFAULTLIB:d3d12.lib")\n'
        'static const char *slashes = "//"; _Pragma("comment(lib, \\"glu32.lib\\")")\n',
    "tools/probe/probe.cpp": '_Pragma("comment(lib, \\"vulkan-1.lib\\")")\n',
    # Graphics links in every form the check reads, and links that only look like them.
    "src/base/game-math/graphics.cmake":
        "# Links to graphics APIs, and links that only look like them.\n"
        "find_library(oa_vulkan_library NAMES vulkan-1 vulkan PATHS /opt/sdk)\n"
        "if(WIN32)\n"
        "  target_link_libraries(oa-base-game-math\n"
        "    PRIVATE\n"
        "      oa-core-types # d3d11, in a comment\n"
        "      \"C:/Program Files (x86)/Windows Kits/10/Lib/x64/d3d9.lib\")\n"
        "endif()\n"
        "target_link_libraries(oa-base-game-math PRIVATE \"$<$<PLATFORM_ID:Darwin>:-framework Metal>\"\n"
        "  $<$<STREQUAL:${OA_RENDERER},GL>:oa-core-types> SDL3::SDL3 \"-framework AppKit\" ws2_32 Threads::Threads)\n"
        "target_link_libraries(oa-base-game-math PRIVATE ${oa_vulkan_library} ${OPENGL_gl_LIBRARY})\n"
        "#[[ target_link_libraries(oa-base-game-math PRIVATE opengl32)\n"
        "]]\n"
        "set_property(TARGET oa-base-game-math APPEND PROPERTY INTERFACE_LINK_LIBRARIES\n"
        "  $<IF:$<PLATFORM_ID:Windows>,opengl32,OpenGL::GL>)\n"
        "target_link_options(oa-base-game-math PRIVATE \"LINKER:-framework,QuartzCore\" /NODEFAULTLIB:d3d10)\n"
        "foreach(oa_api IN ITEMS dxgi d3d12)\n"
        "  target_link_libraries(oa-base-game-math PRIVATE ${oa_api})\n"
        "endforeach()\n"
        "string(APPEND CMAKE_EXE_LINKER_FLAGS \" -lGL\")\n"
        "Set_Target_Properties(oa-base-game-math PROPERTIES IMPORTED_LOCATION_RELEASE /usr/lib/libvulkan.so.1)\n"
        "link_libraries(oa-core-types d3d10)\n"
        "add_link_options(-Wl,-rpath,/opt/vendor/GL \"LINKER:-L,/opt/GL\" \"SHELL:-L /usr/lib/EGL\")\n"
        "add_link_options(-l:libGL.so.1)\n"
        "set_target_properties(oa-base-game-math PROPERTIES LINK_FLAGS \"-framework OpenGL\"\n"
        "  INTERFACE_LINK_OPTIONS -lvulkan)\n"
        "list(APPEND CMAKE_SHARED_LINKER_FLAGS -lEGL)\n"
        "set(CMAKE_MODULE_LINKER_FLAGS -ld3d9 CACHE STRING \"Never Metal\" FORCE)\n"
        "set(CMAKE_EXE_LINKER_FLAGS \"-Wl,--as-needed\" CACHE STRING \"Linker flags; never GL or Metal\" FORCE)\n"
        "set(oa_cached_api ddraw CACHE STRING \"\")\n"
        "target_link_libraries(oa-base-game-math PRIVATE $CACHE{oa_cached_api})\n"
        "#[=[ target_link_libraries(oa-base-game-math PRIVATE opengl32) ]]\n"
        "  target_link_libraries(oa-base-game-math PRIVATE gbm) ]=]\n"
        "set_property(DIRECTORY APPEND PROPERTY LINK_OPTIONS -lGLESv2)\n"
        "set_directory_properties(PROPERTIES LINK_OPTIONS -ldcomp)\n"
        "if(WIN32)\n"
        "  set(oa_system_libraries ${oa_system_libraries} ws2_32)\n"
        "  list(APPEND oa_system_libraries ${oa_system_libraries} winmm)\n"
        "elseif(APPLE)\n"
        "  set(oa_system_libraries ${oa_system_libraries} \"-framework AppKit\")\n"
        "  list(APPEND oa_system_libraries ${oa_system_libraries} \"-framework IOKit\")\n"
        "else()\n"
        "  set(oa_system_libraries ${oa_system_libraries} m)\n"
        "  list(APPEND oa_system_libraries ${oa_system_libraries} dl)\n"
        "  set(oa_system_libraries ${oa_system_libraries} rt)\n"
        "  set(oa_system_libraries ${oa_system_libraries} gbm)\n"
        "endif()\n"
        "target_link_libraries(oa-base-game-math PRIVATE ${oa_system_libraries})\n"
        "set(oa_first_libraries ${oa_second_libraries} glx)\n"
        "set(oa_second_libraries ${oa_first_libraries} ws2_32)\n"
        "target_link_libraries(oa-base-game-math PRIVATE ${oa_first_libraries})\n"
        "target_link_libraries(oa-base-game-math PRIVATE ${oa_second_libraries})\n",
}

# (rule, where) of each finding the self-test tree must give.
SELF_TEST_EXPECTED = [
    ("reach", "src/sim/unit/CMakeLists.txt:3"),  # add_subdirectory(../..)
    ("foreign", "hud"),  # linked from another directory
    ("foreign", "src/sim/unit/socket/CMakeLists.txt:3"),  # linked from a subdirectory, on Windows only
    ("reach", "oa-sim-unit"),  # platform's include directory
    ("include", "src/sim/unit/src/unit.cpp:2"),  # a relative path into game-math
    ("include", "src/sim/unit/include/oa/sim/unit.hpp:2"),  # game-math without a link
    ("include", "src/platform/include/oa/platform/files.hpp:2"),  # out of include/ to a private header
    ("layer", "oa-base-game-math"),  # base links sim
    ("name", "oa-platform-shims"),  # no alias
    ("name", "hud"),  # not oa-ui-hud
    ("name", "src/sim/unit/include/oa/misplaced.hpp"),  # outside oa/sim/unit
    ("name", "oa-test-launch"),  # no alias outside src/: the alias is a comment
    ("name", "oa-extension-windows"),  # no alias, in a branch this platform does not take
    ("name", "recorder"),  # not oa-<group>-<part>
    ("stale", "oa-sim-unit -> oa-core-types"),  # a baseline entry for a link that keeps the order
    ("graphics", "oa-platform-shims"),  # d3d11 in a generator expression, as configured
    ("graphics", "oa-platform-shims"),  # libvulkan by its path
    ("graphics", "oa-platform-shims"),  # OpenGL::GL in the interface, link only
    ("graphics", "CMakeLists.txt:7"),  # a framework path, on Apple only, at the root
    ("graphics", "src/base/game-math/graphics.cmake:7"),  # d3d9.lib on Windows only, over several lines
    ("graphics", "src/base/game-math/graphics.cmake:9"),  # -framework Metal in a generator expression
    ("graphics", "src/base/game-math/graphics.cmake:11"),  # vulkan-1 or vulkan, through find_library
    ("graphics", "src/base/game-math/graphics.cmake:11"),  # the OpenGL package's library variable
    ("graphics", "src/base/game-math/graphics.cmake:15"),  # opengl32 or OpenGL::GL, set as a property
    ("graphics", "src/base/game-math/graphics.cmake:16"),  # QuartzCore as a link option
    ("graphics", "src/base/game-math/graphics.cmake:18"),  # dxgi and d3d12 through a loop
    ("graphics", "src/base/game-math/graphics.cmake:20"),  # -lGL for every program
    ("graphics", "src/base/game-math/graphics.cmake:21"),  # an imported library's file, the command in mixed case
    ("graphics", "tests/extension/a.cpp:2"),  # a source that asks the linker for d3d11
    ("graphics", "tools/probe/probe.cpp:1"),  # the same, in the _Pragma form
    ("graphics", "tests/extension/a.cpp:7"),  # d3d12 as a linker option, after comments that ask for others
    ("graphics", "tests/extension/a.cpp:8"),  # glu32 after a string that holds //
    ("graphics", "src/base/game-math/graphics.cmake:22"),  # d3d10 for every target
    ("graphics", "src/base/game-math/graphics.cmake:24"),  # libGL.so.1 by its file name
    ("graphics", "src/base/game-math/graphics.cmake:25"),  # the OpenGL framework as link flags
    ("graphics", "src/base/game-math/graphics.cmake:26"),  # -lvulkan as an interface link option
    ("graphics", "src/base/game-math/graphics.cmake:27"),  # -lEGL appended to a link variable
    ("graphics", "src/base/game-math/graphics.cmake:28"),  # d3d9 in a cache entry, not its doc string
    ("graphics", "src/base/game-math/graphics.cmake:31"),  # ddraw through a cache reference
    ("graphics", "src/base/game-math/graphics.cmake:34"),  # GLESv2 as a directory's link option
    ("graphics", "src/base/game-math/graphics.cmake:35"),  # dcomp through set_directory_properties
    ("graphics", "src/base/game-math/graphics.cmake:48"),  # gbm in a variable set over its own value
    ("graphics", "src/base/game-math/graphics.cmake:51"),  # glx in a variable that names it back
    ("graphics", "src/base/game-math/graphics.cmake:52"),  # glx through that variable
]

# Link entries, and the graphics libraries graphics_names must find in each.
GRAPHICS_SELF_TEST_ENTRIES = (
    ("d3d9", ["d3d9"]),
    ("D3D11.lib", ["d3d11"]),
    ("libd3d12.a", ["d3d12"]),
    ("-ldxgi", ["dxgi"]),
    ("d3dcompiler_47", ["d3dcompiler_47"]),
    ("/DEFAULTLIB:opengl32.lib", ["opengl32"]),
    ("vulkan-1.lib", ["vulkan-1"]),
    ("/usr/lib/x86_64-linux-gnu/libGL.so.1", ["gl"]),
    ("-lEGL", ["egl"]),
    ("libGLESv2.so", ["glesv2"]),
    ("libvulkan.so", ["vulkan"]),
    ("-framework Metal", ["metal"]),
    ("-Wl,-framework,QuartzCore", ["quartzcore"]),
    ("SHELL:-framework OpenGL", ["opengl"]),
    ("/System/Library/Frameworks/Metal.framework/Metal", ["metal"]),
    ("$<LINK_LIBRARY:FRAMEWORK,MetalKit>", ["metalkit"]),
    ("$<IF:$<PLATFORM_ID:Windows>,opengl32,GL>", ["opengl32", "gl"]),
    ("$<$<NOT:$<PLATFORM_ID:Windows>>:vulkan;oa-core-types>", ["vulkan"]),
    ("$<TARGET_NAME_IF_EXISTS:Vulkan::Vulkan>", ["vulkan::vulkan"]),
    ("$<BUILD_INTERFACE:$<$<PLATFORM_ID:Linux>:-lGLESv2>>", ["glesv2"]),
    ("/NODEFAULTLIB:opengl32.lib", []),
    ("$<$<STREQUAL:${renderer},GL>:oa-core-types>", []),
    ("$<$<CONFIG:Debug>:oa-sim-unit>", []),
    ("-framework AppKit", []),
    ("ws2_32", []),
    ("SDL3::SDL3", []),
    ("oa-present-gl", []),
    ("libgcc", []),
    ("/SUBSYSTEM:WINDOWS", []),
    ("-l:libGL.so.1", ["gl"]),
    ("-Wl,-rpath,/opt/vendor/GL", []),
    ("-Wl,-rpath -Wl,/opt/vendor/GL -lEGL", ["egl"]),
    ("-Xlinker -rpath -Xlinker /opt/GL", []),
    ("-L /opt/vendor/EGL -lvulkan", ["vulkan"]),
    ("-F /opt/frameworks/Metal", []),
)

# Variables set over their own values and each other's, an entry naming them,
# and the items its expansion must hold: each value once, however often a
# variable names itself.
EXPANSION_SELF_TEST = (
    "set(x ${x} a)\nlist(APPEND x ${x} b)\nset(x ${x} ${x} c)\nset(y ${x} ${y} d)\nset(z ${y} ${z})\n"
    "set(y ${z} e)\n",
    "${z};${x}",
    ["a", "b", "c", "d", "e", "a", "b", "c"],
)


def self_test_targets(root):
    """Return the layout-targets text of the self-test tree."""
    def target(name, kind, folder, sources="", includes="", links="", interface_links="", extra_includes=""):
        lines = [f"{name}\tTYPE\t{kind}", f"{name}\tSOURCE_DIR\t{root / folder}"]
        if sources:
            lines.append(f"{name}\tSOURCES\t{sources}")
        if includes:
            lines.append(f"{name}\tINTERFACE_INCLUDE_DIRECTORIES\t{root / folder / includes}")
            lines.append(f"{name}\tINCLUDE_DIRECTORIES\t{root / folder / includes}{extra_includes}")
        if links:
            lines.append(f"{name}\tLINK_LIBRARIES\t{links}")
        if interface_links:
            lines.append(f"{name}\tINTERFACE_LINK_LIBRARIES\t{interface_links}")
        return "\n".join(lines)

    return "\n".join([
        target("oa-core-types", "INTERFACE_LIBRARY", "src/core", includes="include"),
        target("oa-base-game-math", "STATIC_LIBRARY", "src/base/game-math", "src/math.cpp", "include",
               "oa-core-types;oa-sim-unit", "oa-core-types;oa-sim-unit"),
        target("oa-sim-unit", "STATIC_LIBRARY", "src/sim/unit", "src/unit.cpp", "include",
               extra_includes=f";{root}/src/platform/include"),
        target("oa-platform-shims", "STATIC_LIBRARY", "src/platform", "src/files.cpp", "include",
               "-framework AppKit;$<$<PLATFORM_ID:Windows>:d3d11>;/usr/lib/x86_64-linux-gnu/libvulkan.so.1;"
               "SDL3::SDL3", "$<LINK_ONLY:OpenGL::GL>;$<LINK_ONLY:SDL3::SDL3>"),
        target("hud", "STATIC_LIBRARY", "src/ui/hud", "src/hud.cpp", links="::@(0x1);oa::sim::unit;::@"),
        target("oa-test-support", "INTERFACE_LIBRARY", "."),
        target("oa-test-launch", "STATIC_LIBRARY", ".", "launch.cpp"),
        target("oa-options", "INTERFACE_LIBRARY", "."),
        target("recorder", "STATIC_LIBRARY", "tests/extension", "b.cpp"),
        "ALIAS\toa::sim::unit\toa-sim-unit",
    ]) + "\n"


def self_test():
    """Check the built-in link entries and tree and compare the findings with the expected ones."""
    for entry, expected in GRAPHICS_SELF_TEST_ENTRIES:
        found = graphics_names(entry)
        if found != expected:
            print(f"check_layout self-test: {entry} names the graphics libraries {found}, not {expected}")
            return 1
    script, entry, expected = EXPANSION_SELF_TEST
    found = split_list(VariableExpander(collect_variables(cmake_commands(script))).expand(entry, set()))
    if found != expected:
        print(f"check_layout self-test: {entry} expands to {found}, not {expected}")
        return 1
    with tempfile.TemporaryDirectory() as folder:
        root = Path(folder).resolve()
        for path, text in SELF_TEST_FILES.items():
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_text(text)
        targets = root / "targets.tsv"
        targets.write_text(self_test_targets(root))
        stale = {"layer_exceptions": [{"from": "oa-sim-unit", "to": "oa-core-types"}]}
        findings = Checker(root, targets, stale).run()
        got = sorted((rule, where) for rule, where, _ in findings)
        if got != sorted(SELF_TEST_EXPECTED):
            for item in sorted(set(SELF_TEST_EXPECTED) - set(got)):
                print(f"check_layout self-test: missing finding {item}")
            for rule, where, message in findings:
                if (rule, where) not in SELF_TEST_EXPECTED:
                    print(f"check_layout self-test: unexpected finding {where}: {rule}: {message}")
            return 1
        allowed = {"layer_exceptions": [{"from": "oa-base-game-math", "to": "oa-sim-unit"}]}
        if any(rule in ("layer", "stale") for rule, _, _ in Checker(root, targets, allowed).run()):
            print("check_layout self-test: a baseline entry did not allow its link")
            return 1
        malformed = root / "baseline.json"
        malformed.write_text('{"layer_exceptions": [{"from": 1}]}')
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            status = check(root, targets, malformed)
        if status != 2 or "layer_exceptions" not in errors.getvalue():
            print("check_layout self-test: a malformed baseline was not refused")
            return 1
    print(f"check_layout self-test: {len(SELF_TEST_EXPECTED)} findings and {len(GRAPHICS_SELF_TEST_ENTRIES)} "
          "link entries as expected")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=str(ROOT), help="the engine tree (default: this checkout)")
    parser.add_argument("--targets", help="the layout-targets.tsv a configuration wrote")
    parser.add_argument("--baseline", default=str(BASELINE), help="the known layer exceptions")
    parser.add_argument("--self-test", action="store_true", help="check a small built-in tree")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.targets:
        parser.error("--targets is required")
    return check(args.root, args.targets, args.baseline)


if __name__ == "__main__":
    sys.exit(main())
