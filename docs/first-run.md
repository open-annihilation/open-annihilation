# The welcome setup

A start that does not already have the game copied, and that did not name
a folder, walks five steps before the menu: the language, where the game
is, the copy and its check, online features, and the two buttons. The
first three are drawn in Open Annihilation's own window, with its bundled
fonts, and the language chosen at the first step is the language the later
steps speak. A player who already has the game copied, or who named a
folder with `--game-dir`, sees only the welcome: the language, online
features, and the two buttons, over the main menu, as 1 of 3, 2 of 3 and
3 of 3.

The left footer button says the same word on every step of one walk.
A first start, when nothing is set up yet, says **Quit**. **Quit** leaves
without writing `open-annihilation.first-run` and without leaving a copy
running. Closing the window does the same, so the next start shows the
walk again. A re-run (Settings › System › Welcome setup, **SHOW AGAIN**)
and the welcome a player who already has the game sees say **Skip** on
every step. **Skip**, or **Start playing** at the end, writes
`open-annihilation.first-run` as `done`, and the walk does not return.

The welcome stays hidden in a headless check, a benchmark, a stage run, a
run driven with `--fark`, an unattended run, and a run that names
`--preferences-file`. `--first-run` shows it anyway, including after it is
done. The five-step walk is not shown to a player who already has the
game, and it is not shown when `--game-dir` names a folder. While the
welcome is up, or about to be, the question about a missing language stays
closed, and a language chosen here is not asked about afterwards.

On a phone, the [Game files](game-files.md) screen still brings the files
in. That import is written into the same managed folders, and the staged
folder is removed once the copy finishes, so the game is not kept twice.

## Welcome

![Welcome in a 1280 by 720 window: Welcome at the top left, the title Welcome to Open Annihilation, Choose your language, and a carousel of languages with English in front, Skip, Back and Next at the bottom.](images/first-run/welcome.png)

On the welcome walk the page word is Welcome, the step is WELCOME, and the
count is 1 of 3. The footer is **Skip**, **Back** and **Next**. On the
five-step walk the page word is Setup, the step is LANGUAGE, and the count
is 1 of 5. That footer's left button is **Quit** on a first start and
**Skip** on a re-run, and the same word stays through the game, the copy
and online features. Full screen, the header shows Open Annihilation's
app icon and its name before the page word. In a window, whose title bar
already names the game, the header leaves both out: the page word stands
at its left and the step at its right. Switching between a window and
full screen while the setup is up redraws the header the other way. The
footer buttons are the dialog buttons Settings uses, with the primary
button on the right, and the header and footer sit inside the window. A
footer button is at least as wide as its caption, so **Copy the game**
stays inside the window.

The setup grows with the window. Up to 960 by 540 points it is laid out
in the window's own points, Compact below that. A larger window draws
that same 960 by 540 room larger, so the words, the footer's buttons and
the cards grow together: by four thirds in a 1280 by 720 window, and
twice in a 1920 by 1080 one, whatever the display's pixel density. The
footer's buttons are then at least 69 by 22 and 104 by 34 points, and the
small text 13 and 20 points. Over the main menu the setup grows the same
way while the Interface size is Auto, on whole steps when the modern
fonts are off. An Interface size chosen in Settings is kept as it is.

The title is "Welcome to Open Annihilation" and the line under it is
"Choose your language."

The language in use starts in the centre, in front of the cards beside it.
A move eases to the next card over half a second, and the window draws
every frame of that move. The place at a moment depends on how long the
move has been running, not on how many times the window has been drawn.
Cards one or two steps away stay readable. The two furthest from the
centre show no pill, because a pill turned with those cards cannot be
read. The centre card and the card one step to either side keep theirs.
The floor's grid is drawn behind the cards, at half the strength of a
rule, so the lines do not meet the cards as a table.

![The language step of a first start, full screen at 1920 by 1080: the app icon, Open Annihilation and Setup at the top left, English centred and ringed, Active and Built in whole, and a fixture pack offering Get.](images/first-run/language-1920x1080.png)

![The same step in a 1280 by 720 window, with Setup alone at the top left.](images/first-run/language-1280x720.png)

![The same step in a 640 by 480 window.](images/first-run/language-640x480.png)

The card in the middle is the one the step marks: the accent ring once
it faces the player, the keyboard focus, and, while the step speaks its
language, the accent badge and the **Active** pill. Left and Right, a
controller, the wheel, the arrows and a press on a side card all move
that mark with the carousel, at every moment of the move. The language
in use loses it while another card is chosen, and its pill then says
**Built in** or **Installed**. While a pack that is not installed is in
the middle, no card says **Active**.

The carousel lists the built-in languages in the order the game keeps them
(English, Deutsch, Español, Français, Italiano), then language packs that
are already installed, then each language pack Core Prime lists when
downloads are on and that catalogue can be read. The pre-game window reads
that catalogue the same way the welcome over the menu does. A pack that is
not installed yet wears **Get** and its size. While a built-in language, or
a pack already installed, sits in the middle, this step speaks that
language before it is chosen: the title,
"Choose your language.", the note and the offline lines under the
carousel, a download's status, the step line, and **Skip** or **Quit**,
**Back** and **Next**. Turning to a language the game cannot read yet
puts those words back in the language in use, and nothing is downloaded
just to preview one. The other two steps stay in the language in use
until the player chooses. Choosing a built-in language, or
an installed one the game can show, switches at once and the whole game
is drawn again in that language. The carousel is a ring: left from the
first card goes to the last, and right from the last goes to the first,
one card either way. A single click on a side card brings it to the
centre. A double-click on any card centres it and chooses it. Choosing a
pack downloads it. The card says
"Downloading" and how much of the file has come. **Cancel download** stops
it. The card then says "Checking…", and the pack is installed and switched
to as soon as it is ready. A download that fails says "Couldn't download",
with the reason under the card, and **Retry**. **Next** stays available
either way, and the language stays as it was until a pack is installed.

Under the carousel, a note says that other languages download from Core
Prime and that the first download makes a random install ID, which the
later step explains. **Next** is disabled while a download is running.
Escape, and B on a controller, do nothing on this step.

## When the packs cannot be fetched

![The same welcome step with the catalogue unread: the language packs say Offline, and a line says the catalogue could not be read.](images/first-run/welcome-offline.png)

When downloads are off, or the catalogue cannot be read, the packs stay on
the carousel as **Offline** and a line under them says why: "Downloads are
off, so language packs stay offline" or "The catalogue could not be read,
so language packs stay offline." The built-in languages still switch.

## The game

![The game: Locate Total Annihilation Game Files, two installer rows with the fuller one selected, Locate another, and Copy the game.](images/first-run/game.png)

This step is only on the five-step walk, and it is the second step.
Language comes first. A start that will show these steps does not ask for
a folder before them: no system folder dialog, no folder locator, and no
Game files screen.

The page word is Setup, the step is THE GAME, and the count is 2 of 5.
The title is "Locate Total Annihilation Game Files". The column is as wide as
Settings. Each row is a place that was found: an installer or an
installation, never a folder of loose files such as Downloads. When a
folder was only searched for installers, those installers are the rows.
The fullest place is selected. A row names the file or the folder, the
path shortened under the home folder, and the bundles that place holds.
It does not name a bundle the place lacks. The selected row's bundles
are accent pills, and every other row's are quiet pills. The list is
ready when the step opens. Copying still checks each file as it reads
it. When the rows do not fit above **Locate another…**, they scroll
between the title and it, with a scroll bar at their right. The list
keeps the focused or chosen row in view; the wheel, Page Up, Page Down,
Home and End, or dragging the bar, scroll it, and choosing a row brings
the list back to it. **Locate another…** is centred under the rows, at the footer
buttons' height, as wide as the footer makes a button of the same words,
with the same room either side of them, in every language. It asks for a
folder. **Copy the game** is available
once a row is chosen. **Back** returns to the language. The left button
is the walk's **Quit** or **Skip**.

The line under the rows says the copies go to Documents › Open
Annihilation, and that the installation is left as it is.

## Copying

![Copying: the overall bar, the five bundles this source holds in a panel that scrolls, two done and one copying, and Pause.](images/first-run/copying.png)

![The same step after the check: 100 percent, All files match with the overall bar, the panel scrolled to Music's tracks note, and Continue counting down.](images/first-run/check.png)

The page word is Setup, the step is COPYING, and the count is 3 of 5. The
title is "Copying Total Annihilation". Under it, a line says where the
bytes are read from. The column is the game step's, as wide as Settings.
The overall count is the files this copy takes from
this source. A file the source does not hold is not in that count, so a
finished copy reads 100%. The bundles the source holds sit in a panel
under the bar. A bundle the source does not hold is not listed. While a
file is being copied its name is the line under the bundle's name. Once
that bundle is complete the file's name goes, and that same line says
the result, such as "All files match" or "17 of 18 tracks". The row
keeps its height, so the bundles under it stay put. A track the music
bundle never held is a note, and is not a failure. So is a movie the
source holds at another size than Total Annihilation 3.1c's: it is left
out, the game plays without it, and the line names it with both sizes,
such as "Data/2.zrb left out: 41000000 bytes, not 41229000". While the copy runs
the footer's right-hand button is **Pause**, drawn plain. Once the check
is done it is **Continue**, in the accent, and counts down.

The panel ends above two centred lines at the foot of the page, "If OA
stops, the copy carries on from here next time." and "To Documents › Open
Annihilation › Games and Expansions", which keep their places at every
window size. When the bundles do not fit, they scroll inside the panel,
with a scroll bar at its right. The panel follows the bundle being copied;
after the check it shows the first bundle that differs, else the first
with a note. The wheel, the Up and Down arrows, Page Up, Page Down, Home
and End, or dragging the list or its bar, scroll it, and it stays where
the player leaves it until the step opens again.
**Pause** holds the copy, and **Continue** lets it go on. The left button
is still the walk's **Quit** or **Skip**: it stops the copy and leaves.
Escape returns to the game step.

When the copy and the check finish with every taken file matching,
**Continue** shows a countdown of about three seconds, then goes on to
online features. A key, a pointer press, the wheel or a gamepad stops the
countdown. Moving the pointer does not. When the check finds a difference,
the rows say so and **Continue** waits for a press. **Quit** or **Skip**
is still there.

![The copying step stopped before it began: a question over it says Your Total Annihilation has been changed, names rev31.gp3 from Total Annihilation 3.1c and ccdata.ccx from Core Contingency with their sizes there and the sizes they should be, and offers Choose another place and Quit.](images/first-run/changed.png)

When the place chosen holds an archive at another size than Total
Annihilation 3.1c's, as a mod or another program leaves it, the copy
stops before it begins and nothing is copied. That is one of the base
game's archives (`rev31.gp3`, `totala1.hpi`, `totala2.hpi`, or the demo's
`TADemo.hpi`), or an archive of an expansion the place holds, such as
Core Contingency's `ccdata.ccx` or Battle Tactics' `tactics1.hpi`. An
expansion the place holds none of is simply not there, and stops nothing.
A question over the step, "Your Total Annihilation has been changed",
names each such archive in one list, with the base game or the expansion
it is from, its size there and the size it should be, and says that Open
Annihilation needs a fresh Total Annihilation install, without mods, to
set up its copy. Past six archives it names five and counts the rest,
which the game's log names. **Choose another place**, in the accent and
marked, returns to the game step, and so does Escape. Beside it is the
walk's **Quit** or **Skip**. While the question shows, only its buttons
take the keys and the pointer.

A copy that stopped part way opens this setup again at the next start, and
a copy from the same place goes on from the files already copied. The
setup does not open once the base game's
copy names every one of its files. A start that then cannot play the copy
says why and stops ([a copy that does not finish](troubleshooting.md#the-copy-of-the-game-does-not-finish)).

## Online features

![Online features: a picture of the Library's Active tab, the heading ONLINE FEATURES, three lines about downloads, the install ID panel, and the choices Turn on online features and Not now.](images/first-run/online.png)

The page word is still Welcome. The step is ONLINE, 2 of 3. At Regular
and Large a picture of the Library's Active tab sits beside the words, or
above them when the column is too narrow for both. It is drawn live in
the language just chosen, from a fixed sample (the made-up mod Ridge). No
picture of it is shipped with the game. At Compact that picture is left
out, and the words use the whole column from the top of the page. The
kicker is ONLINE FEATURES, then the heading "Downloads, updates and a
number only you hold", three lines, and the install ID as
[downloads.md](downloads.md#the-install-id) describes it:

- Get language packs, mods and map packs from Core Prime.
- Lists of what is new are checked as the game starts and when the Library
  opens.
- A random install ID, one per registry, is sent only when downloading from
  that registry.

**Turn on online features** is selected to start with. **Next** then leaves
Check for content updates at Automatically, sets Check for updates to
Automatically, and leaves Core Prime's install ID on. It does not make an
ID: the next download does.
**Not now** sets Check for content updates to Never, sets Check for updates
to Never, and turns Core Prime's install ID off, which forgets an ID a
language pack made on the first step. Built-in languages, and files you
open yourself, keep working either way.

**Back** returns to the language on the welcome, and to the copy on the
five-step walk. Choosing again and pressing **Next**
applies the new choice. The walk is not finished until the last step, or
until **Skip**. **Quit** leaves it unfinished.

## The two buttons

![The main menu dimmed except a spotlight around the Library and Settings buttons, with coach marks 1 Library and 2 Settings, and a panel on the left: THE MAIN MENU · 3 OF 3, Back and Start playing.](images/first-run/buttons.png)

This step has no setup frame. It lies over the real main menu, dimmed
except a spotlight around both buttons. Two coach marks name them:

1. **Library.** "Get, update and switch mods, map packs and languages. The
   number counts every package you have added: mods, map packs, language
   packs and expansions."
2. **Settings.** "Open Annihilation's own settings: controls, graphics, the
   interface's size, downloads and your files."

On the welcome walk the panel says "THE MAIN MENU · 3 OF 3". On the
five-step walk, after a copy on this start, it says "THE MAIN MENU · 5 OF
5". Either way it says "Two buttons for everything Open Annihilation
adds", then: "The Library is where you get mods, map packs and languages.
Settings is where you change how Open Annihilation looks and plays.
Everything else on this menu is the game's own."
**Back** returns to online features. **Start playing** finishes the walk.
This step has no **Skip** or **Quit**. Escape and Enter do what **Start playing**
does.

The logo and the intro play after that finish, or after **Skip setup** when
the walk will not return, and before the menu can be used. They do not play
between the steps. A switch that skips the movies skips them on this start
too. A start that does not show the setup plays the movies first, before
the menu, as it always has.

## Keys and controllers

Tab and the arrows move between the controls. Enter, or A on a controller,
presses the one with the focus. Escape, or B, is **Back** on the online
step, and does nothing on the first step. On the last step Escape and Enter
finish the walk, as **Start playing** does. On the language step the
shoulder buttons, the wheel and the focus keys also move the carousel one
card, wrapping at either end.

## Showing it again

Settings › System has **Welcome setup**, with **SHOW AGAIN** and the hint
"Walk through the language, online features and the OA buttons again."
Pressing it and then **OK** clears `open-annihilation.first-run`, and the
walk shows at the next main menu, once the dialog has closed. **Cancel**
leaves the preference. During a game the row is locked, and it says
"Choose from the main menu."

## For a run that is driven

`--first-run` shows the walk even when it is already done, and even in a
run that would otherwise hide it. A stage file's line `first-run language`, `first-run game`, `first-run copying`,
`first-run online` or `first-run buttons` opens that step for a picture. A
stage file without one of those lines does not show the walk.

The log says `first-run: showing step language` (or `game`, `copying`,
`online`, or `buttons`) when the step opens, and `first-run: not showing:`
with one of `done`, `headless check`, `preferences file`, `benchmark`,
`stage`, `fark` or `unattended` when it stays hidden.

A program that presses the controls finds them as `oa.first-run.` and the
step (`language`, `game`, `copying`, `online` or `buttons`) and the
control. The language cards are `oa.first-run.language.card.` and the tag
in lower case, with anything else a hyphen (`de`, `en-xa`). The rest are
`skip`, `back`, `next`, `previous`, `next-card`, `cancel` and `retry` on
the language step; `skip`, `back`, `next` (Copy the game), `locate`
and `source.<index>` on the game step; `skip` and `next` (Pause,
or Continue once the check has finished) on the copying step; `skip`,
`back`, `next`, `on` and `not-now` on the online step; and `back` and
`start` on the buttons. On every step that has it, `skip` is **Quit** on a
first start and **Skip** on a re-run. [automation.md](automation.md#the-welcome-setup)
lists them with the other screens.
