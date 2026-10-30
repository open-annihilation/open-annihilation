# The engine feed

A release is offered from one signed feed, `v1/engine/<channel>.json`, and
a detached signature beside it. The game reads that file only after the
signature over its exact bytes verifies against a pinned key. This build
publishes one channel, `stable`. A preview channel is 0.9. The reader
accepts any channel name of lower-case letters and digits, and the check
compares that name with the channel it was asked for.

The format is `feed` 1. A member this build does not know is ignored. A
member named twice refuses the feed. Releases come back newest first, and
a feed holds at most 32.

## The release number

`release` orders releases. `version` is only shown. The number this
release step writes is

```
major × 10000 + minor × 100 + patch
```

and it is defined only while minor and patch are at most 99, so 0.8.7 is
807. A feed may still number a release however the maintainer chooses: a
release 900 whose version reads 0.8.5 is newer than 807.

`oa-tool engine-feed write` uses that number when `--release` is left out.
A version whose minor or patch is above 99 has no number, and the command
asks for `--release`.

## What a feed holds

`channel` is the channel the file is for. `online_min`, when present, is
the oldest version the feed names. It is read and kept. Nothing in this
build acts on it.

Each release has:

| Member | Meaning |
|---|---|
| `release` | A whole number from 1 to 2^53. The number decides. |
| `version` | The version as `MAJOR.MINOR.PATCH`, shown to the player. |
| `date` | The day, `YYYY-MM-DD`. |
| `from` | The oldest version updated in place. An older build is offered the page. |
| `rollout` | A whole number from 0 to 100. Absent is 100. Read and kept; not acted on until 0.9. |
| `notes` | An object of language tag to lines. Present notes include `en`. At most 12 lines a language, 200 bytes a line, UTF-8, no control characters. |
| `page` | An `http` or `https` address. A query is allowed. Absent when the release has no page. |
| `packages` | One object a package id. |

A package id is the name of a release package: `macos-universal`,
`windows-x64`, `windows-x86`, `windows-arm64`, `windows-95`,
`linux-x86_64`, `linux-arm64`, `linux-armhf` or `steam-deck`. The grammar
is `[a-z0-9]+(-[a-z0-9_]+)*`. A build only takes the package whose id is
its own. A `windows-95` build does not take `windows-x86`.

Each package has `file`, `size` and `sha256` for the zip, an optional
`installer` with the same three for a Windows `-setup.exe` or the macOS
`.pkg`, and `mirrors`. `file` is a plain file name, 1 to 128 bytes of
`[A-Za-z0-9._-]`, never a path and never `.` or `..`. `size` is a whole
number of bytes from 1 to 2^40. `sha256` is 64 lower-case hex digits. A
mirror is an `http` or `https` address that ends in `/` and has no query.
The download address is the mirror plus the file name. A package needs at
least one mirror.

## Trust

The signature file is one line, `ed25519`, the key id and the signature,
the same line a catalogue uses. The key id has to be one of the keys
passed to the check. Those are the engine's pinned keys unless the caller
passes its own. A key id they do not hold is refused, including a key a
player added. The signature is verified over the exact bytes before a
single member is read.

## Publishing a release

After the packages are built, from the folder they were written to:

```
oa-tool engine-feed write \
  --version 0.8.7 --date 2026-10-24 --from 0.7.0 \
  --page https://example.net/v0.8.7 \
  --mirror http://downloads.coreprime.net/v1/engine/0.8.7/ \
  --out stable.json \
  open-annihilation-0.8.7-windows-x64.zip ...
```

`--release` overrides the number. `--previous stable.json` copies the
older releases from an earlier feed in under the new one, newest first,
five releases in all. A previous release newer than the one being written
is refused. `--notes` is a JSON file, an object of language tag to lines.
`--rollout` defaults to 100, which is what this build publishes.
`--channel` defaults to `stable`.

On the signing machine:

```
oa-tool engine-feed sign --key KEY_FILE stable.json
oa-tool engine-feed check stable.json
```

`sign` refuses a feed the reader would refuse before it opens the key,
writes `stable.json.sig`, and checks that pair with the key it just used.
`check` prints `good signature by <id>` and one line a release.

Upload the packages to `v1/engine/<version>/` on
`downloads.coreprime.net`, then the feed and its signature. Those two are
served with `Cache-Control: public, max-age=60`. Afterwards:

```
oa-tool engine-feed check --url http://downloads.coreprime.net/v1/engine/stable.json
```

That fetches the feed and the feed's address plus `.sig`, and runs the
same check.

## The plan and the state

The game writes a plan for `oa-update`, and the helper writes
`state.json` after a swap. Both live under the data folder's `updates`
directory. Both are JSON. The plan's `plan` member is 1. A member this
build does not know is ignored. A member named twice is refused. Every
path is absolute.

The plan's members, in the order `plan_text` writes them, are `plan`,
`action` (`install` or `roll-back`), `route` (`check-folder`,
`macos-bundle`, `windows-installer`, `windows-folder` or
`linux-versions`), `wait_for` (a process id, or 0), `install`, `program`,
`arguments`, `from_version`, `feed`, `signature`, `release`, `package`,
`file`, `installer`, `preferences`, `updates`, `title` and `line`. The
title and the line are already in the player's language, and each is at
most 200 bytes.

`state.json` records `installed_version`, `installed_release`,
`kept_version`, `kept_path`, `kept_route`, `preferences_copy`,
`confirmed` (false after a swap) and `failed_starts` (0 after a swap).
The installed version and release are the ones the feed named. The kept
version is the one that was running. The file is replaced whole.

The helper checks the feed and the package before it unpacks anything,
and it makes no request. What the player sees is in
[Updates](../updates.md).
