#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Fetch FreeType and the text fonts, pinned, and build them into local/deps.

The engine draws text the game's own fonts cannot draw with FreeType, from
fonts that travel with the game (src/platform/text-font). This script
downloads FreeType 2.14.3 and the fonts from their projects' own releases,
checks every download against its SHA-256 and every licence text against
the one licenses/ holds, and builds:

  local/deps/freetype-install-2.14.3
      FreeType, a static library for this machine, without zlib, bzip2,
      PNG, HarfBuzz or Brotli and with only the modules the fonts need
      (tools/text-fonts/freetype_build_options.cmake); the folder is named
      after the release, so that a project building on the engine that
      pins another FreeType in the same local/deps keeps its own
  local/deps/text-fonts
      the fonts the build copies beside the game: DejaVu Sans Bold and
      DejaVu Sans 2.37, the endonym face cut from Noto Sans CJK SC Bold,
      Noto Emoji, and the interface faces of the modern look: Barlow 1.422
      Regular, Medium, SemiBold and Bold, and Barlow Condensed SemiBold and
      Bold

The full Simplified Chinese face travels with the language pack from
open-annihilation/languages, not with the engine. Cuts are made with the
fontTools that tools/text-fonts/requirements.txt pins, installed in a
virtual environment of its own (local/deps/fonttools-venv).

The endonym face, NotoSansCJKsc-Bold-Endonyms.otf, is Noto Sans CJK SC
Bold cut to the characters of tools/text-fonts/endonyms.txt that neither
DejaVu Sans Bold nor DejaVu Sans holds: the languages' own names and the
lines of the notice shown before a language pack is installed. It is at
most 64 KiB and its SHA-256 is pinned.

The test fixture's face, NotoSansCJKsc-Bold-Fixture.otf, is that same face
cut to the characters of the Simplified Chinese test fixture
(src/data/languages/tests/fixture-packs/zh-Hans) that neither sans face
holds. It is written under local/deps/text-fonts-tests, never beside the
game and never inside local/deps/text-fonts. Its SHA-256 is pinned.

Records for the shipped fonts and the fixture face are entries of
local/deps/text-fonts.records.json, beside the fonts folder.

The fonts folder is shared by every version of the engine, so this script
only adds to it. It makes a font of its own list that is missing or whose
record differs, writing the file to a temporary name and renaming it into
place, and it never deletes or rewrites any other file.

--self-test checks the pins and the endonym list, and downloads nothing.
tools/bootstrap_macos_deps.py and tools/bootstrap_windows_deps.py build
the same FreeType for the release and the Windows targets.
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import venv
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bootstrap_sdl  # noqa: E402

ROOT = bootstrap_sdl.ROOT
FREETYPE_VERSION = "2.14.3"
FREETYPE_SHA256 = "e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1"
FREETYPE_URL = f"https://download.savannah.gnu.org/releases/freetype/freetype-{FREETYPE_VERSION}.tar.gz"
# FreeType's own options: a static library that reads the fonts and nothing
# else, and tools/text-fonts/freetype_build_options.cmake, read after its
# project() call, which keeps only the modules the fonts need.
FREETYPE_OPTIONS = [
    "-DBUILD_SHARED_LIBS=OFF",
    "-DFT_DISABLE_ZLIB=ON",
    "-DFT_DISABLE_BZIP2=ON",
    "-DFT_DISABLE_PNG=ON",
    "-DFT_DISABLE_HARFBUZZ=ON",
    "-DFT_DISABLE_BROTLI=ON",
    f"-DCMAKE_PROJECT_INCLUDE={(ROOT / 'tools' / 'text-fonts' / 'freetype_build_options.cmake').as_posix()}",
]
# What a FreeType install prefix holds once complete.
FREETYPE_INSTALLED = "lib/cmake/freetype/freetype-config.cmake"

# Barlow's release tag 1.422 on the project's own repository.
BARLOW_RELEASE = "https://github.com/jpt/barlow/raw/1.422"
# The downloads: each file's name in local/deps, where it comes from and its
# SHA-256.
DOWNLOADS = {
    "dejavu": ("dejavu-fonts-ttf-2.37.zip",
               "https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/"
               "dejavu-fonts-ttf-2.37.zip",
               "7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a"),
    "noto-cjk": ("NotoSansCJK-Bold-2.004.ttc",
                 "https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/OTC/NotoSansCJK-Bold.ttc",
                 "faa5f3656a78b2e2d450d27fe8382c778bc2b6bb5ea29c986664a6a435056ceb"),
    "noto-cjk-licence": ("NotoSansCJK-2.004-LICENSE",
                         "https://github.com/notofonts/noto-cjk/raw/Sans2.004/LICENSE",
                         "6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2"),
    "noto-emoji": ("NotoEmoji-3.002.ttf",
                   "https://raw.githubusercontent.com/google/fonts/b979dba422e445492b0eb9951ac52ee0b4d648c3/"
                   "ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf",
                   "de6c18832938afc99caf132b39d6a30a19bac7f2e812e28db2535b4608d27551"),
    "noto-emoji-licence": ("NotoEmoji-3.002-OFL.txt",
                           "https://raw.githubusercontent.com/google/fonts/b979dba422e445492b0eb9951ac52ee0b4d648c3/"
                           "ofl/notoemoji/OFL.txt",
                           "500bb1ccf43df7bbb522112f9133a52b16e1c35e809632f5d8609b179152de5b"),
    # Barlow and Barlow Condensed, the static faces of the project's release
    # tag 1.422, each file as the tag holds it.
    "barlow-regular": ("Barlow-Regular-1.422.ttf",
                       f"{BARLOW_RELEASE}/fonts/ttf/Barlow-Regular.ttf",
                       "a72240db047b468c957834cdbd607f013da32edee62d096f5fe0a4c2dc8433ce"),
    "barlow-medium": ("Barlow-Medium-1.422.ttf",
                      f"{BARLOW_RELEASE}/fonts/ttf/Barlow-Medium.ttf",
                      "5a26763b11d11a437ab9f1c46281d3615ab1f6b93c90b8fa181e03978971b9e9"),
    "barlow-semibold": ("Barlow-SemiBold-1.422.ttf",
                        f"{BARLOW_RELEASE}/fonts/ttf/Barlow-SemiBold.ttf",
                        "cafab6a24fc131fd2dbbe56b9cf8daa616db3d5929fa89c35acba690e620585e"),
    "barlow-bold": ("Barlow-Bold-1.422.ttf",
                    f"{BARLOW_RELEASE}/fonts/ttf/Barlow-Bold.ttf",
                    "b5af7fa4dd961a259b075cd23094cd4e961afb6a0b5ac5cf0e3395fc54b7a4b5"),
    "barlow-condensed-semibold": ("BarlowCondensed-SemiBold-1.422.ttf",
                                  f"{BARLOW_RELEASE}/fonts/ttf/BarlowCondensed-SemiBold.ttf",
                                  "0dae49e31a5dd598184b8ed18ed877f9280e07c4cc3fa7c2cad25a5e7d8a3ad4"),
    "barlow-condensed-bold": ("BarlowCondensed-Bold-1.422.ttf",
                              f"{BARLOW_RELEASE}/fonts/ttf/BarlowCondensed-Bold.ttf",
                              "e4fd0ebbc8b4346229f5c409f34e052b5e0d7fbcb7a7c4c30f7c5d334b4953b2"),
    "barlow-licence": ("Barlow-1.422-OFL.txt",
                       f"{BARLOW_RELEASE}/OFL.txt",
                       "186d750eb496a4c17a76385f82be6aea2ac1cf2de074a811d63786cf374ea73f"),
}
# The interface faces: each file in the fonts folder and its download. The
# names are the engine's interface_face_files (oa/platform/text_font.hpp),
# in the order of their Face values.
INTERFACE_FONTS = (
    ("Barlow-Regular.ttf", "barlow-regular"),
    ("Barlow-Medium.ttf", "barlow-medium"),
    ("Barlow-SemiBold.ttf", "barlow-semibold"),
    ("Barlow-Bold.ttf", "barlow-bold"),
    ("BarlowCondensed-SemiBold.ttf", "barlow-condensed-semibold"),
    ("BarlowCondensed-Bold.ttf", "barlow-condensed-bold"),
)
# The fonts the game ships: the engine's face_files, then its
# interface_face_files (oa/platform/text_font.hpp). cmake/OaTextFonts.cmake's
# OA_TEXT_FONT_FILES names the same files in the same order.
FONT_FILES = ("DejaVuSans-Bold.ttf", "DejaVuSans.ttf", "NotoSansCJKsc-Bold-Endonyms.otf",
              "NotoEmoji.ttf") + tuple(name for name, _ in INTERFACE_FONTS)
# The face of the CJK collection the cuts are taken from: Noto Sans CJK SC
# Bold, whose character map gives the Simplified Chinese forms.
CJK_FACE_INDEX = 2
CJK_FACE_NAME = "Noto Sans CJK SC Bold"
# The endonym face: SC Bold cut to tools/text-fonts/endonyms.txt.
ENDONYM_LIST = ROOT / "tools" / "text-fonts" / "endonyms.txt"
ENDONYM_FONT = "NotoSansCJKsc-Bold-Endonyms.otf"
# The SHA-256 and character count of the cut the pinned fontTools makes.
ENDONYM_SUBSET_SHA256 = "98c6b1f26c100411670f568efab9cd347633ccaf76875725d0b0043a2b05e41d"
ENDONYM_SUBSET_CHARACTERS = 41
# The fixture face, cut from the test pack and kept beside the fonts folder.
FIXTURE_PACK = ROOT / "src" / "data" / "languages" / "tests" / "fixture-packs" / "zh-Hans"
FIXTURE_FILES = ("language.yaml", "translate.tdf", "units.tdf", "interface.tdf",
                 "pictures.tdf", "warmup.txt")
FIXTURE_FONT = "NotoSansCJKsc-Bold-Fixture.otf"
TEST_FONTS = "text-fonts-tests"
# The SHA-256 and character count of the cut the pinned fontTools makes.
FIXTURE_SUBSET_SHA256 = "3fc02fe3ac4b4700ff400a87e78dc1e024e5fa58ac6e5d25fb9c84befc753e1f"
FIXTURE_SUBSET_CHARACTERS = 1271
# Font records, beside the fonts folder, never inside it.
ENDONYM_RECORDS = "text-fonts.records.json"
# Each licence text in licenses/ and the file of a download, or a member of
# an archive, that must hold the same text.
LICENCES = {
    "licenses/FreeType-FTL.txt": ("freetype", f"freetype-{FREETYPE_VERSION}/docs/FTL.TXT"),
    "licenses/DejaVu-LICENSE.txt": ("dejavu", "dejavu-fonts-ttf-2.37/LICENSE"),
    "licenses/NotoSansCJK-OFL.txt": ("noto-cjk-licence", None),
    "licenses/NotoEmoji-OFL.txt": ("noto-emoji-licence", None),
    "licenses/Barlow-OFL.txt": ("barlow-licence", None),
}
FONTTOOLS_REQUIREMENTS = ROOT / "tools" / "text-fonts" / "requirements.txt"
SETTINGS_FILE = "build-settings.json"


def fetch(deps, key):
    """The path of a verified download under deps, fetched unless present."""
    name, url, sha256 = DOWNLOADS[key]
    path = deps / name
    bootstrap_sdl.fetch_archive(path, url, sha256)
    return path


def freetype_source(deps):
    """The verified, extracted FreeType source tree under deps."""
    deps.mkdir(parents=True, exist_ok=True)
    archive = deps / f"freetype-{FREETYPE_VERSION}.tar.gz"
    bootstrap_sdl.fetch_archive(archive, FREETYPE_URL, FREETYPE_SHA256)
    source = deps / f"freetype-{FREETYPE_VERSION}"
    bootstrap_sdl.extract_source(archive, source)
    return source


def check_licences(deps):
    """Fails unless every licence text of licenses/ is the one its download holds."""
    for notice, (key, member) in LICENCES.items():
        if key == "freetype":
            text = (freetype_source(deps).parent / member).read_bytes()
        elif member is None:
            text = fetch(deps, key).read_bytes()
        else:
            with zipfile.ZipFile(fetch(deps, key)) as archive:
                text = archive.read(member)
        if text != (ROOT / notice).read_bytes():
            raise RuntimeError(f"{notice} differs from the licence {DOWNLOADS.get(key, ('FreeType',))[0]} holds")


def is_current(folder, files, wanted):
    """Whether folder holds files and records the settings wanted."""
    if not all((folder / name).exists() for name in files):
        return False
    try:
        return json.loads((folder / SETTINGS_FILE).read_text()) == wanted
    except (OSError, ValueError):
        return False


def record(folder, wanted):
    """Records in folder the settings it was made with."""
    (folder / SETTINGS_FILE).write_text(json.dumps(wanted, indent=2, sort_keys=True) + "\n")


def build_freetype(deps, jobs):
    """Builds FreeType for this machine into deps/freetype-install-<release> unless it is current."""
    install = deps / f"freetype-install-{FREETYPE_VERSION}"
    wanted = {"version": FREETYPE_VERSION, "sha256": FREETYPE_SHA256, "options": FREETYPE_OPTIONS}
    if is_current(install, [FREETYPE_INSTALLED], wanted):
        return install
    build = deps / f"freetype-build-{FREETYPE_VERSION}"
    shutil.rmtree(build, ignore_errors=True)
    shutil.rmtree(install, ignore_errors=True)
    subprocess.run(["cmake", "-S", str(freetype_source(deps)), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                    f"-DCMAKE_INSTALL_PREFIX={install}", *FREETYPE_OPTIONS], check=True)
    subprocess.run(["cmake", "--build", str(build), "--config", "Release", "--parallel", str(jobs)], check=True)
    subprocess.run(["cmake", "--install", str(build), "--config", "Release"], check=True)
    record(install, wanted)
    return install


def fonttools_python(deps):
    """The Python of the virtual environment that holds the pinned fontTools, made on first use."""
    environment = deps / "fonttools-venv"
    names = (pathlib.Path("bin") / "python", pathlib.Path("Scripts") / "python.exe")
    python = next((environment / name for name in names if (environment / name).is_file()), None)
    stamp = environment / "requirements.txt"
    if python is None or not stamp.is_file() or stamp.read_bytes() != FONTTOOLS_REQUIREMENTS.read_bytes():
        print(f"Installing the pinned fontTools into {environment}", flush=True)
        venv.EnvBuilder(with_pip=True, clear=True).create(environment)
        python = next(environment / name for name in names if (environment / name).is_file())
        subprocess.run([str(python), "-m", "pip", "install", "--quiet", "--require-hashes", "-r",
                        str(FONTTOOLS_REQUIREMENTS)], check=True)
        shutil.copyfile(FONTTOOLS_REQUIREMENTS, stamp)
    return python


def cut_face(collection, face, unicodes, out):
    """Writes one face of a collection to out, cut to unicodes.

    Layout tables are dropped, since the engine shapes nothing, and the
    hinting kept; the font's modification time is left as it was, so the
    output depends only on its input. The vertical metrics are kept, so a
    cut of Noto Sans CJK SC Bold has the same rows as the face.
    """
    from fontTools import subset
    from fontTools.ttLib import TTFont

    font = TTFont(collection, fontNumber=face, recalcTimestamp=False, lazy=False)
    options = subset.Options()
    options.layout_features = []
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.notdef_outline = True
    options.recalc_timestamp = False
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=list(unicodes))
    subsetter.subset(font)
    font.save(out)


def endonym_lines(path=None):
    """The strings of the endonym list: comments and blank lines left out.

    The file must be UTF-8 and hold no tab.
    """
    path = ENDONYM_LIST if path is None else path
    raw = path.read_bytes()
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise RuntimeError(f"{path} is not UTF-8") from error
    if b"\t" in raw:
        raise RuntimeError(f"{path} holds a tab")
    lines = []
    for line in text.splitlines():
        if line.startswith("#") or line.strip() == "":
            continue
        lines.append(line)
    return lines


def endonym_characters(lines, sans_cmaps, cjk_cmap):
    """The code points the endonym face keeps, sorted.

    Each non-ASCII character of lines that neither DejaVu Sans Bold nor
    DejaVu Sans holds. The sans faces come first in the chain, so a
    character either of them holds needs no place in the cut. A character
    no face holds is named in the error, every such character in one.
    """
    held_by_sans = set()
    for cmap in sans_cmaps:
        held_by_sans.update(cmap)
    missing = []
    seen = set()
    kept = set()
    for line in lines:
        for character in line:
            code = ord(character)
            if code < 128 or code in held_by_sans:
                continue
            if code in cjk_cmap:
                kept.add(code)
                continue
            if code not in seen:
                seen.add(code)
                missing.append(character)
    if missing:
        named = ", ".join(f"U+{ord(character):04X} ({character})" for character in missing)
        raise RuntimeError(f"no face holds {named}")
    return sorted(kept)


def best_cmap(path, face=0):
    """The font's best character map, as code point to glyph id."""
    from fontTools.ttLib import TTFont

    font = TTFont(path, fontNumber=face, recalcTimestamp=False, lazy=False)
    try:
        return font.getBestCmap() or {}
    finally:
        font.close()


def cut_endonym_face(collection, dejavu_bold, dejavu, out):
    """Writes the endonym face to out, cut from SC Bold of the collection.

    Reads the endonym list, the two sans fonts' character maps and the
    collection's face 2. Runs where fontTools is installed
    (--cut-endonym-face).
    """
    kept = endonym_characters(endonym_lines(),
                              (best_cmap(dejavu_bold), best_cmap(dejavu)),
                              best_cmap(collection, CJK_FACE_INDEX))
    if len(kept) != ENDONYM_SUBSET_CHARACTERS:
        raise RuntimeError(f"the endonym face keeps {len(kept)} characters, "
                           f"not {ENDONYM_SUBSET_CHARACTERS}")
    cut_face(collection, CJK_FACE_INDEX, kept, out)
    print(f"{pathlib.Path(out).name}: {len(kept)} characters", flush=True)


def write_over(path, data):
    """Writes data to path by renaming a temporary file over it."""
    part = path.with_name(path.name + ".part")
    part.write_bytes(data)
    part.replace(path)


def endonym_record():
    """The endonym face's settings: the collection, fontTools and the list."""
    return {"collection": DOWNLOADS["noto-cjk"][2],
            "fonttools": FONTTOOLS_REQUIREMENTS.read_text(),
            "list": hashlib.sha256(ENDONYM_LIST.read_bytes()).hexdigest()}


def fixture_lines():
    """The fixture pack's lines, comments and blank lines left out.

    The same skipping as endonym_lines. The tables keep tabs, which the
    endonym list does not.
    """
    lines = []
    for name in FIXTURE_FILES:
        path = FIXTURE_PACK / name
        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise RuntimeError(f"{path} is not UTF-8") from error
        for line in text.splitlines():
            if line.startswith("#") or line.strip() == "":
                continue
            lines.append(line)
    return lines


def fixture_record():
    """The fixture face's settings: the collection, fontTools and each pack file."""
    files = {name: hashlib.sha256((FIXTURE_PACK / name).read_bytes()).hexdigest()
             for name in FIXTURE_FILES}
    return {"collection": DOWNLOADS["noto-cjk"][2],
            "files": files,
            "fonttools": FONTTOOLS_REQUIREMENTS.read_text()}


def cut_fixture_face(collection, dejavu_bold, dejavu, out):
    """Writes the fixture face to out, cut from SC Bold of the collection.

    The characters are those of the fixture pack that neither sans face
    holds. Runs where fontTools is installed (--cut-fixture-face).
    """
    kept = endonym_characters(fixture_lines(),
                              (best_cmap(dejavu_bold), best_cmap(dejavu)),
                              best_cmap(collection, CJK_FACE_INDEX))
    print(f"fixture characters: {len(kept)}", flush=True)
    if len(kept) != FIXTURE_SUBSET_CHARACTERS:
        raise RuntimeError(f"the fixture face keeps {len(kept)} characters, "
                           f"not {FIXTURE_SUBSET_CHARACTERS}")
    cut_face(collection, CJK_FACE_INDEX, kept, out)
    print(f"{pathlib.Path(out).name}: {len(kept)} characters", flush=True)


def read_records(deps):
    """The font records beside the fonts folder, or none when the file is absent or unreadable."""
    path = deps / ENDONYM_RECORDS
    try:
        found = json.loads(path.read_text())
    except (OSError, ValueError):
        return {}
    return found if isinstance(found, dict) else {}


def write_records(deps, records):
    """Writes the font records beside the fonts folder, renaming a temporary file over."""
    path = deps / ENDONYM_RECORDS
    part = path.with_name(path.name + ".part")
    part.write_text(json.dumps(records, indent=2, sort_keys=True) + "\n")
    part.replace(path)


def make_copied_font(deps, fonts, name, key, member=None):
    """Copies one downloaded font into fonts when it is missing or its record differs."""
    wanted = {"download": DOWNLOADS[key][2]}
    if member is not None:
        wanted["member"] = member
    records = read_records(deps)
    path = fonts / name
    if path.is_file() and records.get(name) == wanted:
        return
    if member is None:
        data = fetch(deps, key).read_bytes()
    else:
        with zipfile.ZipFile(fetch(deps, key)) as archive:
            data = archive.read(member)
    write_over(path, data)
    records = read_records(deps)
    records[name] = wanted
    write_records(deps, records)


def make_endonym_font(deps, fonts):
    """Makes the endonym face when it is missing or its record differs."""
    wanted = endonym_record()
    records = read_records(deps)
    path = fonts / ENDONYM_FONT
    if path.is_file() and records.get(ENDONYM_FONT) == wanted:
        return
    part = fonts / f"{ENDONYM_FONT}.part"
    print(f"Writing {ENDONYM_FONT} (cut to the languages' own names and the notice)", flush=True)
    command = [str(fonttools_python(deps)), str(pathlib.Path(__file__).resolve()), "--cut-endonym-face",
               str(fetch(deps, "noto-cjk")), str(fonts / "DejaVuSans-Bold.ttf"),
               str(fonts / "DejaVuSans.ttf"), str(part)]
    subprocess.run(command, check=True)
    data = part.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    print(f"  {ENDONYM_FONT}: SHA-256 {digest}, {len(data)} bytes", flush=True)
    if digest != ENDONYM_SUBSET_SHA256:
        part.unlink()
        raise RuntimeError(f"the endonym face has SHA-256 {digest}, not the pinned {ENDONYM_SUBSET_SHA256}")
    if len(data) > 65536:
        part.unlink()
        raise RuntimeError(f"the endonym face is {len(data)} bytes, over 64 KiB")
    part.replace(path)
    records[ENDONYM_FONT] = wanted
    write_records(deps, records)


def make_fixture_font(deps):
    """Makes the fixture face when it is missing or its record differs.

    The face is written under text-fonts-tests, never inside the fonts folder.
    """
    wanted = fixture_record()
    records = read_records(deps)
    tests = deps / TEST_FONTS
    tests.mkdir(parents=True, exist_ok=True)
    path = tests / FIXTURE_FONT
    if path.is_file() and records.get(FIXTURE_FONT) == wanted:
        return
    fonts = deps / "text-fonts"
    part = tests / f"{FIXTURE_FONT}.part"
    print(f"Writing {FIXTURE_FONT} (cut to the test fixture)", flush=True)
    command = [str(fonttools_python(deps)), str(pathlib.Path(__file__).resolve()), "--cut-fixture-face",
               str(fetch(deps, "noto-cjk")), str(fonts / "DejaVuSans-Bold.ttf"),
               str(fonts / "DejaVuSans.ttf"), str(part)]
    subprocess.run(command, check=True)
    data = part.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    print(f"  {FIXTURE_FONT}: SHA-256 {digest}, {len(data)} bytes", flush=True)
    if digest != FIXTURE_SUBSET_SHA256:
        part.unlink()
        raise RuntimeError(f"the fixture face has SHA-256 {digest}, not the pinned {FIXTURE_SUBSET_SHA256}")
    part.replace(path)
    records = read_records(deps)
    records[FIXTURE_FONT] = wanted
    write_records(deps, records)


def make_fonts(deps):
    """Puts the game's fonts in deps/text-fonts, adding a font that is missing or out of date.

    Each font of FONT_FILES is made when it is missing or its record differs.
    The fixture face is made the same way, under text-fonts-tests. Nothing
    else in the fonts folder is deleted or rewritten.
    """
    fonts = deps / "text-fonts"
    fonts.mkdir(parents=True, exist_ok=True)
    make_copied_font(deps, fonts, "DejaVuSans-Bold.ttf", "dejavu",
                     "dejavu-fonts-ttf-2.37/ttf/DejaVuSans-Bold.ttf")
    make_copied_font(deps, fonts, "DejaVuSans.ttf", "dejavu",
                     "dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf")
    make_copied_font(deps, fonts, "NotoEmoji.ttf", "noto-emoji")
    for name, key in INTERFACE_FONTS:
        make_copied_font(deps, fonts, name, key)
    make_endonym_font(deps, fonts)
    make_fixture_font(deps)
    return fonts


def self_test():
    """Checks the pins and the endonym list; returns the exit status."""
    failures = []
    pins = [("FreeType", FREETYPE_SHA256, FREETYPE_URL)]
    pins += [(key, sha256, url) for key, (_, url, sha256) in DOWNLOADS.items()]
    pins += [("the endonym cut", ENDONYM_SUBSET_SHA256, "https://")]
    pins += [("the fixture cut", FIXTURE_SUBSET_SHA256, "https://")]
    for what, sha256, url in pins:
        if not bootstrap_sdl.SHA256_RE.fullmatch(sha256) or not url.startswith("https://"):
            failures.append(f"the pin of {what} is malformed")
    for count, what in ((ENDONYM_SUBSET_CHARACTERS, "endonym"),
                        (FIXTURE_SUBSET_CHARACTERS, "fixture")):
        if not isinstance(count, int) or isinstance(count, bool) or count < 1:
            failures.append(f"the {what} character count is not a positive integer")
    for name, key in INTERFACE_FONTS:
        if key not in DOWNLOADS:
            failures.append(f"{name} names no download")
    if len(set(FONT_FILES)) != len(FONT_FILES):
        failures.append("a font is named twice")
    for notice, (key, _) in LICENCES.items():
        if key != "freetype" and key not in DOWNLOADS:
            failures.append(f"{notice} names no download")
        if not (ROOT / notice).is_file():
            failures.append(f"{notice} is missing")
    try:
        raw = ENDONYM_LIST.read_bytes()
    except OSError:
        failures.append("the endonym list is missing")
        raw = b""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        failures.append("the endonym list is not UTF-8")
        text = ""
    if b"\t" in raw:
        failures.append("the endonym list holds a tab")
    non_ascii = set()
    for line in text.splitlines():
        if line.startswith("#") or line.strip() == "":
            continue
        for character in line:
            if ord(character) >= 128:
                non_ascii.add(ord(character))
    if len(non_ascii) < ENDONYM_SUBSET_CHARACTERS:
        failures.append(f"the endonym list holds {len(non_ascii)} non-ASCII characters, "
                        f"fewer than {ENDONYM_SUBSET_CHARACTERS}")
    for failure in failures:
        print(f"bootstrap_text_fonts self-test: {failure}")
    if failures:
        return 1
    print(f"bootstrap_text_fonts self-test: {len(pins)} pins and {len(LICENCES)} licences, "
          f"as expected")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--deps", type=pathlib.Path, default=ROOT / "local" / "deps",
                        help="archive, source and build cache (default: local/deps)")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--fonts-only", action="store_true",
                        help="make the fonts and build no FreeType (for builds that bring their own)")
    parser.add_argument("--self-test", action="store_true",
                        help="check the pins and the endonym list, and exit")
    parser.add_argument("--cut-endonym-face", nargs=4,
                        metavar=("COLLECTION", "DEJAVU_BOLD", "DEJAVU", "OUT"), help=argparse.SUPPRESS)
    parser.add_argument("--cut-fixture-face", nargs=4,
                        metavar=("COLLECTION", "DEJAVU_BOLD", "DEJAVU", "OUT"), help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.cut_endonym_face:
        cut_endonym_face(*args.cut_endonym_face)
        return 0
    if args.cut_fixture_face:
        cut_fixture_face(*args.cut_fixture_face)
        return 0
    deps = args.deps.resolve()
    deps.mkdir(parents=True, exist_ok=True)
    check_licences(deps)
    if not args.fonts_only:
        install = build_freetype(deps, args.jobs)
        print(f"FreeType {FREETYPE_VERSION} ready in {install}")
    fonts = make_fonts(deps)
    for name in FONT_FILES:
        print(f"  {name}: {(fonts / name).stat().st_size} bytes")
    print(f"Text fonts ready in {fonts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
