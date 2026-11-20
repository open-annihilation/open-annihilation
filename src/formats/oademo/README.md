# oademo

A `.oademo` replay: a zip of a description, a manifest, an optional
timeline and one recording. The recording is a `.tad` stream, every packet
plain, either one zstd frame or a deflated entry. Target
`oa-formats-oademo`, header `oa/formats/oademo/oademo.hpp`, namespace
`oa::formats::oademo`. The same description and recording always give the
same bytes. A member this version does not know is kept and written back.

## Entry points

- `recognised` reads the zip local header and the start of the first entry.
  A file whose first entry is the stored `oademo` line of any version is
  recognised, so `read` can say it is not a version this game reads.
- `read` reads a whole file. It checks the recording's size against the
  recording format's limit before that recording is allocated, then checks
  its SHA-256 against the description. A zstd frame's size comes from its
  header (`oa::base::zstd::content_size`), and the buffer is made only once
  that size is within the limit. A frame that names no size is read into a
  buffer that doubles, up to the limit.
- `read_description` reads the description and the manifest only. It makes
  the layout checks and refusals `read` makes up to and including those two
  entries, with the same sentences, and never reads the timeline or the
  recording, so a replay whose recording is damaged still has its details.
- `write` writes a file. Deflated entries use deflate level 6. A zstd
  recording is one frame, at the level given, from 1 to the highest zstd
  accepts. The default level is 3. Level 19 is the one an export uses. The
  frame declares a window of at most 8 MiB at every level (see Invariants).
  `write` refuses a recording that is not a TA Demo recording, one past the
  recording format's limit, and one that holds a sector of network addresses.
- `export_replay` writes a `.oademo` of a replay already read, at level 19.
  A recording that holds a network address, or a packet that is not plain,
  is cleaned first and the description's recording identity follows the
  cleaned bytes. One that is already plain and holds no address is stored
  unchanged. A timeline the caller supplies replaces the file's; none
  supplied keeps it. Open Annihilation's own sectors are kept and names are
  not cut.
- `export_tad` wraps a `.tad` the same way. The description is filled from
  the recording's header. `recorded_with` is the version that writes it, and
  a timeline is written when the caller supplies one.
- `set_recording_identity` fills `recording.sha256` and `recording.bytes`
  from the uncompressed recording.
- `meta_text` and `manifest_text` write the description and the manifest
  as compact JSON, known members first.
- `unit_table_digest` computes `units.table_sha256` from a recording's
  unit-check chunk. `index_bits` is the bit length of `count + 1`.
- `timeline_index_text` writes the replay's index, `timeline.json`, as compact
  JSON (`oa/formats/oademo/timeline_index.hpp`): the length in ticks, how the
  replay ended, the markers, each player's totals every ten seconds of game
  time, the battle's intensity and the result. `parse_timeline_index` reads
  it back. The members and what they hold are in
  [docs/replays.md](../../../docs/replays.md#the-replay-index). The index
  pass (`--replay-index`) fills the structure in; this module only reads and
  writes the schema.

The sentences `read` returns are the texts in
`oa/formats/oademo/interface_words.hpp`. A zip that does not open with the
stored `oademo` entry says "This file is not a replay."; a replay whose
`oademo` line names a version other than 1 says "This replay is not a version
this game reads."

## State

None. `read` and `write` take their arguments and return a result. The
archive is bytes in memory. The module opens no file.

## Invariants

- Two writes of the same replay and options are the same bytes. Entry
  times are fixed.
- The first 45 bytes of every file are the same: the local header of the
  stored `oademo` entry (version 20, flags 0, stored, the fixed time and
  date, the CRC-32 and both sizes of the line, no extra field), the name, and
  the line `oademo 1` with its newline. Every other entry's local header has
  flags 0 and no extra field as well. A reader that finds the signature by
  its position, without reading the zip's directory, depends on it.
- The entries follow in order: `meta.json` and `manifest.json`, deflated,
  then `timeline.json` when there is one, then either `recording.tad.zst`
  stored or `recording.tad` deflated.
- A recording whose uncompressed size is above the recording format's
  limit is refused before that buffer is allocated. A size equal to the
  limit is accepted.
- A zstd frame whose header names a size above that limit is refused
  before the output is allocated.
- **Every zstd frame a replay holds declares a window of at most 8 MiB**,
  so a reader that refuses a larger window reads it. Levels up
  to 19 stay inside that on their own; levels 20 to 22 are held to 8 MiB by
  `oa::base::zstd::compress` (`base-zstd` checks a level-22 frame of an input
  over 8 MiB).
- The description's `recording.sha256` is the SHA-256 of the uncompressed
  recording. The writer stores that digest in lower case.
- No replay this module writes holds a sector of network addresses.
- A member this version does not know, and an object under `game` whose
  name is `game.kind`, are kept and written back. `needs[].maps` (each map's
  name and SHA-256) is a known member.

## Tests

`formats-oademo` checks that a `timeline.json` is written in a fixed order
and reads back to the index it was written from (every kind of marker, a
member this version does not know, a replay that carries one), that a marker
of a kind it does not know is skipped, and that a text which is not JSON,
names another version or has a wrong member is refused. It also checks that two writes are the same bytes in the entry
order above, that the first 45 bytes and every local header are as pinned,
that each refusal returns its sentence (a zip that opens with some other
entry, a version other than 1, neither recording or both, a digest that does
not match, a damaged frame, a real frame cut short, a size past the limit),
that `read_description` gives back the description and manifest written,
also beside a damaged recording, and `read`'s sentence for cut bytes, bytes
that are no zip and another version, that a deflated recording reads as a
zstd one does, that a frame with no size reads, that an unknown member, a `game.<kind>` object and a package's maps
survive a read and a write, and that `write` refuses a recording with an
address sector or one that is no recording.

`demo-playback` loads one recording as a `.tad` and as a `.oademo` and
compares the playback. `tools-oa-tool-replay` packs a recording, prints
one JSON object, unpacks the plain stream, and checks what pack reads from
a recording of the original recorder (sides and colours, text, mutators and
brains).

## Measurements

The command, on the Mac Studio, three times: `open-annihilation --skip-intro
--mute --headless-check --net-loopback-check 9000 --net-record FILE.tad`. It
hosts and joins a match in one process over the loopback, 9000 ticks of
Canal Crossing, plain packets, no profile. Each run is packed with
`oa-tool replay pack` at deflate 6 (`--deflate`), zstd 3 and zstd 19.

**A headless loopback's packet delays are wall-clock time, so the stream
changes from run to run with the machine's load.** Every run wrote the same
19,401 packets and 283,176 bytes, and none wrote the same bytes, so the
compressed sizes differ between runs. The table gives the three runs and
their median. The Studio was shared with other builds and tests while they ran
(load average about 20). Pack leaves out the two empty address sectors, so
the stream it stores is 283,164 bytes in every run (12 bytes less than the
game wrote); that stored stream is the plain recording the shares are taken
against. The recording entry is the size of the zip entry that holds it.

| Store | Run 1 | Run 2 | Run 3 | Median | Share of the stored stream | Whole file (median) |
|---|---:|---:|---:|---:|---:|---:|
| plain stream, as stored | 283,164 | 283,164 | 283,164 | 283,164 | 100% | |
| deflate 6 | 48,863 | 49,937 | 48,747 | 48,863 | 17.3% | 49,864 |
| zstd 3 | 45,540 | 47,029 | 45,226 | 45,540 | 16.1% | 46,549 |
| zstd 19 | 37,137 | 36,776 | 36,769 | 36,776 | 13.0% | 37,783 |

The whole file adds the zip's headers and the description, about 1,000
bytes at every level.

**The levels stay: 3 when a game ends, 19 on export, and 6 for `--deflate`.**
The shares are not within 5 points of the design's section 2.4. Its fifteen
recordings of the original recorder compress to 52.9% (deflate 6), 49.3%
(zstd 3) and 42.3% (zstd 19) of plain, and this engine's loopback recording
compresses to 17.3%, 16.1% and 13.0%. This engine stores one record in each
packet, so its recording is larger before compression and gains more from it
(the design says as much), and every level's share falls to about a third.
The shares are not what picks the levels. They are picked by how much better
zstd 3 and zstd 19 do than the zip default, deflate 6, and that comparison
does hold:

| | zstd 3 against deflate 6 | zstd 19 against deflate 6 |
|---|---:|---:|
| section 2.4, fifteen recordings (49.3 and 42.3 against 52.9) | 7% smaller | 20% smaller |
| this recording, median of three runs | 6.8% smaller | 24.6% smaller |
| this recording, the three runs | 6.8%, 5.8%, 7.2% | 24.0%, 26.4%, 24.6% |

On the median, zstd 3 is within 0.2 points of the design's 7% and zstd 19
within 4.6 points of its 20% (one run is 6.4 points over, in zstd 19's
favour). A recording the engine makes gains at least what the design counted
on from each level, so the levels the design set stay.

A skirmish recording, measured the same way. The command, on the Mac
Studio, three times: `open-annihilation --skip-intro --mute --headless-check
--brain-match builtin,builtin --brain-map "Ashap Plateau" --match-ticks 9000
--seed 7 --record-game FILE.tad`. Two built-in computer players, 9000 ticks.
Each run is packed with `oa-tool replay pack` at the same three levels.

**Every run wrote the same 18,255 packets and 573,540 bytes, not the same
bytes: the packet delays are wall-clock time.** The compressed sizes still
differ, so the table is the sizes. A skirmish recording holds no address
sector: the stream pack stores is the file the game wrote. The Studio was
shared with other builds while the runs were taken (load average about 23).
The recording entry is the size of the zip entry that holds it. If a later
change touches skirmish recording, measure again with this command.

| Store | Run 1 | Run 2 | Run 3 | Median | Share of the stored stream | Whole file (median) |
|---|---:|---:|---:|---:|---:|---:|
| plain stream, as stored | 573,540 | 573,540 | 573,540 | 573,540 | 100% | |
| deflate 6 | 60,946 | 60,925 | 60,938 | 60,938 | 10.6% | 61,918 |
| zstd 3 | 46,387 | 46,453 | 46,446 | 46,446 | 8.1% | 47,434 |
| zstd 19 | 38,350 | 38,374 | 38,444 | 38,374 | 6.7% | 39,360 |

The whole file adds the zip's headers and the description, about 1,000
bytes at every level, as it does for the loopback recording.

## Limitations

The reader accepts version 1 only. It does not check the description's
packet count or unit-table digest against a parse of the recording: the
identity is the SHA-256 and the byte count. `read` keeps a replay's
timeline as one JSON value and does not interpret it, and does not check
that it names the recording's length or its players: `parse_timeline_index`
reads one, and the caller compares. An index keeps at most
`most_markers_of_a_kind` markers of each kind, so a game with thousands of
chat lines still gives a file of a few hundred kilobytes. The module spells
no extension's name.
