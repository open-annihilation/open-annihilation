#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the macOS and iOS bundles declare .oamod, .oalang, .oamap, .oamut, .oabrain, .oareg and .oalaunch as the game's own types.

Reads the two Info.plist templates (src/app/Info.plist.in and platforms/ios/Info.plist.in, or
the two named) as property lists and fails unless each, for every type in TYPES:

  exports    the type (UTExportedTypeDeclarations), named as the table says, conforming to
             public.data alone (public.plain-text alone for a launch file), with the file
             name extension and the MIME type, as the run-time registration
             (src/platform/file-types) names them;
  opens      that type as its owner (CFBundleDocumentTypes: LSHandlerRank Owner, the role
             Viewer, the type alone in LSItemContentTypes);
  icon       on macOS, gives the type the icon the system draws from the app's icon
             (CFBundleTypeIconSystemGenerated, 1) and the bundle's own icon file before
             macOS 11 (CFBundleTypeIconFile, the same as CFBundleIconFile), an icon file that
             cmake/OaGameBundle.cmake takes from branding/ and that is there; on iOS, names no
             icon file, since iOS draws document icons from the app's icon;
  in place   on iOS, still opens documents in place (LSSupportsOpeningDocumentsInPlace),
             which the Files app needs, and still declares the game's archive type;
  plain text on macOS, takes plain text it is handed without being its opener (one document
             type of public.plain-text alone, LSHandlerRank None), so that `open -f -a`
             hands the game a launch piped in.

It also fails unless the files kept by hand beside the bundles name every type in TYPES:

  installer  the Windows uninstaller (packaging/windows/open-annihilation.nsi) withdraws each
             type once, with the extension, program identifier and MIME type the run-time
             registration names, and withdraws no other;
  opened     iOS's platform code (platforms/ios/src/ios_platform.mm) brings in each type the
             game installs or adds when the system opens one in the game (kOpenedExtensions):
             every type but the launch file, once each.

Exit status is 1 on any finding and 2 when a file cannot be read.
"""
import argparse
import plistlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# type id, name, extension, MIME type, the header constants' prefix, what it conforms to.
TYPES = (
    ("net.coreprime.open-annihilation.oamod", "Open Annihilation mod", "oamod",
     "application/x-oamod", "mod", ["public.data"]),
    ("net.coreprime.open-annihilation.oalang", "Open Annihilation language pack", "oalang",
     "application/x-oalang", "language", ["public.data"]),
    ("net.coreprime.open-annihilation.oamap", "Open Annihilation map pack", "oamap",
     "application/x-oamap", "map", ["public.data"]),
    ("net.coreprime.open-annihilation.oamut", "Open Annihilation mutator", "oamut",
     "application/x-oamut", "mutator", ["public.data"]),
    ("net.coreprime.open-annihilation.oabrain", "Open Annihilation brain", "oabrain",
     "application/x-oabrain", "brain", ["public.data"]),
    ("net.coreprime.open-annihilation.oareg", "Open Annihilation registry", "oareg",
     "application/x-oareg", "registry", ["public.data"]),
    ("net.coreprime.open-annihilation.oalaunch", "Open Annihilation launch file", "oalaunch",
     "application/x-oalaunch", "launch", ["public.plain-text"]),
)
BUNDLE_ID = "net.coreprime.open-annihilation"
PLAIN_TEXT = "public.plain-text"
ARCHIVE_TYPE_ID = "net.coreprime.open-annihilation.game-archive"
ICON_PLACEHOLDER = "${MACOSX_BUNDLE_ICON_FILE}"
# The run-time registration's constants, which the bundles must agree with.
FILE_TYPES_HEADER = Path("src/platform/file-types/include/oa/platform/file_types.hpp")
# The Windows installer, whose uninstaller withdraws each type for the account that runs it.
INSTALLER = Path("packaging/windows/open-annihilation.nsi")
NSIS_DEFINE_RE = re.compile(r'^!define (\w+) "([^"]*)"', re.MULTILINE)
NSIS_REFERENCE_RE = re.compile(r"\$\{(\w+)\}")
WITHDRAW_RE = re.compile(r'^\s*\$\{OA_WithdrawUserType\} "([^"]*)" "([^"]*)" "([^"]*)"', re.MULTILINE)
# iOS's platform code, which brings in each file opened in the game that it knows by extension.
IOS_PLATFORM = Path("platforms/ios/src/ios_platform.mm")
OPENED_RE = re.compile(r"kOpenedExtensions\[\] = \{([^}]*)\}")
OPENED_ITEM_RE = re.compile(r'@"([^"]*)"')
# The prefix of the one type the game does not install or add.
LAUNCH_PREFIX = "launch"
CONSTANT_RE = re.compile(r'inline constexpr std::string_view (\w+) = "([^"]*)";')
ICON_FILE_RE = re.compile(r"MACOSX_BUNDLE_ICON_FILE\s+(\S+)")
# Bound on each file read.
MAX_FILE_BYTES = 1 << 20


class ReadError(Exception):
    """A file that cannot be read."""


def read_bytes(path):
    """Reads a file of at most MAX_FILE_BYTES bytes."""
    try:
        with open(path, "rb") as stream:
            data = stream.read(MAX_FILE_BYTES + 1)
    except OSError as error:
        raise ReadError(f"{path}: {error.strerror or error}") from error
    if len(data) > MAX_FILE_BYTES:
        raise ReadError(f"{path}: larger than {MAX_FILE_BYTES} bytes")
    return data


def read_plist(path):
    """Reads a property list template; its ${...} and @...@ placeholders are plain strings."""
    try:
        value = plistlib.loads(read_bytes(path))
    except (plistlib.InvalidFileException, ValueError) as error:
        raise ReadError(f"{path}: not a property list: {error}") from error
    if not isinstance(value, dict):
        raise ReadError(f"{path}: not a dictionary at its top")
    return value


def check_exported_type(name, plist, findings, type_id, type_name, extension, mime_type,
                        conforms_to):
    """Checks that a bundle exports one type as the registration names it."""
    exported = [entry for entry in plist.get("UTExportedTypeDeclarations", [])
                if isinstance(entry, dict) and entry.get("UTTypeIdentifier") == type_id]
    if len(exported) != 1:
        findings.append(f"{name}: exports: {type_id} is declared {len(exported)} times, not once")
        return
    entry = exported[0]
    tags = entry.get("UTTypeTagSpecification", {})
    expected = {
        "UTTypeDescription": (entry.get("UTTypeDescription"), type_name),
        "UTTypeConformsTo": (entry.get("UTTypeConformsTo"), conforms_to),
        "public.filename-extension": (tags.get("public.filename-extension"), [extension]),
        "public.mime-type": (tags.get("public.mime-type"), [mime_type]),
    }
    for key, (found, wanted) in expected.items():
        if found != wanted:
            findings.append(f"{name}: exports: {key} is {found!r}, not {wanted!r}")


def check_document_type(name, plist, findings, macos, type_id, type_name):
    """Checks that a bundle opens one type as its owner, with the icon keys its system takes."""
    documents = [entry for entry in plist.get("CFBundleDocumentTypes", [])
                 if isinstance(entry, dict) and type_id in entry.get("LSItemContentTypes", [])]
    if len(documents) != 1:
        findings.append(f"{name}: opens: {len(documents)} document types name {type_id}, not one")
        return
    entry = documents[0]
    expected = {
        "CFBundleTypeName": type_name,
        "CFBundleTypeRole": "Viewer",
        "LSHandlerRank": "Owner",
        "LSItemContentTypes": [type_id],
    }
    for key, wanted in expected.items():
        if entry.get(key) != wanted:
            findings.append(f"{name}: opens: {key} is {entry.get(key)!r}, not {wanted!r}")
    if macos:
        generated = entry.get("CFBundleTypeIconSystemGenerated")
        # Xcode writes the integer 1; the boolean true is read the same way.
        if generated is not True and not (type(generated) is int and generated == 1):
            findings.append(f"{name}: icon: CFBundleTypeIconSystemGenerated is {generated!r}, not 1")
        icon = entry.get("CFBundleTypeIconFile")
        if icon != ICON_PLACEHOLDER or plist.get("CFBundleIconFile") != ICON_PLACEHOLDER:
            findings.append(f"{name}: icon: CFBundleTypeIconFile is {icon!r} and CFBundleIconFile "
                            f"{plist.get('CFBundleIconFile')!r}, not both {ICON_PLACEHOLDER}")
    else:
        for key in ("CFBundleTypeIconFile", "CFBundleTypeIconFiles", "CFBundleTypeIconSystemGenerated"):
            if key in entry:
                findings.append(f"{name}: icon: {key} is given; iOS draws the icon from the app's")


def check_plain_text(name, plist, findings):
    """Checks that the macOS bundle takes plain text it is handed, without opening it by default."""
    documents = [entry for entry in plist.get("CFBundleDocumentTypes", [])
                 if isinstance(entry, dict) and PLAIN_TEXT in entry.get("LSItemContentTypes", [])]
    if len(documents) != 1:
        findings.append(f"{name}: plain text: {len(documents)} document types name {PLAIN_TEXT}, not one")
        return
    entry = documents[0]
    expected = {
        "CFBundleTypeRole": "Viewer",
        "LSHandlerRank": "None",
        "LSItemContentTypes": [PLAIN_TEXT],
    }
    for key, wanted in expected.items():
        if entry.get(key) != wanted:
            findings.append(f"{name}: plain text: {key} is {entry.get(key)!r}, not {wanted!r}")


def check_bundle_icon(root, findings):
    """Checks that the icon file the macOS bundle names is the one branding/ holds."""
    bundle = root / "cmake" / "OaGameBundle.cmake"
    match = ICON_FILE_RE.search(read_bytes(bundle).decode("utf-8"))
    if match is None:
        findings.append(f"{bundle}: icon: MACOSX_BUNDLE_ICON_FILE is not set")
        return
    icon = root / "branding" / match.group(1)
    if not icon.is_file():
        findings.append(f"{bundle}: icon: the bundle's icon file {match.group(1)} is not in branding/")


def check_constants(root, findings):
    """Checks that the run-time registration names each type as the bundles do."""
    header = root / FILE_TYPES_HEADER
    constants = read_constants(root)
    if constants.get("desktop_id") != BUNDLE_ID:
        findings.append(f"{header}: exports: desktop_id is {constants.get('desktop_id')!r}, not {BUNDLE_ID!r}")
    for _type_id, type_name, extension, mime_type, prefix, _conforms_to in TYPES:
        expected = {
            f"{prefix}_extension": extension,
            f"{prefix}_mime_type": mime_type,
            f"{prefix}_type_name": type_name,
        }
        for key, wanted in expected.items():
            if constants.get(key) != wanted:
                findings.append(f"{header}: exports: {key} is {constants.get(key)!r}, not {wanted!r}")


def read_constants(root):
    """Returns the run-time registration's string constants, by name."""
    return dict(CONSTANT_RE.findall(read_bytes(root / FILE_TYPES_HEADER).decode("utf-8")))


def check_installer(root, findings):
    """Checks that the Windows uninstaller withdraws each type as the registration names it."""
    constants = read_constants(root)
    script = root / INSTALLER
    text = read_bytes(script).decode("utf-8")
    defines = dict(NSIS_DEFINE_RE.findall(text))

    def expanded(value):
        return NSIS_REFERENCE_RE.sub(lambda match: defines.get(match.group(1), match.group(0)), value)

    withdrawn = [tuple(expanded(value) for value in found) for found in WITHDRAW_RE.findall(text)]
    wanted = [("." + str(constants.get(f"{prefix}_extension")), constants.get(f"{prefix}_program_id"),
               constants.get(f"{prefix}_mime_type"))
              for _type_id, _type_name, _extension, _mime_type, prefix, _conforms_to in TYPES]
    for each in wanted:
        count = withdrawn.count(each)
        if count != 1:
            findings.append(f"{script}: installer: the uninstaller withdraws {each[0]} as {each[1]}, "
                            f"{each[2]} {count} times, not once")
    for each in withdrawn:
        if each not in wanted:
            findings.append(f"{script}: installer: the uninstaller withdraws {each[0]} as {each[1]}, "
                            f"{each[2]}, which is none of the types")


def check_ios_opened(root, findings):
    """Checks that iOS's platform code brings in each type the game installs or adds."""
    source = root / IOS_PLATFORM
    match = OPENED_RE.search(read_bytes(source).decode("utf-8"))
    if match is None:
        findings.append(f"{source}: opened: kOpenedExtensions is not there")
        return
    listed = OPENED_ITEM_RE.findall(match.group(1))
    wanted = [extension for _type_id, _type_name, extension, _mime_type, prefix, _conforms_to in TYPES
              if prefix != LAUNCH_PREFIX]
    if sorted(listed) != sorted(wanted):
        findings.append(f"{source}: opened: kOpenedExtensions is {listed}, not {wanted} in any order")


def check(root, macos_path, ios_path):
    """Returns the findings of every check."""
    findings = []
    macos = read_plist(macos_path)
    ios = read_plist(ios_path)
    for type_id, type_name, extension, mime_type, _prefix, conforms_to in TYPES:
        check_exported_type(str(macos_path), macos, findings, type_id, type_name, extension, mime_type,
                            conforms_to)
        check_document_type(str(macos_path), macos, findings, True, type_id, type_name)
        check_exported_type(str(ios_path), ios, findings, type_id, type_name, extension, mime_type,
                            conforms_to)
        check_document_type(str(ios_path), ios, findings, False, type_id, type_name)
    check_plain_text(str(macos_path), macos, findings)
    if ios.get("LSSupportsOpeningDocumentsInPlace") is not True:
        findings.append(f"{ios_path}: in place: LSSupportsOpeningDocumentsInPlace is not true")
    archives = [entry for entry in ios.get("UTImportedTypeDeclarations", [])
                if isinstance(entry, dict) and entry.get("UTTypeIdentifier") == ARCHIVE_TYPE_ID]
    if len(archives) != 1:
        findings.append(f"{ios_path}: in place: {ARCHIVE_TYPE_ID} is not declared once")
    if macos.get("CFBundleIdentifier") != BUNDLE_ID:
        findings.append(f"{macos_path}: exports: the types are not named under the bundle's identifier")
    check_bundle_icon(root, findings)
    check_constants(root, findings)
    check_installer(root, findings)
    check_ios_opened(root, findings)
    return findings


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("macos_plist", nargs="?", type=Path, default=ROOT / "src" / "app" / "Info.plist.in",
                        help="the macOS bundle's Info.plist template")
    parser.add_argument("ios_plist", nargs="?", type=Path, default=ROOT / "platforms" / "ios" / "Info.plist.in",
                        help="the iOS bundle's Info.plist template")
    parser.add_argument("--root", type=Path, default=ROOT, help="the engine tree")
    arguments = parser.parse_args(argv)
    try:
        findings = check(arguments.root, arguments.macos_plist, arguments.ios_plist)
    except ReadError as error:
        print(f"check_bundle_file_types: {error}", file=sys.stderr)
        return 2
    for finding in findings:
        print(finding, file=sys.stderr)
    if findings:
        print(f"check_bundle_file_types: {len(findings)} finding(s)", file=sys.stderr)
        return 1
    print("check_bundle_file_types: both bundles declare and open .oamod, .oalang, .oamap, "
          ".oamut, .oabrain, .oareg and .oalaunch, macOS takes plain text it is handed, the Windows "
          "uninstaller withdraws all seven and iOS brings in the six the game installs or adds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
