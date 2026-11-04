#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Print content-bundle table rows for files in an install folder.

Reads only the files named by --file and the files under each --under
directory. Skips a file whose last name is in --skip, or whose extension is
in --skip-ext (compared without regard to case). Prints constexpr source
names, digests and ContentFile rows, or, with --fixture, the name, size and
hash prefix a unit test compares. Writes no file and prints no file bytes.
"""

import argparse
import hashlib
import sys
from pathlib import Path


def identifier(relative):
    """A C identifier: every character that is not a letter or digit becomes '_'."""
    return "".join(character if character.isalnum() else "_" for character in relative)


def collect(root, files, folders, skip_names, skip_extensions):
    """The relative paths to print, named files first, then each folder in order.

    @param root the install folder
    @param files paths relative to root, in the order given
    @param folders directories relative to root
    @param skip_names last names to leave out, already folded
    @param skip_extensions extensions to leave out, including the dot, already folded
    @return (relative posix path, size, sha256 hex) in that order
    """
    chosen = []
    seen = set()

    def take(relative):
        if relative in seen:
            return
        last = Path(relative).name.casefold()
        extension = Path(relative).suffix.casefold()
        if last in skip_names or extension in skip_extensions:
            return
        full = root / relative
        if not full.is_file() or full.is_symlink():
            sys.exit(f"bundle_table_entries: not a file: {full}")
        digest = hashlib.sha256(full.read_bytes()).hexdigest()
        chosen.append((relative, full.stat().st_size, digest))
        seen.add(relative)

    for name in files:
        take(Path(name).as_posix())
    for folder in folders:
        base = root / folder
        if not base.is_dir() or base.is_symlink():
            sys.exit(f"bundle_table_entries: not a directory: {base}")
        found = []
        for path in base.rglob("*"):
            if path.is_symlink() or not path.is_file():
                continue
            found.append(path.relative_to(root).as_posix())
        for relative in sorted(found):
            take(relative)
    return chosen


def print_table(rows, array_name):
    """Prints the constexpr arrays and the ContentFile array."""
    idents = []
    for relative, _size, _digest in rows:
        ident = identifier(relative)
        if ident in idents:
            sys.exit(f"bundle_table_entries: identifier {ident} is used twice")
        idents.append(ident)
    pieces = []
    for (relative, _size, digest), ident in zip(rows, idents):
        pieces.append(f'constexpr std::string_view k_src_{ident}[]{{"{relative}"}};')
        pieces.append(
            "constexpr oa::base::sha256::Digest k_dig_"
            f'{ident}[]{{oa::base::sha256::parse_hex(\n    "{digest}").value()}};'
        )
    pieces.append(f"constexpr ContentFile k_files_{array_name}[]{{")
    for (relative, size, _digest), ident in zip(rows, idents):
        pieces.append(
            f'    ContentFile{{"{relative}", k_src_{ident}, {size}, k_dig_{ident}}},'
        )
    pieces.append("};")
    sys.stdout.write("\n".join(pieces) + "\n")


def print_fixture(rows, prefix):
    """Prints one brace row per file: name, size and the first prefix hex digits."""
    for relative, size, digest in rows:
        sys.stdout.write(f'        {{"{relative}", {size}, "{digest[:prefix]}"}},\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, help="the install folder")
    parser.add_argument("--file", action="append", default=[], help="one file, relative to --root")
    parser.add_argument(
        "--under", action="append", default=[], help="one directory, relative to --root"
    )
    parser.add_argument("--skip", action="append", default=[], help="a last name to leave out")
    parser.add_argument(
        "--skip-ext", action="append", default=[], help="an extension to leave out, with or without the dot"
    )
    parser.add_argument("--array", default="extra", help="the ContentFile array's suffix")
    parser.add_argument(
        "--fixture", action="store_true", help="print name, size and hash prefix rows for a test"
    )
    parser.add_argument("--prefix", type=int, default=16, help="hex digits in a fixture prefix")
    args = parser.parse_args()
    if args.prefix < 1 or args.prefix > 64:
        sys.exit("bundle_table_entries: --prefix must be from 1 to 64")
    skip_extensions = set()
    for extension in args.skip_ext:
        folded = extension.casefold()
        if not folded.startswith("."):
            folded = "." + folded
        skip_extensions.add(folded)
    rows = collect(
        Path(args.root),
        args.file,
        args.under,
        {name.casefold() for name in args.skip},
        skip_extensions,
    )
    if not rows:
        sys.exit("bundle_table_entries: no files")
    if args.fixture:
        print_fixture(rows, args.prefix)
    else:
        print_table(rows, args.array)
    print(f"bundle_table_entries: {len(rows)} files", file=sys.stderr)


if __name__ == "__main__":
    main()
