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
- `oa-update --plan FILE --elevated-swap` is the copy a folder swap or a
  macOS bundle swap starts with raised rights. It only moves files: on
  Windows it swaps the folder; on macOS it moves the bundles of an install
  or a roll back, and nothing else (see
  [An administrator's rights on macOS](#an-administrators-rights-on-macos)).
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
| 4 | Only from the macOS copy with an administrator's rights: the player could not start the bundle it put in place, and every step was undone. The run that asked exits 3. |

## Files

It reads the plan, the saved feed, the signature beside that feed, the
downloaded package and the preferences file. It writes under the plan's
`updates` directory: `oa-update.lock` while it runs, `oa-update.log` (one
line a step, kept under 1 MiB), `receipt.json` for the game, `journal.json`
and `journal-held/` while a swap is in progress, `<release>/stage/` for the
unpacked package, `previous/` for the version that was kept and the
preferences copy, and `state.json` for what is installed and what was kept.
A Windows folder swap copies this program, and the libraries beside it, to
`oa-update-running.exe` and continues from there, so the install's own
`oa-update.exe` can be renamed.

A release zip holds one top folder, named for the release and its package:
`open-annihilation-v0.8.8-macos-universal`, as the releases spell it, or
the same name without the `v`. The first name in the zip picks which, and
every other name must lie in that folder. Another version, another
package, two top folders, a link or an unsafe name refuses the zip before
anything is unpacked. The Linux versions folder is named for the version
without the `v`.

On macOS and Linux each file and folder keeps the mode the zip records for
it, when the zip was made on Unix or macOS, within 0755: no set-user,
set-group or sticky bit, and nothing the group or others may write. The
release zips record 0755 for the programs and folders and 0644 for every
other file. A folder always keeps read, write and search for its owner. An
entry that records no mode is 0644, or 0755 for `open-annihilation`,
`oa-intro`, `oa-tool` and `oa-update`, and a folder the zip records no
mode for, or does not name, is 0755 (`unpacked_mode`, `unpack.hpp`). The
process's file mode mask does not change these.

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

## An administrator's rights on macOS

The macOS bundle swap moves the installed bundle into `previous/` under
the updates folder and the staged one into its place. A rename that moves a
folder into another folder rewrites that folder's own entry for its parent,
so the bundle itself must be writable, not only Applications. The bundle
the installer package leaves belongs to the administrator, and an account
that is not an administrator cannot change Applications at all. When the
install's folder, the install or a kept bundle cannot be moved by the
player (`macos_swap_access`), the run records the kept version as the
player, then asks macOS for an administrator's password: `/usr/bin/osascript`
runs one shell command `with administrator privileges`, the standard
macOS prompt, with a sentence that names the version and the folder. The
window says why it asks. That command is this program again, with
`--elevated-swap` and the same plan.

The copy with the administrator's rights acts as the player before it
touches a file: it takes the user, group and groups of the updates folder's
owner, refusing an updates folder the administrator owns or a plan another
user owns. It writes no receipt, takes no lock of its own and starts
nothing. It checks the bundles itself, from the plan and their signatures,
never from a list it is handed: each must be valid, signed by the team that
signed this program and this game, and the one moving in notarized. It then continues the run's
journal with the moves, taking the administrator's rights for each rename
and dropping them after. A bundle moved into the updates folder becomes the
player's, and one moved into the install's place takes the owner the
install had, so nothing in the player's Library is left to the
administrator and the new bundle is owned as the installer package owned
the old one. In the same step, with the same rights, the bundle in the
install's place is opened to every account (`opened_mode`, `a+rX`): each
folder and file is readable by everyone, each folder and each file some
account could already run is runnable by everyone, and none keeps a
set-user, set-group or sticky bit or anything the group or others may
write. A link, and a file with another name outside the bundle, are left
as they are. A failed move undoes every step of the journal, the player's
too. A roll back of such an install asks the same way.

Before the moves count as done, the copy checks that the player can start
what it put in place (`macos_player_can_run`), with the player's own user,
group and groups, not its own: each of the bundle, `Contents` and
`Contents/MacOS` can be searched, `Contents/Info.plist` read, and
`Contents/MacOS/open-annihilation` and `Contents/MacOS/oa-update` read and
run. The owner's, the group's or the others' bits are read as POSIX reads
them for that account, and the system is asked as well, as the player, so
an access list that refuses is seen. A missing file, a link or a file of
another kind refuses. When the check refuses, the copy undoes every step of
the journal, writes which file refused to the log, and exits 4; the run
that asked records the swap as failed with "the player's account could not
start the version put in place", and starts the old version through Launch
Services. A swap the player makes without an administrator is checked the
same way, by the player's own run, before the receipt says installed, and
undone with the file that refused in the receipt.

When the player closes the prompt, the run undoes its record, writes
cancelled with the step "the administrator's permission was not given",
and starts the old version. When the prompt cannot be shown, as over a
remote login, nothing moved and the receipt says not installed. The game
is never started by a process with an administrator's rights.

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
- On macOS a bundle the player cannot start is never left in the
  install's place: the swap is undone before the receipt says installed,
  whoever made the moves.
- On macOS the game, the new version or the old one after a refusal or a
  failure, is opened through Launch Services, as the Finder opens it, never
  started as a plain program: macOS refuses a program started that way the
  player's Documents folder.
- The progress window lets go of the font files before a Windows folder is
  renamed and before a setup runs, then opens them again afterwards. An
  open font file keeps its folder from moving. The game starts the helper
  without handing over the files it still has open, and a font file is
  opened so it is not handed over.

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
counts as started. It covers both spellings of a zip's top folder and the
refusals around them; when a macOS swap needs an administrator, including
a scratch bundle the test cannot write, which the rename itself refuses;
the prompt's command and how its ending is read; whose a moved bundle
becomes; a journal another copy continues through the rename hook; and how
the game is started again. It covers the modes an entry is unpacked with,
from a zip that records 0755, 0644, 0700 and set-user bits and from one that
records none; the mode a tree opened to every account takes, and, on macOS,
a tree with narrow modes handed to its owner and opened; which of the
owner's, the group's and the others' bits an account is held to; the check
that the player can start the bundle put in place, for an account that is
neither the owner nor in the group, which the test stands in for the
administrator with; a swap whose game is run by its owner alone, which is
undone so the old bundle is back; and a release zip unpacked and handed to
an owner who is not the player, which the player must still be able to
start. It also checks that a closed window can let go of font
files, and, on Windows, that an open file keeps its folder from being
renamed. No test signs a bundle or starts a program. `tools-oa-update-run`
runs the program on a plain folder, in a self-check build. `app-update-plan`
covers the plan from the game's side. `app-update-route` covers which route
a machine is given.

## Limitations

- This build carries out `windows-installer` and `windows-folder`, the
  Linux versions folder and the macOS bundle, and `check-folder` only in a
  self-check build. A signed bundle is swapped in place, with an
  administrator's rights for the moves when the player cannot make them.
  A plan that names the macOS installer is still opened in Installer, but
  the game no longer writes one. An ad hoc signature is swapped only when a
  self-check run was given a feed key, and only for another ad hoc
  signature; a roll back with an administrator's rights is not given the
  feed key, so it takes team signatures only.
- The administrator's moves are renames: a bundle on another volume than
  the updates folder is not swapped with an administrator's rights. A run
  killed while the administrator's copy was moving leaves a journal the
  next run, as the player, may not be able to undo; it then says recovery
  did not finish.
- A rename the journal does itself stays on the volume the file is already
  on. The Windows folder route may copy across volumes, and a macOS bundle
  that would cross a volume is copied, in both directions, because the
  staged application lives under the updates folder.
- Fonts may be absent beside a build-tree program. The window still
  finishes, and shows no text.
- A crash after the game has started and before the journal is removed
  undoes that swap the next time the program runs.
