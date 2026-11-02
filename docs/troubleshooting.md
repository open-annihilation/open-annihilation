# When something goes wrong

## Zooming out

Zooming out draws the units as dots when the picture of them would take more
memory than the game can spare. The game keeps running. After you zoom back
in, that extra memory is given back.

If a box says Open Annihilation ran out of memory, close other programs and
start the game again. The box says how full the heap is. ErrorLog.txt, beside
the program, keeps the line "Out of memory! Your hard disk may be full".

A run can limit the memory the units' pictures take by setting
`OA_RENDERED_UNITS_BUDGET` to a number of bytes.

The `+stats` console command shows that memory on its "models" row: the
megabytes the units' picture holds, "MB of" the most it may take, and "dots"
when the last frame drew the units as dots.

## A slow frame rate on a laptop

Many Windows laptops have two graphics cards: the processor's own graphics,
which saves power, and a faster card from NVIDIA or AMD. Windows and the
card's driver decide which one draws each program, and they give a program
they do not know the processor's graphics unless it asks for the faster
card. Open Annihilation asks for it.

To see which card the game draws on, type `+stats` in a match. The row
under "models" names how the game draws and, after it, the graphics card
that draws the window. Once you have turned the camera in Full 3D Mode, a
row that starts "3D" names the card that draws the turned view. On a
laptop with an NVIDIA card both should name it, such as "NVIDIA GeForce RTX
4070". A name such as "Intel(R) UHD Graphics" or "AMD Radeon(TM) Graphics"
is the processor's graphics. A long name is cut short to fit the panel.

Your own choice for the game wins over the game's request. To choose the
card yourself, close the game and use either of these:

- In Windows, open Settings › System › Display › Graphics. If Open
  Annihilation is not listed, add `open-annihilation.exe` from the folder
  you installed the game in. Open its options and choose High performance
  for the faster card, or Power saving for the processor's graphics.
- In the NVIDIA Control Panel (Manage 3D settings › Program Settings) or in
  AMD Software's graphics settings, add `open-annihilation.exe` and choose
  the high-performance card.

Then start the game again and check `+stats`. A laptop that is set, in its
own settings or its maker's software, to use only one of its cards keeps
that card whatever the game or Windows asks for.

## Files kept in iCloud

If Documents is in iCloud Drive and Optimize Mac Storage is on, macOS can
move files you have not used for a while out of the Open Annihilation folder.
That includes the game's own copy of Total Annihilation in Games and
Expansions. The files stay in iCloud, and each one downloads again the next
time something reads it, which can take seconds for each file and minutes
for the whole game. A match still starts at once, and its music begins when
the track has arrived. A mod, map or language pack whose files were moved
can be slow the first time it opens.

A start whose archives are in iCloud waits for them, and the window says
"Downloading the game's files from iCloud…" with a bar of the files that
have arrived. Closing the window then ends the game, and the files that
have arrived stay on this Mac for the next start.

When the game starts, it also asks macOS to download the files under Mods,
Maps, Languages, Mutators, Games and Expansions that are not on this Mac,
without waiting for them, and the music folder once a game is mounted. The
log says how many, in a line that starts `open-annihilation:`.

To keep the folder on this Mac, in the Finder Control-click the Open
Annihilation folder in Documents and choose Keep Downloaded. Turning off
Optimize Mac Storage in iCloud Drive's settings keeps every file of iCloud
Drive on this Mac instead.

## Downloads

A download that fails says why on its line in the Library, and **RETRY**
tries it again. Each failure also writes one line to the game's log, which
starts `open-annihilation: content: download of` and says what the
registry's server answered: the status, the content type and the length, or
the network failure. "Core Prime asked for a check the game cannot do. Try
again later." means the server answered with a check meant for a web
browser, which the game never answers: try again later
([when a download fails](downloads.md#when-a-download-fails)).

## The view stops scrolling at one edge

If moving the pointer to one edge of the screen stops scrolling the view,
while the other edges still do, start the game with `--trace-input`, play
until it happens, push the pointer against that edge, and send the game's
log. Its lines say where the game's window lies on its display, whether the
window holds the pointer, and whether the pointer left the window there
([what the lines mean](command-line.md#--trace-input)). Say also where
your taskbar is, whether a tool bar or overlay shows at that edge, and how
your displays are arranged.

## The copy of the game does not finish

A copy into Open Annihilation that stopped part way, because the game was
closed or the computer stopped, carries on the next time the game starts:
the setup opens again, and a copy from the same place keeps the files
already copied and goes on from there. The base game is played only once
every one of its files is copied, or known to be missing from the place it
was copied from.

A movie the place copied from holds at another size than Total
Annihilation 3.1c's is left out, and the game plays without it. The
copying step's line for the base game names it with both sizes, and so
does the game's log, in a line such as `open-annihilation: Data/2.zrb of
Total Annihilation 3.1c is left out: the source holds it at 41000000 bytes,
not 41229000`.

An archive at another size, of the base game (`rev31.gp3`,
`totala1.hpi`, `totala2.hpi`) or of an expansion the place holds (such
as Core Contingency's `ccdata.ccx`), means that Total Annihilation has
been changed, as a mod or another program changes it. The copy stops
before it begins and copies nothing, and the copying step says so and
names each archive, with what it is from, its size there and the size it
should be ([the question it asks](first-run.md#copying)). The log names
each one too, in a line such as `open-annihilation: ccdata.ccx of Core
Contingency is not copied: the source holds it at 26214713 bytes, not
25871597`. Choose another place to copy from: a fresh install of Total
Annihilation without mods, or the game's disc or installer. A copy from
that place then finishes as usual. An expansion the place holds none of
is simply not copied, and stops nothing.

When the base game cannot be played for any reason, the start says why
in a box and in the log, and names the files that were not copied. If
the files of a finished copy are gone, delete the folder the box names
(under Open Annihilation › Games in your Documents folder) and start the
game again to copy it afresh.
