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

## Turning it off

On the welcome's online step, **Not now** sets Check for updates to Never.
**Next** on Turn on online features sets it to Automatically. Never makes
no request. The setting is `open-annihilation.update-check` in the
[preferences](../src/platform/preferences/README.md).
