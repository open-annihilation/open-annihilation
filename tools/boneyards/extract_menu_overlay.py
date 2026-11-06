#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Write the main-menu overlay graphic as a C++ byte array.

Reads anims/Main_Console.gaf from an installed by.ccx, or an already
extracted GAF, and keeps only the sequences the main-menu status widget
draws. Every frame of each sequence is copied raw. A compressed frame or a
frame with layers stops the script. The build does not read game data: run
this and commit the generated file.
"""

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# The sequences the status widget draws, and nothing else. Order is the
# order they are written, which is the order they appear in the source.
WANTED = (
    "Activity_Light",
    "Main_Menu_Overlay",
    "MM_Enter_Button",
    "MM_Single_Button",
    "MM_Multi_Button",
    "MM_Intro_Button",
    "MM_Exit_Button",
)

ENTRY = "anims/Main_Console.gaf"


def fail(message):
    print("extract_menu_overlay: " + message, file=sys.stderr)
    return 1


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_source(args):
    """The GAF bytes, extracted when a by.ccx is given."""
    if args.gaf is not None:
        return Path(args.gaf).read_bytes()
    if args.by_ccx is None:
        raise SystemExit(fail("pass --gaf or --by-ccx"))
    tool = args.tool
    if tool is None:
        found = shutil.which("oa-tool")
        tool = found if found is not None else "build/oa-tool"
    destination = Path(args.extract_to)
    destination.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [tool, "extract", str(args.by_ccx), ENTRY, str(destination)],
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        raise SystemExit(fail("oa-tool extract failed"))
    return destination.read_bytes()


def sequence_name(raw):
    return raw.split(b"\0", 1)[0].decode("latin1")


def parse_archive(data):
    """Every sequence, with each simple frame's record, duration and pixels."""
    if len(data) < 12:
        raise SystemExit(fail("the GAF is shorter than its header"))
    version, count, reserved = struct.unpack_from("<III", data, 0)
    total = count & 0xFFFF
    if 12 + total * 4 > len(data):
        raise SystemExit(fail("the sequence table runs past the file"))
    sequences = []
    for index in range(total):
        pointer = u32(data, 12 + index * 4)
        if pointer + 40 > len(data):
            raise SystemExit(fail(f"sequence {index} is past the file"))
        frame_count = u16(data, pointer)
        repeat = u16(data, pointer + 2)
        seq_reserved = u32(data, pointer + 4)
        name_raw = data[pointer + 8 : pointer + 40]
        frames = []
        table = pointer + 40
        if table + frame_count * 8 > len(data):
            raise SystemExit(fail(f"{sequence_name(name_raw)} frame table is past the file"))
        for frame_index in range(frame_count):
            item = table + frame_index * 8
            frame_at = u32(data, item)
            duration = u32(data, item + 4)
            if frame_at + 24 > len(data):
                raise SystemExit(fail("a frame record is past the file"))
            record = data[frame_at : frame_at + 24]
            width = u16(record, 0)
            height = u16(record, 2)
            compressed = record[9]
            layers = record[10]
            if compressed != 0 or layers != 0:
                raise SystemExit(
                    fail(
                        f"{sequence_name(name_raw)} frame {frame_index} is compressed "
                        "or has layers; this script copies raw frames only"
                    )
                )
            pixels_at = u32(record, 16)
            size = width * height
            if pixels_at + size > len(data):
                raise SystemExit(fail(f"{sequence_name(name_raw)} frame {frame_index} is short"))
            pixels = data[pixels_at : pixels_at + size]
            frames.append((record, duration, pixels, width, height))
        sequences.append((sequence_name(name_raw), frame_count, repeat, seq_reserved, name_raw, frames))
    return version, reserved, sequences


def build(version, reserved, sequences):
    """A GAF of the wanted sequences, and a list of what each frame is."""
    chosen = [seq for seq in sequences if seq[0] in WANTED]
    found = [seq[0] for seq in chosen]
    missing = [name for name in WANTED if name not in found]
    if missing:
        raise SystemExit(fail("missing sequences: " + ", ".join(missing)))
    extra = [name for name in found if found.count(name) > 1]
    if extra:
        raise SystemExit(fail("a sequence is listed twice: " + ", ".join(extra)))
    # File order, which is the widget's source order for this install.
    order = {name: index for index, name in enumerate(WANTED)}
    chosen.sort(key=lambda seq: order[seq[0]])

    out = bytearray()
    out += struct.pack("<III", version, len(chosen), reserved)
    pointer_at = len(out)
    out += b"\0" * (4 * len(chosen))
    report = []
    for index, (name, frame_count, repeat, seq_reserved, name_raw, frames) in enumerate(chosen):
        struct.pack_into("<I", out, pointer_at + index * 4, len(out))
        out += struct.pack("<HHI", frame_count, repeat, seq_reserved)
        out += name_raw
        list_at = len(out)
        out += b"\0" * (8 * frame_count)
        described = []
        for frame_index, (record, duration, pixels, width, height) in enumerate(frames):
            struct.pack_into("<II", out, list_at + frame_index * 8, len(out), duration)
            record = bytearray(record)
            struct.pack_into("<I", record, 16, len(out) + 24)
            out += record
            out += pixels
            described.append((frame_index, width, height, len(pixels)))
        report.append((name, described))
    return bytes(out), report


def self_check(source_sequences, built):
    """The rebuilt frames' raw pixels match the source."""
    _version, _reserved, parsed = parse_archive(built)
    source = {seq[0]: seq for seq in source_sequences}
    if [seq[0] for seq in parsed] != [name for name in WANTED]:
        raise SystemExit(fail("the rebuilt sequence order changed"))
    for name, frame_count, repeat, seq_reserved, name_raw, frames in parsed:
        original = source[name]
        if frame_count != original[1] or repeat != original[2] or seq_reserved != original[3]:
            raise SystemExit(fail(f"{name} header changed"))
        if name_raw != original[4]:
            raise SystemExit(fail(f"{name} name bytes changed"))
        if len(frames) != len(original[5]):
            raise SystemExit(fail(f"{name} frame count changed"))
        for index, (record, duration, pixels, width, height) in enumerate(frames):
            src_record, src_duration, src_pixels, src_width, src_height = original[5][index]
            if duration != src_duration or pixels != src_pixels or width != src_width or height != src_height:
                raise SystemExit(fail(f"{name} frame {index} pixels differ"))
            # The record matches apart from the data offset, which the rebuild moved.
            if record[:16] != src_record[:16] or record[20:] != src_record[20:]:
                raise SystemExit(fail(f"{name} frame {index} record changed"))


def emit(built, report, source_note, header_path, source_path):
    lines = []
    for name, frames in report:
        parts = [f"frame {index} {width}x{height}" for index, width, height, _size in frames]
        lines.append(f"// {name}: " + "; ".join(parts))
    body = ",\n".join(
        "    " + ", ".join(f"0x{byte:02x}" for byte in built[index : index + 16])
        for index in range(0, len(built), 16)
    )
    header = """// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main-menu overlay graphic, generated by tools/boneyards/extract_menu_overlay.py.
// Do not edit by hand. Run that script again and commit what it writes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::boneyards::console {

/// Returns the main-menu overlay graphic embedded in the game.
///
/// The bytes are a GAF of the sequences the main-menu strip draws. Those
/// sequences are the same pixels as the expansion's archive. The strip reads
/// this graphic for them, so the menu does not decode the rest of that
/// archive. A sequence this graphic does not hold is read from the archive
/// once the player needs it.
///
/// @return the file's bytes; valid for the process
[[nodiscard]] std::span<const uint8_t> menu_overlay_art() noexcept;

} // namespace oa::boneyards::console
"""
    source = f"""// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Generated by tools/boneyards/extract_menu_overlay.py. Do not edit by hand.
// Source: {source_note}
// The maintainer approved embedding this art on 2026-11-02.
// Sequences and frames:
{chr(10).join(lines)}
#include "oa/boneyards/console/menu_overlay_art.hpp"

namespace oa::boneyards::console {{
namespace {{

constexpr uint8_t k_menu_overlay_art[] = {{
{body}
}};

}} // namespace

std::span<const uint8_t> menu_overlay_art() noexcept {{
    return k_menu_overlay_art;
}}

}} // namespace oa::boneyards::console
"""
    Path(header_path).write_text(header, encoding="utf-8")
    Path(source_path).write_text(source, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gaf", type=Path, help="an already extracted Main_Console.gaf")
    parser.add_argument("--by-ccx", type=Path, help="an installed by.ccx to extract from")
    parser.add_argument("--tool", help="oa-tool used to extract; otherwise PATH or build/oa-tool")
    parser.add_argument(
        "--extract-to",
        type=Path,
        default=Path("/tmp/E01-extract-Main_Console.gaf"),
        help="where an extracted GAF is written; it is not committed",
    )
    parser.add_argument(
        "--header",
        type=Path,
        default=Path("src/boneyards/console/include/oa/boneyards/console/menu_overlay_art.hpp"),
    )
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("src/boneyards/console/src/menu_overlay_art.cpp"),
    )
    parser.add_argument(
        "--source-note",
        default="anims/Main_Console.gaf inside by.ccx of the BY1117a install (OA_GAME_DIR)",
    )
    args = parser.parse_args()
    data = read_source(args)
    version, reserved, sequences = parse_archive(data)
    built, report = build(version, reserved, sequences)
    self_check(sequences, built)
    emit(built, report, args.source_note, args.header, args.source)
    print(f"wrote {len(built)} bytes, {len(report)} sequences")
    for name, frames in report:
        print(f"  {name}: {len(frames)} frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
