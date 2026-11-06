# Boneyards settings

The engine's own settings stay in its settings dialog. That dialog is
described in the engine's `docs/settings.md`. This page is the Boneyards'
settings beside it: the three numbers on the Profile's local settings tab,
the login preference that opens the in-game registration route, the
compile-time release constants, and the master-server list a build bakes
in.

The Profile numbers and `BetaLoginUX` are player preferences. The running
game reads and writes them through the engine, in the section `Boneyards`
of the preferences file (`Boneyards|<key>`). A missing key reads as the
default below. APPLY is what stores a change on the local settings tab. A
value outside the range below is refused, and nothing on that tab is
stored.

The release constants and the master-server list are chosen when the
Boneyards is built. They are not rows in the preferences file.

## Contents

- [Background drone](#background-drone)
- [Console slide](#console-slide)
- [Unit limit](#unit-limit)
- [Account password](#account-password)
- [BetaLoginUX](#betaloginux)
- [Release constants](#release-constants)
- [Master servers](#master-servers)

## Background drone

| | |
|---|---|
| File | preferences file, section `Boneyards` |
| Key | `BYStopThatIncessantDroning` |
| Default | `0` (the drone plays) |
| Where the player sets it | Profile, local settings, the check box. APPLY stores `1` when it is checked and `0` when it is clear |

While the value is `0`, the Boneyards plays its background drone (the sound
`BGM`) when the main window is built, on each big-button slide, and when
chat is built again after a battle the Boneyards launched. APPLY that turns
the drone off stops the sounds that are playing. APPLY that turns it back
on starts the drone again. A set value also keeps the drone off at those
later points. Returning from a launched battle, and the "Host not found"
box, stop the sounds either way.

The key is declared in
`src/boneyards/screens/include/oa/boneyards/screens/profile.hpp`.
The navigation reads it when the screens start
(`src/boneyards/screens/src/navigator.cpp`). APPLY calls the
background-sound hook in
`src/boneyards/screens/src/profile.cpp`.

## Console slide

| | |
|---|---|
| File | preferences file, section `Boneyards` |
| Key | `BYConsoleSlide` |
| Default | `20` |
| Range | `1` through `40` |
| Where the player sets it | Profile, local settings. APPLY stores the number |

Opening or closing a console slides the green layer in from the right, or
back out. The slide takes this many pulses and two more: the frame it
starts on, the steps across, and the pulse that shows the console or hands
the map back. The
layer reads the key when it is created and holds a value outside `1`..`40`
to that range
(`src/boneyards/console/src/console_layer.cpp`). APPLY gives the
open layer the new count, so the next slide uses it
(`src/boneyards/console/src/profile_view.cpp`).

A number outside `1`..`40` is refused: "Invalid Entry! Slide Steps Must be
a number between 1 and 40". The constants are
`kConsoleSlideKey`, `kSlideStepsDefault`, `kSlideStepsMin` and
`kSlideStepsMax` in
`src/boneyards/screens/include/oa/boneyards/screens/profile.hpp`.

## Unit limit

| | |
|---|---|
| File | preferences file, section `Boneyards`, and the engine's unit-limit preference |
| Key | `UnitLimit` (the engine stores the same number as `open-annihilation.unit-limit`) |
| Default | `250` when `UnitLimit` is absent |
| Range | `21` through `500` |
| Where the player sets it | Profile, local settings. APPLY stores the number |

This is the units-per-player limit the Profile keeps. It is separate from
the slider in the engine's Common Tweaks, which runs from 50 to 1500 in
steps of 50 and starts from the installation or from 250. APPLY of a new
Profile value stores `UnitLimit` and passes that number to the engine's
unit-limit preference. The engine keeps it as the configured limit a
multiplayer game uses, and a multiplayer game plays the host's limit. The
engine still clamps a value to the range that installation offers.

A number outside `21`..`500` is refused: "Invalid Entry! Max User Limit
Must be a number between 21 and 500". The constants are `kUnitLimitKey`,
`kUnitLimitDefault`, `kUnitLimitMin` and `kUnitLimitMax` in
`src/boneyards/screens/include/oa/boneyards/screens/profile.hpp`.
APPLY reaches the engine from
`src/boneyards/extension/boneyards_extension.cpp`.

## Account password

A password typed on the Profile Account tab is not applied by Core Prime.
APPLY still checks that the two entries match and that the password has
no apostrophe, and the client still sends the `PASSWORD` field with the
other account edits (`src/boneyards/screens/src/profile.cpp`). Core Prime
reserves that field and does not change the password. Passwords change
on the web.

## BetaLoginUX

| | |
|---|---|
| File | preferences file, section `Boneyards` |
| Key | `BetaLoginUX` |
| Default | `0` (the in-game registration route stays hidden) |
| Where it is set | a completed login, not a Profile row |

A login that reaches the account dossier stores `1` when that account is
Admin (status 4) or Beta Tester (status 5), and `0` for every other
status. A login that never reaches the dossier stores nothing, so the
previous value stays. The next time the login console opens, REGISTER
offers the in-game form as well as the web page when the stored value is
not `0`. An ordinary account's later login stores `0` and the in-game route
hides again, unless the registration-form release constant is on (below).

The key is `kBetaLoginUxKey` in
`src/boneyards/toolkit/include/oa/boneyards/release.hpp`. The dossier stores
it from `src/boneyards/console/src/status_widget.cpp`. The login
console reads it in
`src/boneyards/console/src/main_frame.cpp`.

## Release constants

Each unfinished feature has a CMake option. The default is off, which
compiles the matching `OA_BONEYARDS_<FEATURE>_RELEASED` macro as `0`.
Turning the option on compiles it as `1`. The macros are named in
`src/boneyards/toolkit/include/oa/boneyards/release.hpp` and set from
`cmake/OaBoneyards.cmake`.

| CMake option | Macro | Default | Who sees the feature while it is `0` |
|---|---|---|---|
| `OA_BONEYARDS_MATCHMAKING_RELEASED` | `OA_BONEYARDS_MATCHMAKING_RELEASED` | off (`0`) | Admin (4) and Beta Tester (5); other accounts have no MATCHMAKING in Chat and no match control panel on the battle list |
| `OA_BONEYARDS_REGISTRATION_FORM_RELEASED` | `OA_BONEYARDS_REGISTRATION_FORM_RELEASED` | off (`0`) | a player whose `BetaLoginUX` is set; account status does not count |

While a constant is `1`, that feature is on screen for every account:
Matchmaking for every account status, and the registration form without
waiting on `BetaLoginUX`. When a feature is finished, it stays on for
every account and its option and macro are removed. They are not written
into the preferences file.

QuickPlay and the Locator are finished. They were released to every
account on 2026-11-06 and have no option or macro: their buttons on the
Main Frame are lit for every account whenever the other console buttons
are, no dust cover is drawn over either, and each opens its console.

## Master servers

| | |
|---|---|
| File | the server list named by the CMake cache entry `OA_BONEYARDS_MASTER_SERVERS` |
| Default file | `data/boneyards/master-servers.byd` |
| Default entry | `alpha`, domain `clusters.coreprime.net`, cluster size `2`, location `CorePrime (Alpha)`, region `US East`, selected |
| Effect | the yard master Rover dials when the player has no saved list and no own server.byd |

The file uses the same lines as `boneyards/server.byd`. Each line is five
tab-separated fields: name, domain, cluster size, location, flags. An
optional sixth field, after the flags, is the server's region. Locator's
Server Transfer shows it as WHERE. A line with no region field is an older
list and shows nothing there. The default Alpha line's region is `US East`.
A `D` in the flags marks the selected server. A `P` keeps the server when a
yard master sends its own list. A cluster size of `0` reaches the yard
master at `name.domain` (a domain that starts with a space means the bare
name). Any other cluster size uses `name` with `1` before the dot. Rover
dials that host on port 9110.

Rover tries, in order, `boneyards/serv_sav.byd` in the player's `Boneyards/`
folder (a `boneyards/` folder left by an earlier build is read, and renamed
to `Boneyards` the next time the list is saved, where the file system has
two names for it), then the player's `boneyards/server.byd`, then this
baked list. A `server.byd` in the expansion or the game folder is not
read. The login console's server list and Locator's Server Transfer show
whichever list that search produced. `--boneyards-server` still writes a
one-entry list and that list is used for the run. Point
`OA_BONEYARDS_MASTER_SERVERS` at another file to bake that list in
instead. The reader is
`src/boneyards/rover/src/servers.cpp`; the cache entry is set in
`cmake/OaBoneyards.cmake`.
