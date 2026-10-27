# Playing with mutators

A mutator is a small package that changes one game, on top of the base
game or the mod you are playing. You get it in the Library. You switch it
on for a skirmish, a battle room or a campaign, and it applies before that
match starts. What a mutator may change is [Mutators](mutators/README.md).
To make one, read [Writing a mutator](mutators/authoring.md).

## Getting one

The Library's **Mutators** tab lists the mutators the registries offer and
the ones you have installed. **GET** downloads one. **UPDATE** takes a
newer release. The Library does not switch a mutator on: that is the
setup's **Mutators** button, below.

A `.oamut` file, from a website or a friend, installs too: double-click
it, choose Open With and Open Annihilation, drag it onto the game's
window, or start the game with its path. It waits for the main menu and
asks "Install the mutator {name} {version}?" with **CANCEL** and
**INSTALL**. It installs into the `Mutators` folder of your own Open
Annihilation folder, as `Mutators/<id>`
([where that folder is](installation/macos.md#where-it-keeps-its-files)),
and the Library's Mutators tab lists it. **MUTATOR INSTALLED** then says
"{name} is installed. Turn it on with the Mutators button when you set up
a game." with **OPEN FOLDER** and **OK**. **Mutator packages (.oamut)** in
[Settings › System](settings.md#system) chooses whether the file manager
opens these files with the game.

A mutator can need another. The details then say **Needs**, and a Large
row carries the chip "Needs {name}". **GET** becomes **GET WITH {name}**,
or **GET WITH {count} MORE** when several packages besides it will
download. The question is "Get {name}?" **GET BOTH** or **GET ALL**
downloads the ones you do not have. They apply before it when it is
switched on. **CANCEL** downloads nothing. Each stays an ordinary
installed package: getting one does not switch it on.

## Choosing them

Skirmish setup, the screen before a campaign, and the host's battle room
each have a **Mutators** button. It is one of the screen's own on and off
buttons, drawn from the screen's own art with the OA stamp at its left,
so a mod's look carries through, and it sits beside the screen's own
buttons, at their size: on skirmish setup it is as large as **Previous
Menu** and sits between **Difficulty** and **Previous Menu**; before a
campaign it is as large as the **SELECT SIDE** panel and sits beneath
it; in the battle room it is as large as **Select Map** and sits directly
above it. Where its words need more room and the screen is free beside
it, it grows to the left.

It reads **Mutators: off**, no lamp lit, while that setup will play none,
and **Mutators: 3 on**, its green lamp lit, while it will play three. In
the battle room its red lamp lights while a seated player lacks one of
them. Resting the pointer on it, or the information key while it has the
focus, shows a card: "3 mutators on", "1 mutator on" or "No mutators
on", a row for each in the order they apply ("and 2 more" past six), then
"Applied in this order." and "Click to change.". The button is in the Tab
order, where the screen's own focus marker shows on it. The controller's
Y opens the picker. Enter or Space opens it while the button has the
focus. While the host's launch locks the room's options, the button is
greyed as the screen's own greyed buttons are.

The picker is headed **Mutators**, with the setup and the mod beside it,
such as "Skirmish · No Mod". The filters are **All**, **Installed** and
**Downloads**, with **TAGS** and "Search mutators". Two lists follow.

- **ON · APPLIED TOP TO BOTTOM** is the set the game will play, in that
  order. The first mutator applies first. Each later one sees the game as
  the ones above it left it.
- **OFF · NOT APPLIED** is everything else.

A row has a drag handle. Dragging a row in the On list moves it.
Dropping an Off row into the On list switches it on. Dropping an On row
into the Off list switches it off, and asks first when another mutator
needs it. The details have **Order**, **▲ UP** and **▼ DOWN**, which
move a row the same way. A row's switch reads **ON** or **OFF**. An
option is a control in the details: a toggle, a choice or a number.

A mutator that is not installed can still be switched on. The row says
"Not installed: switching it on downloads it". Switching it on queues
the download at once, and the summary reads "Getting {name} · {percent}%"
from then on. **USE THESE** queues any On mutator that is not yet
queued. While one is still missing it asks **Get mutators**, with
"Downloading {name} · {index} of {total}", and hands the set back after
the last install. When a download stops, the summary reads
"{name}: {problem}" and the question says "{problem} RETRY tries again."
with **RETRY**. **CANCEL** closes the question. The picker stays open
and the downloads carry on.

A row is locked, and cannot be switched on, only for a reason the picker
states:

- "Not compatible with this mod: {reason}", when it does not work with
  this mod or this engine. The reason is "it works with {names}", or
  "Needs Open Annihilation {range}".
- the same line when a prerequisite is missing or out of range. The
  reason is "Needs {name} {range}, which no registry offers", or
  "Needs {name}, which no registry offers".
- the same line when mutators need each other. The reason is
  "These mutators need each other: {ids}".
- "Cannot run beside {name}".

A check's own lines do not lock the row. "Stopped with this mod:
{reason}", "No effect with this mod", and a check's "Not compatible with
this mod: {reason}" are warnings, and the row can still be switched on.
The Library still offers **GET**.

The footer's check shows only at Regular and Large, and only on a picker
you can change. It shows "Checked with No Mod ✓", or "Checked with
{mod} ✓", when every On mutator has been checked and changes something.
Otherwise it reads one of "Checking with {mod}…", "Checking with No
Mod…", "{name} stopped with {mod}", "{name} stopped with No Mod", "Not
compatible with {mod}: {name}", "Not compatible with No Mod: {name}",
"No effect with {mod}: {name}" or "No effect with No Mod: {name}". There
is no such line while a verdict is still unchecked, while nothing is On,
at Compact, on a row's details page, or on a guest's read-only picker. **USE THESE** makes the On
list the set this setup plays. **CANCEL** closes the picker and changes
nothing: a setup that was off stays off, and a chosen set stays chosen.
With a controller, Y moves to the search, X flips the chosen row's
switch, and Menu does what **USE THESE** does when that button is on.

The hint "Your last skirmish set, ready to use. CANCEL plays without
mutators.", or the same sentence for "Your last multiplayer set, ready
to use. CANCEL plays without mutators." or "Your last campaign set,
ready to use. CANCEL plays without mutators.", shows only when the
picker opened on a remembered set. That is when the setup had nothing
chosen. A campaign picker always adds "The set holds for every mission
of this campaign, and is saved with it." A guest sees "The host's
mutators. Only the host can change them." instead.

## The last set

Skirmish and a campaign each remember their own last set, with its order
and its options. The set is stored when **START** hands it on, before it
applies, so a set that then stops the start is still remembered. Each
setup starts with mutators off: the button reads **Mutators: off** until
**USE THESE** makes a set the setup's. **Mutators** opens on the set already
chosen. When nothing is chosen it opens on the
remembered set, already on. **USE THESE** makes that list the setup's,
and **START** plays it. **CANCEL** changes nothing.

A battle room is the same for the host, with one difference. **Mutators**
opens on the room's set or, when the room has none, on the last set the
host started a battle-room game with. Only the host's machine remembers
that set, and only when **START** really starts the match. In the room,
**USE THESE** makes the set the room's, and **CANCEL** leaves the room's
list as it was. A guest cannot change it.

A set that could not all be kept is named: "Left out of your last set:
{names}".

## When one cannot start

If a mutator stops before the match can start, the question is "Start
without {name}?" or, when more than one stopped, "Start without them?"
The paragraph says that nothing it changed is kept, and that the other
mutators are unaffected. An amber line names each one and why: "{name}:
{reason}". **START WITHOUT IT**, or **START WITHOUT THEM**, leaves out
every mutator that stopped, and every chosen mutator that needs one of
them, takes them out of that setup's set, and starts again. **BACK**
closes the question and keeps the set. After a campaign mission's start
that leaves you on the briefing, not on the campaign setup.

In a battle room a difference is not a private choice. When another
machine's mutators would play a different game, the host's **START** asks
"The game did not start". It names the player and the mutator. When that
player has the host's release and still built a different result, it
says "{name}: release {here} here, release {there} on {player}'s
machine." When that player does not have the host's release, it says
"{name}: {player} does not have it." and, when a registry the host uses
offers that release, "{player}'s GET MUTATORS button fetches release
{release}." Every release it names is the one the host plays.

## After a crash

The question is asked only after a crash in a skirmish or a campaign
game that was started from its setup. A battle-room game, or a run named
with `--mutator` or a launch file, only writes the log line. The
question appears on the main menu after the next launch. It asks
**Forget the last set?** The paragraph names them: "The last game ended
in a crash with {names} on. Forget the set and the Mutators button opens
empty next time; nothing is uninstalled." The hint says the game's log names them.
**FORGET IT** clears that setup's remembered set and uninstalls nothing.
**KEEP IT** leaves the set as it was.

## Playing with none

[`--no-mutators`](command-line.md#mutators) plays this run with no
mutators. A list `--mutator` or a launch file names is refused and
logged, and no mutator is applied. The last set of each setup is left as
it is. There is no key to hold at launch: the command line is how a run
refuses mutators.

## Saves and recordings

A saved game, a recording and a campaign save keep the mutators that game
played, in the order they applied. The note beside the file names each
one. Opening the save or the recording needs those same mutators. When
one is missing the game says "This needs a mutator you do not have: {detail}"
and names what is missing, such as a mutator that is not installed or a
copy at another version. A run started with `--no-mutators`
refuses one that was made with mutators: "This was made with mutators,
and this run plays none." A note the game cannot read says "The mutators
this was made with cannot be read."

[Saves, recordings and films](saves.md) says where those files are kept.
