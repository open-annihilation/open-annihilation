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
line a step, kept under 1 MiB), `receipt.json` for the game, `journal.json`
and `journal-held/` while a swap is in progress, `<release>/stage/` for the
unpacked package, `previous/` for the version that was kept and the
preferences copy, and `state.json` for what is installed and what was kept.

## The receipt

`receipt.json` is begun with the outcome running as soon as the helper
holds the lock, and rewritten after every step: waiting for the game,
checking the package, unpacking, the swap or the installer, starting the
new version, watching it reach the main menu, an undo, and a roll back's
restore, each done or failed with the reason. The outcome is named before
any version of the game starts: installed, installs at the next start,
opened the installer, restart Windows, not installed (nothing moved),
cancelled (the permission prompt was declined), failed and put back,
recovery incomplete, rolled back, or a roll back that failed. A refusal
that names no outcome of its own is not installed. An elevated copy that
only swaps a folder leaves the receipt to the run that started it. The
game shows the receipt once (`src/app/update`, `receipt.hpp`). The staging folder
and the journal are removed after the new version has started.

## Invariants

- The feed's signature is checked, and the package's name, size and
  SHA-256 are compared with that feed, before anything is unpacked or
  moved. The plan's own fields do not stand in for the feed.
- A step that fails undoes every step that finished. The old version is
  started, without the update flag, except when that version is still
  running or the plan cannot be read. Once files have changed, the window
  says "The update failed. The game was put back." When the undo itself
  does not finish, it says "The update failed. The game could not be put
  back." It does not say that nothing was changed.
- The program makes no network request.
- A start of the new version that ends before the main menu is told apart
  by `state.json`: the game marks a quit there as it leaves. A quit ends
  the watch and nothing is started again. A start that crashed is started
  once more, and a second crash puts the kept version back. A start the
  watch cannot see (no process and no pid) is never counted as failed.
  On macOS the helper treats the new application as started when it is
  already running or when `state.json` confirms that version, and does not
  wait out the launch report.
- The progress window lets go of the font files before a Windows folder is
  renamed and before a setup runs, then opens them again afterwards. An
  open font file keeps its folder from moving.

## Tests

`tools-oa-update` covers the plan and state texts, the refusals, a package
that does not match the feed, zip-slip, the executable bit, a journal that
fails at each step of a five-step list, a journal left on disk, the wait
giving up, the window's text placed without fonts, the receipt beside the
log and the outcomes a roll back records, how the watch reads a start's
end (reached the menu, quit, failed, not seen), the Windows route
decisions, the move retries, the `WININIT.INI` text and a postponed rename,
the Linux versions folder, a second Linux update while the first kept copy
is still there, a failed Windows setup whose undo leaves the helper's own
file alone, the macOS decision and operation list, and when a macOS launch
counts as started. It also checks that a closed window can let go of font
files, and, on Windows, that an open file keeps its folder from being
renamed. No test signs a bundle or starts a program. `tools-oa-update-run`
runs the program on a plain folder, in a self-check build. `app-update-plan`
covers the plan from the game's side. `app-update-route` covers which route
a machine is given.

## Limitations

- This build carries out `windows-installer` and `windows-folder`, the
  Linux versions folder and the macOS bundle, and `check-folder` only in a
  self-check build. A signed bundle is swapped in place. A folder the player
  cannot write opens the signed installer. An ad hoc signature is swapped
  only when a self-check run was given a feed key, and only for another ad
  hoc signature. `roll-back` is read and not carried out.
- A rename the journal does itself stays on the volume the file is already
  on. The Windows folder route may copy across volumes, and a macOS bundle
  that would cross a volume is copied, in both directions, because the
  staged application lives under the updates folder.
- Fonts may be absent beside a build-tree program. The window still
  finishes, and shows no text.
- A crash after the game has started and before the journal is removed
  undoes that swap the next time the program runs.
