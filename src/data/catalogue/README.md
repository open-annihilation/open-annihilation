# data/catalogue

A registry's catalogue: the packages it offers, and the check that the bytes
are the ones a trusted key signed. The format is `catalogue` 1. The signature
is a detached file, one line, over the exact bytes. Nothing here fetches.

[catalogue.hpp](include/oa/data/catalogue/catalogue.hpp) and
[check.hpp](include/oa/data/catalogue/check.hpp) are the entry points
(`oa::data::catalogue`).

## Entry points

- `read_catalogue` reads the structure and the entries. It does not check a
  signature or a sequence. A member this build does not know is ignored. A
  member named twice in one object refuses the catalogue. One package that
  breaks a rule is left out, and the reason is kept when fewer than 100
  reasons are kept already.
- `valid_reference`, `valid_package_key`, `parse_utc_time` and `parse_day`
  are the rules for a path, a package key, a UTC time and a day written
  `YYYY-MM-DD`, as a package's `published` names the day its release came
  out. Any kind may name `published`; a malformed one leaves the entry out.
- `parse_signature_file` and `signature_file_text` read and write the
  detached signature line.
- `check_catalogue` checks a fetched catalogue: its size, then for a signed
  registry the signature over the exact bytes, then the JSON, the registry
  id, the sequence and the entries. `Checked::usable` is true for `accepted`
  and `unchanged`. An expired catalogue is still usable, with `expired` set.
  An added registry whose `keys` is present and differs from the trusted set
  reports `new_keys`. A built-in registry's keys are ignored, and an empty
  key list is never a rotation.

## State

None. The caller keeps the catalogue and the previous sequence.

## Invariants

- Nothing is parsed before its signature is verified. A signed registry's
  catalogue is not read as JSON until the signature has been checked against
  the exact bytes.
- The sequence never goes back. The same sequence with the same bytes is
  unchanged. The same sequence with other bytes is refused.
- One bad entry never hides the rest. A bad signature or a bad structure
  refuses the whole catalogue.

## Tests

`data-catalogue` reads the format, including skipped entries, `published`
on every kind and malformed days, and refused catalogues. `data-catalogue-check` signs catalogues in the test and covers
the signature, the sequence, expiry and key rotation.

## Limitations

- No network and no cache. Fetching and storing a catalogue, and writing a
  rotation, are F08's.
- The format is version 1.
- Time is the caller's clock, compared with `expires` as seconds since 1970.
  A catalogue is expired when that clock is at or after `expires`.
- A map's title is 1 to 64 bytes, and a name under `compatible` is 1 to 256
  bytes. A map's stem is whatever text is written before the one `@`.
- A field that belongs to another kind is ignored. A mutator may name
  prerequisites, conflicts, what it works with, layers and options. One
  broken member leaves that entry out. An unknown layer name or option
  type is skipped. Prerequisites, conflicts, layers and clashes are ignored
  on every kind but a mutator. What a package works with, and its options,
  are read on a mutator and on a brain.
- A brain names a version of one to three parts and one to six levels, and
  may name full awareness, the networks' size, what it works with and
  options. A level whose difficulty this build does not know is skipped. An
  entry left with no level is left out. Tags are not limited to the nine a
  brain manifest allows.
- A package's `size` may be 2^32.
