# Zip archives

This module reads and writes zip archives in memory, reads them as a stream
from a file, and writes them as a stream through a positioned hook. A
director bundle (`.oamovie`) is a zip archive of one `.oascript` and the
recording it names: the application opens a bundle with the in-memory
reader and makes one with the in-memory writer. A mod package (`.oamod`), a
language pack (`.oalang`) or a map pack (`.oamap`) is a zip archive of any
size: the application unpacks it with the streamed reader and a tool makes
it with the streaming writer. The module opens no files itself; the caller
hands it the archive's bytes, or a hook that reads or writes them.

## Entry points

`oa/formats/zip.hpp`, namespace `oa::formats::zip`:

- `read_directory` finds the end record, walks the central directory and
  checks every entry's local header against its central record.
- `find_entry` looks an entry up by its exact name.
- `read_entry` reads one entry's data, inflating it when it is deflated,
  and checks its size and CRC-32.
- `write_archive` writes entries stored, in the order given.
- `name_is_safe` is the name rule both directions apply; `crc32_of` is the
  format's CRC-32; `zip_status_message` describes a status.

`oa/formats/zip/stream.hpp`, the same namespace:

- `read_stream_directory` reads an archive's central directory through a
  `SourceHooks` read hook, within `StreamLimits`.
- `EntryStream` reads one entry's data in steps of a budget of archive
  bytes, handing each piece to a `SinkHooks` write hook, and checks its size
  and CRC-32 at its end.
- `read_stream_entry` reads a small entry whole into memory.

`oa/formats/zip/inflate.hpp`, the same namespace:

- `Inflater` inflates one raw deflate stream, with no zlib or gzip wrapper,
  a piece at a time. The zip readers use it, and so does a format whose own
  wrapper sits in front of the same deflate.

`oa/formats/zip/writer.hpp`, the same namespace:

- `StreamWriter` writes an archive through an `OutputHooks` write hook.
  `begin_entry` starts one stored or deflated entry of a declared size,
  `write` takes its bytes, `end_entry` completes its local header,
  `add_folder` writes a folder and `finish` writes the central directory
  and the end record. `bytes_written` is the archive's size so far.
- `WriterOptions` chooses the deflate level (9 by default) and `Zip64`:
  `when_needed` or `always`.

Every function returns its errors as values (`ZipError`: a status, the byte
offset of the record at fault and the entry's name) and leaves its output
empty on failure. A streaming call that fails leaves the archive unfinished.

## What the reader takes

One archive on one disk, without the 64-bit extension, of at most
`max_archive_bytes`, with at most `max_entry_count` entries, each stored
(method 0) or deflated (method 8) and at most `max_entry_bytes` once read.
The end record is the last one in the final 65,557 bytes whose comment
reaches exactly to the end of the archive, so a comment may hold anything.
Extra fields and comments of entries are skipped. An entry whose name ends
in '/' names a directory and holds nothing.

It refuses, with the status named:

- disk numbers other than zero, or a disk's entry count unlike the total
  (`several_disks`);
- any 16-bit field holding 0xFFFF or 32-bit field holding 0xFFFFFFFF that
  the 64-bit extension would replace, in the end record, a central record or
  a local header (`zip64`);
- the encryption flags, in either record (`encrypted`);
- methods other than stored and deflated (`unsupported_method`);
- names `name_is_safe` refuses: empty, starting with '/' or a drive letter
  and colon, holding NUL or '\\', with an empty, "." or ".." component, or
  ending in a name Windows keeps for a device, such as `con.txt` or `NUL`
  (`unsafe_name`); names longer than `max_name_bytes` (`name_too_long`); a
  name given twice, compared byte for byte (`duplicate_name`);
- a local header without its signature, or whose name, method, CRC-32 or
  sizes differ from its central record (`bad_local_record`). When the local
  header's data-descriptor flag is set, its CRC-32 and sizes may also be
  zero, as a writer that cannot seek leaves them;
- a stored entry whose two sizes differ, a directory that holds data, or a
  deflated entry that claims more than 1032 bytes for each byte of its data,
  more than any deflate stream holds (`size_mismatch`). This last rule
  refuses an entry before its buffer is allocated;
- a central directory whose records do not fill it exactly
  (`bad_central_record`), and any record or data that runs past the
  directory or the archive (`truncated`).

Deflated data is inflated as a raw stream into a buffer of exactly the
recorded size; the stream must end there and use all of the recorded
compressed data (`size_mismatch` otherwise), and data that is not deflate
is `inflate_failed`. The data's CRC-32 must be the recorded one
(`crc_mismatch`).

## What the streamed reader takes

What the in-memory reader takes, of any size up to `StreamLimits` (64 GiB,
65,535 entries, an 8 MiB directory, 4 GiB an entry), and the 64-bit
extension: the 64-bit end record its locator names, whose counts, size and
offset must equal the end record's own wherever those are not sentinels,
and each record's 64-bit extra field for exactly the fields that hold their
sentinel. It holds, besides the directory, about 44 KiB of decoder state and
two 64 KiB buffers for an open entry, and nothing that grows with an entry's
size: each step reads at most its budget of the archive and hands on at most
four times as much, or 256 KiB.

Besides the in-memory reader's refusals it refuses:

- 64-bit records that are missing where a field holds a sentinel, or that
  disagree with the end record (`bad_zip64_record`);
- a directory larger than its limit (`directory_too_large`);
- an entry whose local header, name and data reach the next entry's local
  header or the directory, as the central record's name gives them and again
  as the local header's own name and extra field give them
  (`overlapping_entries`): no two entries share data;
- a name marked UTF-8 (general purpose bit 11) that is not well-formed UTF-8
  (`bad_name_encoding`);
- a read that fails (`read_failed`), and a write hook that refuses the data
  (`write_failed`).

A name not marked UTF-8 is read as code page 437, the format's default,
unless a Unicode path extra field (0x7075) whose CRC-32 matches the recorded
name gives its UTF-8 spelling. The backslashes of an entry made on MS-DOS or
Windows (hosts 0, 10 and 14) become '/'. An entry made on Unix or macOS
(hosts 3 and 19) whose external attributes give the link file type is
marked `symbolic_link`, and one of another file type than a file or a
folder `special_file`; the reader refuses neither, and the caller decides.
Such an entry's `unix_mode` is the file type and permission bits it
records, the high 16 bits of its external attributes; an entry made on
another system has 0 there.
Inflated data past an entry's recorded size is never handed on: its first
piece stops the read with `size_mismatch`.

## What the writer makes

Entries stored uncompressed, each with version 2.0 needed and made by,
00:00 on 1980-01-01, no extra field, no comment and zero attributes; the
UTF-8 flag (bit 11) is set only for a name with a byte outside 7-bit ASCII.
Local headers and data come first, in the order given, then the central
directory and an end record with no comment. The same entries always give
the same bytes on every platform. The writer applies the reader's limits and
name rule, and refuses a directory entry with data.

## What the streaming writer makes

The same fixed fields as the in-memory writer: version 2.0 needed and made
by (4.5 when the entry uses the 64-bit extension), 00:00 on 1980-01-01, no
extra field but the 64-bit one, no comment, no data descriptor and zero
external attributes. The UTF-8 flag is set only for a name with a byte
outside 7-bit ASCII. Local headers and data come first, in the order the
entries were begun, then the central directory and an end record with no
comment. Each local header is written with a zero CRC-32 and zero sizes,
then those fields are written back in place when the entry ends, so every
unzipper reads them from the header.

An entry's local header carries the 64-bit extra field (the size, then the
stored size) when `Zip64::always` is set, when its declared size is at
least `0xF0000000`, or when the entry starts at or past `0xFFFFFFFF`. The
central record then carries the sentinel in each 32-bit size and offset and
the 64-bit values beside them. The end of the archive gains the 64-bit end
record and its locator when `Zip64::always` is set, when the entry count
reaches `0xFFFF`, or when the directory's offset or size reaches
`0xFFFFFFFF`.

A deflated entry uses zlib's raw deflate:
`deflateInit2(level 9, Z_DEFLATED, -15, memLevel 8, Z_DEFAULT_STRATEGY)`,
unless `WriterOptions::deflate_level` says another level. The same zlib
gives the same bytes; another deflate implementation may not. The tests pin
exact bytes only for stored entries.

Names follow `name_is_safe`, must be at most `max_name_bytes` and
well-formed UTF-8, and no two may be equal with ASCII case ignored. A folder
is an entry whose name ends in '/' and whose size is zero. The writer
refuses a call made out of order and an entry that ends with other than its
declared number of bytes. After the write hook fails, or an entry ends at
the wrong size, every later call fails.

## Tests

`formats-zip` (`tests/zip_test.cpp`): write and read round trips; the
writer's exact bytes for fixed entries, pinned by SHA-256; archives made by
zlib's deflate in the test, with and without a data descriptor; three
archives made by the Info-ZIP zip command, kept as bytes in
`tests/tool_archives.hpp` (stored, deflated, a directory, extra fields, a
data descriptor, and one whose local header uses the 64-bit extension);
end record comments; every refusal of the reader and the writer; and a
malformed sweep: every prefix and suffix of two archives, and random byte
changes under a fixed seed.

`formats-zip-stream` (`tests/stream_test.cpp`): stored and deflated entries
read in steps of 1, 7 and 65,536 bytes, each step within its budget; small
entries written with the 64-bit extension, the zip command's archive whose
local header uses it, and a sparse archive of 5 GiB whose one entry lies
past 4 GiB; the zip command's other archives read as the in-memory reader
reads them; names in code page 437, from a Unicode path field and from
MS-DOS; links and special files marked; every refusal; and a malformed sweep
in which no allocation exceeds 1 MiB. `oa/test/raw_zip.hpp` (in
`tests/support/include`) builds the archives field by field, as the mod
packages' tests do too.

`formats-zip-writer` (`tests/writer_test.cpp`): stored and deflated entries,
empty files, folders and a name outside ASCII, read back with both readers;
the exact bytes of a fixed stored archive, pinned by SHA-256; two writes of
the same deflated entries byte for byte; `Zip64::always` on small entries;
one stored entry of 4 GiB + 16 zero bytes and a small entry after it, through
a sparse hook, read back past 4 GiB; every refusal; and 1,000 entries under
a fixed seed. The test also writes `writer-sample.zip` (a few deflated text
entries, a folder and a name outside ASCII) into its working directory.

## Limitations

The in-memory reader keeps the whole archive and each entry in memory, which
the limits bound to 1 GiB each, and refuses the 64-bit extension. Neither
reader reads archives with data before the first entry (self-extracting
archives) or split archives, and both ignore the extended time and
attribute fields, since neither a bundle nor a mod package needs them. The
streaming writer needs a positioned output: it writes back into each local
header, so it cannot write a pipe. It does not encrypt, and it writes no
comment, extended time or Unix attributes. Reproducible deflate bytes depend
on zlib, as the section above says.
