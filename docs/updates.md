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

Settings › Updates shows the version you are running and where the last
check stands: up to date, checking, a release out, could not check, or not
checked yet. The line names Stable, and when the check last reached the
server. CHECK NOW runs a check at once. While a release is offered the
button is UPDATE…, which opens that release's page the way the prompt's
UPDATE does. The button is greyed while a check is running. A copy that cannot replace
itself keeps CHECK NOW, and the line says who updates it
([Copies a package manager updates](#copies-a-package-manager-updates)).

Check for updates is Automatically, Only when I ask, or Never. It takes
effect when you press OK. Cancel leaves the choice the dialog opened with.
Automatically is the check described above. Only when I ask runs a check
only from CHECK NOW. Never shows "OA never checks", hides the button, and
makes no request, including from CHECK NOW.

The hint under the choice says OA looks for a newer Open Annihilation once
a day when it starts, and that it never installs one until you say so.
[Settings › Updates](settings.md#updates) lists the rows.

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

## The prompt

When a check has verified a release this build has not skipped, the main
menu asks. The question names the release, the version you have, the size
of this build's package, and the release's notes in the language the game
is showing. Notes the language does not have are shown in English. It says
your saved games, settings and mods stay as they are.

The buttons are SKIP THIS VERSION, LATER and UPDATE. UPDATE opens the
release's page from the signed feed. On a Steam Deck in Game Mode, where
no browser opens, the question names the page instead of opening it. A
release that names no page opens the releases page on the project's site.

The question waits while a match is being played, while Settings is open,
or while a message box is open. It does not take a match's input. A match
that starts while it is open closes it, and the next settled menu of that
start asks again.

SKIP THIS VERSION stays quiet for that release and every older one. A newer
release asks again. The number is `open-annihilation.update-skipped`. LATER
writes nothing and asks again at the next start. The question is asked once
a start, whether the check ran on its own or you asked.

Where this copy can install a release, UPDATE downloads the package into
`updates/<release>/` under the data folder. A download that stops continues
from the bytes already stored, and the game tries each mirror in the feed
in order. It pauses while a match is being played and continues when you
leave. The file is ready only when its size and SHA-256 match the feed.

Download in the background is off unless you turn it on in Settings ›
Updates. While it is on, a release that can be installed here starts
downloading by itself at the settled main menu, and the ready prompt is
the first question. While a package is being fetched, Settings and the
Library show "{version} downloading" beside the version. When it is ready
they show "{version} ready".

The prompt says "Open Annihilation {version} is ready". WHEN I QUIT leaves
the file where it is. The next time you quit normally, or when you press
RESTART NOW, the game writes the plan and starts `oa-update`. Where this
copy has no route, UPDATE opens the release page as before.

The first settled main menu of a start that names `--updated-from` with an
older version shows "Updated to {version}" once, with the notes from the
saved feed when it has them.

## Turning it off

On the welcome's online step, **Not now** sets Check for updates to Never.
**Next** on Turn on online features sets it to Automatically. Settings ›
Updates sets the same choice, and it takes effect when you press OK.
Never makes no request. The setting is `open-annihilation.update-check` in
the [preferences](../src/platform/preferences/README.md).

## What happens when you update

When a verified package is ready and you quit, or you press RESTART NOW,
the game writes a plan and starts `oa-update`, then quits. The helper waits until the game has left. It
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

The version you had is kept under the data folder, in `updates/previous`,
and a copy of the preferences file sits beside it. `updates/state.json`
records what is installed and what was kept. The game then starts again.
If a step fails, the helper puts back what it had changed and starts the
version you had. The helper makes no request of its own.

On a Mac, the helper checks that the new app is signed by the same team,
that the signature is valid, and that Apple has notarized it. It then moves
the app you were running aside and puts the new one in its place, and
starts it. A folder you cannot write opens the signed installer instead,
and the window says Installer will put the release in Applications. A copy
macOS is running from a protected place asks you to move it to Applications
first, and is not replaced where it stands.

## Copies a package manager updates

A copy from Flathub, Nix, Homebrew or a distribution is built so it does
not replace itself. It still checks for a release when you allow it. The
prompt says who updates it, for example "This copy came from Flathub,
which updates it." and "Open your software centre, or run:" with the
command on the next line. There is no Skip, Later or Update. OK dismisses
it until the next start. Settings keeps CHECK NOW, and the status says
"Updated by Flathub". The command is shown as it was written.

The repository's Nix package for Linux is built this way. The official
Windows package still replaces itself.

iOS is built the same way and names nobody. The notice says this copy
cannot replace itself and offers the release's page beside OK. You build
the new release the same way
([iOS](installation/ios.md)).

A packager sets this in
[What a platform can do](development/platform.md#what-a-platform-can-do).
