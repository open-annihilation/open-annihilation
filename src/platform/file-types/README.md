# platform/file-types

Makes the running copy of the game the opener of the eight file types for
the user who runs it — a mod package (`.oamod`), a language pack
(`.oalang`), a map pack (`.oamap`), a mutator package (`.oamut`), a brain
package (`.oabrain`), a key layout package (`.oakeys`), a registry
file (`.oareg`) and a launch
file (`.oalaunch`) — with the game's icon on those files where the system
takes one, so that opening one in the file manager starts the game with it.
The first seven the game installs or adds (`Opening::install`); a launch file
starts the game in that launch's battle room, as `-c FILE` does
(`Opening::launch`). On Windows and Linux the game does this at run time,
writing only what differs from what is there. macOS and iOS declare the
types in the bundle's `Info.plist`
([src/app/Info.plist.in](../../app/Info.plist.in),
[platforms/ios/Info.plist.in](../../../platforms/ios/Info.plist.in)). On
macOS a switch in Settings chooses whether this copy is the default opener
of each of the eight; a start never replaces a choice made in Finder. On
macOS the bundle owns a launch file's type. iOS does nothing at run time.

`oa/platform/file_types.hpp` (namespace `oa::platform::file_types`):

- `running_executable()` returns the program's path as the system would
  start it again: the module's file name on Windows, `/proc/self/exe` on
  Linux. It gives none on macOS and iOS, and none on Linux once the
  executable was deleted or replaced since it started. macOS names the
  running bundle with `running_application()` instead.
- `TypeSet` names which of the eight a call acts on. Each is on unless the
  caller turns it off, so a call that passes none acts on all eight; Settings
  has a switch for each of the eight, each standing for its own type alone
  (`oa/ui/engine_settings/file_types.hpp` turns the switches into a set and
  into opener changes). `installs_any()` tells whether it holds one of the
  seven the game installs or adds, and `complement()` is the types it does
  not hold.
- `register_file_types(executable, icon_png, places, types)` registers the
  program for `types` on Windows and Linux and says what it did in a
  `Registration`: whether this system registers at run time, whether
  anything was written, a line for the log for each thing written, skipped
  or run, and the error that stopped it. It never throws. `Places` names
  where it writes, and on Windows where it reads the computer's classes,
  so that the tests write into scratch folders and keys.
  On Linux the MIME package and the two desktop entries then list exactly
  `types`. A type left out loses its mimetype icon. With none of the seven
  the game installs or adds, their desktop entry is removed; without a
  launch file, the launch file's entry is; with none of the eight, the MIME
  package is too. The program's icon stays.
- `withdraw_file_types(types, places)` withdraws those types. On Windows a
  value is this game's, and is removed, only when it is the type's
  program-identifier key, the `OpenWithProgids` value whose name is that
  program identifier, or the extension's (Default) or `Content Type` while
  that value's data is still the game's program identifier or MIME type.
  Another program's values, and the player's `UserChoice`, stay. A
  withdrawal of launch files then hides the installer's association for
  every user from this one, with an override of the user's own
  ([below](#the-installers-association-and-launch-files-off)). On Linux
  it takes those types out of the package and the desktop entries and
  removes their mimetype icons, and removes an entry or the package once it
  lists no type. The program's icon stays. A missing value is already
  withdrawn. macOS and iOS withdraw nothing at run time. On macOS the
  switches call `apply_opener_changes`, which changes the default opener
  and leaves the declaration.
- The texts it writes: `desktop_entry`, `launch_desktop_entry`,
  `mime_package`, `desktop_exec_argument`, `open_command` and
  `default_icon`.
- `file_types` names the eight, each with its extension, MIME type, the name
  file managers show, its Windows program identifier, the MIME type it is a
  kind of (`application/zip` for the six packages, `text/plain` for
  `.oareg` and `.oalaunch`) and what the game does with it (`opening`).
- `exported_content_types` names the same eight as macOS does: the exported
  type identifier, and the content type a switch that is off hands back to
  (`public.zip-archive` for the six packages, `public.plain-text` for
  `.oareg` and `.oalaunch`).
- `default_opener_is_a_setting()` is true on macOS. There the switches show
  what `reported_openers` reads, and OK calls `apply_opener_changes` for
  the switches that moved. `HandlerCalls` is the two questions that asks,
  so a test can stand in for the system. `system_handler_calls()` is those
  questions on macOS and nothing elsewhere. `running_application()` is this
  copy when it is an application bundle.

## Windows

Under `HKEY_CURRENT_USER\Software\Classes`, six string values for each of
the eight file types, 48 in all:

| Key | Value | Data |
|---|---|---|
| `.<extension>` | (Default) | the type's program identifier |
| `.<extension>` | `Content Type` | the type's MIME type |
| `.<extension>\OpenWithProgids` | the program identifier | empty |
| `<program identifier>` | (Default) | the type's name |
| `<program identifier>\DefaultIcon` | (Default) | `<exe>,0`, the executable's first icon |
| `<program identifier>\shell\open\command` | (Default) | `"<exe>" --open "%1"`; for a launch file `"<exe>" -c "%1"` |

| Extension | Program identifier | MIME type | Name |
|---|---|---|---|
| `.oamod` | `OpenAnnihilation.Mod` | `application/x-oamod` | Open Annihilation mod |
| `.oalang` | `OpenAnnihilation.Language` | `application/x-oalang` | Open Annihilation language pack |
| `.oamap` | `OpenAnnihilation.MapPack` | `application/x-oamap` | Open Annihilation map pack |
| `.oamut` | `OpenAnnihilation.Mutator` | `application/x-oamut` | Open Annihilation mutator |
| `.oabrain` | `OpenAnnihilation.Brain` | `application/x-oabrain` | Open Annihilation brain |
| `.oakeys` | `OpenAnnihilation.KeyLayout` | `application/x-oakeys` | Open Annihilation key layout |
| `.oareg` | `OpenAnnihilation.Registry` | `application/x-oareg` | Open Annihilation registry |
| `.oalaunch` | `OpenAnnihilation.Launch` | `application/x-oalaunch` | Open Annihilation launch file |

Each value is read first and written only when it is missing or differs.
A registration of some of the eight writes those types and leaves the others
as they are; the caller withdraws the rest. Withdrawing a type deletes its
program-identifier key leaf first (`shell\open\command`, then `shell\open`,
`shell`, `DefaultIcon`, then the program identifier) with `RegDeleteKey`,
which stops if an unexpected subkey remains, and deletes the
`OpenWithProgids` value whose name is that program identifier. The
extension's (Default) is deleted only while its data is still that program
identifier, and `Content Type` only while its data is still that MIME type.
The extension key itself stays, and so does every other value on it. The
player's choice of opener, Explorer's `UserChoice`, is never opened. A
missing value is already withdrawn. After any write or withdrawal the shell
is told that a file association changed, so Explorer shows the icon at
once. A copy started from the temporary folder, as one
started from inside a zip in Explorer is, registers nothing and withdraws
nothing. Only calls that
Windows XP has are used. Removing the sixteen keys `.oamod`,
`OpenAnnihilation.Mod`, `.oalang`, `OpenAnnihilation.Language`, `.oamap`,
`OpenAnnihilation.MapPack`, `.oamut`, `OpenAnnihilation.Mutator`, `.oabrain`,
`OpenAnnihilation.Brain`, `.oakeys`, `OpenAnnihilation.KeyLayout`, `.oareg`,
`OpenAnnihilation.Registry`,
`.oalaunch` and `OpenAnnihilation.Launch` forgets the registration.

The Windows installers
([packaging/windows/open-annihilation.nsi](../../../packaging/windows/open-annihilation.nsi))
write the launch file's six values under `HKEY_LOCAL_MACHINE\Software\Classes`
for everyone who uses the computer, with the installed
`open-annihilation.exe`, and tell the shell. Uninstalling removes the
program identifier's key and the `OpenWithProgids` value, and the
extension's (Default) and `Content Type` while they still name the game.

Before it removes the program's files, the uninstaller also withdraws, for
the account that runs it, the game's own registration of the eight types
under `HKEY_CURRENT_USER\Software\Classes` while it names the installed
program, `<install folder>\open-annihilation.exe`, and leaves every value
a withdrawal would leave. For each type:

- the program identifier's open command goes while the program it names,
  between its first two quotes, is the installed one, and its
  `DefaultIcon` while it is that program followed by `,0`. A path matches
  without regard to letter case, and also in its short form, so either
  spelling of the same file does;
- when one of them named the installed program and neither names another,
  the rest of the type's six values goes too: the program identifier's
  name, its keys leaf first (each only once nothing is left in it), the
  `OpenWithProgids` value named for it, and the extension's (Default) and
  `Content Type` while they are still the game's program identifier and
  MIME type;
- otherwise the type stays as it is. A copy unzipped elsewhere that
  registered a type last names its own path there, so its registration of
  that type keeps working, and a command or icon in a form the game does
  not write counts as another program's.

The override below is then removed as well. The uninstaller cannot reach
another account's registration: that one still names the removed program
until the account starts another copy of the game, whose start registers
that copy in its place and withdraws the types that are off.

### The installer's association and Launch files Off

Windows opens a file through `HKEY_CLASSES_ROOT`, the user's classes laid
over the computer's, so withdrawing the user's own registration would leave
the installer's association opening launch files. A withdrawal of the types
in `installer_types` (launch files alone) therefore goes on, once the
user's own values are gone: it reads the extension's (Default) under
`Places::machine_classes_key` (`HKEY_LOCAL_MACHINE\Software\Classes`, opened
to read only) and the user's own, and does what `user_override_step` says.
The override is the user's `.oalaunch` (Default) set to an empty text
(`user_override_program_id`).

| The computer's (Default) | The user's (Default) | The withdrawal |
|---|---|---|
| `OpenAnnihilation.Launch` | missing | writes the override |
| `OpenAnnihilation.Launch` | empty, the override | keeps it, and writes nothing |
| missing, or another program's | empty, the override | removes it: it would hide another program's association, or nothing |
| anything | a program identifier, the player's or another program's | leaves it: it already decides for this user |
| missing, or another program's | missing | nothing |

Why this override, from Windows XP to Windows 11:

- Since Windows 2000, a value under the user's classes takes precedence over
  the same value under the computer's, and Explorer reads an extension's
  program identifier from that merged view whenever the player has not
  chosen an opener in Windows itself (Explorer's `UserChoice`, from Vista
  on). The game's own per-user registration relies on the same rule.
- The override is a value that is there, not one that is missing, so it
  wins however a reader merges the two keys.
- An empty program identifier names no program. Explorer then treats a
  launch file as a file with no opener and asks which program to open it
  with, as it does for a copy whose registration was withdrawn and nothing
  else registered. Open With still lists the game.
- It never names the game or its folder, so nothing left behind can start a
  removed copy, and being empty tells it apart from a program's value.

The other ways were not taken. Explorer's `UserChoice` exists from Vista on
only, and Windows 8 and later reset one a program writes. A per-user key
for `OpenAnnihilation.Launch` cannot hide the computer's
`shell\open\command` beneath it, and that key is the user's own
registration's. A plain-text program identifier such as `txtfile` would
choose a program for the player, and could not be told apart from the
player's own choice of it.

A registration of launch files (the switch On) writes the user's own
(Default), `OpenAnnihilation.Launch`, over the override, so the user's six
values are back as before. The game never writes under
`HKEY_LOCAL_MACHINE`. The uninstaller removes the override of the account
that uninstalls: `.oalaunch`'s (Default) under
`HKEY_CURRENT_USER\Software\Classes`, while it is the empty text. A
program identifier there stays.

## Linux

In the user's XDG data folder (`$XDG_DATA_HOME` when it is absolute, else
`~/.local/share`), twelve files, each written only when its bytes differ,
through a temporary file renamed over it:

| File | What |
|---|---|
| `mime/packages/net.coreprime.open-annihilation.xml` | the registered MIME types, each with its comment, its parent and its glob: the packages a kind of `application/zip`, `.oareg` and `.oalaunch` a kind of `text/plain`. A launch file is also known by its content: any text that begins `oalaunch:` (a `magic` match at offset 0). A type left out is not in the package |
| `applications/net.coreprime.open-annihilation.desktop` | the opener of the seven the game installs or adds: `Exec="<exe>" %f`, `MimeType=` listing only those registered (`application/x-oamod;application/x-oalang;application/x-oamap;application/x-oamut;application/x-oabrain;application/x-oakeys;application/x-oareg;` when all seven are), `NoDisplay=true` |
| `applications/net.coreprime.open-annihilation.launch.desktop` | the opener of launch files: `Exec="<exe>" -c %f`, `MimeType=application/x-oalaunch;`, `NoDisplay=true` |
| `icons/hicolor/256x256/apps/net.coreprime.open-annihilation.png` | the program's icon |
| `icons/hicolor/256x256/mimetypes/application-x-oamod.png` | the mod packages' icon |
| `icons/hicolor/256x256/mimetypes/application-x-oalang.png` | the language packs' icon |
| `icons/hicolor/256x256/mimetypes/application-x-oamap.png` | the map packs' icon |
| `icons/hicolor/256x256/mimetypes/application-x-oamut.png` | the mutator packages' icon |
| `icons/hicolor/256x256/mimetypes/application-x-oabrain.png` | the brain packages' icon |
| `icons/hicolor/256x256/mimetypes/application-x-oakeys.png` | the key layout packages' icon |
| `icons/hicolor/256x256/mimetypes/application-x-oareg.png` | the registry files' icon |
| `icons/hicolor/256x256/mimetypes/application-x-oalaunch.png` | the launch files' icon |

The first desktop entry opens a file as a bare argument naming one of the
seven file types the game installs or adds, which the game opens; the
launch file's entry opens it after `-c`, as the start's launch. Started
with no file, either entry starts the game as usual. A launch file opened
while the game runs starts a second copy with it, as any second start
does. `NoDisplay=true` keeps it out of the application menus:
it is an opener that "Open With" lists, not a launcher. After a change the
game runs `update-mime-database` for a new MIME package,
`update-desktop-database` for a new desktop entry, and
`gtk-update-icon-cache` for a new icon where the user's hicolor folder
already holds an icon cache, each found on `PATH` and started without a
shell. A tool that is not installed is no error, and one still running
after five seconds is stopped. A run that succeeds leaves an empty stamp
file, `.net.coreprime.open-annihilation.updated`, in the folder the tool
works on (`mime`, `applications` or `icons/hicolor`); a change to the
tool's files removes it first. While a stamp is missing, after a run that
failed, was stopped or was never made, each start runs that tool again. A
build that starts no other programs (`OA_PROCESS_SPAWNING` off) runs none.
A copy under `$TMPDIR` or `/tmp`, inside a Flatpak or a Snap (which
register their own types), or whose path holds a control character or is
not UTF-8, registers nothing and withdraws nothing. Withdrawing a type
edits the package and the desktop entry that are already there, removes
that type's mimetype icon, and removes a file once it lists no type. The
program's icon is never removed. With all eight off, registering the empty
set removes both desktop entries and the MIME package the same way. The stamp
rules are a write's: a file whose bytes change loses its stamp first, and
a removed file loses it too.

## macOS

The bundle keeps the eight declarations and their icons. A switch does not
remove one. It chooses the default opener. The bundle owns the launch
file's type (`net.coreprime.open-annihilation.oalaunch`, a kind of
`public.plain-text`), and its switch is the eighth; the default-opener
questions answer for it as for the others (`TypeSet::launch`;
`apply_opener_changes` hands it back to the opener of plain text). The
bundle also takes `public.plain-text` it is handed without being its opener
(`LSHandlerRank None`), which `open -f -a` needs.

| Switch | On | Off |
|---|---|---|
| `.oamod`, `.oalang`, `.oamap`, `.oamut`, `.oabrain`, `.oakeys` | this copy opens that exported type | the default opener of `public.zip-archive` |
| `.oareg`, `.oalaunch` | this copy opens that exported type | the default opener of `public.plain-text` |

On every macOS version both questions use
`LSSetDefaultRoleHandlerForContentType` and
`LSCopyDefaultRoleHandlerForContentType`. Those calls are deprecated and
still answer at once; the deprecation warning is accepted only in
`file_types_mac.mm`. After a change the opener is read back. A change that
read does not show is logged (`the change did not take`) and is not
counted as done. A switch shows on when the default opener's bundle
identifier is this copy's. A question that fails is logged, that switch
is left as it was, and the others are still asked.
Nothing here runs at start: a choice made in Finder stays until a switch
moves and OK is pressed. A copy that is not an application bundle asks
nothing. iOS has no such questions, and System keeps the text row.

## Tests

`platform-file-types` checks the texts on every system (the launch file's
own desktop entry, its `-c` command and its magic among them), and the
default-opener questions against a stand-in: a switch that is on asks for
this copy and does not look up a zip or text opener, then reads the opener
back; a switch that is off asks for that opener, sets it on the exported
type and reads it back; a set the read does not show is logged and is not
counted; a type that did not move is not asked, a failed question is
reported and does not stop the others, and a system with no questions
skips. Elsewhere than on
Windows it registers into a scratch data folder, with three recording
scripts standing in for the tools on a scratch `PATH`, given two minutes
each so that a loaded machine never stops them: the nine icon files, the
MIME package and the two desktop entries are written with their bytes and
each tool runs once, the next run writes and runs nothing, a moved
executable rewrites the desktop entries alone, a new icon with an icon
cache runs the
cache tool, missing tools are no error and run at the next start that has
them, a tool that hangs is stopped at a one-second limit and runs again at
the next start, and the starts that register nothing write nothing.
`test_xdg_subset_and_withdrawal` registers two of the seven and checks that
the package, the desktop entry and the icons hold only those, withdraws one
and checks that the other remains, and with every install type off checks that the
desktop entry and the MIME package are gone and the program's icon remains;
with only a launch file on, its entry, its icon and its part of the package
stay, and the entry of the seven goes; withdrawn, they go too.
`test_brain_off_registers_the_other_six` registers with the brain off and
checks that the other seven are written and `.oabrain` is not, then withdraws
the brain alone and checks that the other seven stay. `test_keys_off_registers_the_other_seven`
registers with the key layout off and checks that the other seven are written
and `.oakeys` is not, then withdraws the layout alone and checks that the other
seven stay. The same cases run
under the Windows scratch key and against the default-opener stand-in.
On
Windows it registers under a scratch key of `HKEY_CURRENT_USER` and checks
the 48 values, the launch file's `-c` command among them, that the next run
writes nothing, that a moved executable rewrites two values per type, and
that a copy in the temporary folder writes nothing. `test_windows_withdrawal` registers two types, writes
another program's (Default), `Content Type` and `OpenWithProgids` value on
one extension and a `UserChoice` key beside them, withdraws one type and
checks that the other type and the other program's values are unchanged,
withdraws the launch file alone, then removes the scratch key.
`test_windows_shared_association` writes the installer's six values under a
second scratch key that stands for the computer's classes: Off writes the
override and the merged lookup finds no program, a later Off writes
nothing, On writes the user's six values back over it; a (Default) of the
user's own naming a program, and an association for every user naming
another program, are left alone; once that association names another
program, or none, an override left behind goes; another type gets no
override; and the computer's scratch key never changes. The other Windows
tests name an empty scratch key for the computer's classes, so a computer
with the game installed does not change what they see.
`test_user_override_step` checks the override's decision on every system.
`app-bundle-file-types` (`tools/check_bundle_file_types.py`) checks the two
`Info.plist` templates, the launch file's type and the macOS bundle's plain
text among them.

## Limitations

- Windows XP draws no PNG-compressed icon, and every entry of the
  executable's icon is one, so on XP neither the program nor one of the
  eight file types shows the game's icon.
- The override acts for the Windows account that runs the game. Another
  account keeps the installer's association until its own switch is off.
  The uninstaller removes the override of the account that uninstalls (an
  administrator who uninstalls from another account removes that
  account's); another account's override, which names no program, stays
  until the game starts there again. Explorer's `UserChoice`, from Vista
  on, wins over both registrations and the override.
- A registration only follows the copy that ran last: two copies in
  different folders take the files from each other at each start.
- The Windows uninstaller withdraws the registration of the account that
  runs it alone (an administrator's, when Windows asked for another
  administrator's password). Another account's registration still names the
  removed program until that account starts another copy of the game.
  Removing a copy the player unzipped, or the game on Linux, removes
  nothing; the installation pages say which keys and files to remove.
- On macOS the declaration stays in the bundle after the game is removed,
  until the bundle itself is removed. Off does not restore an opener that
  was neither this copy nor the default opener of a zip or of plain text:
  it hands the type to that default.
