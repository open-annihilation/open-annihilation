# Saves, recordings and films

A saved game is still the same file an older build reads. Beside it, and
beside a recording, Open Annihilation writes a short note named with
`.oa-content` on the end of the file's name. A film's note is `oa-content.txt`
in the film's folder. The note names the base game, the expansions that
change the rules, and the mod's name. Music is not in the note.

A file from before this note has no note. It loads as it does today.

Loading a save whose note names a base game, or a rule-changing expansion,
that is off shows **This save needs other content**. The question compares
the save with the game as it is now. **Turn on and load** turns on what the
save needs, leaves every other expansion as it is, restarts the game's files
and then loads the save. Choosing another base game clears the mod, as
choosing a base game does. **Cancel** leaves the save unopened.

A recording opened with `--play-demo` asks the same question and, on **Turn
on and load**, plays that recording after the restart. A recording whose
note does not match, opened where the question is not shown, is refused with
the same sentence. A film keeps the note and does not ask on its own: nothing
loads a film as a game.

Saved games stay in `Saves/<mod>/`, and recordings and films stay in their
own folders for that mod.

A save or a recording made with mutators keeps those mutators, and so does
a campaign save. Loading it needs the same ones. When one is missing, the
game names it. [Playing with mutators](playing-with-mutators.md) says what
the note holds and what the refusal reads.

A save keeps each computer seat's brain as well: the id, the version, the
level and the options. The note beside the save, and beside a recording,
names each brain after the mutators, with the player and the id, then the
version, the level's name and the brain's name. A recording keeps that
text and does not run the brain when it is played back. Loading a save
whose brain is not installed asks "Load with the built-in player?"
**LOAD WITH THE BUILT-IN PLAYER** takes those seats from the units as
they stand. [Playing with brains](playing-with-brains.md#saves-and-recordings)
quotes the question.

## What a saved game keeps

A saved game keeps what a 3.1c saved game keeps. Shots in flight are not
among them: shells, rockets, beams and missiles, nuclear missiles included,
are gone when the game loads, as in 3.1c. A nuclear missile launched just
before saving does not land after loading, and its silo does not get it
back: the round left the silo's stockpile at launch, and the save holds the
stockpile as it was after the launch. Save before you launch, or after the
missile lands.

## Mods with more weapon types or sides

A game played under a mod that numbers more weapon types, or loads more
sides, saves and loads like any other game, and only under that mod.

Its recording keeps the mod's two limits. It plays back only under a mod
with the same weapon types and the same sides. Under any other it says
"This recording needs a mod with {n} weapon types and {m} sides."

A recording made without those limits (one from 3.1c, or one Open
Annihilation made of a game under 3.1c's limits) does not play under a mod
that raises either. It says "This recording was made with 256 weapon types
and five sides. The mod you are playing has more."

Other replayers stop at the first of Open Annihilation's wide weapon records.
