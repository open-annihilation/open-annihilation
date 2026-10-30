#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""The English texts the engine passes to the interface catalogue.

A lookup wrapper is a function whose body calls interface_text, or that
passes the text on to one. This module reads the engine's sources and
collects the literals and named constants those wrappers are handed, plus
the hack and area titles the settings dialog looks up through a variable.
oa-tool's interface-words command prints the lists the modules export.
The test fails when a wrapper is handed a text no list holds.
"""

from __future__ import annotations

import os
import sys
from collections import defaultdict
from dataclasses import dataclass

SOURCE_SUFFIXES = (".hpp", ".h", ".cpp", ".cc", ".inc", ".mm")
SKIP_DIRS = {"tests", "third_party", "build", ".git"}

# Argument 0 is the English text.
ARG0_ALL = ("interface_text", "shown_text", "own_words", "shown_with")
# Argument 0, and only in these trees: the name is too common to match everywhere.
ARG0_SCOPED = {
    "tr": (
        "src/ui/game-files/",
        "src/ui/folder-chooser/",
        "src/app/package-install/",
    ),
    "fill": (
        "src/ui/game-files/",
        "src/app/package-install/",
    ),
    "word": ("src/ui/map-browser/",),
}
# Argument 1 is the English text. An empty first argument ({}) is a lookup
# the caller did not ask for.
ARG1_SCOPED = {
    "shown": ("src/ui/library/", "src/app/update/"),
    "filled": ("src/ui/library/", "src/app/update/"),
    "interface_word": ("src/",),
    "library_word": ("src/",),
    "shown_word": ("src/",),
}
# filled's first argument is the English text in these trees. The Library's
# filled takes the hooks first, so it is not in this list.
FILLED_ARG0 = (
    "src/ui/engine-settings/",
    "src/ui/frontend-multiplayer/",
    "src/ui/map-browser/",
    "src/app/runtime_map_browser.cpp",
)
# language_notice.cpp's shown looks its argument up. The Library's shown
# does not use this rule.
NOTICE_SHOWN = "src/ui/engine-settings/src/language_notice.cpp"
# Row factories. The id is the first argument and is not looked up. Every
# other string is a label, hint, caption or button the translating view
# passes to the catalogue. Drawing helpers in other files are also named
# toggle, so only the files that build rows count.
ROW_FILES = {
    "src/ui/engine-settings/src/settings_rows.cpp",
    "src/ui/engine-settings/src/downloads_page.cpp",
    "src/ui/engine-settings/src/updates_page.cpp",
    "src/ui/engine-settings/src/registered.cpp",
}
ROW_FUNCS = {
    "engine_switch",
    "option_switch",
    "toggle",
    "choice",
    "slider",
    "levels",
    "buttons",
    "text_row",
    "link_row",
    "value_and_button",
    "linear",
    "link",
}
# The comma and the failure prefix are looked up without the space that
# used to sit on their edge. The shown English still has that space.
EDGE_KEYS = {", ", "The download failed: "}


@dataclass
class Tok:
    """One source token."""

    kind: str  # str, ident, op
    value: str


def source_files(root: str) -> list[str]:
    """C++ sources under src/, tests left out."""
    found = []
    src = os.path.join(root, "src")
    for folder, dirs, names in os.walk(src):
        dirs[:] = [name for name in dirs if name not in SKIP_DIRS and not name.startswith(".")]
        for name in names:
            if not name.endswith(SOURCE_SUFFIXES):
                continue
            # Test translations sit beside the sources (mod_summary_test.cpp)
            # as well as under tests/. They are not the catalogue.
            if name.endswith(("_test.cpp", "_test.hpp", "_test.cc", "_test.inc")):
                continue
            found.append(os.path.join(folder, name))
    return found


def _decode_body(body: str) -> str:
    """The text a string literal's body holds, escapes read as C++ does."""
    raw = bytearray()
    index = 0
    simple = {
        "n": b"\n",
        "t": b"\t",
        "r": b"\r",
        "\\": b"\\",
        "'": b"'",
        '"': b'"',
        "?": b"?",
        "a": b"\a",
        "b": b"\b",
        "f": b"\f",
        "v": b"\v",
    }
    while index < len(body):
        char = body[index]
        if char != "\\":
            raw += char.encode("utf-8")
            index += 1
            continue
        index += 1
        if index >= len(body):
            break
        esc = body[index]
        index += 1
        if esc in simple:
            raw += simple[esc]
        elif esc == "x":
            digits = ""
            while index < len(body) and body[index] in "0123456789abcdefABCDEF":
                digits += body[index]
                index += 1
            raw.append(int(digits or "0", 16) & 0xFF)
        elif esc in "01234567":
            digits = esc
            for _ in range(2):
                if index < len(body) and body[index] in "01234567":
                    digits += body[index]
                    index += 1
                else:
                    break
            raw.append(int(digits, 8) & 0xFF)
        elif esc in ("u", "U"):
            width = 4 if esc == "u" else 8
            digits = body[index : index + width]
            index += width
            raw += chr(int(digits, 16)).encode("utf-8")
        else:
            raw += esc.encode("utf-8")
    return raw.decode("utf-8", errors="replace")


def lex(text: str) -> list[Tok]:
    """Tokens of one source, comments dropped and string escapes decoded."""
    tokens: list[Tok] = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char in " \t\r\n\v\f":
            index += 1
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "/":
            index = text.find("\n", index)
            if index < 0:
                break
            continue
        if char == "/" and index + 1 < length and text[index + 1] == "*":
            end = text.find("*/", index + 2)
            index = length if end < 0 else end + 2
            continue
        if char == "'" and (index == 0 or text[index - 1] != "\\"):
            # A character literal, kept as punctuation so it is not a string.
            index += 1
            if index < length and text[index] == "\\":
                index += 2
            elif index < length:
                index += 1
            if index < length and text[index] == "'":
                index += 1
            tokens.append(Tok("op", "''"))
            continue
        prefix = ""
        if text.startswith(("u8", "u", "U", "L"), index):
            if text.startswith("u8", index):
                prefix = "u8"
            else:
                prefix = text[index]
            # The prefix counts only when a string follows.
            nxt = index + len(prefix)
            if nxt < length and text[nxt] in ('"', "R"):
                index = nxt
            else:
                prefix = ""
        if text.startswith('R"', index):
            end_delim = text.find("(", index + 2)
            if end_delim < 0:
                index += 2
                continue
            delim = text[index + 2 : end_delim]
            close = ")" + delim + '"'
            body_at = end_delim + 1
            stop = text.find(close, body_at)
            if stop < 0:
                break
            tokens.append(Tok("str", text[body_at:stop]))
            index = stop + len(close)
            continue
        if char == '"' or (prefix and text[index : index + 1] == '"'):
            if text[index] != '"':
                index += 1
            else:
                index += 1
            body = []
            while index < length:
                if text[index] == "\\":
                    body.append(text[index : index + 2] if index + 1 < length else "\\")
                    # \x consumes every following hex digit; keep them in the body.
                    if index + 1 < length and text[index + 1] == "x":
                        index += 2
                        body[-1] = "\\x"
                        while index < length and text[index] in "0123456789abcdefABCDEF":
                            body[-1] += text[index]
                            index += 1
                        continue
                    if index + 1 < length and text[index + 1] in "uU":
                        width = 4 if text[index + 1] == "u" else 8
                        piece = text[index : index + 2 + width]
                        body[-1] = piece
                        index += len(piece)
                        continue
                    index += 2
                    continue
                if text[index] == '"':
                    index += 1
                    break
                body.append(text[index])
                index += 1
            tokens.append(Tok("str", _decode_body("".join(body))))
            continue
        if char.isalpha() or char == "_":
            start = index
            index += 1
            while index < length and (text[index].isalnum() or text[index] == "_"):
                index += 1
            tokens.append(Tok("ident", text[start:index]))
            continue
        tokens.append(Tok("op", char))
        index += 1
    return tokens


def _matching(tokens: list[Tok], start: int, open_ch: str, close_ch: str) -> int | None:
    """The token after the close that matches tokens[start], or None."""
    depth = 0
    for index in range(start, len(tokens)):
        value = tokens[index].value
        if value == open_ch:
            depth += 1
        elif value == close_ch:
            depth -= 1
            if depth == 0:
                return index + 1
    return None


def _call_end(tokens: list[Tok], name_at: int) -> int | None:
    """The token after the call's closing parenthesis, or None when name_at is not a call."""
    index = name_at + 1
    if index < len(tokens) and tokens[index].value == "<":
        # A template argument list, then the call. A bare less-than is not one:
        # it has to close and be followed by '('.
        end = _matching(tokens, index, "<", ">")
        if end is None or end >= len(tokens) or tokens[end].value != "(":
            return None
        index = end
    if index >= len(tokens) or tokens[index].value != "(":
        return None
    return _matching(tokens, index, "(", ")")


def _split_args(tokens: list[Tok]) -> list[list[Tok]]:
    """Arguments of a parenthesised token sequence, the parentheses included."""
    if not tokens or tokens[0].value != "(":
        return []
    args: list[list[Tok]] = [[]]
    depth = 0
    angle = 0
    for tok in tokens:
        if tok.value == "(":
            depth += 1
            if depth == 1:
                continue
        elif tok.value == ")":
            depth -= 1
            if depth == 0:
                break
        elif tok.value in "[{":
            depth += 1
        elif tok.value in "]}":
            depth -= 1
        elif tok.value == "<" and depth >= 1:
            angle += 1
        elif tok.value == ">" and angle > 0:
            angle -= 1
        if depth == 1 and angle == 0 and tok.value == ",":
            args.append([])
            continue
        if depth >= 1:
            args[-1].append(tok)
    if len(args) == 1 and not args[0]:
        return []
    return args


def _fold(tokens: list[Tok], start: int) -> tuple[str, int]:
    """Adjacent string tokens from start, and the token after them."""
    text = ""
    index = start
    while index < len(tokens) and tokens[index].kind == "str":
        text += tokens[index].value
        index += 1
    return text, index


def _string_constants(tokens: list[Tok]) -> dict[str, str]:
    """Named string constants in one file, the last definition of each name."""
    found: dict[str, str] = {}
    for index, tok in enumerate(tokens):
        if tok.kind != "ident" or index + 2 >= len(tokens):
            continue
        nxt = index + 1
        if tokens[nxt].value == "[":
            # char name[] = "..."
            while nxt < len(tokens) and tokens[nxt].value != "]":
                nxt += 1
            nxt += 1
        if nxt >= len(tokens) or tokens[nxt].value not in ("=", "{"):
            continue
        window = tokens[max(0, index - 12) : index]
        # A constexpr string. A nearby parameter of type string_view must not
        # make the next std::string assignment look like a constant.
        declared = any(item.value == "constexpr" for item in window) and any(
            item.value in ("string_view", "char") for item in window
        )
        if not declared:
            continue
        nxt += 1
        if nxt >= len(tokens) or tokens[nxt].kind != "str":
            continue
        text, after = _fold(tokens, nxt)
        # A string joined with + is not the constant's whole value.
        if after < len(tokens) and tokens[after].value not in (";", ",", "}", ")"):
            continue
        found[tok.value] = text
    return found


def _resolve(arg: list[Tok], local: dict[str, str], words: dict[str, str]) -> str | None:
    """The English text an argument names, when it is a literal or a constant."""
    significant = [tok for tok in arg if not (tok.kind == "op" and tok.value in ("(", ")"))]
    if not significant:
        return None
    if all(tok.kind == "str" for tok in significant):
        return "".join(tok.value for tok in significant)
    names = [tok.value for tok in significant if tok.kind == "ident" or tok.value == ":"]
    # words::name, or a single constant name, or qualifier::name.
    idents = [tok.value for tok in significant if tok.kind == "ident"]
    ops = [tok.value for tok in significant if tok.kind == "op"]
    if not idents or any(tok.kind == "str" for tok in significant):
        return None
    if any(op not in (":",) for op in ops):
        return None
    if idents[0] == "words" and len(idents) >= 2:
        return words.get(idents[-1])
    if len(idents) == 1 or (len(idents) == 2 and ops.count(":") == 2):
        name = idents[-1]
        if name in local:
            return local[name]
    return None


def _literals(arg: list[Tok]) -> list[str]:
    """String literals in an argument, adjacent pieces joined."""
    found = []
    index = 0
    while index < len(arg):
        if arg[index].kind == "str":
            text, index = _fold(arg, index)
            found.append(text)
        else:
            index += 1
    return found


def _rel(root: str, path: str) -> str:
    return os.path.relpath(path, root).replace("\\", "/")


def _scoped(rel: str, prefixes: tuple[str, ...]) -> bool:
    return any(rel.startswith(prefix) or rel == prefix for prefix in prefixes)


def required_words(root: str) -> set[str]:
    """Texts a lookup wrapper is handed, from the sources under root."""
    words: dict[str, str] = {}
    per_file: dict[str, dict[str, str]] = {}
    parsed: dict[str, list[Tok]] = {}
    for path in source_files(root):
        rel = _rel(root, path)
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        tokens = lex(text)
        parsed[rel] = tokens
        constants = _string_constants(tokens)
        per_file[rel] = constants
        if rel.endswith("src/ui/library/include/oa/ui/library/text.hpp"):
            words = constants
    # A name defined once, so a call in another file can use it.
    global_values: dict[str, set[str]] = defaultdict(set)
    for rel_path, constants in per_file.items():
        for name, value in constants.items():
            global_values[name].add(value)


    def resolve_any(arg: list[Tok], rel: str) -> str | None:
        text = _resolve(arg, per_file.get(rel, {}), words)
        if text is not None:
            return text
        idents = [tok.value for tok in arg if tok.kind == "ident"]
        ops = [tok.value for tok in arg if tok.kind == "op" and tok.value not in ("(", ")")]
        # Only a bare name, or a ::-qualified name. A member (model.side) is not a constant.
        if any(op not in (":",) for op in ops):
            return None
        # A constant's name, not a parameter called text or title. Cross-file
        # resolution is only for names that are written as constants.
        def constant_name(name: str) -> bool:
            if "_" in name:
                return True
            return len(name) > 2 and name[0] == "k" and name[1].isupper()

        if len(idents) == 1 and constant_name(idents[0]):
            values = global_values.get(idents[0], set())
            if len(values) == 1:
                return next(iter(values))
        # layout::check_now_text and the same shape. A member access has a dot
        # and was rejected above.
        if len(idents) >= 2 and idents[0] != "words" and constant_name(idents[-1]):
            values = global_values.get(idents[-1], set())
            if len(values) == 1:
                return next(iter(values))
        return None

    found: set[str] = set()
    origins: dict[str, set[str]] = defaultdict(set)

    here = [""]

    def add(text: str | None) -> None:
        if text:
            found.add(text)
            origins[text].add(here[0])

    for rel, tokens in parsed.items():
        here[0] = rel
        for index, tok in enumerate(tokens):
            if tok.kind != "ident":
                continue
            if index > 0 and tokens[index - 1].value == "operator":
                continue
            end = _call_end(tokens, index)
            if end is None:
                continue
            here[0] = f"{rel}:{tok.value}"
            open_at = index + 1
            if tokens[open_at].value == "<":
                open_at = _matching(tokens, open_at, "<", ">") or open_at
            args = _split_args(tokens[open_at:end])
            name = tok.value
            if name in ARG0_ALL and args:
                add(resolve_any(args[0], rel))
            elif name in ARG0_SCOPED and _scoped(rel, ARG0_SCOPED[name]) and args:
                add(resolve_any(args[0], rel))
            elif name == "filled" and _scoped(rel, FILLED_ARG0) and args:
                add(resolve_any(args[0], rel))
            elif name == "shown" and rel == NOTICE_SHOWN and args:
                add(resolve_any(args[0], rel))
                for literal in _literals(args[0]):
                    add(literal)
            elif name in ARG1_SCOPED and _scoped(rel, ARG1_SCOPED[name]) and len(args) >= 2:
                compact = "".join(item.value for item in args[0] if item.kind != "str")
                if "".join(compact.split()) == "{}":
                    continue
                add(resolve_any(args[1], rel))
            elif rel in ROW_FILES and name in ROW_FUNCS:
                # Ids are a single token of letters, digits, dots and hyphens.
                # Captions, labels and hints are the texts the row shows.
                for arg in args[1:]:
                    for literal in _literals(arg):
                        if len(literal) > 1 and all(ch.islower() or ch.isdigit() or ch in ".-_" for ch in literal):
                            continue
                        add(literal)
                    resolved = resolve_any(arg, rel)
                    if resolved and not (
                        len(resolved) > 1 and all(ch.islower() or ch.isdigit() or ch in ".-_" for ch in resolved)
                    ):
                        add(resolved)
            if rel.endswith("registry_table.inc") and name == "title":
                pass
        here[0] = rel
        if rel.endswith("src/data/mod-profile/src/registry_table.inc"):
            for index, tok in enumerate(tokens[:-2]):
                if tok.value == "." and tokens[index + 1].value == "title" and tokens[index + 2].value == "=":
                    nxt = index + 3
                    if nxt < len(tokens) and tokens[nxt].kind == "str":
                        add(tokens[nxt].value)
        if rel.endswith("src/ui/kit/include/oa/ui/kit/rows.hpp"):
            for name, value in per_file[rel].items():
                if name in ("switch_off_text", "switch_on_text"):
                    add(value)
        if rel.endswith("src/app/content/include/oa/app/content/downloads.hpp"):
            # failure_text's phrases are passed to interface_text by the
            # download screen. reason_text's words are not.
            start = None
            for index, tok in enumerate(tokens):
                if tok.value == "failure_text" and _call_end(tokens, index) is not None:
                    # The definition is the one whose body is a compound statement.
                    end = _call_end(tokens, index)
                    if end is not None and end < len(tokens) and tokens[end].value == "{":
                        start = end
                        break
            if start is not None:
                end = _matching(tokens, start, "{", "}")
                body = tokens[start : end or len(tokens)]
                for index, tok in enumerate(body):
                    if tok.value == "return" and index + 1 < len(body) and body[index + 1].kind == "str":
                        add(body[index + 1].value)
    found -= EDGE_KEYS
    found.discard("")
    required_words.last_origins = origins  # type: ignore[attr-defined]
    return found


def module_of(rel: str) -> str | None:
    """The module that exports a text found in rel, or None to leave it out."""
    if "/tests/" in f"/{rel}" or rel.endswith("interface_words.hpp"):
        return None
    order = (
        ("src/ui/first-run/", "first-run"),
        ("src/ui/library/", "library"),
        ("src/ui/engine-settings/", "engine-settings"),
        ("src/ui/map-browser/", "map-browser"),
        ("src/ui/frontend-multiplayer/", "get-map"),
        ("src/app/package-install/", "package-install"),
        ("src/ui/game-files/", "game-files"),
        ("src/ui/folder-chooser/", "folder-chooser"),
        ("src/ui/touch-hud/", "touch-hud"),
        ("src/netgame/match/", "net-match"),
        ("src/ui/kit/", "engine-settings"),
        ("src/data/mod-profile/", "engine-settings"),
        ("src/app/", "app"),
    )
    for prefix, module in order:
        if rel.startswith(prefix):
            return module
    return None


def literals_by_file(root: str) -> dict[str, set[str]]:
    """Every string literal in each production source, adjacent pieces joined."""
    found: dict[str, set[str]] = {}
    for path in source_files(root):
        rel = _rel(root, path)
        if module_of(rel) is None and not rel.startswith("src/"):
            continue
        if "/tests/" in f"/{rel}":
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        values: set[str] = set()
        tokens = lex(text)
        index = 0
        while index < len(tokens):
            if tokens[index].kind == "str":
                folded, index = _fold(tokens, index)
                if folded:
                    values.add(folded)
            else:
                index += 1
        found[rel] = values
    return found


def load_sections(path: str) -> set[str]:
    """Section names of an interface table."""
    sections = set()
    for line in open(path, encoding="utf-8"):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]") and not stripped.startswith("[["):
            sections.add(stripped[1:-1])
    return sections


def located_words(
    root: str, sections: set[str]
) -> tuple[dict[str, set[str]], set[str], dict[str, set[str]]]:
    """Pack sections that are still string literals, by the module that holds them."""
    by_module: dict[str, set[str]] = defaultdict(set)
    found: set[str] = set()
    stray: dict[str, set[str]] = defaultdict(set)
    for rel, literals in literals_by_file(root).items():
        module = module_of(rel)
        hit = literals & sections
        if module is None:
            for item in hit:
                stray[item].add(rel)
            continue
        by_module[module] |= hit
        found |= hit
    return by_module, sections - found, stray


# Lines the Downloads page looks up after it wraps a hint. They are not
# literals: break_lines produces them from the paragraph, and the row
# passes each line to the catalogue.
# Downloads looks the whole hint up, then the kit wraps it. The broken
# lines are not catalogue keys.
WRAPPED_HINTS: tuple[str, ...] = ()

MODULES = (
    (
        "library",
        "oa::ui::library",
        "src/ui/library/include/oa/ui/library/interface_words.hpp",
        "the Library",
        True,
    ),
    (
        "first-run",
        "oa::ui::first_run",
        "src/ui/first-run/include/oa/ui/first_run/interface_words.hpp",
        "the first-run setup",
        False,
    ),
    (
        "engine-settings",
        "oa::ui::engine_settings",
        "src/ui/engine-settings/include/oa/ui/engine_settings/interface_words.hpp",
        "the settings dialog",
        False,
    ),
    (
        "map-browser",
        "oa::ui::map_browser",
        "src/ui/map-browser/include/oa/ui/map_browser/interface_words.hpp",
        "the map browser",
        False,
    ),
    (
        "get-map",
        "oa::ui::frontend_multiplayer",
        "src/ui/frontend-multiplayer/include/oa/ui/frontend_multiplayer/interface_words.hpp",
        "Get Map",
        False,
    ),
    (
        "package-install",
        "oa::app::package_install",
        "src/app/package-install/include/oa/app/package_install/interface_words.hpp",
        "the package prompts",
        False,
    ),
    (
        "game-files",
        "oa::ui::game_files",
        "src/ui/game-files/include/oa/ui/game_files/interface_words.hpp",
        "the Game files screen",
        False,
    ),
    (
        "folder-chooser",
        "oa::ui::folder_chooser",
        "src/ui/folder-chooser/include/oa/ui/folder_chooser/interface_words.hpp",
        "the folder chooser",
        False,
    ),
    (
        "touch-hud",
        "oa::ui::touch_hud",
        "src/ui/touch-hud/include/oa/ui/touch_hud/interface_words.hpp",
        "the touch controls",
        False,
    ),
    (
        "net-match",
        "oa::netgame::match",
        "src/netgame/match/include/oa/netgame/match/interface_words.hpp",
        "network play's notices",
        False,
    ),
    (
        "app",
        "oa::app",
        "src/app/include/oa/app/interface_words.hpp",
        "the shell around the screens",
        False,
    ),
)


def cpp_string(text: str) -> str:
    """A C++ string literal for text."""
    escaped = (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


def header_text(namespace: str, blurb: str, entries: list[str], include_words: bool) -> str:
    """One module's interface-word header."""
    include = '#include "oa/ui/library/text.hpp"\n\n' if include_words else ""
    body = "\n".join(entries)
    return f"""// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts {blurb} passes to the interface catalogue. oa-tool joins
// this list with the other modules' lists.

#pragma once

{include}#include <string_view>

namespace {namespace} {{

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {{
{body}
}};

}}  // namespace {namespace}
"""


def lists_by_module(root: str, sections_path: str) -> dict[str, list[str]]:
    """Each module's catalogue texts, library entries as words:: names."""
    required = required_words(root)
    origins = required_words.last_origins  # type: ignore[attr-defined]
    sections = load_sections(sections_path)
    by_module, _absent, _stray = located_words(root, sections)
    for word, places in origins.items():
        if word not in required:
            continue
        homes = set()
        for place in places:
            homes.add(module_of(place.split(":", 1)[0]) or "app")
        for home in homes:
            if home is None:
                continue
            by_module[home].add(word)
    for line in WRAPPED_HINTS:
        by_module["engine-settings"].add(line)
    covered = set()
    for values in by_module.values():
        covered |= values
    uncovered = sorted(sections - covered)
    print(f"pack sections not listed {len(uncovered)}")
    for item in uncovered[:30]:
        print(f"  MISS {item!r}")
    for key in EDGE_KEYS:
        for words in by_module.values():
            words.discard(key)
    # The Library names its texts. Emit the constant when its value is listed.
    text_hpp = os.path.join(root, "src/ui/library/include/oa/ui/library/text.hpp")
    tokens = lex(open(text_hpp, encoding="utf-8").read())
    constants = list(_string_constants(tokens).items())
    listed: dict[str, list[str]] = {}
    library_values = by_module.get("library", set())
    covered = set()
    library_entries = []
    for name, value in constants:
        if value in library_values and value:
            library_entries.append((value, f"    words::{name},"))
            covered.add(value)
    for value in sorted(library_values - covered):
        if value:
            library_entries.append((value, f"    {cpp_string(value)},"))
    library_entries.sort()
    listed["library"] = [entry for _value, entry in library_entries]
    for key, values in by_module.items():
        if key == "library":
            continue
        listed[key] = [f"    {cpp_string(value)}," for value in sorted(v for v in values if v)]
    return listed


def write_headers(root: str, sections_path: str) -> None:
    """Write each module's interface_words header."""
    listed = lists_by_module(root, sections_path)
    for key, namespace, rel, blurb, include_words in MODULES:
        entries = listed.get(key, [])
        if not entries:
            raise SystemExit(f"{key} has no interface words")
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as out:
            out.write(header_text(namespace, blurb, entries, include_words))
        print(f"wrote {rel} {len(entries)}")


def main() -> int:
    """Print how the scanned texts sit against a pack's sections."""
    if len(sys.argv) > 1 and sys.argv[1] == "--write":
        write_headers(sys.argv[2], sys.argv[3])
        return 0
    root = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    words = required_words(root)
    print(f"required {len(words)}")
    tdf = None
    if len(sys.argv) > 2:
        tdf = sys.argv[2]
    if tdf:
        sections = load_sections(tdf)
        print(f"tdf {len(sections)}")
        _by_module, absent, stray = located_words(root, sections)
        print(f"tdf literal absent {len(absent)}")
        for item in sorted(absent):
            print(f"  ABSENT {item!r}")
        only_stray = {item: files for item, files in stray.items() if item in absent}
        print(f"tdf only outside a module {len(only_stray)}")
        for item, files in sorted(only_stray.items())[:30]:
            print(f"  STRAY {item!r} {sorted(files)[:2]}")
        missing = sorted(sections - words)
        extra = sorted(words - sections)
        print(f"tdf not required {len(missing)}")
        for item in missing[:80]:
            print(f"  TDF {item!r}")
        print(f"required not tdf {len(extra)}")
        for item in extra:
            where = getattr(required_words, "last_origins", {}).get(item, ())
            print(f"  EXT {item!r} :: {sorted(where)[:4]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
