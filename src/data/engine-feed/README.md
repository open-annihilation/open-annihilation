# data/engine-feed

The signed engine feed: the releases one channel offers, and the check that
the bytes are the ones a pinned key signed. The format is `feed` 1. The
signature is a detached file, one line, over the exact bytes. The release
number orders releases; the version text is only shown. Nothing here fetches
or opens a file.

[feed.hpp](include/oa/data/engine_feed/feed.hpp) is the entry point
(`oa::data::engine_feed`).

## Entry points

- `read_feed` reads the structure. It does not check a signature. A member
  this build does not know is ignored. A member named twice refuses the
  feed. Releases come back newest first.
- `check_feed` checks a fetched feed: its size, then the signature over the
  exact bytes, then the JSON and the channel. `Checked::usable` is true
  only for `accepted`. The keys default to the engine's pinned keys.
- `release_number` is major × 10000 + minor × 100 + patch, and only while
  minor and patch are at most 99. `choose` offers the newest release above
  the build's number that has the build's package. `notes_for` takes the
  exact language tag, then its first subtag, then `en`.
- `feed_text` writes the canonical JSON. Reading that text back yields the
  same feed.

## State

None. The caller keeps the feed and the build it is choosing for.

## Invariants

- Nothing is parsed before the signature verifies. The bytes are not read
  as JSON until the signature has been checked against the exact bytes.
- Only the keys passed to `check_feed` can accept a feed. Those default to
  the pinned keys. A key id they do not hold is `unknown_key`.
- The release number decides which release is newer. The version text is
  only shown. A release with no package for the build is not offered.

## Tests

`data-engine-feed` signs feeds in the test and covers the signature, every
refusal of the reader, `choose`, the release number, the notes and a
round trip through `feed_text`.

## Limitations

- `rollout` and `online_min` are read and kept. `choose` does not act on
  them. That waits for 0.9.
- One channel is published, `stable`. The reader accepts any channel whose
  name is lower-case letters and digits, and `check_feed` compares it with
  the channel it was asked for. A preview channel is 0.9.
- No network and no files. Fetching the feed, and writing the first signed
  one, are later work. `oa-tool engine-feed` writes, signs and checks a
  feed with this code.
