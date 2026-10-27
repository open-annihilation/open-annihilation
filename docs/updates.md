# Updates

Open Annihilation can look for a new release of itself. A newer release is
only offered. Nothing is downloaded or installed unless you ask, and that
install is separate from the check.

## What is checked

The check reads the published stable feed: the list of releases, and the
signature beside it. The signature is checked against the keys this build
pins before any release in the list is considered. A file that does not
verify is discarded. The channel is the stable one.

The version you are running, and the kind of package it is (this computer's
platform, a Windows 95 build, or a Steam Deck package when that folder sits
beside a Linux build), decide whether a release in the list is newer and
has a package for you.

## When

The check runs from the main menu, once that menu has settled and nothing
is open over it. It runs when Check for updates is Automatically, and only
when the last check that reached the server was at least a day ago, or has
never happened, or is dated in the future because the clock moved back.

One automatic check runs per start. A check does not run during a match. It
does not run before the welcome at the first main menu is finished. It does
not run in an unattended start, or a headless check, unless that start was
given a feed to read.

Check for updates at Only when I ask does not run on its own. Never never
makes a request.

## Settings

Settings › Updates opens on a card that says where an update stands, in
the words of [Where an update stands](#where-an-update-stands): "Installed"
and the version you are running, the state, a line of detail, a bar while
a download moves or is paused, the state's actions as buttons, the first in
the accent, and a note under them. Up to date offers **Check now**; a
release that is out, **Download update** and **Skip this version**; a
download, **Pause** and **Cancel download**, or **Resume** while it is
paused; a ready release, **Install when I quit** and the action that
installs now; a scheduled one, **Don't install when I quit** and the same
action; a skipped release, **Try this version** and **Check now**; a
failure, **Try again**, with **Cancel download** after a download that
stopped and **Show details** where a receipt says what happened
([The receipt](#the-receipt)). While a check runs the card has no button. The card's buttons act at once and the dialog stays open.
A copy that cannot replace itself keeps Check now, and the card says who
updates it
([Copies a package manager updates](#copies-a-package-manager-updates)).

Under the card:

- **Download automatically**, Off unless you turn it on. Off, OA asks
  before it downloads a release.
- **Check for updates**, Automatically, Only when I ask, or Never.
  Automatically is the check described above. Only when I ask runs a check
  only from Check now. Never makes no request: the card says "OA never
  checks" and offers no Check now.
- **Install updates**, Ask each time or When I quit. When I quit schedules
  a ready release for the quit without asking.
- **Previous version** and the kept version, with **Roll back…**, while a
  version is kept ([Rolling back](#rolling-back)).

The three choices take effect when you press OK, and Cancel leaves them as
the dialog opened with them. The hint under Check for updates says OA looks
for a newer Open Annihilation once a day when it starts, and that it never
installs one until you say so. While a download moves or is paused the
section list shows its share beside Updates, as "41%", and "ready" while a
release is ready or scheduled.
[Settings › Updates](settings.md#updates) lists the rows.

## The menu's pill and the update centre

While a package downloads, is paused, is being checked, is ready or is
scheduled, a pill at the top right of the main menu says so, in the short
form of the state, with one button:

| The pill reads | Its button |
|---|---|
| "0.8.8 · 41%", with a bar | **Pause** |
| "0.8.8 · paused", with an amber bar | **Resume** |
| "0.8.8 is ready" | **Install…**, which asks the ready prompt again |
| "0.8.8 installs when you quit" | **Change** |
| a download that stopped | **Try again** |

A press anywhere else on the pill, or on Change, opens the update centre:
the state, its detail, the bar, the note and the state's actions as
buttons, such as Pause, Resume, Cancel download, Don't install when I quit
and the action that installs now, and **Close**. It follows the state while
it is open, so Pause turns into Resume and the bar moves. The pill shows
only on the main menu, never in a match, and hides while a window is open
over the menu. Where an extension has moved the OA buttons to the top of
the menu, the pill sits under them.

## What is sent

Two requests, and nothing else. One asks for the feed and one for its
signature. Each uses the game's own user agent, `OpenAnnihilation/` and the
version. There is no query string, no cookie, no install ID and no other
header. The check does not say who is playing.

## What is kept

When both files verify, they are stored under the data folder in
`updates/feed.json` and `updates/feed.json.sig`. They are written only
after they verify. A check that cannot find the server, or runs out of
time, leaves the stored files as they were and tries again on a later
start. A check that reached the server, including one whose feed was
refused, waits a day before the next automatic check.

The first main menu of each start reads the stored feed again, with no
request, and checks its signature as a fetched feed is checked. A release
it offers is offered again, so LATER asks again at the next start, and a
package already downloaded for it is found. Check for updates at Never
reads nothing.

## Where an update stands

The menu's pill, the update centre, the prompts, Settings › Updates and
the headers of Settings and the Library read one state, and show the same
words and the same size for it:

| State | Words |
|---|---|
| Checking | Checking for updates… |
| Up to date | Up to date |
| A release is out | {version} is out |
| Skipped | {version} is skipped |
| Downloading | Downloading {version}, with the bytes, the share and the time left |
| Paused by you | Download paused |
| Paused by a match | Paused during a match. Downloads pause during a match and carry on after it. |
| Checking the download | Checking the download |
| Ready, nothing scheduled | {version} is ready |
| Ready, installs when you quit | {version} installs when you quit |
| Installing | Installing {version} |
| Finishing | Finishing the update to {version} |
| Updated | Updated to {version}, from the version before |
| Back on the kept version | Back on {version} |

A check, a download or an install that fails says which way it failed:

| Outcome | Words |
|---|---|
| The server did not answer | Could not connect. Try again later. |
| The signature, or the downloaded file, was refused | The update could not be verified. There is no way past this check. |
| Stopped before any file moved | Not installed. {version} is unchanged. |
| You declined Windows's permission prompt | The update was cancelled. |
| A step failed and every change was undone | The update failed. {version} was put back. |
| Putting the files back did not finish | Recovery could not finish. |

The header beside the version shows the short form, the pill's words,
while a package is downloading, paused, being checked, ready or scheduled,
such as "0.8.8 · 41%" or "0.8.8 installs when you quit". Where those words
would run into the title, as in Settings on a 640 by 480 window, Settings'
header leaves them out.

## The prompt

When a check has verified a release this build has not skipped, the main
menu asks: "Open Annihilation {version} is out", then "You have {current}.
The update is {size}." The size is the file this copy would download: the
installer where this copy installs from one, the zip otherwise. It is the
Library's size, in binary KB and MB, and the pill, the update centre and
Settings › Updates name the same figure. The release's notes stand in a
What's new box, one note a line with its bullet, in the language the game
is showing; notes the language does not have are shown in English. Under
them the question says your saved games, settings and mods stay as they
are, and that downloads pause during a match and carry on after it.

The buttons are **Skip this version**, at the left, **Later**, which
Escape answers, and **Download update**. Where this copy cannot install
the release, the last button is **Open the release page**, which opens the
release's page from the signed feed. On a Steam Deck in Game Mode, where
no browser opens, the question names the page instead of opening it. A
release that names no page opens the releases page on the project's site.

The question waits while a match is being played, while Settings is open,
or while a message box is open. It does not take a match's input. A match
that starts while it is open closes it, and the next settled menu of that
start asks again.

Skip this version stays quiet for that release and every older one. A newer
release asks again. The number is `open-annihilation.update-skipped`, and
Try this version on Settings › Updates forgets it. Later writes nothing and
asks again at the next start. The question is asked once a start, whether
the check ran on its own or you asked.

A release that did not install, as the receipt below says, does not ask.
It waits until you choose to try it again. A release whose package is
already downloaded asks with the ready prompt instead.

Download update downloads the package into `updates/<release>/` under the
data folder. Downloading and installing are separate choices: a downloaded
package is never installed until you say when. A download that stops
continues from the bytes already stored, and the game tries each mirror in
the feed in order. It pauses while a match is being played and continues
when you leave. **Pause** on the pill or in the update centre holds it
until you press **Resume**. The file is ready only when its size and
SHA-256 match the feed. A package an earlier start downloaded is checked
against the feed again and kept ready; one that no longer matches is
fetched again. **Cancel download** deletes what the download stored, so
nothing half-downloaded is kept, and the release is offered again.

Download automatically is off unless you turn it on in Settings › Updates.
While it is on, a release that can be installed here starts downloading by
itself at the settled main menu, and the ready prompt is the first
question.

The prompt is titled "Open Annihilation {version} is ready", over a line
that says it was downloaded and checked, with its size, and a box, When
should it go in?, that lists the three answers with what each does. The
buttons are those answers, in the update centre's words, Install when I
quit in the accent:

- **Keep it ready**, which Escape answers, schedules nothing. The package
  stays, quitting installs nothing, and the prompt asks again at the next
  start.
- **Install when I quit** schedules this release. The choice is stored
  (`open-annihilation.update-install-on-quit`), the header says "{version}
  installs when you quit", and the next normal quit writes the plan and
  starts `oa-update`, in this start or a later one. Taking it back leaves
  the package ready.
- The third button installs now, and names the way this copy installs:
  **Install and restart** where the helper swaps the files and starts the
  game again (the Windows installer adds "Windows asks for permission."),
  **Open the installer** where macOS Installer puts the release in
  Applications, and **Close the game** on a Steam Deck, where the version
  installs the next time you start Open Annihilation and the helper does
  not start the game.

Quitting any other way never installs. The action that installs now on
Settings › Updates and in the update centre installs now as well, and
Don't install when I quit takes a schedule back. Where this copy has no
route, Open the release page opens the release page as before. With
Install updates at When I quit (`open-annihilation.update-install` set to
`when-i-quit`), a ready release is scheduled for the quit without asking.

The first settled main menu of a start that names `--updated-from` with an
older version shows "Updated to {version}" once. When the saved feed has
notes for that version, they are listed under "New in {version}:", one
note a line with its bullet. While a version is kept, that notice also
says so, and that Settings › Updates takes you back to it.

## The receipt

The helper writes `updates/receipt.json` as it works: each step, whether
it finished or failed and why, the outcome, the versions and the times.
It rewrites the file after every step, and names the outcome before it
starts any version of the game, so a run that is cut short still leaves
what it had done. The first settled main menu of the next start reads it
and shows it once.

An install that did not put the release in place shows a recovery screen
titled with its outcome, in its own words (see
[Where an update stands](#where-an-update-stands)). A line under the title
says your saved games, settings and mods stay as they are where the
version you had was put back, or, in red, that some files were left
half-moved where recovery did not finish. What happened lists each step
with a tick or a cross and why it failed; where recovery did not finish,
To put the game right lists the steps that would. The screen ends with
"{version} waits until you choose to try again." and three buttons:
**Show the log**, at the left, **Try again** where the release can be
tried again, and **Continue**. The same screen is the update centre's Show
details.

The release is then held: it is not offered, downloaded or installed by
itself, and a scheduled install is taken back, until you press Try again
here, on Settings › Updates or in the update centre
(`open-annihilation.update-held`). A newer release is offered as usual. A
receipt the helper left running for ten minutes was cut short and reads as
recovery that could not finish.

## Rolling back

A version marks itself good when it first reaches the main menu, whatever
is open over it: the welcome, a message or Settings. Until then, a start
that does not get there counts as failed only when it crashed or stopped
with an error. A start you quit before the main menu takes its count back,
and how long a start takes to load never counts. On macOS and Windows the
game counts a start before it loads the game's files or a mod, and after
two failed starts it hands the kept version to `oa-update` and leaves. On
Linux and Steam Deck the launcher counts in `<install>/update-starts` and,
at the same point, points `current` at the kept version and starts it; a
quit takes one off that count. A run that only checks something, and a run
you asked to leave unattended, is not counted as a start. A version that
already reached the main menu is not put back by this.

The helper watches the first start it makes of the new version. A start
that reaches the main menu ends the watch. One you quit before the main
menu ends it too, and nothing is started again. A start that crashed is
started once more, and a second crash puts the kept version back. A start
the helper cannot see is never taken for a failed one.

The kept version comes back with the preferences file copied when the new
version was swapped in. The bad release is not offered again. Saves, mods,
the user folder and installed content stay where they are. The old version
says "Back on {version}", with SHOW THE LOG and OK.

Settings › Updates shows Previous version while one is kept, and
**Roll back…** asks once, then restores the same way. The question, "Roll
back to {version}?", shows what comes back (Open Annihilation {version}
and your settings from before you updated) beside what stays as it is
(saved games and replays; mods, maps and key layouts), warns that
settings you changed after updating are replaced by the ones you had
then, and says the game closes, puts the kept version back and starts
again, and that the version you leave is not offered again until a newer
release. **Cancel** is marked, so Enter does not roll back; **Roll back
and restart** does. A roll back that fails undoes what it had done and
says "The roll back failed. Nothing was changed.", or, when that undo does
not finish, "The roll back failed. The game could not be put back."

## Turning it off

On the welcome's online step, **Not now** sets Check for updates to Never.
**Next** on Turn on online features sets it to Automatically. Settings ›
Updates sets the same choice, and it takes effect when you press OK.
Never makes no request. The setting is `open-annihilation.update-check` in
the [preferences](../src/platform/preferences/README.md).

## What happens when you update

When a verified package is scheduled for the quit and you quit, or you
choose to install it now, the game writes a plan and starts `oa-update`,
then quits. The helper waits until the game has left. It
checks the saved feed's signature against the keys this build pins, and
the downloaded package's name, size and SHA-256 against that feed. A
package that does not match is not unpacked. The window says "The update
could not be checked. Nothing was changed." and the game starts again on
the version you had.

While the helper works it can show a small window, the same kind the game
shows for a long task, drawn with the fonts the game ships. The title and
the line are already in your language, for example "Installing 0.8.7" and
"0.8.6 is kept, in case you want it back. The game starts again by
itself." A swap that finishes quickly shows no window.

The version you had is kept, and a copy of the preferences file sits in
`updates/previous`. `updates/state.json` records what is installed and what
was kept. On Windows, an installed copy keeps the program files in
`updates/previous` and then runs the new setup. If you decline the
permission prompt, nothing changes and the game says "The update was
cancelled." A zip copy keeps the program files in `previous` beside the
game. On Windows 95, 98 and Me the window says "Restart Windows to finish
installing {version}." and the game waits for that restart. On the other
systems the game starts again. If a step fails after the helper has
changed the game, it puts back what it had changed and the window says
"The update failed. The game was put back." The game starts again on the
version you had. The helper makes no request of its own.

On a Mac, the helper checks that the new app is signed by the same team,
that the signature is valid, and that Apple has notarized it. It then moves
the app you were running aside and puts the new one in its place, and
starts it. A folder you cannot write opens the signed installer instead,
and the window says Installer will put the release in Applications. A copy
macOS is running from a protected place asks you to move it to Applications
first, and is not replaced where it stands.

## Linux and Steam Deck

A Linux or Steam Deck copy in a folder you own updates in that folder. The
first update moves the program into `versions/<old>/`, puts the new release
in `versions/<new>/`, and points `current` at the old one until the next
start. A launcher takes the place of `open-annihilation`, so a Steam
shortcut or a desktop entry that names that file keeps working. It does not
name a folder inside `versions/`. The next start makes `current` name the
new version and starts it. The launcher deletes nothing. A later update
removes a version that is neither the one running nor the new one.

On a desktop the helper starts that launcher at once. On a Steam Deck it
does not start the game. The window says "{version} installs the next time
you start Open Annihilation."

A copy under `/opt`, `/usr`, `/snap` or `/var/lib/flatpak`, a copy you do
not own, and an AppImage or Flatpak only shows "This copy is installed for
the whole system, so it cannot replace itself. Install the new release the
way this copy was installed."

An install of 0.8.6 or older is still updated by hand once, to 0.8.7.
After that the game updates itself. `oa-tool` is `current/oa-tool` once
the first update has made `current`.

## Copies a package manager updates

A copy from Flathub, Nix, Homebrew or a distribution is built so it does
not replace itself. It still checks for a release when you allow it. The
prompt says who updates it, for example "This copy came from Flathub,
which updates it." and "Open your software centre, or run:" with the
command on the next line. There is no Skip, Later or Download update. OK
dismisses it until the next start. Settings › Updates keeps Check now, and
the card says "Updated by Flathub". The command is shown as it was
written.

The repository's Nix package for Linux is built this way. The official
Windows package still replaces itself.

iOS is built the same way and names nobody. The notice says this copy
cannot replace itself and offers the release's page beside OK. You build
the new release the same way
([iOS](installation/ios.md)).

A packager sets this in
[What a platform can do](development/platform.md#what-a-platform-can-do).
