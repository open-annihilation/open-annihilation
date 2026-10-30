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
cannot be removed: the details offer **Use by default** only while it is
not already the default, and they offer nothing while it is.

**Get** downloads a brain. **Update** takes a newer release. **Uninstall**
removes one you installed. Uninstalling the brain that is your default
adds "It is your default computer player: new computer seats go back to
the built-in player."

A `.oabrain` file, from a website or a friend, installs too: double-click
it, or start the game with `--open` and its path. It waits for the main
menu and asks "Install the brain {name} {version}?" with **Cancel** and
**Install**. It installs into the `Brains` folder of your own Open
Annihilation folder, as `Brains/<id>`
([where that folder is](installation/macos.md#where-it-keeps-its-files)).
**BRAIN INSTALLED** then says "{name} is installed. Choose it for a
computer player when you set up a game." with **Open folder** and **OK**.
**Brain packages (.oabrain)** in [Settings › System](settings.md#system)
chooses whether the file manager opens these files with the game. The
hint there is "Open these files with the game from your file manager."

The search on the Brains tab reads "Search brains, authors, tags". The
tags a brain carries are Economy, Classic, Rush, Turtle, Air, Naval,
Learned, Teaching and Experimental. Any other tag is shown as written.

## The default brain

**Use by default** on an installed brain makes that brain the one new
computer seats start with. The row's status becomes
**Installed · Default**, and the chip **Default** marks it. **Use by
default** on the built-in computer player, while some other brain is the
default, hands new computer seats back to the built-in player. While the
built-in player already is the default, that button is not shown.

A default brain plays a computer seat you have not given its own brain.
It does not take a seat you already set to another brain, or to the
built-in player on purpose.

## A computer seat's brain

On the skirmish setup, when a brain is installed, each **Computer** row's
own Player button names the seat's brain: **Computer**, the room's own
word, for the built-in player, or the brain's name, cut with "..." where
it is longer than the button. A notch at the button's right end marks it.
A click on the notch, or a right-click anywhere on the button, opens the
brain menu. A left-click elsewhere on the button changes the row as 3.1c
does. Tab stops on the notch just after its Player button. With no brain
installed, the row is 3.1c's own, and the built-in computer player plays
the seat.

The brain menu is drawn as the room's own list box, under the button,
titled "Computer player for this seat":

- **Computer**, the built-in player, with the level the Difficulty button
  sets;
- each installed brain, with its level. The brain the seat plays has a
  tick, and its level reads "< Firm >": its arrows step the level. A brain
  that cannot play this seat is greyed;
- **Options...**, while the seat's brain has options of its own. It closes
  the menu and opens the brain picker on that seat (below);
- **Use for every computer seat**, a box. Ticked, it gives every computer
  seat this seat's choice, and the choices that follow go to all of them;
- **Get more brains...**, which leaves the setup for the main menu and
  opens the Library on Brains.

Up and Down move through the menu, Left and Right step the level on the
seat's row, Return or Space choose, and Escape or a click outside closes
it. While it is open, a card beside it shows the selected brain, its
version and its level. Resting the pointer on the button, or the
information key, shows the seat's card. A brain that sees every unit has
the line "Sees every unit on the map".

The brain picker, opened with **Options...**, is titled **Computer
player**, and the line beside it is "{seat} · {mod}". It opens on the
seat's brain, its level and its options. The list leads with **Built-in
computer player**. A brain that cannot play this seat is greyed, and its
line is the reason, such as "Made for {names}". When the seat's current
brain cannot play, the picker opens on the built-in player and says
"{name} cannot play this seat now, so it starts on the built-in player."

The details name the **Level** and the brain's options. The built-in
player's levels, while the Difficulty button owns them, sit under
"Set by the Difficulty button: {level}". **Use for every computer seat**
is a switch, **Off** or **On**. While it is on, the footer reads
"{label} · every computer seat". **Use this** hands the choice back.
**Cancel**, and the header's close mark, hand nothing back. On a narrow
layout the details are a page of their own, opened with **Details** and
closed with **Back**.

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
Mission, when a brain is installed, one button in the room's own button
art sits under **Difficulty** for the campaign's computer player, with
the OA stamp and the notch. It names the brain, and a press anywhere on it
opens the brain menu, titled "Computer player in the missions", without
Use for every computer seat. The campaign remembers that choice. With no
brain installed, the button is not there, and the mission's computer
players are the built-in player.

## Network games

The player who adds a computer seat chooses its brain, and only that
machine runs it. In the battle room a computer row keeps 3.1c's name,
"AI:{owner}". Its empty Resolution cell shows the OA stamp and the seat's
brain, or the built-in player, and its Mem cell the brain's level. On the
machine that added the seat, the name has a notch; the notch, the
Resolution cell and a right-click on the name open the brain menu, which
has no Get more brains... there, since leaving would leave the game. A
click on the Mem cell steps the level, and for the built-in player steps
its difficulty as 3.1c's own Mem cell does. The other players see the
brain and its level in the same cells, need nothing installed, and do not
run the script. Full awareness is flagged on the seat. The `ai.*` hacks
still apply to the match.

With nothing to show, the battle room's cells are 3.1c's own.

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
