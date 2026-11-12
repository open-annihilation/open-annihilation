# Content in the battle room

In a battle room, each player on Open Annihilation carries the base game
they are playing and the expansions that change the rules. The tip beside
that player's badge names the base game, those expansions, the mod and the
language. Music is left out: it changes no rules, and two players are not
compared by it.

When a player's base game or expansions differ from the host's, the badge
carries the rules dot and the tip names the difference on its own line. The
host cannot start until the content matches: the player turns on what is
missing, or the host turns off what the player does not have. A file in a
chosen base game or expansion that does not match a copy the game knows is
a difference too.

A player on Total Annihilation 3.1c is unchanged. That player is still
compared by unit sync, and the battle room does not invent expansions for
them. A player on an older Open Annihilation that does not name its content
is not refused for that alone.

A file that does not match includes an archive that lies in a base game's
or an expansion's folder of Open Annihilation's own copy under a name that
bundle does not list. The game does not read such an archive, and Game
Content names it ([Game content](../game-content.md#only-the-bundles-own-archives)).

## Units that differ

Before a battle starts, the battle room's unit sync compares every unit the
host can build with each player's copy of it, as 3.1c does. A unit whose
copy is not the same on every machine can't be built in that battle, on any
machine, as in 3.1c. When the host's unit sync has finished and nothing else
holds **START**, the room names those units in its message area, on the
host and on every player on Open Annihilation:

```text
*** Metal Extractor (ARMMEX) and Phalanx (ARMYORK) differ on Kestrel's machine and can't be built in this battle.
```

Each unit is named by its name in the game and its FBI UnitName. A line names
three units at most and counts the rest ("and 74 other units"). Units that
differ on the same machines share a line. The machine named is the one whose
copy is not the room's: the copy most machines hold among those whose
presence says their files are Total Annihilation's own, then the copy most
machines hold. When no copy leads, as between two machines that both say so
or neither does, the line names every machine the unit differs between:
"Metal Extractor (ARMMEX) differs between Kestrel's and Wren's
machines and can't be built in this battle." A unit only another machine
has, which this machine cannot name, is counted. The log has each line,
after `multiplayer: unit sync: `. `+syncerr` answers "2 units differ"
instead of "OK".

The host's **START** then waits for the host. The first press does not
start the battle: the room says "*** Press START again to start anyway.
Those units stay unavailable in this battle." The next press starts it,
with those units unavailable. A new finding, after a player joins or leaves
or the units change, asks again.

A room whose units all match is as before: no line, and **START** starts at
the first press.

The finding is the Open Annihilation host's. It reaches the other players on
Open Annihilation in the host's presence record, and nothing new is sent to
a player on Total Annihilation 3.1c, who sees the units greyed out as
before. Under a 3.1c host the room names nothing and nothing waits.

## Battles that count for a ladder

A battle the Boneyards launches counts for its ladder, and it plays only
Total Annihilation's own files. **START** is refused while any player's
presence says their game files differ from a known copy, including the
host's own, and the room names that player once:

```text
*** Kestrel's game files differ from Total Annihilation's; they must repair them before this battle can start.
```

That player reads instead: "*** Your game files differ from Total
Annihilation's, so this battle cannot start. Leave it and repair them in
Settings › Game Content." Once that player's files match, **START** is
allowed again. No other battle counts for a ladder: a casual room, and a
room a launch file or another lobby program opened, keeps the content rules
above. Entering the Boneyards with such files is refused already
([Entering the Boneyards](../boneyards/entry.md#the-game-files)); this is the
rule for a player whose files changed since, or whose Open Annihilation is
older.

The base games and expansions are the ones in [Game content](../game-content.md).

## A mod with more weapons or sides

A mod that numbers more than 256 weapon types, or more than five sides,
plays only with players on Open Annihilation.
[Mods with more weapon types or sides](limits.md) says which line each
machine shows, when **START** stays grey, and when an older Open
Annihilation is refused.

A player who has chosen a side the room does not have also keeps **START**
grey. The room says once "*** {player} chose a side this game does not
have."
