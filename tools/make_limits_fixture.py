#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Make the weapon and side limits fixture from an installed game.

The fixture is two mod folders, written the same way on every run, and
nothing in them comes from anywhere but the installed game and the fixed
text below. ``ridge-wide`` raises the weapon-type limit to 1024 and the
side limit to 10. It adds three clones of the base weapon EMG
(``RIDGE_W300`` at id 300, ``RIDGE_W1000`` at id 1000 and ``RIDGE_W2000``
at id 2000), a clone of the base combat unit ARMPW whose first weapon is
``RIDGE_W300``, and seven sides: the game's first two, then Ridge2 to
Ridge6. ``ridge-sides`` raises only the side limit, to 7, and carries the
same sides and commanders without the weapon clones.

The base files, read with ``oa-tool asset-extract`` in the game's mount
order, are ``gamedata/sidedata.tdf``, ``weapons/WEAPONS.TDF``,
``units/ARMPW.FBI``, ``scripts/ARMPW.COB``, ``units/ARMCOM.FBI``,
``scripts/ARMCOM.COB``, each ``guis/ARMCOM<n>.GUI`` with its
``anims/ARMCOM<n>.GAF`` until a page is missing, and ``guis/ARMGEN.GUI``,
``guis/ARMMAIN2.GUI`` and ``guis/ARMDL.GUI``.

``--game-dir DIR --oa-tool PATH --out DIR`` writes ``ridge-wide`` and
``ridge-sides`` under DIR. ``--verify DIR`` writes them to a temporary
folder and compares them with DIR. ``--self-test`` checks the text edits
on text built into this file, with no game data.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# The three weapon clones, in file order: section name, id, damage default.
WEAPONS = (
    ("RIDGE_W300", 300, 37),
    ("RIDGE_W1000", 1000, 41),
    ("RIDGE_W2000", 2000, 43),
)
# Base weapon ids 300 and 1000 fold onto, and the damages that must not
# already belong to a weapon at one of those ids.
FOLDED_IDS = (44, 232)
FOLDED_DAMAGES = (37, 41)
SIDES = (2, 3, 4, 5, 6)
FOLDERS = ("ridge-wide", "ridge-sides")
COMMENT = "// make_limits_fixture"


class FixtureFailure(Exception):
    """The fixture cannot be made, or two runs differ."""


def line_ending(text):
    """The line end ``text`` uses, carriage return and line feed when it has both."""
    if "\r\n" in text:
        return "\r\n"
    if "\n" in text:
        return "\n"
    return "\n"


def header_count(text, name):
    """How many ``[name]`` headers ``text`` has."""
    return len(re.findall(rf"\[\s*{re.escape(name)}\s*\]", text, flags=re.I))


def require_one_header(text, name, where):
    """Fails unless ``text`` has exactly one ``[name]`` header."""
    count = header_count(text, name)
    if count != 1:
        raise FixtureFailure(f"{where} has [{name}] {count} times")


def section_bounds(text, header_at, where):
    """The start and end of the section whose header begins at ``header_at``."""
    header_end = text.find("]", header_at)
    if header_end < 0:
        raise FixtureFailure(f"{where} has a header that is not closed")
    try:
        brace = text.index("{", header_end)
    except ValueError as error:
        raise FixtureFailure(f"{where} has a section with no opening brace") from error
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return header_at, index + 1
    raise FixtureFailure(f"{where} has a section that is not closed")


def section(text, name, where):
    """The start and end of the first ``[name]`` section."""
    header = re.search(rf"\[\s*{re.escape(name)}\s*\]", text, flags=re.I)
    if header is None:
        raise FixtureFailure(f"{where} has no [{name}]")
    return section_bounds(text, header.start(), where)


def slice_section(text, name, where):
    """The text of the first ``[name]`` section, from its header through its brace."""
    start, end = section(text, name, where)
    return text[start:end]


def key_values(text, key):
    """The values of ``key`` written at the start of a line, semicolon not included.

    ``name`` does not match ``nameprefix``: the key must end where the value
    begins.
    """
    pattern = rf"(?im)^[ \t]*{re.escape(key)}[ \t]*=[ \t]*([^;\r\n]*)"
    return [match.group(1).strip() for match in re.finditer(pattern, text)]


def set_line_key(text, key, value, where):
    """``text`` with the one line-start ``key`` set to ``value``; every other byte kept."""
    pattern = rf"(?im)^([ \t]*{re.escape(key)}[ \t]*=[ \t]*)[^;\r\n]*"
    found = list(re.finditer(pattern, text))
    if len(found) != 1:
        raise FixtureFailure(f"{where} has {key} {len(found)} times")
    match = found[0]
    return text[:match.start()] + match.group(1) + str(value) + text[match.end():]


def rename_header(text, name):
    """``text`` with the section header it starts with renamed to ``[name]``."""
    match = re.match(r"\[\s*[^\]]*\]", text)
    if match is None:
        raise FixtureFailure(f"a section does not start with a [{name}] header")
    return f"[{name}]" + text[match.end():]


def skip_line_break(text, index):
    """``index`` moved past the line break that follows it, when one does."""
    if text.startswith("\r\n", index):
        return index + 2
    if text.startswith("\n", index) or text.startswith("\r", index):
        return index + 1
    return index


def insert_after(text, name, blocks, where):
    """``text`` with ``blocks`` inserted after the ``[name]`` section's line."""
    _start, end = section(text, name, where)
    at = skip_line_break(text, end)
    newline = line_ending(text)
    body = newline.join(blocks) + newline
    return text[:at] + body + text[at:]


def side_section(side0, number):
    """SIDE0's section as ``[SIDEn]``, with Ridge's name, prefix and commander."""
    text = rename_header(side0, f"SIDE{number}")
    where = f"gamedata/sidedata.tdf [SIDE{number}]"
    text = set_line_key(text, "name", f"Ridge{number}", where)
    text = set_line_key(text, "nameprefix", f"RD{number}", where)
    text = set_line_key(text, "commander", f"RD{number}COM", where)
    return text


def extend_sidedata(text):
    """``text`` with Ridge2 to Ridge6 and their commanders' build lists."""
    where = "gamedata/sidedata.tdf"
    if header_count(text, "SIDE2") != 0:
        raise FixtureFailure(f"{where} already has [SIDE2]")
    require_one_header(text, "SIDE0", where)
    require_one_header(text, "SIDE1", where)
    require_one_header(text, "CANBUILD", where)
    require_one_header(text, "ARMCOM", where)
    side0 = slice_section(text, "SIDE0", where)
    armcom = slice_section(text, "ARMCOM", where)
    sides = [side_section(side0, number) for number in SIDES]
    text = insert_after(text, "SIDE1", sides, where)
    commanders = [rename_header(armcom, f"RD{number}COM") for number in SIDES]
    return insert_after(text, "ARMCOM", commanders, where)


def set_damage(text, damage, where):
    """``text`` with its one ``[DAMAGE]`` ``default`` set to ``damage``."""
    require_one_header(text, "DAMAGE", where)
    start, end = section(text, "DAMAGE", where)
    updated = set_line_key(text[start:end], "default", str(damage), f"{where} [DAMAGE]")
    return text[:start] + updated + text[end:]


def emg_section(text):
    """The ``[EMG]`` section of a weapon file, which must be id 16."""
    where = "weapons/WEAPONS.TDF"
    require_one_header(text, "EMG", where)
    body = slice_section(text, "EMG", where)
    ident = key_values(body, "ID")
    if ident != ["16"]:
        shown = ident[0] if len(ident) == 1 else ident
        raise FixtureFailure(f"{where} [EMG] ID is {shown}, not 16")
    return body


def clone_weapon(section_text, name, weapon_id, damage):
    """One weapon clone: ``section_text`` under ``name``, with its id and damage."""
    where = f"weapons/ridgewide.tdf [{name}]"
    text = rename_header(section_text, name)
    text = set_line_key(text, "ID", str(weapon_id), where)
    return set_damage(text, damage, where)


def render_weapons(section_text, newline):
    """The weapon file: one comment naming this tool, then the three clones."""
    parts = [COMMENT]
    for name, weapon_id, damage in WEAPONS:
        parts.append(clone_weapon(section_text, name, weapon_id, damage))
    return newline.join(parts) + newline


def weapon_text(weapons_tdf):
    """The ridge weapon file made from a ``WEAPONS.TDF`` text."""
    return render_weapons(emg_section(weapons_tdf), line_ending(weapons_tdf))


def edit_combat_unit(text):
    """ARMPW's unit file as RIDGEW300, with its first weapon renamed."""
    where = "units/RIDGEW300.FBI"
    text = set_line_key(text, "UnitName", "RIDGEW300", where)
    text = set_line_key(text, "Name", "Ridge 300", where)
    return set_line_key(text, "Weapon1", "RIDGE_W300", where)


def edit_commander(text, number):
    """ARMCOM's unit file as Ridge ``number``'s commander."""
    where = f"units/RD{number}COM.FBI"
    text = set_line_key(text, "UnitName", f"RD{number}COM", where)
    text = set_line_key(text, "Name", f"Ridge {number} Commander", where)
    return set_line_key(text, "Side", f"Ridge{number}", where)


def top_level_sections(text, where):
    """The ``(name, text)`` of every section ``text`` holds at brace depth zero."""
    sections = []
    depth = 0
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if char == "{":
            depth += 1
            index += 1
            continue
        if char == "}":
            depth = max(0, depth - 1)
            index += 1
            continue
        if depth == 0 and char == "[":
            start, end = section_bounds(text, index, where)
            name = text[start + 1:text.find("]", start)].strip()
            sections.append((name, text[start:end]))
            index = end
            continue
        index += 1
    return sections


def check_damage_clashes(files):
    """Fail when a base weapon at id 44 or 232 already uses damage 37 or 41.

    ``files`` is ``(entry, text)`` pairs. A hit by weapon 300 or 1000 is told
    apart from the weapon those ids fold onto only when the damages differ.
    """
    for entry, text in files:
        for name, body in top_level_sections(text, entry):
            ident = key_values(body, "ID")
            if len(ident) != 1 or not re.fullmatch(r"\d+", ident[0]):
                continue
            weapon_id = int(ident[0])
            if weapon_id not in FOLDED_IDS:
                continue
            if header_count(body, "DAMAGE") != 1:
                continue
            damage = key_values(slice_section(body, "DAMAGE", entry), "default")
            if len(damage) == 1 and damage[0] in {str(item) for item in FOLDED_DAMAGES}:
                raise FixtureFailure(
                    f"{entry} [{name}] ID {weapon_id} has default {damage[0]}, "
                    f"which clashes with a ridge weapon"
                )


def self_test():
    """Check the text edits on small texts that are not taken from a game."""
    newline = "\r\n"
    sidedata = (
        "[SIDE0]\r\n"
        "{\r\n"
        "name=Alpha;\r\n"
        "nameprefix=AL;\r\n"
        "commander=ALCOM;\r\n"
        "[MARK]\r\n"
        "{\r\n"
        "x=1;\r\n"
        "}\r\n"
        "}\r\n"
        "[SIDE1]\r\n"
        "{\r\n"
        "name=Beta;\r\n"
        "nameprefix=BE;\r\n"
        "commander=BECOM;\r\n"
        "}\r\n"
        "[CANBUILD]\r\n"
        "{\r\n"
        "[ARMCOM]\r\n"
        "{\r\n"
        "canbuild1=ONE;\r\n"
        "canbuild2=TWO;\r\n"
        "}\r\n"
        "[OTHER]\r\n"
        "{\r\n"
        "canbuild1=THREE;\r\n"
        "}\r\n"
        "}\r\n"
    )
    _start, end = section(sidedata, "SIDE1", "self-test")
    at = skip_line_break(sidedata, end)
    edited = extend_sidedata(sidedata)
    if edited[:at] != sidedata[:at]:
        raise FixtureFailure("self-test: bytes before the first insertion changed")
    if "\n" in edited.replace("\r\n", ""):
        raise FixtureFailure("self-test: a line end is no longer carriage return and line feed")
    headers = re.findall(r"(?im)^\[SIDE\d\]", edited)
    if headers != [f"[SIDE{index}]" for index in range(7)]:
        raise FixtureFailure(f"self-test: side headers are {headers}")
    for number in SIDES:
        body = slice_section(edited, f"SIDE{number}", "self-test")
        where = f"[SIDE{number}]"
        if key_values(body, "name") != [f"Ridge{number}"]:
            raise FixtureFailure(f"self-test: {where} name is {key_values(body, 'name')}")
        if key_values(body, "nameprefix") != [f"RD{number}"]:
            raise FixtureFailure(f"self-test: {where} prefix is {key_values(body, 'nameprefix')}")
        if key_values(body, "commander") != [f"RD{number}COM"]:
            raise FixtureFailure(f"self-test: {where} commander is {key_values(body, 'commander')}")
    if key_values(slice_section(edited, "SIDE0", "self-test"), "name") != ["Alpha"]:
        raise FixtureFailure("self-test: SIDE0's name changed")
    canbuild = slice_section(edited, "CANBUILD", "self-test")
    names = re.findall(r"(?im)^\[([^\]]+)\]", canbuild)
    expected = ["CANBUILD", "ARMCOM", *[f"RD{number}COM" for number in SIDES], "OTHER"]
    if names != expected:
        raise FixtureFailure(f"self-test: [CANBUILD] blocks are {names}")
    weapon_file = (
        "[EMG]\r\n"
        "{\r\n"
        "ID=16;\r\n"
        "default=5;\r\n"
        "[DAMAGE]\r\n"
        "{\r\n"
        "default=2;\r\n"
        "}\r\n"
        "}\r\n"
        "[ZZZ]\r\n"
        "{\r\n"
        "ID=99;\r\n"
        "}\r\n"
    )
    rendered = weapon_text(weapon_file)
    if "\n" in rendered.replace("\r\n", ""):
        raise FixtureFailure("self-test: the weapon file lost its line ends")
    if not rendered.startswith(COMMENT + newline):
        raise FixtureFailure("self-test: the weapon file does not start with the tool's comment")
    idents = re.findall(r"ID=(\d+)", rendered)
    if idents != ["300", "1000", "2000"]:
        raise FixtureFailure(f"self-test: weapon ids are {idents}")
    damages = re.findall(r"(?im)^[ \t]*default=(\d+);", rendered)
    if damages != ["5", "37", "5", "41", "5", "43"]:
        raise FixtureFailure(f"self-test: weapon damages are {damages}")
    unit = (
        "[UNITINFO]\r\n"
        "{\r\n"
        "UnitName=BASE;\r\n"
        "Name=Base Unit;\r\n"
        "GermanName=Basis;\r\n"
        "Weapon1=BASEGUN;\r\n"
        "Objectname=BASE;\r\n"
        "Side=Alpha;\r\n"
        "}\r\n"
    )
    combat = edit_combat_unit(unit)
    if combat.count("UnitName=RIDGEW300;") != 1 or combat.count("Name=Ridge 300;") != 1:
        raise FixtureFailure("self-test: the combat unit keys were not replaced once")
    if combat.count("Weapon1=RIDGE_W300;") != 1 or "GermanName=Basis;" not in combat:
        raise FixtureFailure("self-test: the combat unit changed a key it should keep")
    if "Objectname=BASE;" not in combat or "Side=Alpha;" not in combat:
        raise FixtureFailure("self-test: the combat unit changed a key it should keep")
    commander = edit_commander(unit, 2)
    if commander.count("UnitName=RD2COM;") != 1 or commander.count("Name=Ridge 2 Commander;") != 1:
        raise FixtureFailure("self-test: the commander keys were not replaced once")
    if commander.count("Side=Ridge2;") != 1 or "GermanName=Basis;" not in commander:
        raise FixtureFailure("self-test: the commander changed a key it should keep")
    if "Objectname=BASE;" not in commander or "Weapon1=BASEGUN;" not in commander:
        raise FixtureFailure("self-test: the commander changed a key it should keep")
    try:
        set_line_key(unit, "Missing", "NOPE", "self-test unit")
    except FixtureFailure:
        pass
    else:
        raise FixtureFailure("self-test: a text without a key was accepted")
    clean = "[GUN]\r\n{\r\nID=44;\r\n[DAMAGE]\r\n{\r\ndefault=3;\r\n}\r\n}\r\n"
    clash = "[GUN]\r\n{\r\nID=232;\r\n[DAMAGE]\r\n{\r\ndefault=41;\r\n}\r\n}\r\n"
    check_damage_clashes([("weapons/sample.tdf", clean)])
    try:
        check_damage_clashes([("weapons/sample.tdf", clash)])
    except FixtureFailure as error:
        if "232" not in str(error) or "41" not in str(error):
            raise FixtureFailure(f"self-test: the clash was not named: {error}") from error
    else:
        raise FixtureFailure("self-test: a clashing damage was accepted")
    print("limits fixture self-test: passed")


def mount_order(game_dir):
    """The installation's archives, the first to mount first."""
    files = sorted((path for path in game_dir.iterdir() if path.is_file()),
                   key=lambda path: path.name.lower())
    groups = ([path for path in files if path.name.lower() == "rev31.gp3"],
              [path for path in files if path.suffix.lower() == ".ccx"],
              [path for path in files if path.suffix.lower() == ".ufo"],
              [path for path in files if path.suffix.lower() == ".hpi"])
    return [path for group in groups for path in group]


def run_tool(oa_tool, arguments):
    """Runs ``oa-tool`` with ``arguments`` and returns the completed process."""
    try:
        return subprocess.run([*RUNNER, str(oa_tool), *arguments],
                              check=False, capture_output=True, text=True,
                              errors="replace", timeout=60)
    except (OSError, subprocess.SubprocessError) as error:
        raise FixtureFailure(f"cannot run {oa_tool}: {error}") from error


def extract(oa_tool, game_dir, archives, entry, output, missing_ok=False):
    """The bytes of ``entry``, read in the game's mount order.

    Returns None when ``missing_ok`` and the entry is not in the game.
    """
    result = run_tool(oa_tool, ["asset-extract", str(game_dir), entry, str(output),
                                *map(str, archives)])
    if result.returncode != 0:
        message = (result.stdout + result.stderr).strip()
        if missing_ok and "asset not found" in message:
            return None
        raise FixtureFailure(f"cannot read {entry}: {message}")
    return output.read_bytes()


def weapon_entries(oa_tool, game_dir, archives):
    """Every ``weapons/*.tdf`` entry, loose files first, in a stable order."""
    names = {}
    weapons = game_dir / "weapons"
    if weapons.is_dir():
        for path in weapons.iterdir():
            if path.is_file() and path.suffix.lower() == ".tdf":
                names[path.name.lower()] = f"weapons/{path.name}"
    for archive in archives:
        result = run_tool(oa_tool, ["list", str(archive)])
        if result.returncode != 0:
            message = (result.stdout + result.stderr).strip()
            raise FixtureFailure(f"cannot list {archive.name}: {message}")
        for line in result.stdout.splitlines():
            entry = line.split("\t", 1)[0].replace("\\", "/")
            folded = entry.lower()
            if folded.startswith("weapons/") and folded.endswith(".tdf") and folded.count("/") == 1:
                names.setdefault(folded, entry)
    return [names[key] for key in sorted(names)]


def read_base(oa_tool, game_dir, archives, scratch):
    """The installed files the fixture is made from."""
    def take(entry):
        return extract(oa_tool, game_dir, archives, entry, scratch / "entry")

    sidedata = take("gamedata/sidedata.tdf").decode("latin-1")
    weapons = take("weapons/WEAPONS.TDF").decode("latin-1")
    armpw = take("units/ARMPW.FBI").decode("latin-1")
    armcom = take("units/ARMCOM.FBI").decode("latin-1")
    pewee_script = take("scripts/ARMPW.COB")
    commander_script = take("scripts/ARMCOM.COB")
    pages = []
    number = 1
    while True:
        gui_entry = f"guis/ARMCOM{number}.GUI"
        gui = extract(oa_tool, game_dir, archives, gui_entry, scratch / "entry",
                      missing_ok=number > 1)
        if gui is None:
            break
        gaf_entry = f"anims/ARMCOM{number}.GAF"
        gaf = extract(oa_tool, game_dir, archives, gaf_entry, scratch / "entry")
        pages.append((gui, gaf))
        number += 1
    if not pages:
        raise FixtureFailure("cannot read guis/ARMCOM1.GUI: asset not found")
    shared = {
        "GEN": take("guis/ARMGEN.GUI"),
        "MAIN2": take("guis/ARMMAIN2.GUI"),
        "DL": take("guis/ARMDL.GUI"),
    }
    weapon_files = []
    for entry in weapon_entries(oa_tool, game_dir, archives):
        weapon_files.append((entry, take(entry).decode("latin-1")))
    check_damage_clashes(weapon_files)
    return {
        "sidedata": extend_sidedata(sidedata).encode("latin-1"),
        "weapons": weapon_text(weapons).encode("latin-1"),
        "combat": edit_combat_unit(armpw).encode("latin-1"),
        "pewee_script": pewee_script,
        "commanders": [(number, edit_commander(armcom, number).encode("latin-1"))
                       for number in SIDES],
        "commander_script": commander_script,
        "pages": pages,
        "shared": shared,
    }


def oamod_text(mod_id, name, limits):
    """One profile, UTF-8, with line feeds and no value that changes between runs."""
    return (
        "oamod: 1\n"
        f"id: {mod_id}\n"
        f"name: {name}\n"
        "version: \"1\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: Open Annihilation}\n"
        "packaging: {revision: 1, date: 2026-10-25, packager: Open Annihilation}\n"
        f"limits: {limits}\n"
    ).encode("utf-8")


def commander_files(base):
    """The commander unit files, scripts, build pages and order pages, for sides 2 to 6."""
    files = []
    for number, unit in base["commanders"]:
        prefix = f"RD{number}"
        files.append((f"units/{prefix}COM.FBI", unit))
        files.append((f"scripts/{prefix}COM.COB", base["commander_script"]))
        for page, (gui, gaf) in enumerate(base["pages"], start=1):
            files.append((f"guis/{prefix}COM{page}.GUI", gui))
            files.append((f"anims/{prefix}COM{page}.GAF", gaf))
        files.append((f"guis/{prefix}GEN.GUI", base["shared"]["GEN"]))
        files.append((f"guis/{prefix}MAIN2.GUI", base["shared"]["MAIN2"]))
        files.append((f"guis/{prefix}DL.GUI", base["shared"]["DL"]))
    return files


def folder_files(base):
    """``(folder, files)`` for both mods, each file a ``(relative, bytes)`` pair."""
    commanders = commander_files(base)
    wide = [
        ("oamod.yaml", oamod_text("ridge-wide", "Ridge Wide",
                                  "{weapon-types: 1024, sides: 10}")),
        ("gamedata/SIDEDATA.TDF", base["sidedata"]),
        ("weapons/ridgewide.tdf", base["weapons"]),
        ("units/RIDGEW300.FBI", base["combat"]),
        ("scripts/RIDGEW300.COB", base["pewee_script"]),
        *commanders,
    ]
    sides = [
        ("oamod.yaml", oamod_text("ridge-sides", "Ridge Sides", "{sides: 7}")),
        ("gamedata/SIDEDATA.TDF", base["sidedata"]),
        *commanders,
    ]
    return (("ridge-wide", wide), ("ridge-sides", sides))


def write_files(root, files):
    """Writes ``files`` under ``root`` in the order given."""
    for relative, data in files:
        path = root.joinpath(*relative.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def remove_fixture(out):
    """Deletes ``ridge-wide`` and ``ridge-sides`` under ``out``, and nothing else."""
    for name in FOLDERS:
        path = out / name
        if path.is_symlink():
            path.unlink()
        elif path.exists():
            shutil.rmtree(path)


def write_fixture(out, folders, announce=True):
    """Writes both folders under ``out`` and, unless quiet, prints how many files each holds."""
    out.mkdir(parents=True, exist_ok=True)
    remove_fixture(out)
    counts = []
    for name, files in folders:
        write_files(out / name, files)
        counts.append(f"{name}: {len(files)} files")
    if announce:
        print(", ".join(counts))


def files_under(folder):
    """The sorted relative paths under ``folder`` and the bytes of each file."""
    if not folder.is_dir():
        raise FixtureFailure(f"{folder.name} is missing")
    found = {}
    for path in folder.rglob("*"):
        if path.is_file() and not path.is_symlink():
            found[path.relative_to(folder).as_posix()] = path.read_bytes()
    return found


def compare_trees(generated, previous):
    """Fails with the first file that differs between two fixture trees."""
    total = 0
    for name in FOLDERS:
        left = files_under(generated / name)
        right = files_under(previous / name)
        left_names = sorted(left)
        right_names = sorted(right)
        for one, other in zip(left_names, right_names):
            if one != other:
                raise FixtureFailure(f"{name} file list differs at {one} and {other}")
            if left[one] != right[other]:
                raise FixtureFailure(f"{name}/{one} differs")
        if len(left_names) != len(right_names):
            longer = left_names if len(left_names) > len(right_names) else right_names
            extra = longer[min(len(left_names), len(right_names))]
            raise FixtureFailure(f"{name} file list differs at {extra}")
        total += len(left_names)
    print(f"limits fixture: {total} files, the same on a second run")


def separate_from_game(out, game_dir):
    """Fails when ``out`` is the game directory or lies inside it."""
    out_path = out.resolve()
    game_path = game_dir.resolve()
    if out_path == game_path or game_path in out_path.parents:
        raise FixtureFailure("--out must not be the game directory or inside it")


def build_fixture(game_dir, oa_tool):
    """Reads the game and returns both folders' files."""
    if not game_dir.is_dir():
        raise FixtureFailure(f"game directory {game_dir} is missing")
    if not Path(oa_tool).is_file():
        raise FixtureFailure(f"oa-tool {oa_tool} is missing")
    archives = mount_order(game_dir)
    with tempfile.TemporaryDirectory(prefix="T02-limits-fixture-") as scratch:
        base = read_base(oa_tool, game_dir, archives, Path(scratch))
    return folder_files(base)


def parse_args(argv):
    """The tool's options. ``--self-test`` stands alone; writing needs the game."""
    parser = argparse.ArgumentParser(description="Make the weapon and side limits fixture.")
    parser.add_argument("--self-test", action="store_true",
                        help="check the text edits without game data")
    parser.add_argument("--game-dir", type=Path, help="the installed game")
    parser.add_argument("--oa-tool", type=Path, help="the oa-tool executable")
    parser.add_argument("--out", type=Path, help="the folder that receives the fixture")
    parser.add_argument("--verify", type=Path,
                        help="compare a fresh fixture with the one already under DIR")
    args = parser.parse_args(argv)
    if args.self_test:
        if args.game_dir or args.oa_tool or args.out or args.verify:
            parser.error("--self-test takes no other options")
        return args
    if args.out and args.verify:
        parser.error("pass either --out or --verify")
    if not args.game_dir or not args.oa_tool or (args.out is None and args.verify is None):
        parser.error("--game-dir, --oa-tool and either --out or --verify are required")
    return args


def main(argv=None):
    """Writes the fixture, compares one, or runs the self-test."""
    try:
        args = parse_args(argv)
        if args.self_test:
            self_test()
            return 0
        folders = build_fixture(args.game_dir, args.oa_tool)
        if args.verify:
            with tempfile.TemporaryDirectory(prefix="T02-limits-fixture-") as scratch:
                root = Path(scratch)
                write_fixture(root, folders, announce=False)
                compare_trees(root, args.verify)
            return 0
        separate_from_game(args.out, args.game_dir)
        write_fixture(args.out, folders)
        return 0
    except FixtureFailure as error:
        print(f"make_limits_fixture: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
