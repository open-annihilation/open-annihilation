#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Build coreprime.ccx and copy the mod profile beside it.

The archive is the HPI v1 format the engine reads for .ccx files, stored
uncompressed with header key 0, so two builds of the same inputs match byte
for byte. Sequences under --art become one animation, anims/BY_New.gaf.
QR_Register is rendered from --registration-url (QR version 3, error
correction M); its sample PNG is checked for size and not copied in. Every
file under --data is stored at its relative path. The war map is not packed.
--profile is copied unchanged to oamod.yaml in --output, next to coreprime.ccx.
"""

import argparse
import csv
import os
from pathlib import Path
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qr import qr_matrix

# Sequences the archive is built from, and the frames each one holds.
SEQUENCES = {
    "Scanner": (8, 72, 72),
    "Locator_Icons": (10, 11, 11),
    "Queue_Meter": (11, 104, 9),
    "QR_Register": (1, 113, 113),
}
QR_SEQUENCE = "QR_Register"
QR_VERSION = 3
QR_SCALE = 3
QR_QUIET = 4
QR_BORDER = 90
QR_LIGHT = 81
QR_DARK = 0
TRANSPARENCY = 9
GAF_PATH = "anims/BY_New.gaf"
ARCHIVE_NAME = "coreprime.ccx"
PROFILE_NAME = "oamod.yaml"
FRAME_COLUMNS = (
    "frame",
    "width",
    "height",
    "origin_x",
    "origin_y",
    "transparency",
    "duration_ticks",
    "duration_sec",
)
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# The 36-byte trailer every archive ends with. The year digits are 1997, the
# year the reader accepts alongside any other four digits.
TRAILER = bytes(
    [
        0x43, 0x6F, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, 0x74,
        0x20, 0x31, 0x39, 0x39, 0x37, 0x20, 0x43, 0x61, 0x76,
        0x65, 0x64, 0x6F, 0x67, 0x20, 0x45, 0x6E, 0x74, 0x65,
        0x72, 0x74, 0x61, 0x69, 0x6E, 0x6D, 0x65, 0x6E, 0x74,
    ]
)


def fail(message):
    """Stops the build with `message`."""
    raise SystemExit(message)


def name_bytes(name):
    """Returns `name` and its terminating NUL, or stops when it is not ASCII."""
    try:
        encoded = name.encode("ascii")
    except UnicodeEncodeError:
        fail(f"archive name is not ASCII: {name}")
    return encoded + b"\x00"


def fold(text):
    """Returns `text` with a-z folded, as archive directory names compare."""
    return "".join(chr(ord(c) - (ord("a") - ord("A"))) if "a" <= c <= "z" else c for c in text)


def same_name(left, right):
    """Returns whether two archive names match, case aside."""
    return fold(left) == fold(right)


class TreeNode:
    """One directory or file in the archive tree."""

    def __init__(self, name, directory, file_index=None):
        self.name = name
        self.directory = directory
        self.file_index = file_index
        self.children = []


def build_tree(paths):
    """Returns the directory tree of `paths`, in the order they are given."""
    root = TreeNode("", True)
    for index, path in enumerate(paths):
        directory = root
        parts = path.split("/")
        if any(part == "" for part in parts):
            fail(f"archive path has an empty segment: {path}")
        for part_index, part in enumerate(parts):
            if part_index == len(parts) - 1:
                directory.children.append(TreeNode(part, False, index))
                break
            found = None
            for child in directory.children:
                if child.directory and same_name(child.name, part):
                    found = child
                    break
            if found is None:
                found = TreeNode(part, True)
                directory.children.append(found)
            directory = found
    return root


def sort_children(node):
    """Sorts each directory's children by name, so the block does not follow visit order."""
    node.children.sort(key=lambda child: child.name)
    for child in node.children:
        if child.directory:
            sort_children(child)


def directory_size(node):
    """Returns how many bytes emitting `node` appends."""
    size = 8 + 9 * len(node.children)
    for child in node.children:
        size += len(name_bytes(child.name))
        size += directory_size(child) if child.directory else 9
    return size


def emit_directory(out, node, data_offsets, sizes):
    """Appends `node` and its children. File records point at `data_offsets`."""
    at = len(out)
    list_at = at + 8
    out.extend(b"\x00" * (8 + 9 * len(node.children)))
    struct.pack_into("<I", out, at, len(node.children))
    struct.pack_into("<I", out, at + 4, list_at)
    for index, child in enumerate(node.children):
        entry = list_at + index * 9
        struct.pack_into("<I", out, entry, len(out))
        out.extend(name_bytes(child.name))
        struct.pack_into("<I", out, entry + 4, len(out))
        out[entry + 8] = 1 if child.directory else 0
        if child.directory:
            emit_directory(out, child, data_offsets, sizes)
            continue
        record = len(out)
        out.extend(b"\x00" * 9)
        struct.pack_into("<I", out, record, data_offsets[child.file_index])
        struct.pack_into("<I", out, record + 4, sizes[child.file_index])
        out[record + 8] = 0


def write_hpi(files):
    """Returns an HPI v1 archive of `(path, bytes)` pairs, stored and unencrypted.

    File payloads follow `files`. Directory children are ordered by name.
    """
    if len(TRAILER) != 36:
        fail("archive trailer is not 36 bytes")
    paths = [path for path, _ in files]
    folded = [fold(path) for path in paths]
    if len(folded) != len(set(folded)):
        fail("archive paths are not unique apart from case")
    root = build_tree(paths)
    sort_children(root)
    directory_end = 20 + directory_size(root)
    offsets = []
    cursor = directory_end
    for _, blob in files:
        offsets.append(cursor)
        cursor += len(blob)
    out = bytearray(20)
    out[0:4] = b"HAPI"
    struct.pack_into("<I", out, 4, 0x00010000)
    struct.pack_into("<I", out, 8, directory_end)
    struct.pack_into("<I", out, 12, 0)
    struct.pack_into("<I", out, 16, 20)
    emit_directory(out, root, offsets, [len(blob) for _, blob in files])
    if len(out) != directory_end:
        fail(f"directory block is {len(out)} bytes, expected {directory_end}")
    for _, blob in files:
        out.extend(blob)
    if len(out) != cursor:
        fail(f"file data ends at {len(out)}, expected {cursor}")
    out.extend(TRAILER)
    return bytes(out)


def paeth(left, up, upper_left):
    """Returns the Paeth predictor of the three neighbouring bytes."""
    estimate = left + up - upper_left
    distance_left = abs(estimate - left)
    distance_up = abs(estimate - up)
    distance_upper_left = abs(estimate - upper_left)
    if distance_left <= distance_up and distance_left <= distance_upper_left:
        return left
    if distance_up <= distance_upper_left:
        return up
    return upper_left


def unfilter(raw, width, height):
    """Returns palette indices from the filtered scanlines of an 8-bit indexed PNG."""
    if len(raw) != height * (width + 1):
        fail(f"PNG image data is {len(raw)} bytes, expected {height * (width + 1)}")
    out = bytearray(width * height)
    for y in range(height):
        row = raw[y * (width + 1) : (y + 1) * (width + 1)]
        kind = row[0]
        source = row[1:]
        at = y * width
        for x in range(width):
            left = out[at + x - 1] if x else 0
            up = out[at + x - width] if y else 0
            upper_left = out[at + x - width - 1] if x and y else 0
            value = source[x]
            if kind == 0:
                index = value
            elif kind == 1:
                index = (value + left) & 255
            elif kind == 2:
                index = (value + up) & 255
            elif kind == 3:
                index = (value + ((left + up) // 2)) & 255
            elif kind == 4:
                index = (value + paeth(left, up, upper_left)) & 255
            else:
                fail(f"PNG row filter {kind} is not one of 0..4")
            out[at + x] = index
    return out


def read_png_indices(path):
    """Returns `(width, height, palette_size, indices)` of an 8-bit indexed PNG."""
    data = Path(path).read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        fail(f"{path}: not a PNG")
    pos = len(PNG_SIGNATURE)
    width = height = None
    palette_size = None
    saw_transparency = False
    image = bytearray()
    while pos + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        pos += 4
        kind = data[pos : pos + 4]
        pos += 4
        chunk = data[pos : pos + length]
        if len(chunk) != length or pos + length + 4 > len(data):
            fail(f"{path}: truncated PNG chunk")
        pos += length
        (crc,) = struct.unpack_from(">I", data, pos)
        pos += 4
        if crc != (zlib.crc32(kind + chunk) & 0xFFFFFFFF):
            fail(f"{path}: PNG chunk checksum does not match")
        if kind == b"IHDR":
            if len(chunk) != 13:
                fail(f"{path}: PNG header is the wrong size")
            width, height, bit, colour, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunk)
            if (bit, colour, compression, filtering, interlace) != (8, 3, 0, 0, 0):
                fail(f"{path}: PNG is not an 8-bit indexed image")
            if width <= 0 or height <= 0:
                fail(f"{path}: PNG size is empty")
        elif kind == b"PLTE":
            if len(chunk) == 0 or len(chunk) % 3 != 0:
                fail(f"{path}: PNG palette is empty or not a list of colours")
            palette_size = len(chunk) // 3
        elif kind == b"tRNS":
            saw_transparency = True
        elif kind == b"IDAT":
            image.extend(chunk)
        elif kind == b"IEND":
            break
    else:
        fail(f"{path}: PNG ends without its end chunk")
    if width is None or palette_size is None or not saw_transparency:
        fail(f"{path}: PNG is missing its header, palette or transparency")
    if len(image) == 0:
        fail(f"{path}: PNG has no image data")
    try:
        raw = zlib.decompress(bytes(image))
    except zlib.error as error:
        fail(f"{path}: PNG image data does not decompress ({error})")
    indices = unfilter(raw, width, height)
    if any(index >= palette_size for index in indices):
        fail(f"{path}: a pixel index is outside the palette")
    return width, height, indices


def integer_field(row, column, path, line):
    """Returns the integer in `column` of a frame-table row."""
    text = row.get(column, "")
    try:
        return int(text, 10)
    except ValueError:
        fail(f"{path}:{line}: {column} is not an integer")


def load_frame_table(path):
    """Returns the frame rows of `path`, in frame order from 0."""
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None or tuple(reader.fieldnames) != FRAME_COLUMNS:
            fail(f"{path}: columns must be {','.join(FRAME_COLUMNS)}")
        rows = []
        for line, row in enumerate(reader, start=2):
            rows.append(
                {
                    "frame": integer_field(row, "frame", path, line),
                    "width": integer_field(row, "width", path, line),
                    "height": integer_field(row, "height", path, line),
                    "origin_x": integer_field(row, "origin_x", path, line),
                    "origin_y": integer_field(row, "origin_y", path, line),
                    "transparency": integer_field(row, "transparency", path, line),
                    "duration": integer_field(row, "duration_ticks", path, line),
                }
            )
    rows.sort(key=lambda row: row["frame"])
    expected = list(range(len(rows)))
    if [row["frame"] for row in rows] != expected:
        fail(f"{path}: frame numbers must be 0..{len(rows) - 1} with none repeated")
    return rows


def render_qr(text):
    """Returns the side and palette indices of the registration code for `text`."""
    if text == "":
        fail("registration address is empty")
    try:
        matrix = qr_matrix(text, QR_VERSION)
    except ValueError as error:
        fail(f"registration address: {error}")
    modules = len(matrix)
    if modules == 0 or any(len(row) != modules for row in matrix):
        fail("registration code matrix is not square")
    side = (modules + 2 * QR_QUIET) * QR_SCALE + 2
    pixels = bytearray([QR_LIGHT]) * (side * side)
    for index in range(side):
        pixels[index] = QR_BORDER
        pixels[(side - 1) * side + index] = QR_BORDER
        pixels[index * side] = QR_BORDER
        pixels[index * side + (side - 1)] = QR_BORDER
    for y, row in enumerate(matrix):
        for x, dark in enumerate(row):
            if not dark:
                continue
            origin_x = 1 + (x + QR_QUIET) * QR_SCALE
            origin_y = 1 + (y + QR_QUIET) * QR_SCALE
            for dy in range(QR_SCALE):
                for dx in range(QR_SCALE):
                    pixels[(origin_y + dy) * side + origin_x + dx] = QR_DARK
    return side, pixels


def load_sequence(directory, registration_url):
    """Returns one animation sequence from `directory`'s frame table and PNGs."""
    name = directory.name
    expected = SEQUENCES.get(name)
    if expected is None:
        fail(f"{directory}: not one of the archive's sequences")
    table = load_frame_table(directory / "frames.csv")
    count, width, height = expected
    if len(table) != count:
        fail(f"{directory}: expected {count} frames, found {len(table)}")
    qr_side = None
    qr_pixels = None
    if name == QR_SEQUENCE:
        # The sample PNG shows the default address. The frame is rendered
        # from the address this build was given.
        qr_side, qr_pixels = render_qr(registration_url)
    frames = []
    for row in table:
        if (row["width"], row["height"]) != (width, height):
            fail(f"{directory}: frame {row['frame']} is {row['width']}x{row['height']}, expected {width}x{height}")
        if row["transparency"] != TRANSPARENCY:
            fail(f"{directory}: frame {row['frame']} transparency is not {TRANSPARENCY}")
        if not 0 <= row["duration"] <= 0xFFFF:
            fail(f"{directory}: frame {row['frame']} duration does not fit in 16 bits")
        if not -32768 <= row["origin_x"] <= 32767 or not -32768 <= row["origin_y"] <= 32767:
            fail(f"{directory}: frame {row['frame']} origin does not fit in 16 bits")
        png = directory / f"{row['frame']}.png"
        if not png.is_file():
            fail(f"{png}: missing")
        png_width, png_height, indices = read_png_indices(png)
        if (png_width, png_height) != (width, height):
            fail(f"{png}: is {png_width}x{png_height}, expected {width}x{height}")
        pixels = indices
        if name == QR_SEQUENCE:
            if qr_side != width or qr_side != height:
                fail(f"{directory}: rendered registration code is {qr_side}px, frame is {width}x{height}")
            pixels = qr_pixels
        frames.append(
            {
                "width": width,
                "height": height,
                "origin_x": row["origin_x"],
                "origin_y": row["origin_y"],
                "transparency": row["transparency"],
                "duration": row["duration"],
                "pixels": pixels,
            }
        )
    return {"name": name, "frames": frames}


def load_sequences(art_dir, registration_url):
    """Returns the sequences under `art_dir`, in name order."""
    if not art_dir.is_dir():
        fail(f"{art_dir}: not a directory")
    found = []
    for entry in art_dir.iterdir():
        if not entry.is_dir() or entry.name.startswith("."):
            continue
        if not (entry / "frames.csv").is_file():
            continue
        found.append(load_sequence(entry, registration_url))
    names = sorted(sequence["name"] for sequence in found)
    if names != sorted(SEQUENCES):
        fail(f"{art_dir}: sequences are {names}, expected {sorted(SEQUENCES)}")
    found.sort(key=lambda sequence: sequence["name"])
    return found


def gaf_bytes(sequences):
    """Returns one animation file holding `sequences` as uncompressed frames."""
    body = bytearray(12 + 4 * len(sequences))
    struct.pack_into("<I", body, 0, 0x00010100)
    struct.pack_into("<I", body, 4, len(sequences))
    for index, sequence in enumerate(sequences):
        struct.pack_into("<I", body, 12 + index * 4, len(body))
        encoded = name_bytes(sequence["name"])
        if len(encoded) > 32:
            fail(f"sequence name does not fit in 32 bytes: {sequence['name']}")
        header = bytearray(40)
        struct.pack_into("<H", header, 0, len(sequence["frames"]))
        header[8 : 8 + len(encoded) - 1] = encoded[:-1]
        body.extend(header)
        list_at = len(body)
        frames = sequence["frames"]
        body.extend(b"\x00" * (8 * len(frames)))
        for frame_index, frame in enumerate(frames):
            record_at = len(body)
            struct.pack_into("<I", body, list_at + frame_index * 8, record_at)
            struct.pack_into("<I", body, list_at + frame_index * 8 + 4, frame["duration"])
            body.extend(b"\x00" * 24)
            struct.pack_into("<H", body, record_at, frame["width"])
            struct.pack_into("<H", body, record_at + 2, frame["height"])
            struct.pack_into("<h", body, record_at + 4, frame["origin_x"])
            struct.pack_into("<h", body, record_at + 6, frame["origin_y"])
            body[record_at + 8] = frame["transparency"]
            pixel_count = frame["width"] * frame["height"]
            if len(frame["pixels"]) != pixel_count:
                fail(f"{sequence['name']} frame {frame_index} has {len(frame['pixels'])} pixels, expected {pixel_count}")
            struct.pack_into("<I", body, record_at + 16, len(body))
            body.extend(frame["pixels"])
    return bytes(body)


def archive_path_of(root, path):
    """Returns the archive path of a file under `root`, or stops on a path that will not pack."""
    relative = path.relative_to(root).as_posix()
    parts = relative.split("/")
    if any(part in ("", ".", "..") or "\\" in part for part in parts):
        fail(f"{path}: archive path must be relative, with no . or .. segment")
    if any(part.startswith(".") or part == "__pycache__" for part in parts):
        return None
    return relative


def data_files(root):
    """Returns `(archive path, bytes)` for every file packed from `root`."""
    if not root.is_dir():
        fail(f"{root}: not a directory")
    packed = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        archive_path = archive_path_of(root, path)
        if archive_path is None:
            continue
        packed.append((archive_path, path.read_bytes()))
    if not packed:
        fail(f"{root}: nothing to pack")
    return packed


def atomic_write(path, blob):
    """Writes `blob` to `path`, replacing an existing file only when the write finishes."""
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as handle:
            handle.write(blob)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        os.chmod(path, 0o644)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def build(art, data, profile, registration_url, output):
    """Writes coreprime.ccx and oamod.yaml into `output`."""
    sequences = load_sequences(art, registration_url)
    files = [(GAF_PATH, gaf_bytes(sequences))]
    files.extend(data_files(data))
    if not profile.is_file():
        fail(f"{profile}: profile is missing")
    files.sort(key=lambda item: item[0])
    archive = write_hpi(files)
    output.mkdir(parents=True, exist_ok=True)
    atomic_write(output / ARCHIVE_NAME, archive)
    atomic_write(output / PROFILE_NAME, profile.read_bytes())


def main(argv):
    """Builds the archive from the arguments `--help` lists."""
    parser = argparse.ArgumentParser(
        description=(
            "Build coreprime.ccx, the archive the mod mounts over the install, "
            "and copy the mod profile beside it. Both are written into --output. "
            "Inputs are --art (sequence directories of frames.csv and 8-bit indexed "
            "PNGs: Scanner, Locator_Icons, Queue_Meter, QR_Register), --data (every "
            "file packed at its relative path), --profile (oamod.yaml, copied "
            "unchanged), and --registration-url (drawn as the QR_Register frame at "
            "QR version 3, error correction M; the sample PNG is not copied). The "
            "war map is not packed."
        )
    )
    parser.add_argument("--art", required=True, type=Path, help="directory of sequence folders, each with frames.csv and indexed PNGs")
    parser.add_argument("--data", required=True, type=Path, help="directory packed at the archive root; each file keeps its relative path")
    parser.add_argument("--profile", required=True, type=Path, help="mod profile copied beside the archive as oamod.yaml")
    parser.add_argument(
        "--registration-url",
        required=True,
        help="address rendered as the QR_Register frame (QR version 3, error correction M)",
    )
    parser.add_argument("--output", required=True, type=Path, help="directory that receives coreprime.ccx and oamod.yaml")
    args = parser.parse_args(argv)
    build(args.art, args.data, args.profile, args.registration_url, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
