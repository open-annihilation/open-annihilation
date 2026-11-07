# Entering the Boneyards

The main menu always shows a way into the Boneyards. It looks the same
whether or not the Boneyards expansion is installed. While `by.ccx` is not
mounted, the Boneyards' art is drawn from the graphic embedded in the game.
Once that archive is mounted, it is drawn from the archive, and the picture
is the same.

## The strip and the ENTER plate

The way in takes one of two forms, chosen once a run, as the main menu's
picture is first loaded:

- **The strip** is the Boneyards' 640 by 102 panel across the bottom of the
  menu, from row 378: the status line, Single, Multi, Intro and Exit, the
  credits spot, the red ENTER button and the row of activity lights. It is
  drawn where the menu's art in those rows is the base game's: on plain
  3.1c, and on the mods drawn on its art.
- **The ENTER plate** stands in for the strip on a menu whose art is a
  mod's own. The menu keeps the MAINMENU.GUI it has without the Boneyards,
  the mod's own or the base game's, also when the Boneyards expansion is
  installed (only the played mod's own folder counts as the mod's), so
  every one of the mod's buttons is where the mod put it and works as it
  does without the Boneyards. The plate is the strip's own riveted ENTER
  panel, 135 by 66 pixels from (253,380) of the strip: its screws, the
  Boneyards and ONLINE GAMING lettering, the red ENTER and the eight
  lamps. The strip draws it, in the base game's palette, and it is copied
  at its own size inside a one-pixel dark line, with no frame of Open
  Annihilation's. Its ENTER presses and lights as on the strip; the rest
  of the plate takes no click. While a connection is still closing, ENTER
  is greyed and the lamps run, as they do on the strip. Nothing else of
  the strip is drawn or takes the pointer, and with no palette nothing
  is.

The game sets the menu's FrontendX against the base game's over the
strip's rows. When at most a tenth of those pixels differ (any channel by
more than 24), the strip is drawn; otherwise the plate (button mode). The menus drawn on
the base game's art differ by 4% at most, a recoloured copy among them, and
the menus with art of their own by 17% or more. The log says which form was
chosen and the share, for example `boneyards menu: button mode; the menu's
art differs from the base game's in 66.1% of the strip's rows`. Forcing the
strip with `mode: strip` on art of the mod's own draws it through that
art's palette, so its colours come out wrong unless the palette is the base
game's; the plate is always drawn in the base game's palette.

While the strip is drawn, it takes every pointer event inside its own rows,
so a button of the menu under its art takes no click. Credits, under the
strip's credits spot, still opens the credits. A press that began above the
strip keeps its release wherever it lands. A gadget only partly under the
strip keeps its visible part. A gadget wholly under it takes no keyboard
focus and no quick key, unless it is one of the menu's own SINGLE, MULTI,
INTRO, EXIT or Credits, which the strip's own buttons show: their keys do
what the strip's buttons do.

Open Annihilation's two buttons stay where 0.8.14 put them: at the top
right while the strip is drawn, clear of it, and at the bottom right on
every other menu. Nothing moves them.

The plate stands where the mod's profile puts it, or else at the default
place, the top right 8 pixels in: (497,8). The game does not look for a
place clear of the menu's buttons or of the words painted into its
picture; each mod's profile names the place that suits its menu. The log
says where the plate stands, for example `boneyards menu: the ENTER plate
stands at 497,8, the default place at the top right` or `... at 172,377,
where the mod's profile puts it (media.boneyards-entry)`, and adds a line
when the plate covers a gadget of the menu or one of Open Annihilation's
buttons.

A mod chooses for itself in its `oamod.yaml`, under `media`, with
`boneyards-entry`:

- `mode` is `auto` (the measure above), `strip`, `button` or `none`.
  `none` shows neither the strip nor the plate, and the menu keeps its
  layout without the Boneyards as in `button`; the B key and `-y` still
  enter.
- `x` and `y` are the plate's top-left corner in the 640 by 480 menu's
  pixels, given together: `x` from 0 to 505 and `y` from 0 to 414, so the
  whole plate stays on the menu. They place the plate in button mode only;
  in strip mode the log says `boneyards menu: media.boneyards-entry x X and
  y Y place the ENTER plate in button mode only; this menu is in strip
  mode`.

The mode alone:

```yaml
media:
  boneyards-entry: {mode: button}
```

The place alone, with the mode chosen from the art:

```yaml
media:
  boneyards-entry: {x: 8, y: 8}
```

Both, World Domination's plate between its EXIT and SINGLE:

```yaml
media:
  boneyards-entry: {mode: button, x: 172, y: 377}
```

`boneyards-entry` is a mapping: `boneyards-entry: button` is refused with
`media.boneyards-entry: must be a mapping of mode, x, y`, and `x` without
`y`, or `y` without `x`, with `media.boneyards-entry: x and y must be
given together`.

Leaving the key out is `{mode: auto, x: 497, y: 8}`. The key is a view
key: it never reaches the rules every machine of a battle shares (the
profile's sim hash). An engine older than 0.9.0 refuses a profile with it
as an unknown key, so a mod that uses it requires `>= 0.9.0`
([docs/mods/oamod-standard.md](../mods/oamod-standard.md)).

`native-boneyards-menu-modes` checks both forms with made-up mods, played
with the Boneyards installed as the expansion: one drawn on the base
game's art, whose own EXIT lies under the strip's art and whose profile's
place is logged as unused; one with art of its own and no MAINMENU.GUI,
which keeps the base game's SINGLE at 139,393 and opens the Boneyards from
the plate's ENTER at the default place; and the same with the place
40,200 in its profile. Its lines are `boneyards menu modes check: strip:
...` and `boneyards menu modes check: button: ...`.

## What ENTER does

ENTER, on the strip or the plate, the B key and starting with `-y` do not
open the Boneyards at once. The game first makes sure of four things, in
this order, and only then shows the update screen and opens the
Boneyards. A player never lands in a half-installed or out-of-date
Boneyards.

1. **The online features.** The Boneyards needs the check for updates, the
   check for content updates, and, when the registry the package will come
   from requires one, that registry's install ID. When any of those is
   off, the game asks. Either check being off names both checks, as 0.9.0
   did: "The Boneyards needs Open Annihilation's online features: update
   checks and the content catalogue. Turn them on and continue?" When the
   game already has that registry's list, from this start or an earlier
   one, one question names the checks and the install ID together: "The
   Boneyards needs Open Annihilation's online features: update checks, the
   content catalogue and {registry}'s install ID. Turn them on and
   continue?" The install ID on its own is "The Boneyards needs
   {registry}'s install ID turned on. Turn it on and continue?"
   `{registry}` is the registry's name, Core Prime for the one the game
   ships. A player with a check off and no list kept sees the checks
   question first, then the install-ID question. **Turn on** sets each
   check that is off to Automatically, and, when the question named an
   install ID, clears that registry's off choice. When the question comes
   as you press ENTER, Turn on goes on to the update check first. A
   question asked once the catalogue has answered, such as the install-ID
   question after the checks question, goes straight to the fetch, which
   makes the install ID. **Cancel** stays on the main menu and changes
   nothing. A registry that does not require an install ID is never asked
   about one, even when its key is off. A player who went offline at first
   run has both checks off and Core Prime's install ID off. When the game
   already has Core Prime's list, from this start or an earlier one, that
   player sees the one question that names the checks and Core Prime's
   install ID. With no list kept, that player sees the checks question
   first, then the install-ID question.
   [What entering the Boneyards turns on](../settings.md#what-entering-the-boneyards-turns-on)
   names these settings.
2. **The latest Open Annihilation.** The game asks the update feed for the
   player's channel and waits for its answer; it does not go on without
   one.
   - When a newer release is out, it asks "Open Annihilation {version} is
     out. The Boneyards needs the latest version." with **Update** and
     **Cancel**. There is no Not now: the Boneyards is never entered on an
     older Open Annihilation. **Update** downloads the release the way
     Settings › Updates does, shows its progress (Cancel stops the
     download), and once it is ready installs it and starts the game
     again. The new run goes straight into the entry, as after a mod is
     switched off. Where the release is offered only on its page, to be
     installed by hand, **Update** opens that page and the entry stops.
   - When the check fails, offline or with the feed out of reach, or does
     not answer within half a minute, the game says "Open Annihilation
     could not check for updates." with **Try again**, which asks the feed
     again and waits, and **Cancel**.
3. **The mod**, when one is on (below).
4. **The latest Boneyards.** The game asks each catalogue once more for its
   current list and waits for the answer. The expansion's `.oa-content`
   record names the package it was installed from, the whole `.oaexp`'s
   SHA-256 and its release. The package is downloaded and installed only
   when the catalogue's current package is another, when the expansion is
   not installed, or when its record names no package (a copy a 0.9
   development build installed, which is then fetched once and recorded).
   When the record names the catalogue's package, nothing is downloaded or
   installed. "Fetching the Boneyards… 42%" shows on the menu while a
   download runs, and a click then does nothing. A copy no catalogue lists
   is used as it is. A game folder that holds the Boneyards' files itself,
   as a 1998 Boneyards install does, holds the bytes every package of the
   expansion carries: nothing is fetched then, and no catalogue is asked.

   A package the entry installs, a first install or an update, is put in
   use by starting the game again once. The Boneyards' screens are built
   with the main menu, before the package was there, so the game turns the
   expansion on and starts again, and the new run mounts the package from
   its start and enters on its own. The Boneyards never open without their
   art. A run that was itself started again to enter, and still finds a
   package to install, stops with "The Boneyards is installed. Start Open
   Annihilation again to enter the Boneyards." instead of starting again.

   An install over a copy already there replaces its files where they are:
   a file that already holds the package's bytes is left alone, the
   folders stay, and files the game wrote beside the package's own, such
   as `Boneyards/<area>_map.gwm` or the Boneyards' saved files, are kept.
   No second copy of a folder is left behind, and an empty numbered copy a
   file sync made of one of the package's folders ("Boneyards 2") is
   removed.

Then the update screen, the 1998 one with its "Verifying …" lines, shows
what was checked: the online features, the game files, Open Annihilation
up to date, and the Boneyards expansion up to date or just installed. It
is a short look, and the Boneyards open.

A catalogue with no such package, a download that fails, or a check the
server asked for that the game cannot do, shows the Library's own message
and stays on the main menu. **OK** closes that message. Clicking ENTER
again starts over.

A run with nobody to answer is quiet: `-y`, a headless or unattended run,
and the game's checks. It is asked nothing about the online features or
an update and changes no setting. Its log says what a player would have
been asked, as `open-annihilation: entry: a player would be asked: ...`,
and it goes on as such a run always has: `-y` stops when both checks are
off and does not wait for an update check that has not answered, and a
newer release is not taken. When a needed install ID is off, the same log
line names that question, the off choice is left as it was, and the
download then stops with "Downloads are off for {registry}.", for example
"Downloads are off for Core Prime." Every stop is also logged as
`open-annihilation: entry: stopped: ...`. The run still brings the
Boneyards package up to date when the install ID is not off, and no
question ever holds it: like a player's entry it asks every registry, the
built-in one included, for its current list, and replaces a missing,
unrecorded or changed copy from it. A machine nobody watches, such as a
test machine playing on a live server, so takes a newly published
Boneyards package on its own.

The game's own checks are the exception. A `--headless-check` run, or a
run with one of the Boneyards' check options (`--check-boneyards-*`,
`--enter-boneyards`, `--check-strip-buttons`, or a `--boneyards-replay` of
recorded replies), asks only the registries the player added, never a
built-in one, and never takes the package from a built-in registry,
whatever the copy's state: a copy installed is used as it is, and a missing
copy that no added registry lists stops with the catalogue's message. It
starts no update check of its own either, unless it named a feed
(`--update-feed`), as the checks do with a feed on the same machine. A
check never contacts a live server.

### The mod

When a mod is on, other than the Boneyards' own files, the game asks: "The
Boneyards plays Total Annihilation without mods. Switch {mod} off and
enter the Boneyards?" **Switch off and enter** switches that mod off and
the game starts again. The next run goes straight into the entry, the way
`-y` does, and it does not end the run when the player leaves. **Cancel**
stays on the main menu with the mod still on. The mod that was switched
off is remembered.

When the player leaves the Boneyards and comes back to the main menu, the
game asks once: "Switch {mod} back on?" **Switch on** switches it back on
and the game starts again. **Not now** leaves it off. Either answer forgets
the remembered mod. The same question is asked once on a later ordinary
visit to the main menu when a remembered mod was left behind. It is not
asked on the run that is about to enter from the switch, and it is not
asked for `-y` or for a run with nobody to answer. `-y`, and a run with
nobody to answer, switch a stored mod off without asking and say so in the
log. That quiet switch does not save a one-shot request, because the run
enters again on its own. The request the **Switch off and enter** button,
or **Update**, saves is cleared when the entry begins, so the ordinary start
after that does not enter by itself. Plain 3.1c, Core Contingency, Battle
Tactics and the Boneyards' own files are not a mod, so they are not asked
about.

A mod named on the command line is not switched off. That is `--mod-dir`,
`--mod`, or a launch file's `content.mod`. Since the entry cannot go on,
this is said first, before anything is asked or changed: the player reads
"Start Open Annihilation without the mod on its command line to enter the
Boneyards." on one button and stays on the main menu. `-y` and a headless
run log that sentence and do not start again. The stored Mod setting is
left as it is.

Developer Mode on plain 3.1c, Core Contingency or Battle Tactics is not a
mod, so the game folder is not switched off to enter. A battle is still
refused when Developer Mode has changed the simulation.

Switching away after LOGIN, or while a login started with `-y` is still
connecting, does not drop the connection. Once the session is up it stays
up while the window is away. A battle played alone still pauses while the
window is away.

`native-boneyards-login-inactive` replays a login while the window stays
inactive. ENTER BONEYARDS opens the Boneyards, the guest name is filled and
LOGIN is clicked, and the login finishes before the loop is allowed to wait.
Its lines are `the Boneyards opened while the window was inactive`, `LOGIN
acted on while the window was inactive` and `logged in while the window was
inactive`.

A battle launched from the Boneyards starts with no mutators and a locked
picker, whether or not that launch locked the room's other options. The
Mutators button stays locked.
The set remembered for other battle rooms is not applied. Picks for a
skirmish and a campaign stay as they are.

A launch on any other mod, or with a mutator on, is refused. The message
says the Boneyards launches and reports games only on 3.1c, Core Contingency
and Battle Tactics, then "Switch your mod off to play in the Boneyards."
The client leaves that battle.

`native-boneyards-mod-entry` plays this offline. It switches made-up mod
Ridge on, asks, answers **Switch off and enter**, and finds the next run
with no mod and the entry under way. It passes twice in a row. Each run
leaves no pending entry, no remembered mod and no mod selected.

`native-boneyards-entry-install` and `native-boneyards-entry-update` enter
as quiet runs over a registry on the same machine that lists the Boneyards
as the catalogue does: one `.oaexp` holding the archive beside the
Boneyards folder, built at run time from the installed game, over a game
folder that holds neither. `install` turns both online checks off: the run
is asked nothing, changes neither setting, fetches the package once,
records it, and opens the Boneyards with an image of the expansion
folder, the console's art from its archive and the message chime as that
archive holds it, with no sound missing. `update` is offered a newer game
and goes on without it; a second entry over the recorded package
downloads nothing, a changed catalogue package is fetched and installed
once over the copy in place, keeping a war map written into its Boneyards
folder and leaving no numbered copy of it, and a fourth entry downloads
nothing again. `boneyards-entry-flow` checks the decisions before the
package: the online features, Update and Cancel, the carried request, the
page-only release, Try again and the quiet runs; and which registries a
check that faces a player, a headless check, `-y`, a player's entry and a
run that plays unwatched reach, with the package each then takes.

The login console opens with Registered User chosen and the password field
shown. Guest Login is drawn grey and cannot be chosen. LOGIN stays grey
until a name and a password are filled. Clicking LOGIN, once it is lit, or
pressing Enter in the password field, starts the login. A click while
LOGIN is grey does nothing.
