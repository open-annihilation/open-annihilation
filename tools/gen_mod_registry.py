#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Generate the engine's OAMOD tables and records from the registry.

Reads src/data/mod-profile/registry/hack-registry.yaml, checks that it is
consistent (every baseline, default and preset valid for its parameter, presets
complete, constraints kept, references resolved, and each hack's actions list
naming action ids from gen_input_actions.py --list), and writes:

  src/data/mod-profile/src/registry_table.inc
      the registry as constant C++ tables, which the resolver reads;
  src/data/match-rules/include/oa/data/match_rules/script_extensions.inc
  src/data/match-rules/include/oa/data/match_rules/records.inc
      the plain records the simulation reads: one record per hack, grouped by
      area, every field initialised to its 3.1c baseline;
  src/data/mod-profile/include/oa/data/mod_profile/records.inc
      the records the rest of the engine reads: identity, layout, strings,
      media, data keys and the network, recorder and interface hacks;
  src/data/mod-profile/include/oa/data/mod_profile/bindings.inc
      visit_bindings, which names every registry meaning with the one field
      that holds it. A limit's parameters are held by the engine's own Limits
      record (src/data/limits), named in LIMIT_FIELDS, not by a generated one.

A field's name is its parameter's, in snake case; a record's is its entry's,
in Pascal case. A string written with byte escapes in the registry stays
escaped in what this writes.

  gen_mod_registry.py            write the files
  gen_mod_registry.py --check    write nothing; exit 1 when a file differs
  gen_mod_registry.py --self-test  check the reader, the registry check and the escaping
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import oamod_yaml  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "src/data/mod-profile/registry/hack-registry.yaml"
TABLE = ROOT / "src/data/mod-profile/src/registry_table.inc"
SCRIPT_EXTENSIONS = ROOT / "src/data/match-rules/include/oa/data/match_rules/script_extensions.inc"
SIM_RECORDS = ROOT / "src/data/match-rules/include/oa/data/match_rules/records.inc"
PROFILE_RECORDS = ROOT / "src/data/mod-profile/include/oa/data/mod_profile/records.inc"
BINDINGS = ROOT / "src/data/mod-profile/include/oa/data/mod_profile/bindings.inc"

KEBAB = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
ENTRY_ID = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*(?:\.[a-z0-9]+(?:-[a-z0-9]+)*)+$")
TYPES = ["bool", "int", "decimal", "string", "enum", "list<int>", "list<decimal>", "list<enum>",
         "list<string>", "set<enum>", "int-or-none"]
KINDS = {"identity": "identity", "layout": "layout", "limit": "limit", "script-ext": "script_extension",
         "data-key": "data_key", "hack": "hack", "string": "string", "media": "media"}
VALUE_TYPES = {"bool": "boolean", "int": "integer", "decimal": "decimal", "string": "string",
               "enum": "enumeration", "list<int>": "integer_list", "list<decimal>": "decimal_list",
               "list<enum>": "enumeration_list", "list<string>": "string_list",
               "set<enum>": "enumeration_set", "int-or-none": "integer_or_none"}
COMPARISONS = {"<=": "less_equal", "<": "less", ">=": "greater_equal", ">": "greater"}
TRANSFORMS = {"unit-type-bits": "unit_type_bits"}
# The field of oa::data::limits::Limits that holds each parameter of each limit.
LIMIT_FIELDS = {
    "limits.units-per-player": {"default": "units_per_player.default_limit",
                                "min": "units_per_player.minimum",
                                "max": "units_per_player.maximum",
                                "limits-screen-fallback": "units_per_player.limits_screen_fallback"},
    "limits.unit-types": {"bitset-bits": "unit_types.bitset_bits",
                          "partial-widening": "unit_types.partial_widening"},
    "limits.effects": {"queue": "effects.queue", "reserve": "effects.reserve"},
    "limits.path-search-budget": {"nodes": "path_search.nodes"},
    "limits.model-composite": {"width": "model_composite.width", "height": "model_composite.height",
                               "clamp-oversize": "model_composite.clamp_oversize"},
    "limits.build-list-entries": {"copy": "build_lists.copy", "overflow": "build_lists.overflow"},
    "limits.category-mask-types": {"types": "category_masks.types"},
    "limits.weapon-types": {"types": "weapon_types.types"},
    "limits.sides": {"count": "sides.count"},
}
# Hack areas whose rules the simulation reads; the others are read outside it.
SIM_AREAS = ["ai", "orders", "air", "weapons", "repair", "veterancy", "economy", "intel", "setup",
             "teams", "sharing", "units", "console"]
PROFILE_AREAS = ["network", "recorder", "ui"]
# The most characters a hack's or an area's title holds, so that it fits its line in Developer Mode's list.
TITLE_LENGTH = 32
# Names that cannot be a C++ member or enumerator as they are, and those the
# Windows headers define as macros.
RENAMES = {"default": "default_value", "min": "minimum", "max": "maximum", "interface": "interface_value"}
CXX_KEYWORDS = {"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
                "catch", "char", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
                "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
                "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
                "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
                "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
                "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
                "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
                "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
                "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq", "near", "far",
                "small", "min", "max", "interface"}
INT32_MIN, INT32_MAX = -(1 << 31), (1 << 31) - 1
GENERATED = ("// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT\n"
             "// SPDX-License-Identifier: GPL-3.0-only\n\n"
             "// Generated by tools/gen_mod_registry.py from src/data/mod-profile/registry/hack-registry.yaml;\n"
             "// do not edit. Run the tool after changing the registry.\n")


class RegistryError(Exception):
    pass


# ---------------------------------------------------------------------------------------------- names
def snake(name):
    out = name.replace("-", "_").replace(".", "_")
    if out in RENAMES:
        return RENAMES[out]
    if out in CXX_KEYWORDS:
        return out + "_value"
    if out[0].isdigit():
        return "value_" + out
    return out


def pascal(name):
    return "".join(part[:1].upper() + part[1:] for part in re.split(r"[-._]", name) if part)


def member(name):
    """The member a kebab-case parameter or key is held in."""
    return snake(name)


# ---------------------------------------------------------------------------------------------- values
def is_number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def check_value(spec, v, where):
    """Raise RegistryError when v does not fit the spec, as the resolver checks a profile's value."""
    t = spec["type"]
    if t.startswith("list<") or t == "set<enum>":
        if not isinstance(v, list):
            raise RegistryError(f"{where}: expected a list, got {v!r}")
        n = spec.get("length")
        if isinstance(n, int) and len(v) != n:
            raise RegistryError(f"{where}: expected {n} items, got {len(v)}")
        if isinstance(n, list) and not n[0] <= len(v) <= n[1]:
            raise RegistryError(f"{where}: expected {n[0]}-{n[1]} items, got {len(v)}")
        elem = dict(spec, type={"list<int>": "int", "list<decimal>": "decimal", "list<string>": "string"}.get(t, "enum"))
        for i, x in enumerate(v):
            check_value(elem, x, f"{where}[{i}]")
        if (spec.get("distinct") or t == "set<enum>") and len(set(v)) != len(v):
            raise RegistryError(f"{where}: repeated items in {v!r}")
        if spec.get("order") == "ascending" and any(a >= b for a, b in zip(v, v[1:])):
            raise RegistryError(f"{where}: items must ascend: {v!r}")
        return
    if t == "int-or-none" and v == "none":
        return
    if t == "bool":
        if not isinstance(v, bool):
            raise RegistryError(f"{where}: expected true or false, got {v!r}")
        return
    if t in ("int", "int-or-none", "decimal"):
        if not is_number(v) or (t != "decimal" and not isinstance(v, int)):
            raise RegistryError(f"{where}: expected a number, got {v!r}")
        lo, hi = spec.get("min"), spec.get("max")
        if (lo is not None and v < lo) or (hi is not None and v > hi):
            raise RegistryError(f"{where}: {v} is outside [{lo}, {hi}]")
        m = spec.get("multiple-of")
        if m and v % m:
            raise RegistryError(f"{where}: {v} is not a multiple of {m}")
        return
    if t == "string":
        if not isinstance(v, str):
            raise RegistryError(f"{where}: expected a string, got {v!r}")
        if spec.get("max-length") is not None and len(v) > spec["max-length"]:
            raise RegistryError(f"{where}: longer than {spec['max-length']} characters")
        if spec.get("pattern") and not re.search(spec["pattern"], v):
            raise RegistryError(f"{where}: {v!r} does not match {spec['pattern']}")
        return
    if t == "enum":
        if v not in spec["values"]:
            raise RegistryError(f"{where}: {v!r} is not one of {spec['values']}")
        return
    raise RegistryError(f"{where}: unknown type {t}")


def holds(constraint, values):
    a, op, b = constraint.split()
    x, y = values[a], values[b]
    if x == "none" or y == "none":
        return True
    return {"<=": x <= y, "<": x < y, ">=": x >= y, ">": x > y}[op]


def list_action_ids():
    """The action ids `gen_input_actions.py --list` prints, one per line."""
    script = Path(__file__).resolve().parent / "gen_input_actions.py"
    run = subprocess.run(
        [sys.executable, str(script), "--list"],
        check=False,
        capture_output=True,
        text=True,
    )
    if run.returncode != 0:
        detail = (run.stderr or run.stdout).strip()
        raise RegistryError(detail or "gen_input_actions.py --list failed")
    return {line.strip() for line in run.stdout.splitlines() if line.strip()}


def check_registry(reg, known_actions=None):
    """The registry's own consistency; returns a list of problems.

    `known_actions` is the set of action ids. When an entry lists `actions` and
    this is left out, the ids come from `gen_input_actions.py --list`. The
    `actions` lists are not written into the generated tables.
    """
    problems = []
    entries = reg["entries"]
    if known_actions is None and any("actions" in entry for entry in entries.values()):
        try:
            known_actions = list_action_ids()
        except RegistryError as error:
            problems.append(str(error))
            known_actions = set()
    transforms = set(reg.get("setting-transforms", {}))
    area_titles = reg.get("area-titles", {})
    titles = {}

    def attempt(fn, *args):
        try:
            fn(*args)
        except RegistryError as e:
            problems.append(str(e))

    for eid, e in entries.items():
        where = f"registry {eid}"
        if not ENTRY_ID.match(eid):
            problems.append(f"{where}: id is not dotted kebab-case")
        if e.get("kind") not in KINDS:
            problems.append(f"{where}: unknown kind {e.get('kind')!r}")
            continue
        if e.get("scope") not in ("sim", "view"):
            problems.append(f"{where}: scope must be sim or view")
        listed = e.get("actions")
        if "actions" in e:
            if e["kind"] != "hack":
                problems.append(f"{where}: only a hack lists actions")
            elif not isinstance(listed, list) or not listed:
                problems.append(f"{where}: actions is a list of action ids")
            else:
                seen_actions = set()
                for action_id in listed:
                    if not isinstance(action_id, str) or not action_id:
                        problems.append(f"{where}: an action id is not text")
                    elif action_id in seen_actions:
                        problems.append(f"{where}: action {action_id} is listed twice")
                    elif action_id not in known_actions:
                        problems.append(f"{where}: action {action_id} is not in the action registry")
                    else:
                        seen_actions.add(action_id)
        if e["kind"] in ("identity", "layout", "string", "media"):
            attempt(check_value, e["value"], e["value"]["baseline"], f"{where} baseline")
            if e["value"].get("scope") not in ("sim", "view"):
                problems.append(f"{where}: the value's scope must be sim or view")
        if e["kind"] == "hack":
            if not isinstance(e.get("implemented"), bool):
                problems.append(f"{where}: a hack says whether it is implemented")
            area = eid.split(".")[0]
            if area not in SIM_AREAS + PROFILE_AREAS:
                problems.append(f"{where}: area {area} is neither read by the simulation nor outside it")
            if e.get("area") != area:
                problems.append(f"{where}: a hack's area is the first word of its id, {area}")
            title = e.get("title")
            if not isinstance(title, str) or not title.strip():
                problems.append(f"{where}: a hack has a title, the name players see")
            elif len(title) > TITLE_LENGTH or title != title.strip():
                problems.append(f"{where}: title {title!r} is over {TITLE_LENGTH} characters or padded")
            elif title.lower() in titles:
                problems.append(f"{where}: title {title!r} is also {titles[title.lower()]}'s")
            else:
                titles[title.lower()] = eid
            if area not in area_titles:
                problems.append(f"{where}: area {area} has no name in area-titles")
        elif "implemented" in e:
            problems.append(f"{where}: only hacks say whether they are implemented")
        if "title" in e and e["kind"] != "hack":
            problems.append(f"{where}: only hacks have a title")
        if e["kind"] == "script-ext" and e.get("direction") not in ("get", "set"):
            problems.append(f"{where}: direction must be get or set")
        if e["kind"] == "data-key":
            if e.get("file") not in ("unit", "weapon"):
                problems.append(f"{where}: file must be unit or weapon")
            ref = e.get("requires")
            if ref and entries.get(ref, {}).get("kind") != "hack":
                problems.append(f"{where}: requires {ref} is not a hack")
            ov = e.get("overrides")
            if ov:
                hid, p = ov.rsplit(".", 1)
                spec = entries.get(hid, {}).get("params", {}).get(p)
                if not spec or (spec.get("per-unit") or spec.get("per-weapon")) != eid:
                    problems.append(f"{where}: overrides {ov} does not name it back")
        mutator = e.get("mutator", False)
        if "mutator" in e and not isinstance(e.get("mutator"), bool):
            problems.append(f"{where}: mutator must be true or false")
        elif mutator is True:
            if e["kind"] not in ("hack", "limit"):
                problems.append(f"{where}: mutator: true is only for a hack or a limit")
            else:
                params = e.get("params") or {}
                match_names = [
                    name for name, spec in params.items()
                    if isinstance(spec, dict) and spec.get("adjustable") == "match"
                ]
                if not match_names:
                    problems.append(f"{where}: mutator: true needs a parameter adjustable per match")
                touches = []
                if e.get("visual") is True:
                    touches.append("visual")
                if e.get("scope") == "view":
                    touches.append("scope view")
                if eid.startswith("ui."):
                    touches.append("ui.")
                if any(isinstance(params.get(name), dict) and params[name].get("scope") == "view"
                       for name in match_names):
                    touches.append("a match parameter of scope view")
                if touches:
                    problems.append(
                        f"{where}: mutator: true is refused on an entry that touches the interface"
                        f" ({', '.join(touches)})"
                    )
        if e["kind"] not in ("limit", "hack"):
            continue
        params = e["params"]
        if e["kind"] == "limit" and set(params) != set(LIMIT_FIELDS.get(eid, {})):
            problems.append(f"{where}: the Limits fields in LIMIT_FIELDS do not match its parameters")
        for p, spec in params.items():
            w = f"{where}.{p}"
            if not KEBAB.match(p):
                problems.append(f"{w}: parameter name is not kebab-case")
            if spec["type"] not in TYPES:
                problems.append(f"{w}: unknown type {spec['type']}")
                continue
            if spec.get("adjustable") not in ("fixed", "install", "match"):
                problems.append(f"{w}: adjustable must be fixed, install or match")
            if spec.get("scope") not in ("sim", "view"):
                problems.append(f"{w}: scope must be sim or view")
            if spec["type"] in ("enum", "list<enum>", "set<enum>") and not spec.get("values"):
                problems.append(f"{w}: enum without values")
            for k in ("baseline", "default"):
                attempt(check_value, spec, spec[k], f"{w} {k}")
            if spec.get("setting-transform") and spec["setting-transform"] not in transforms:
                problems.append(f"{w}: unknown setting-transform")
            if spec.get("setting-transform") and spec["setting-transform"] not in TRANSFORMS:
                problems.append(f"{w}: the engine has no setting-transform {spec['setting-transform']}")
            if spec.get("setting") and spec["adjustable"] == "fixed":
                problems.append(f"{w}: a setting on a fixed parameter")
            for k in ("per-unit", "per-weapon"):
                ref = spec.get(k)
                if ref and entries.get(ref, {}).get("overrides") != f"{eid}.{p}":
                    problems.append(f"{w}: {k} {ref} is not a data key overriding it")
            df = spec.get("default-from")
            if df:
                src = params.get(df["param"])
                if not src or src["default"] * df["factor"] != spec["default"]:
                    problems.append(f"{w}: default-from does not reproduce the default")
            for ref in spec.get("clamp-between", []):
                if ref not in params:
                    problems.append(f"{w}: clamp-between names unknown {ref}")
            if spec["type"] == "string" and e["kind"] == "hack" and eid.split(".")[0] in SIM_AREAS \
                    and spec.get("max-length") is None:
                problems.append(f"{w}: a string the simulation reads needs a max-length")
        sh = e.get("shorthand")
        if sh and (len(params) != 1 or sh not in params or params[sh]["type"] == "bool"):
            problems.append(f"{where}: shorthand must name the single non-bool parameter")
        well_formed = []
        for c in e.get("constraints", []):
            parts = c.split()
            if len(parts) != 3 or parts[1] not in COMPARISONS or parts[0] not in params or parts[2] not in params:
                problems.append(f"{where}: constraint {c!r} is not 'param op param'")
            else:
                well_formed.append(c)
        defaults = {p: s["default"] for p, s in params.items()}
        baselines = {p: s["baseline"] for p, s in params.items()}
        sets = [("default", defaults)] + [(f"preset {n}", v) for n, v in e.get("presets", {}).items()]
        if "baseline" in e.get("presets", {}):
            sets.append(("baseline", baselines))
            if e["presets"]["baseline"] != baselines:
                problems.append(f"{where}: preset baseline differs from the baselines")
        for label, vals in sets:
            if set(vals) != set(params):
                problems.append(f"{where}: {label} is not a complete parameter set")
                continue
            for p, v in vals.items():
                attempt(check_value, params[p], v, f"{where} {label}.{p}")
            for c in well_formed:
                if not holds(c, vals):
                    problems.append(f"{where}: {label} breaks {c}")
    hack_areas = {e.get("area") for e in entries.values() if e.get("kind") == "hack"}
    for area, title in area_titles.items():
        if area not in hack_areas:
            problems.append(f"registry area-titles: {area} is the area of no hack")
        if not isinstance(title, str) or not title.strip() or len(title) > TITLE_LENGTH:
            problems.append(f"registry area-titles: {area} needs a name of 1 to {TITLE_LENGTH} characters")
    return problems


# ---------------------------------------------------------------------------------------------- C++ text
def cxx_string(text, escape_all=False):
    """A C++ string literal; bytes outside printable ASCII, and every byte of an escaped string, as octal.

    A digit after an octal escape starts a new literal ("\\303" "1"), which the compiler joins,
    so no compiler reads it as part of the escape or warns that it might.
    """
    out = []
    after_escape = False
    for byte in text.encode("utf-8"):
        c = chr(byte)
        if escape_all or byte < 0x20 or byte >= 0x7F:
            out.append(f"\\{byte:03o}")
            after_escape = True
            continue
        if after_escape and c.isdigit():
            out.append('" "')
        if c in '"\\':
            out.append("\\" + c)
        else:
            out.append(c)
        after_escape = False
    return '"' + "".join(out) + '"'


def literal_string(text):
    return cxx_string(text, isinstance(text, oamod_yaml.EscapedStr))


def number_parts(v):
    """(negative, significand, exponent, integer) of a registry number, as the engine stores it."""
    if isinstance(v, int):
        integer, text = True, str(v)
    else:
        integer, text = False, repr(float(v))
    negative = text.startswith("-")
    text = text.lstrip("-")
    mantissa, _, exp = text.lower().partition("e")
    whole, _, frac = mantissa.partition(".")
    digits = (whole + frac).lstrip("0")
    exponent = (int(exp) if exp else 0) - len(frac)
    if not digits:
        return False, 0, 0, integer
    while digits.endswith("0"):
        digits = digits[:-1]
        exponent += 1
    return negative, int(digits), exponent, integer


def cxx_number(v):
    negative, significand, exponent, integer = number_parts(v)
    return (f"{{{'true' if negative else 'false'}, {significand}u, {exponent}, "
            f"{'true' if integer else 'false'}}}")


def cxx_double(v):
    text = repr(float(v))
    if "e" not in text and "." not in text and "inf" not in text:
        text += ".0"
    return text


def comment(text, indent=""):
    """A /// comment of wrapped lines."""
    words = text.split()
    lines, line = [], ""
    for w in words:
        if len(indent) + 4 + len(line) + len(w) + 1 > 100 and line:
            lines.append(line)
            line = w
        else:
            line = (line + " " + w).strip()
    if line:
        lines.append(line)
    return "".join(f"{indent}/// {l}\n" for l in lines)


# ---------------------------------------------------------------------------------------------- the table
class Table:
    """The registry as C++ constant tables."""

    def __init__(self, reg):
        self.reg = reg
        self.literals = []
        self.enum_values = []
        self.parameters = []
        self.presets = []
        self.preset_values = []
        self.constraints = []
        self.entries = []

    def literal(self, v):
        if isinstance(v, list):
            items = [self.scalar_literal(x) for x in v]
            first = len(self.literals)
            self.literals.extend(items)
            self.literals.append(f"{{.kind = LiteralKind::list, .first_item = {first}, .item_count = {len(v)}}}")
        else:
            self.literals.append(self.scalar_literal(v))
        return len(self.literals) - 1

    @staticmethod
    def scalar_literal(v):
        if isinstance(v, bool):
            return f"{{.kind = LiteralKind::boolean, .boolean = {'true' if v else 'false'}}}"
        if is_number(v):
            return f"{{.kind = LiteralKind::number, .number = {cxx_number(v)}}}"
        if isinstance(v, str):
            return f"{{.kind = LiteralKind::string, .text = {literal_string(v)}}}"
        raise RegistryError(f"cannot write {v!r} as a literal")

    def value_spec(self, spec, baseline, scope):
        fields = [f".type = ValueType::{VALUE_TYPES[spec['type']]}", f".scope = Scope::{scope}"]
        for key, name in (("min", "minimum"), ("max", "maximum"), ("multiple-of", "multiple_of")):
            if spec.get(key) is not None:
                fields.append(f".has_{name} = true")
                fields.append(f".{name} = {cxx_number(spec[key])}")
        n = spec.get("length")
        if n is not None:
            lo, hi = (n, n) if isinstance(n, int) else n
            fields += [".has_length = true", f".length_minimum = {lo}", f".length_maximum = {hi}"]
        if spec.get("order") == "ascending":
            fields.append(".ascending = true")
        if spec.get("distinct"):
            fields.append(".distinct = true")
        if spec.get("max-length") is not None:
            fields.append(f".max_length = {spec['max-length']}")
        if spec.get("pattern"):
            fields.append(f".pattern = {cxx_string(spec['pattern'])}")
        if spec.get("values"):
            fields.append(f".first_value = {len(self.enum_values)}")
            fields.append(f".value_count = {len(spec['values'])}")
            self.enum_values.extend(spec["values"])
        if baseline is not None:
            fields.append(f".baseline = {self.literal(baseline)}")
        return "{" + ", ".join(fields) + "}"

    def build(self):
        for eid, e in self.reg["entries"].items():
            kind = KINDS[e["kind"]]
            fields = [f".id = {cxx_string(eid)}", f".kind = EntryKind::{kind}", f".scope = Scope::{e['scope']}"]
            if e.get("visual"):
                fields.append(".visual = true")
            if e.get("mutator"):
                fields.append(".mutator = true")
            if e.get("implemented"):
                fields.append(".implemented = true")
            if "key" in e:
                fields.append(f".key = {cxx_string(e['key'])}")
            if "value" in e:
                value = e["value"]
                baseline = value.get("baseline")
                fields.append(f".value = {self.value_spec(value, baseline, value.get('scope', e['scope']))}")
            if e["kind"] in ("limit", "hack"):
                params = list(e["params"])
                fields.append(f".first_parameter = {len(self.parameters)}")
                fields.append(f".parameter_count = {len(params)}")
                for p, spec in e["params"].items():
                    pf = [f".name = {cxx_string(p)}",
                          f".value = {self.value_spec(spec, spec['baseline'], spec['scope'])}",
                          f".default_value = {self.literal(spec['default'])}",
                          f".adjustable = Adjustable::{spec['adjustable']}"]
                    setting = spec.get("setting")
                    if setting:
                        (source, name), = setting.items()
                        pf.append(f".setting_source = SettingSource::{source}")
                        pf.append(f".setting_name = {cxx_string(name)}")
                    if spec.get("setting-transform"):
                        pf.append(f".transform = SettingTransform::{TRANSFORMS[spec['setting-transform']]}")
                    df = spec.get("default-from")
                    if df:
                        pf.append(f".default_from = {params.index(df['param'])}")
                        pf.append(f".default_factor = {cxx_number(df['factor'])}")
                    cb = spec.get("clamp-between")
                    if cb:
                        pf.append(f".clamp_low = {params.index(cb[0])}")
                        pf.append(f".clamp_high = {params.index(cb[1])}")
                    ref = spec.get("per-unit") or spec.get("per-weapon")
                    if ref:
                        pf.append(f".per_type_key = {cxx_string(ref)}")
                    if spec.get("unit"):
                        pf.append(f".unit = {cxx_string(spec['unit'])}")
                    self.parameters.append("{" + ", ".join(pf) + "}")
                if e.get("shorthand"):
                    fields.append(f".shorthand = {params.index(e['shorthand'])}")
                presets = e.get("presets", {})
                fields.append(f".first_preset = {len(self.presets)}")
                fields.append(f".preset_count = {len(presets)}")
                for name, vals in presets.items():
                    first = len(self.preset_values)
                    for p, v in vals.items():
                        self.preset_values.append(
                            f"{{.parameter = {params.index(p)}, .literal = {self.literal(v)}}}")
                    self.presets.append(f"{{.name = {cxx_string(name)}, .first_value = {first}, "
                                        f".value_count = {len(vals)}}}")
                cons = e.get("constraints", [])
                fields.append(f".first_constraint = {len(self.constraints)}")
                fields.append(f".constraint_count = {len(cons)}")
                for c in cons:
                    a, op, b = c.split()
                    self.constraints.append(f"{{.left = {params.index(a)}, .comparison = Comparison::"
                                            f"{COMPARISONS[op]}, .right = {params.index(b)}, "
                                            f".text = {cxx_string(c)}}}")
            if e["kind"] == "script-ext":
                fields.append(f".direction = ScriptDirection::{e['direction']}")
                fields.append(f".default_index = {e['default-index']}")
            if e["kind"] == "data-key":
                fields.append(f".file = DataFile::{e['file']}")
                fields.append(f".standard_key = {cxx_string(e['standard-key'])}")
                if e.get("overrides"):
                    fields.append(f".overrides = {cxx_string(e['overrides'])}")
                if e.get("requires"):
                    fields.append(f".required_hack = {cxx_string(e['requires'])}")
            if e.get("summary"):
                fields.append(f".summary = {cxx_string(e['summary'])}")
            if e.get("area"):
                fields.append(f".area = {cxx_string(e['area'])}")
            if e.get("title"):
                fields.append(f".title = {cxx_string(e['title'])}")
            self.entries.append("{" + ", ".join(fields) + "}")

    def text(self):
        reg = self.reg
        indices = reg["script-indices"]
        fidelity = reg.get("profile-defaults", {}).get("script-extensions", {}).get("fidelity", "exact")
        out = [GENERATED, "\n", "// The OAMOD registry as constant tables (registry.hpp describes them).\n\n"]

        def array(type_name, name, items):
            out.append(f"constexpr std::array<{type_name}, {len(items)}> {name}{{{{\n")
            for item in items:
                out.append(f"    {item},\n")
            out.append("}};\n\n")

        array("Literal", "literals", self.literals)
        array("std::string_view", "enum_values", [cxx_string(v) for v in self.enum_values])
        array("Parameter", "parameters", self.parameters)
        array("Preset", "presets", self.presets)
        array("PresetValue", "preset_values", self.preset_values)
        array("Constraint", "constraints", self.constraints)
        array("Entry", "entries", self.entries)
        array("AreaTitle", "area_titles", [f"{{.area = {cxx_string(a)}, .title = {cxx_string(n)}}}"
                                            for a, n in reg.get("area-titles", {}).items()])
        out.append(f"constexpr int32_t catalogue = {reg['catalogue']};\n")
        out.append(f"constexpr int32_t script_index_minimum = {indices['min']};\n")
        out.append(f"constexpr int32_t script_index_maximum = {indices['max']};\n")
        out.append(f"constexpr int32_t base_index_first = {indices['base-range'][0]};\n")
        out.append(f"constexpr int32_t base_index_last = {indices['base-range'][1]};\n")
        out.append(f"constexpr std::string_view default_fidelity = {cxx_string(fidelity)};\n")
        return "".join(out)


# ---------------------------------------------------------------------------------------------- records
class Records:
    """The C++ records of every hack, limit and value, and the bindings to their fields."""

    def __init__(self, reg):
        self.reg = reg
        self.entries = reg["entries"]
        self.sim = []           # text of the simulation records
        self.profile = []       # text of the other records
        self.bindings = []      # lines of visit_bindings

    def field_type(self, spec, sim, enum_name):
        t = spec["type"]
        if t == "bool":
            return "bool"
        if t in ("int", "int-or-none"):
            lo, hi = spec.get("min", INT32_MIN), spec.get("max", INT32_MAX)
            base = "int32_t" if INT32_MIN <= lo and hi <= INT32_MAX else "int64_t"
            return f"std::optional<{base}>" if t == "int-or-none" else base
        if t == "decimal":
            return "double"
        if t == "string":
            if sim:
                return f"FixedText<{4 * spec['max-length']}>"
            return "std::string"
        if t == "enum":
            return enum_name
        if t == "set<enum>":
            return f"EnumSet<{enum_name}, {len(spec['values'])}>"
        elem = {"list<int>": self.field_type(dict(spec, type="int"), sim, None),
                "list<decimal>": "double", "list<enum>": enum_name,
                "list<string>": "std::string"}[t]
        n = spec.get("length")
        if isinstance(n, int):
            return f"std::array<{elem}, {n}>"
        if t == "list<string>":
            return f"std::vector<{elem}>"
        return f"FixedList<{elem}, {n[1] if n else 64}>"

    def initialiser(self, spec, v, enum_name):
        t = spec["type"]

        def scalar(x, elem_type):
            if elem_type == "bool":
                return "true" if x else "false"
            if elem_type in ("int", "int-or-none"):
                return "std::nullopt" if x == "none" else str(x)
            if elem_type == "decimal":
                return cxx_double(x)
            if elem_type == "string":
                return literal_string(x)
            return f"{enum_name}::{snake(x)}"

        if t.startswith("list<") or t == "set<enum>":
            elem = {"list<int>": "int", "list<decimal>": "decimal", "list<string>": "string"}.get(t, "enum")
            return "{" + ", ".join(scalar(x, elem) for x in v) + "}"
        return "{" + scalar(v, t) + "}"

    def enum(self, out, name, values, doc):
        out.append(comment(doc))
        out.append(f"enum class {name} : uint8_t {{\n")
        for v in values:
            out.append(f"    {snake(v)}, ///< {v}\n")
        out.append("};\n")
        out.append(f"/// @return the number of {name} values\n")
        out.append(f"[[nodiscard]] constexpr size_t enum_size({name}) noexcept {{ return {len(values)}; }}\n\n")

    def record(self, out, name, eid, e, sim, path):
        """One record of a hack or limit: enums for its enum parameters, then the struct."""
        fields = []
        for p, spec in e["params"].items():
            enum_name = None
            if spec["type"] in ("enum", "list<enum>", "set<enum>"):
                enum_name = name + pascal(p)
                self.enum(out, enum_name, spec["values"], f"The values of {eid} {p}.")
            ftype = self.field_type(spec, sim, enum_name)
            init = self.initialiser(spec, spec["baseline"], enum_name)
            unit = f" ({spec['unit']})" if spec.get("unit") else ""
            fields.append((member(p), ftype, init, f"{p}{unit}; 3.1c: {self.show(spec['baseline'])}"))
            self.bindings.append(f'    visitor.parameter("{eid}", "{p}", {path}.{member(p)});\n')
        out.append(comment(f"{eid}: {e.get('summary', '')}"))
        out.append(f"struct {name} {{\n")
        if e["kind"] == "hack":
            out.append("    bool enabled{}; ///< the profile turns the hack on\n")
        for fname, ftype, init, doc in fields:
            out.append(comment(doc, "    "))
            out.append(f"    {ftype} {fname}{init};\n")
        out.append("    /// Compares every field.\n")
        out.append(f"    bool operator==(const {name}&) const = default;\n")
        out.append("};\n\n")

    @staticmethod
    def show(v):
        if isinstance(v, bool):
            return "true" if v else "false"
        if isinstance(v, list):
            return "[" + ", ".join(Records.show(x) for x in v) + "]"
        if isinstance(v, oamod_yaml.EscapedStr):
            return "(see the registry)"
        return str(v)

    def group(self, out, name, members, doc):
        out.append(comment(doc))
        out.append(f"struct {name} {{\n")
        for mtype, mname in members:
            out.append(f"    {mtype} {mname}{{}};\n")
        out.append("    /// Compares every field.\n")
        out.append(f"    bool operator==(const {name}&) const = default;\n")
        out.append("};\n\n")

    def build(self):
        entries = self.entries
        sim, prof = self.sim, self.profile
        # Script extensions.
        self.script_ids = [eid for eid, e in entries.items() if e["kind"] == "script-ext"]
        # Limits: held by the engine's own Limits record.
        for eid, e in entries.items():
            if e["kind"] == "limit":
                for p in e["params"]:
                    self.bindings.append(
                        f'    visitor.parameter("{eid}", "{p}", profile.limits.{LIMIT_FIELDS[eid][p]});\n')
        # Hacks, grouped by area.
        by_area = {}
        for eid, e in entries.items():
            if e["kind"] == "hack":
                by_area.setdefault(eid.split(".")[0], []).append((eid, e))
        area_members = {}
        for area in SIM_AREAS + PROFILE_AREAS:
            in_sim = area in SIM_AREAS
            out = sim if in_sim else prof
            container = "profile.rules" if in_sim else "profile"
            members = []
            for eid, e in by_area.get(area, []):
                name = pascal(eid)
                mname = member(eid.split(".", 1)[1])
                self.bindings.append(f'    visitor.hack("{eid}", {container}.{member(area)}.{mname}.enabled);\n')
                self.record(out, name, eid, e, in_sim, f"{container}.{member(area)}.{mname}")
                members.append((name, mname))
            group = pascal(area) + "Rules"
            self.group(out, group, members, f"The rules of every hack in area {area}.")
            area_members[area] = group
        sim_members = [(area_members[a], member(a)) for a in SIM_AREAS]
        sim.append(comment("The rules the simulation reads, one record per area; a default-constructed "
                           "record is 3.1c."))
        sim.append("struct MatchRules {\n")
        for mtype, mname in sim_members:
            sim.append(f"    {mtype} {mname}{{}};\n")
        sim.append("    /// The extensions unit scripts read with get, by index.\n")
        sim.append("    ScriptExtensionTable script_get{};\n")
        sim.append("    /// The extensions unit scripts write with set, by index.\n")
        sim.append("    ScriptExtensionTable script_set{};\n")
        sim.append("    /// How the extensions treat reads the original leaves undefined.\n")
        sim.append("    ScriptFidelity script_fidelity{ScriptFidelity::exact};\n")
        sim.append("    /// Compares every field.\n")
        sim.append("    bool operator==(const MatchRules&) const = default;\n")
        sim.append("};\n")
        # Identity, layout, strings and media: values under (dotted) keys.
        for kind, record in (("identity", "Identity"), ("layout", "Layout"), ("string", "Strings"),
                             ("media", "Media")):
            values = [(eid, e) for eid, e in entries.items() if e["kind"] == kind]
            groups = {}
            for eid, e in values:
                parts = e["key"].split(".")
                groups.setdefault(parts[0] if len(parts) > 1 else None, []).append((eid, e, parts[-1]))
            block = {"string": "strings"}.get(kind, kind)
            top = []
            for sub, items in groups.items():
                if sub is None:
                    continue
                name = record + pascal(sub)
                prof.append(comment(f"The {block} values under {sub}."))
                prof.append(f"struct {name} {{\n")
                for eid, e, last in items:
                    self.value_field(prof, eid, e, f"profile.{block}.{member(sub)}.{member(last)}", last)
                prof.append("    /// Compares every field.\n")
                prof.append(f"    bool operator==(const {name}&) const = default;\n")
                prof.append("};\n\n")
                top.append((name, member(sub)))
            prof.append(comment(f"The profile's {block} values; a default-constructed record holds 3.1c's."))
            prof.append(f"struct {record} {{\n")
            for eid, e, last in groups.get(None, []):
                self.value_field(prof, eid, e, f"profile.{block}.{member(last)}", last)
            for mtype, mname in top:
                prof.append(f"    {mtype} {mname}{{}};\n")
            prof.append("    /// Compares every field.\n")
            prof.append(f"    bool operator==(const {record}&) const = default;\n")
            prof.append("};\n\n")
        # Data keys.
        prof.append(comment("The key a mod's files spell each data-key meaning with; empty when the profile "
                            "binds none."))
        prof.append("struct DataKeyBindings {\n")
        for eid, e in entries.items():
            if e["kind"] == "data-key":
                prof.append(comment(f"{eid} ({e['file']} files): {e.get('summary', '')}", "    "))
                prof.append(f"    std::string {member(eid)}{{}};\n")
                self.bindings.append(f'    visitor.data_key("{eid}", profile.data_keys.{member(eid)});\n')
        prof.append("    /// Compares every field.\n")
        prof.append("    bool operator==(const DataKeyBindings&) const = default;\n")
        prof.append("};\n")

    def value_field(self, out, eid, e, path, last):
        spec = e["value"]
        enum_name = None
        ftype = self.field_type(spec, False, enum_name)
        out.append(comment(f"{eid}: {e.get('summary', '')} 3.1c: {self.show(spec['baseline'])}.", "    "))
        out.append(f"    {ftype} {member(last)}{self.initialiser(spec, spec['baseline'], enum_name)};\n")
        self.bindings.append(f'    visitor.value("{eid}", {path});\n')

    def script_text(self):
        out = [GENERATED, "\n"]
        out.append("/// A unit-script value the base game lacks, mounted at a get or set index.\n")
        out.append("enum class ScriptExtension : uint8_t {\n")
        out.append("    none, ///< nothing mounted\n")
        for sid in self.script_ids:
            out.append(f"    {snake(sid)}, ///< {sid}\n")
        out.append("};\n\n")
        out.append("/// The registry id of each extension, by its value.\n")
        out.append(f"inline constexpr std::array<std::string_view, {len(self.script_ids) + 1}> "
                   "script_extension_ids{\n")
        out.append('    "",\n')
        for sid in self.script_ids:
            out.append(f'    "{sid}",\n')
        out.append("};\n")
        return "".join(out)

    def sim_text(self):
        return GENERATED + "\n" + "".join(self.sim)

    def profile_text(self):
        return GENERATED + "\n" + "".join(self.profile)

    def bindings_text(self):
        out = [GENERATED, "\n"]
        out.append("/// Calls a visitor once for every meaning of the registry, with the field that holds it:\n")
        out.append("/// value(id, field) for identity, layout, string and media values, hack(id, enabled) for\n")
        out.append("/// each hack, parameter(id, name, field) for each parameter of a limit or hack, and\n")
        out.append("/// data_key(id, field) for each data-key meaning.\n")
        out.append("///\n")
        out.append("/// @param[in,out] visitor what is done with each field\n")
        out.append("/// @param[in,out] profile the profile whose fields are visited; const to only read them\n")
        out.append("template <class Visitor, class Profile> void visit_bindings(Visitor& visitor, Profile& profile) {\n")
        out.extend(self.bindings)
        out.append("}\n")
        return "".join(out)


def generate():
    reg = oamod_yaml.load_file(REGISTRY)
    problems = check_registry(reg)
    if problems:
        raise RegistryError("\n".join(problems))
    table = Table(reg)
    table.build()
    records = Records(reg)
    records.build()
    return {
        TABLE: table.text(),
        SCRIPT_EXTENSIONS: records.script_text(),
        SIM_RECORDS: records.sim_text(),
        PROFILE_RECORDS: records.profile_text(),
        BINDINGS: records.bindings_text(),
    }


def self_test():
    """Checks the reader's rules, the registry check and the escaping on small cases."""
    failures = []

    def refused(text, rule):
        try:
            oamod_yaml.loads(text)
        except oamod_yaml.OamodYamlError as e:
            if e.rule != rule:
                failures.append(f"{text!r}: refused as {e.rule}, expected {rule}")
            return
        failures.append(f"{text!r}: accepted, expected {rule}")

    refused("a: &x 1\nb: *x\n", "anchor")
    refused("a: *x\n", "alias")
    refused("a: !!str 1\n", "tag")
    refused("a: 1\na: 2\n", "duplicate_key")
    refused('{"a": 1, "a": 2}\n', "duplicate_key")
    refused("a: 1\n---\nb: 2\n", "several_documents")
    refused("%YAML 1.2\n---\na: 1\n", "directive")
    refused("<<: {a: 1}\n", "merge_key")
    refused("a: |\n  x\n", "block_scalar")
    refused("a:\n\tb: 1\n", "tab_indentation")
    refused("true: 1\n", "key_not_string")
    refused("a: 9007199254740993\n", "integer_too_large")
    refused("a: 0.1234567890123456\n", "too_many_digits")
    refused("- a\n", "not_mapping")
    try:
        oamod_yaml.loads(b"\xef\xbb\xbfa: 1\n")
        failures.append("a byte order mark was accepted")
    except oamod_yaml.OamodYamlError as e:
        if e.rule != "byte_order_mark":
            failures.append(f"a byte order mark was refused as {e.rule}")
    strings = oamod_yaml.loads("a: off\nb: yes\nc: 0x10\nd: 1_000\ne: 2024-12-01\n")
    if strings != {"a": "off", "b": "yes", "c": "0x10", "d": "1_000", "e": "2024-12-01"}:
        failures.append(f"YAML 1.1 forms are not strings: {strings}")
    scalars = oamod_yaml.loads("a: 1.50\nb: -3\nc: true\nd: null\n")
    if scalars != {"a": 1.5, "b": -3, "c": True, "d": None} or not isinstance(scalars["b"], int):
        failures.append(f"scalars read wrong: {scalars}")
    keys = oamod_yaml.loads("get: {71: a, '71': b}\n")["get"]
    if keys != {71: "a", "71": "b"}:
        failures.append(f"an integer key and a quoted key are not apart: {keys}")
    escaped = oamod_yaml.loads('a: "\\x41b"\nb: plain\n')
    if not isinstance(escaped["a"], oamod_yaml.EscapedStr) or isinstance(escaped["b"], oamod_yaml.EscapedStr):
        failures.append("escaped strings are not told apart")
    if literal_string(escaped["a"]) != '"\\101\\142"' or cxx_string('a"b') != '"a\\"b"' \
            or cxx_string("\u00d710") != '"\\303\\227" "10"':
        failures.append(f"strings are escaped wrong: {literal_string(escaped['a'])}")
    if number_parts(0.7) != (False, 7, -1, False) or number_parts(1500) != (False, 15, 2, True):
        failures.append("numbers are split wrong")

    good = {"kind": "hack", "area": "units", "title": "Example", "scope": "sim", "implemented": False, "summary": "s",
            "params": {"ticks": {"type": "int", "min": 0, "max": 10, "baseline": 0, "default": 5,
                                 "adjustable": "fixed", "scope": "sim"}},
            "presets": {"baseline": {"ticks": 0}}}
    broken = [
        ({"presets": {"baseline": {}}}, "not a complete parameter set"),
        ({"params": {"ticks": dict(good["params"]["ticks"], default=50)}}, "outside"),
        ({"implemented": None}, "says whether it is implemented"),
        ({"shorthand": "nothing"}, "shorthand"),
        ({"constraints": ["ticks < nothing"]}, "constraint"),
        ({"area": "teams"}, "first word of its id"),
        ({"title": None}, "a hack has a title"),
        ({"title": "x" * (TITLE_LENGTH + 1)}, "over"),
    ]
    named = {"units": "Units"}
    for change, needle in broken:
        entry = dict(good, **change)
        problems = check_registry({"entries": {"units.example": entry}, "setting-transforms": {},
                                   "area-titles": named})
        if not any(needle in p for p in problems):
            failures.append(f"the registry check misses {needle!r}: {problems}")
    for reg, needle in (
            ({"entries": {"units.example": good, "units.other": good}, "area-titles": named}, "also"),
            ({"entries": {"units.example": good}, "area-titles": {}}, "no name in area-titles"),
            ({"entries": {"units.example": good}, "area-titles": dict(named, ui="Interface")}, "area of no hack"),
            ({"entries": {"units.example": good, "units.key": {"kind": "data-key", "scope": "sim", "title": "Key",
                                                               "file": "unit"}},
              "area-titles": named}, "only hacks have a title")):
        problems = check_registry(dict(reg, **{"setting-transforms": {}}))
        if not any(needle in p for p in problems):
            failures.append(f"the registry check misses {needle!r}: {problems}")
    if check_registry({"entries": {"units.example": good}, "setting-transforms": {}, "area-titles": named}):
        failures.append("the registry check refuses a good entry")
    known = {"order.attack"}
    listed = dict(good, actions=["order.attack"])
    if check_registry({"entries": {"units.example": listed}, "setting-transforms": {},
                       "area-titles": named}, known):
        failures.append("the registry check refuses a hack action that is listed")
    for change, needle in (
            ({"actions": ["no.such"]}, "no.such"),
            ({"actions": ["order.attack", "order.attack"]}, "listed twice"),
            ({"actions": []}, "a list of action ids"),
            ({"actions": [1]}, "not text"),
    ):
        problems = check_registry(
            {"entries": {"units.example": dict(good, **change)}, "setting-transforms": {},
             "area-titles": named},
            known,
        )
        if not any(needle in problem for problem in problems):
            failures.append(f"the registry check misses {needle!r}: {problems}")
    keyed = {"kind": "data-key", "scope": "sim", "file": "unit", "actions": ["order.attack"]}
    problems = check_registry(
        {"entries": {"units.key": keyed}, "setting-transforms": {}, "area-titles": named}, known
    )
    if not any("only a hack lists actions" in problem for problem in problems):
        failures.append(f"the registry check misses an actions list on a data key: {problems}")
    live = dict(good, actions=["megamap.toggle"])
    if check_registry({"entries": {"units.example": live}, "setting-transforms": {},
                       "area-titles": named}):
        failures.append("gen_input_actions.py --list did not accept megamap.toggle")
    marked = dict(good, mutator=True)
    marked["params"] = {"ticks": dict(good["params"]["ticks"], adjustable="match")}
    if check_registry({"entries": {"units.example": marked}, "setting-transforms": {}, "area-titles": named}):
        failures.append("the registry check refuses a marked entry")
    quiet = dict(good, mutator=False)
    if check_registry({"entries": {"units.example": quiet}, "setting-transforms": {}, "area-titles": named}):
        failures.append("the registry check refuses mutator: false")
    visual = dict(marked, visual=True)
    viewed = dict(marked, scope="view")
    view_param = dict(marked)
    view_param["params"] = {"ticks": dict(marked["params"]["ticks"], scope="view")}
    ui = dict(marked, area="ui")
    identity = {"kind": "identity", "scope": "sim", "key": "example", "mutator": True,
                "value": {"type": "bool", "baseline": True, "scope": "sim"}}
    for entry_id, entry, titles, needle in (
            ("units.example", dict(good, mutator="yes"), named, "mutator must be true or false"),
            ("units.example", dict(good, mutator=True), named, "needs a parameter adjustable per match"),
            ("units.example", visual, named, "touches the interface (visual)"),
            ("units.example", viewed, named, "touches the interface (scope view)"),
            ("ui.example", ui, {"ui": "Interface"}, "touches the interface (ui.)"),
            ("units.example", view_param, named, "touches the interface (a match parameter of scope view)"),
            ("identity.example", identity, named, "only for a hack or a limit"),
    ):
        problems = check_registry({"entries": {entry_id: entry}, "setting-transforms": {}, "area-titles": titles})
        if not any(needle in problem and entry_id in problem for problem in problems):
            failures.append(f"the registry check misses {needle!r} for {entry_id}: {problems}")
    for f in failures:
        print("FAIL", f)
    print(f"gen_mod_registry self-test: {len(failures)} failures")
    return 1 if failures else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="write nothing; exit 1 when a file differs")
    parser.add_argument("--self-test", action="store_true", help="check the tool itself on small cases")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    try:
        files = generate()
    except (RegistryError, oamod_yaml.OamodYamlError) as e:
        print(f"gen_mod_registry: {e}", file=sys.stderr)
        return 2
    differ = []
    for path, text in files.items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current != text:
            differ.append(path)
            if not args.check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding="utf-8")
    rel = [str(p.relative_to(ROOT)) for p in differ]
    if args.check:
        if differ:
            print("gen_mod_registry: these files differ from the registry; run tools/gen_mod_registry.py:",
                  file=sys.stderr)
            for r in rel:
                print(f"  {r}", file=sys.stderr)
            return 1
        print("gen_mod_registry: every generated file matches the registry")
        return 0
    for r in rel:
        print(f"wrote {r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
