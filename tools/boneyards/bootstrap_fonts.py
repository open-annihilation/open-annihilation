#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Fetch the page fonts into a dependency folder of this engine tree.

The fonts stand in for the Windows faces the pages select by name. Every
font file and licence text is pinned by SHA-256 and copied unmodified. The
pages' fonts go in <deps>/fonts, the layout copied beside the executable.
Fira Sans Extra Condensed and Noto Sans CJK Medium, the analogue faces drawn
into the committed bitmap atlas, go in <deps>/font-sources so the pages'
folder is not asked to carry them. The atlas itself is
data/boneyards/fonts/boneyards-analogues.bin; this script does not draw it.
<deps> is --deps, otherwise OA_DEPS_DIR, otherwise
this tree's local/deps. Nothing outside it is modified. When every file is
already present with its pinned hash, the run downloads nothing.

FreeType and Noto Emoji belong to this engine: tools/bootstrap_text_fonts.py
fetches them, and this script does not. The download helper is
tools/bootstrap_sdl.py in this tree.

    python3 tools/boneyards/bootstrap_fonts.py --deps <dir>
"""
import argparse
import hashlib
import os
import pathlib
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def engine_dir():
    """This engine tree, or OA_ENGINE_DIR when that names one.

    A checkout that is not the engine (an engine/ directory inside it, or
    the engine checkout beside it) is still accepted, so a copy of this
    file run from elsewhere keeps finding the download helper.
    """
    if os.environ.get("OA_ENGINE_DIR"):
        return pathlib.Path(os.environ["OA_ENGINE_DIR"]).resolve()
    if (ROOT / "CMakeLists.txt").is_file() and (ROOT / "tools" / "bootstrap_sdl.py").is_file():
        return ROOT
    for candidate in (ROOT / "engine", ROOT.parent / "open-annihilation"):
        if (candidate / "CMakeLists.txt").is_file():
            return candidate
    sys.exit("bootstrap_fonts: no open-annihilation engine found; set OA_ENGINE_DIR to an engine checkout, "
             "or run 'git submodule update --init engine'")


ENGINE = engine_dir()
sys.path.insert(0, str(ENGINE / "tools"))
import bootstrap_sdl  # noqa: E402

# Liberation 2.1.5: the TTF tarball attached to the release notes of tag 2.1.5
# (commit 4b0192046158094654e865245832c66d2104219e).
LIBERATION_ARCHIVE = "liberation-fonts-ttf-2.1.5.tar.gz"
LIBERATION_URL = ("https://github.com/liberationfonts/liberation-fonts/files/7261482/"
                  + LIBERATION_ARCHIVE)
LIBERATION_SHA256 = "7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0"
LIBERATION_MEMBERS = {
    "LiberationSans-Regular.ttf": "76d04c18ea243f426b7de1f3ad208e927008f961dc5945e5aad352d0dfde8ee8",
    "LiberationSans-Bold.ttf": "788abee4c806d660e8aee46689dd8540cd4bb98da03dcc9d171ce3efd99a9173",
    "LiberationSans-Italic.ttf": "e5bae5c4cde31f22142753855f4f8fb86da6ff39955ed3c0a11248b0d16948b0",
    "LiberationSans-BoldItalic.ttf": "698da70fc191cc5f33ad4d6d3fe830fe4624b898ea2e3169955928b7c491f1ee",
    "LiberationSerif-Regular.ttf": "058ea80864aef09a23f45cbec2bb5400bc3dfbdea01c3f10538a21fcb497fb74",
    "LiberationSerif-Bold.ttf": "d754ba427cfe0bca54ae052384baa8f842da5bd6550ad4da024ac441e7a7d5ce",
    "LiberationSerif-Italic.ttf": "0e3dea9f8d613e006ccfa62201f33e265d19167bd0907725c3e145368b04fc2e",
    "LiberationSerif-BoldItalic.ttf": "f17db8af71e24d2066b587546021d4f0b296be389512b658dec3c09affeb11a7",
    "LICENSE": "93fed46019c38bbe566b479d22148e2e8a1e85ada614accb0211c37b2c61c19b",
}
# Saira ExtraCondensed ExtraBold 0.072 from google/fonts at the commit that added it.
SAIRA_BASE = ("https://raw.githubusercontent.com/google/fonts/"
              "679bd07fa26e1d0a05dc8d3fb45234564841ca7a/ofl/sairaextracondensed/")
SAIRA_FILES = {
    "SairaExtraCondensed-ExtraBold.ttf": "0e282d00d8036c8a09576e3485b854dfad0fa8b1019dff41712df37cfe102594",
    "OFL.txt": "0e2ccc5da1dd979f08ba82bca2fb61592506a1e58d3cbfc26d904f0db71bd812",
}
LIBERATION_DIR = "liberation"
SAIRA_DIR = "saira-extracondensed"
# Fira Sans Extra Condensed SemiBold, the weight the bitmap faces fall back
# to for Latin, Greek, Cyrillic and Vietnamese. google/fonts at the commit
# that added the static file; the file has not changed since.
FIRA_BASE = ("https://raw.githubusercontent.com/google/fonts/"
             "a8eaa3ebfc21d9c4b7d830a84ce7ba2f16eec1e1/ofl/firasansextracondensed/")
FIRA_FILES = {
    "FiraSansExtraCondensed-SemiBold.ttf":
        "54fc425c016631fbaa1ae2aa1f59f53e46e9c3805fa1df5a217613d8b6228ec7",
    "OFL.txt": "8f24842e9174beda18a556c2ae7d54f5dc444340c19a3a9ef77e23bca366adbd",
}
# Noto Sans CJK Medium 2.004, the same release as the engine's Bold
# collection. The Simplified Chinese face is the one the atlases draw.
CJK_MEDIUM_NAME = "NotoSansCJK-Medium.ttc"
CJK_MEDIUM_URL = ("https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/OTC/"
                  + CJK_MEDIUM_NAME)
CJK_MEDIUM_SHA256 = "197d5e1e019faca33a4d55931c7d68b8056f3b97cb862049f5cb8de9efdfb8ce"
CJK_LICENCE_NAME = "NotoSansCJK-2.004-LICENSE"
CJK_LICENCE_URL = "https://github.com/notofonts/noto-cjk/raw/Sans2.004/LICENSE"
CJK_LICENCE_SHA256 = "6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2"
FIRA_DIR = "fira-sans-extra-condensed"
CJK_DIR = "noto-sans-cjk"


def sha256_of(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def installed(target, members):
    """True when every name in members is a file in target with that SHA-256."""
    for name, sha256 in members.items():
        path = target / name
        if not path.is_file() or sha256_of(path) != sha256:
            return False
    return True


def install_liberation(archives, target):
    if installed(target, LIBERATION_MEMBERS):
        return
    archive = archives / LIBERATION_ARCHIVE
    bootstrap_sdl.fetch_archive(archive, LIBERATION_URL, LIBERATION_SHA256)
    target.mkdir(parents=True, exist_ok=True)
    top = LIBERATION_ARCHIVE.removesuffix(".tar.gz")
    with tarfile.open(archive, "r:gz") as bundle:
        for name, sha256 in LIBERATION_MEMBERS.items():
            out = target / name
            if out.exists() and sha256_of(out) == sha256:
                continue
            data = bundle.extractfile(f"{top}/{name}").read()
            if hashlib.sha256(data).hexdigest() != sha256:
                raise RuntimeError(f"{top}/{name} SHA-256 mismatch")
            out.write_bytes(data)


def install_saira(archives, target):
    if installed(target, SAIRA_FILES):
        return
    target.mkdir(parents=True, exist_ok=True)
    for name, sha256 in SAIRA_FILES.items():
        cached = archives / f"saira-extracondensed-{name}"
        bootstrap_sdl.fetch_archive(cached, SAIRA_BASE + name, sha256)
        out = target / name
        if not out.exists() or sha256_of(out) != sha256:
            out.write_bytes(cached.read_bytes())


def install_pinned(archives, target, cache_prefix, files):
    """Copies each (name, url, sha256) into target, cached under archives."""
    if installed(target, {name: sha256 for name, _url, sha256 in files}):
        return
    target.mkdir(parents=True, exist_ok=True)
    for name, url, sha256 in files:
        cached = archives / f"{cache_prefix}{name}"
        bootstrap_sdl.fetch_archive(cached, url, sha256)
        out = target / name
        if not out.exists() or sha256_of(out) != sha256:
            out.write_bytes(cached.read_bytes())


def install_fira(archives, target):
    install_pinned(
        archives,
        target,
        "fira-sans-extra-condensed-",
        [(name, FIRA_BASE + name, sha256) for name, sha256 in FIRA_FILES.items()],
    )


def install_cjk(archives, target):
    install_pinned(archives, target, "", (
        (CJK_MEDIUM_NAME, CJK_MEDIUM_URL, CJK_MEDIUM_SHA256),
        (CJK_LICENCE_NAME, CJK_LICENCE_URL, CJK_LICENCE_SHA256),
    ))


def fetch_fonts(deps):
    """Installs the bundled fonts and their licences under <deps>."""
    archives = deps / "font-archives"
    archives.mkdir(parents=True, exist_ok=True)
    fonts = deps / "fonts"
    install_liberation(archives, fonts / LIBERATION_DIR)
    install_saira(archives, fonts / SAIRA_DIR)
    sources = deps / "font-sources"
    install_fira(archives, sources / FIRA_DIR)
    install_cjk(archives, sources / CJK_DIR)
    return fonts


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--deps", type=pathlib.Path, default=None,
                        help="dependency folder; writes <dir>/fonts and <dir>/font-sources "
                             "(default: OA_DEPS_DIR, or local/deps)")
    args = parser.parse_args()
    # Worktrees link local/deps to one shared directory; build under its real path.
    if args.deps is None:
        deps = pathlib.Path(os.environ.get("OA_DEPS_DIR") or ROOT / "local" / "deps")
    else:
        deps = args.deps
    fonts = fetch_fonts(deps.resolve())
    print(f"Fonts ready under {fonts}")


if __name__ == "__main__":
    main()
