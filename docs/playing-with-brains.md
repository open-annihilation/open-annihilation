# Playing with brains

A brain is a computer player you can choose. The built-in computer player
plays a computer seat unless you choose a brain for it. Classic is an
example of that built-in player, written so it can be read. It is not a
replacement, and it is not the default. What a brain may do is
[Brains](brains/README.md). To make one, read
[Writing a brain](brains/authoring.md).

## Getting one

The Library's **Brains** tab lists the brains the registries offer and
the ones you have installed. The built-in computer player is listed
first. Its name is **Built-in computer player**, and the line under it
reads "Open Annihilation's own computer player. It plays as Total
Annihilation's does." While it is the default, its status is
**Built in · Default**. While it is not, the status is **Built in**. It
cannot be removed: the details offer **USE BY DEFAULT** only while it is
not already the default, and they offer nothing while it is.

**GET** downloads a brain. **UPDATE** takes a newer release. **UNINSTALL**
removes one you installed. Uninstalling the brain that is your default
adds "It is your default computer player: new computer seats go back to
the built-in player."

A `.oabrain` file, from a website or a friend, installs too: double-click
it, or start the game with `--open` and its path. It waits for the main
menu and asks "Install the brain {name} {version}?" with **CANCEL** and
**INSTALL**. It installs into the `Brains` folder of your own Open
Annihilation folder, as `Brains/<id>`
([where that folder is](installation/macos.md#where-it-keeps-its-files)).
**BRAIN INSTALLED** then says "{name} is installed. Choose it for a
computer player when you set up a game." with **OPEN FOLDER** and **OK**.
**Brain packages (.oabrain)** in [Settings › System](settings.md#system)
chooses whether the file manager opens these files with the game. The
hint there is "Open these files with the game from your file manager."

The search on the Brains tab reads "Search brains, authors, tags". The
tags a brain carries are Economy, Classic, Rush, Turtle, Air, Naval,
Learned, Teaching and Experimental. Any other tag is shown as written.

## The default brain

**USE BY DEFAULT** on an installed brain makes that brain the one new
computer seats start with. The row's status becomes
**Installed · Default**, and the chip **Default** marks it. **USE BY
DEFAULT** on the built-in computer player, while some other brain is the
default, hands new computer seats back to the built-in player. While the
built-in player already is the default, that button is not shown.

A default brain plays a computer seat you have not given its own brain.
It does not take a seat you already set to another brain, or to the
built-in player on purpose.

## A computer seat's brain

On skirmish setup, when a brain is installed, each **Computer** row has
a control to the right of its Energy button. The control reads the
seat's label, **Built-in · {level}** for the built-in player and
"{brain} · {level}" for a brain, cut to the space on the row. It opens
the picker. The picker's title is **Computer player**, and the line
beside it is "{seat} · {mod}". With no brain installed, the row has no
such control, and the built-in computer player plays the seat.

The list leads with **Built-in computer player**. A brain you have not
chosen shows **Built into Open Annihilation** on the built-in row. A
brain that cannot play this seat is greyed, and its line is the reason,
such as "Made for {names}". When the seat's current brain cannot play,
the picker opens on the built-in player and says "{name} cannot play
this seat now, so it starts on the built-in player." With only the
built-in player to show, the list says "No brains here yet. The Library
on the main menu gets them."

The details name the **Level** and the brain's options. The built-in
player's levels, while the Difficulty button owns them, sit under
"Set by the Difficulty button: {level}". **Use for every computer seat**
is a switch, **OFF** or **ON**. While it is on, the footer reads
"{label} · every computer seat". **USE THIS** hands the choice back.
**CANCEL**, and the header's close mark, hand nothing back. On a narrow
layout the details are a page of their own, opened with **DETAILS** and
closed with **BACK**.

Resting the pointer, or the information key, shows the seat's card. A
brain that sees every unit has the line "Sees every unit on the map".

Skirmish, the battle room on this machine, and a campaign each remember
their own computer seats: the brain, the level and the options. An empty
brain in that memory is the built-in player, chosen on purpose. A level
left empty takes the setup's difficulty.

## Full awareness

Some brains see every unit on the map, as the built-in computer player
does. The manifest says so. The Library shows the chip **Full awareness**
and the fact "Sees every unit on the map, as Total Annihilation's own
computer player does". The seat's card uses the shorter line "Sees every
unit on the map". The same chip is the flag on a seat in a network game.
Full awareness is allowed there.

## Campaign missions

A campaign mission uses the default brain. On **NEWGAME** and Any
Mission, when a brain is installed, one control sits under **Difficulty**
for the campaign's computer player. It opens the same picker, and the
campaign remembers that choice. With no brain installed, the control is
not there, and the mission's computer players are the built-in player.

## Network games

The player who adds a computer seat chooses its brain, and only that
machine runs it. The other players see the seat's label,
"{brain} {version} · {level}", and need nothing installed. Another
machine's brain is that label in the same place as the button, and it
does not run the script. Full awareness is flagged on the seat. The
`ai.*` hacks still apply to the match.

With nothing to draw, the battle room shows no brain control.

## When a brain stops

The built-in computer player plays on. The notice is one of these:

- "{brain} stopped ({reason}); the engine plays this seat on"
- "{brain} could not start ({reason}); the engine plays this seat"
- "{brain} does not fit this game ({reason}); the engine plays this seat"

A brain that stops mid-game hands the seat over at the next order. A
brain that never starts leaves the built-in player in the seat from the
first tick. The log of that seat is under the account's data folder,
`logs/brains/seat-<n>.log`. What the lines mean for someone writing a
brain is [When a brain fails](brains/authoring.md#12-when-a-brain-fails).

## Saves and recordings

A save keeps each computer seat's brain: the id, the version, the level
and the options. The note beside the save, and beside a recording, names
each one after the mutators: a `brain` line of the player and the id,
then `brain-version`, `brain-level` (the level's name) and `brain-name`.
A recording stores the same seats and does not run the brain when it is
played back.

Loading a save whose brain is not installed asks **Load with the built-in
player?** The body reads "This game was saved with {name} {version}
playing seat {n}, which is not installed here." and "The built-in player
takes those seats from the units as they stand. GET the brain in the
Library to load the game as it was saved." **LOAD WITH THE BUILT-IN
PLAYER** loads on that answer. The built-in player takes those seats
from the units as they stand.

## From the command line

Seating a brain, playing two brains against each other, and writing a
trace or a record are [the command line](command-line.md).
`--computer-player` takes an installed brain's id.
`--brain-match` also accepts a folder.
`--open` accepts a `.oabrain` file, beside the other packages.
