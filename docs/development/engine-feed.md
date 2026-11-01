# The engine feed

A release is offered from a signed feed and a detached signature beside it.
The game reads that file only after the signature over its exact bytes
verifies against a pinned key, and only when the file names the channel it
was fetched for. Two channels are published: `stable`, which every copy
reads unless its player chose otherwise, and `testing`, where a release
goes first. The reader accepts any channel name of lower-case letters and
digits, and the check compares that name with the channel it was asked
for.

## The files

The feeds sit side by side in `v1/engine/` on the downloads host, each with
its `.sig`:

| File | Read by | Channel |
|---|---|---|
| `stable.json` | 0.8.9, 0.8.10 and 0.8.11 | `stable` |
| `stable-2.json` | 0.8.12 and later on the stable channel | `stable` |
| `testing-2.json` | 0.8.12 and later on the testing channel | `testing` |

0.8.12 and later read the `-2` file of their channel; the copies before
them only know `stable.json`. The two stable files hold the same releases,
signed with the same key, and each names `stable`; the suffix is only the
file's name. They differ where an older copy must not be offered
something: the Mac updater in 0.8.9 to 0.8.11 can leave an app the
installer package put in Applications that the player cannot start, so
`stable.json` never names `macos-universal` while such copies exist, and
`stable-2.json` may, with the package's own `from` and an `install_page`
for the copies below it. No release before
0.8.12 has a testing channel, so none reads a testing file; `testing.json`
need not be published.

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

`ping`, when present, is where a copy reports a check of the feed: an
object of `url`, an `http` or `https` address, and `registry`, the id of
the registry whose install ID the report carries.

```json
"ping": {"url": "https://example.net/v1/engine/pings", "registry": "example"}
```

A copy of 0.8.12 or later that fetched and verified the feed sends one
POST there for that check, at most once an hour and never waiting for it,
with the game's own HTTP client and user agent:

```json
{"version":"0.8.12","platform":"macos-universal","channel":"stable","install_id":"","trigger":"automatic"}
```

`platform` is the package id `choose` uses, `trigger` is `manual` for
Check now and `automatic` otherwise, and `install_id` is the named
registry's install ID, empty while that registry's install ID is off or
none has been made ([What is sent](../updates.md#what-is-sent)). The
engine names no address and no registry of its own: both come only from
the signed feed. A run that is unattended, headless or given a test feed
reports only to an address on this machine. A failed report changes
nothing and is logged. Readers older than 0.8.12 ignore the member, so
both files may carry it.

Each release has:

| Member | Meaning |
|---|---|
| `release` | A whole number from 1 to 2^53. The number decides. |
| `version` | The version as `MAJOR.MINOR.PATCH`, shown to the player. |
| `date` | The day, `YYYY-MM-DD`. |
| `from` | The oldest version updated in place. An older build is offered the page. A package may name its own. |
| `rollout` | A whole number from 0 to 100. Absent is 100. Read and kept; not acted on until 0.9. |
| `notes` | An object of language tag to lines. Present notes include `en`. At most 12 lines a language, 200 bytes a line, UTF-8, no control characters. |
| `page` | An `http` or `https` address. A query is allowed. Absent when the release has no page. |
| `install_page` | An `http` or `https` address, where a copy that may not update in place installs the release by hand. A query is allowed. Optional. |
| `packages` | One object a package id. |

A package id is the name of a release package: `macos-universal`,
`windows-x64`, `windows-x86`, `windows-arm64`, `windows-95`,
`linux-x86_64`, `linux-arm64`, `linux-armhf` or `steam-deck`. The grammar
is `[a-z0-9]+(-[a-z0-9_]+)*`. A build only takes the package whose id is
its own. A `windows-95` build does not take `windows-x86`.

Each package has an optional `from`, `file`, `size` and `sha256` for the
zip, an optional `installer` with the same three for a Windows
`-setup.exe` or the macOS `.pkg`, and `mirrors`. `file` is a plain file name, 1 to 128 bytes of
`[A-Za-z0-9._-]`, never a path and never `.` or `..`. `size` is a whole
number of bytes from 1 to 2^40. `sha256` is 64 lower-case hex digits. A
mirror is an `http` or `https` address that ends in `/` and has no query.
The download address is the mirror plus the file name. A package needs at
least one mirror.

## Who updates in place

A build takes the newest release above its own number that has its
package. It updates in place when its version is at or after that
package's `from`, or the release's `from` when the package names none. An
older build is offered a page instead and downloads nothing: the
`install_page` when the release names one, where the game says the
version is installed by hand, and the release's `page` otherwise. A
package's `from` may be above the release's, so older copies of one
platform go to the install page while the others update in place, or
below it.

Both members keep `feed` at 1. A build older than 0.8.12 does not read
them: for it the release's `from` decides for every package, and the page
it opens is `page`. To keep an older copy of one platform from updating in
place, the release's `from` has to be above that copy's version, which
holds back every platform's copies of that version.

## Trust

The signature file is one line, `ed25519`, the key id and the signature,
the same line a catalogue uses. The key id has to be one of the keys
passed to the check. Those are the engine's pinned keys unless the caller
passes its own. A key id they do not hold is refused, including a key a
player added. The signature is verified over the exact bytes before a
single member is read.

## Publishing a release

After the packages are built, from the folder they were written to,
write each file the release goes into.

```
oa-tool engine-feed write \
  --version 0.8.7 --date 2026-10-24 --from 0.7.0 \
  --page https://example.net/v0.8.7 \
  --mirror http://downloads.coreprime.net/v1/engine/0.8.7/ \
  --out stable.json \
  open-annihilation-0.8.7-windows-x64.zip ...
```

`--release` overrides the number. `--package-from ID=VERSION`, once a
package, gives that package its own `from`, and `--install-page URL`
names the page where older copies install by hand:

```
oa-tool engine-feed write \
  --version 0.8.12 --date 2026-11-02 --from 0.8.0 \
  --package-from macos-universal=0.8.12 \
  --install-page https://example.net/install \
  --page https://example.net/v0.8.12 \
  --mirror http://downloads.coreprime.net/v1/engine/0.8.12/ \
  --out stable.json \
  open-annihilation-0.8.12-macos-universal.zip ...
```

A `--package-from` that names no package given is refused.
`--previous stable.json` copies the older releases from an earlier feed
in under the new one, newest first, five releases in all. A previous release newer than the one being written
is refused, and so is an earlier feed of another channel. `--notes` is a JSON file, an object of language tag to lines.
`--rollout` defaults to 100, which is what this build publishes.
`--channel` defaults to `stable`; `--channel testing` writes the testing
channel's feed. `--ping-url URL --ping-registry ID`, given together, add
the `ping` member:

```
oa-tool engine-feed write \
  --version 0.8.12 --date 2026-11-02 --from 0.8.0 \
  --page https://example.net/v0.8.12 \
  --mirror http://downloads.coreprime.net/v1/engine/0.8.12/ \
  --ping-url https://example.net/v1/engine/pings --ping-registry example \
  --channel testing --previous testing-2.json --out testing-2.json \
  open-annihilation-0.8.12-macos-universal.zip ...
```

On the signing machine:

```
oa-tool engine-feed sign --key KEY_FILE stable.json
oa-tool engine-feed check stable.json
```

`sign` refuses a feed the reader would refuse before it opens the key,
writes `stable.json.sig`, and checks that pair, with the feed's own
channel, with the key it just used. `check` reads the feed as the stable
channel's unless it is given `--channel testing`, and refuses a feed of
the other channel as `wrong_channel`. It prints `good signature by <id>`,
`ping <url> registry <id>` when the feed names a ping, and one line a
release: the number, the version, the day, `from` and the release's
minimum, then each package's id and zip size, with `from` and its own
minimum when it names one, and `install_page` and the address last when
the release has one.

Upload the packages to `v1/engine/<version>/` on
`downloads.coreprime.net`, then each feed and its signature. Those are
served with `Cache-Control: public, max-age=60`. Afterwards:

```
oa-tool engine-feed check --url http://downloads.coreprime.net/v1/engine/stable-2.json
oa-tool engine-feed check --channel testing \
  --url http://downloads.coreprime.net/v1/engine/testing-2.json
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
`arguments`, `from_version`, `feed`, `signature`, `channel`, `release`,
`package`, `file`, `installer`, `preferences`, `updates`, `title` and
`line`. `channel` is the channel the saved feed was fetched for, which the
helper requires the feed to name; it is written only when it is
`testing`, and absent reads as `stable`. The
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
