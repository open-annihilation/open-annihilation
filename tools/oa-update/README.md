# oa-update

`oa-update` puts a release in place after the game has quit, then starts
the game again. The game writes the plan and exits. This program waits,
checks the saved feed and the downloaded package, unpacks into a staging
folder, carries out the route under a journal, and starts the game. It
makes no request of its own.

From a build tree, `build/oa-update` is the program. It is built beside
`oa-tool`, and only where the build may start another program.

## Entry points

- `oa-update --plan FILE` reads the plan and carries it out.
- `oa-update --recover --updates DIR` undoes a journal a killed run left.
- `oa-update --version` prints the game's version line and exits 0.
- `oa-update --help` prints the usage and exits 0.
- A self-check build also accepts `--feed-key ID=ed25519:` and the key's
  base64, repeated, in place of the keys the build pins, and the
  `check-folder` route. Any other build refuses both with exit 2.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The install was swapped and the new version was started. |
| 1 | The package was refused. Nothing was changed. The old version was started, unless it was still running. |
| 2 | The arguments or the plan cannot be used. A plan that cannot be read starts nothing. |
| 3 | A step failed, every finished step was undone, and the old version was started. |

## Files

It reads the plan, the saved feed, the signature beside that feed, the
downloaded package and the preferences file. It writes under the plan's
`updates` directory: `oa-update.lock` while it runs, `oa-update.log` (one
line a step, kept under 1 MiB), `journal.json` and `journal-held/` while a
swap is in progress, `<release>/stage/` for the unpacked package,
`previous/` for the version that was kept and the preferences copy, and
`state.json` for what is installed and what was kept. The staging folder
and the journal are removed after the new version has started.

## Invariants

- The feed's signature is checked, and the package's name, size and
  SHA-256 are compared with that feed, before anything is unpacked or
  moved. The plan's own fields do not stand in for the feed.
- A step that fails undoes every step that finished. The old version is
  started, without the update flag, except when that version is still
  running or the plan cannot be read.
- The program makes no network request.

## Tests

`tools-oa-update` covers the plan and state texts, the refusals, a package
that does not match the feed, zip-slip, the executable bit, a journal that
fails at each step of a five-step list, a journal left on disk, the wait
giving up, the window's text placed without fonts, and the macOS decision
and operation list. No test signs a bundle or starts a program.
`tools-oa-update-run` runs the program on a plain folder, in a
self-check build. `app-update-plan` covers the plan from the game's side.

## Limitations

- Windows and Linux routes are later work. This build carries out
  `check-folder`, and only in a self-check build, and the macOS bundle.
  A signed bundle is swapped in place. A folder the player cannot write
  opens the signed installer. An ad hoc signature is swapped only when a
  self-check run was given a feed key, and only for another ad hoc
  signature. `roll-back` is read and not carried out.
- A rename stays on the volume the file is already on. A macOS bundle that
  would cross a volume is copied, in both directions, because the staged
  application lives under the updates folder.
- Fonts may be absent beside a build-tree program. The window still
  finishes, and shows no text.
- A crash after the game has started and before the journal is removed
  undoes that swap the next time the program runs.
