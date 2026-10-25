# Mods with more weapon types or sides

A mod's profile can raise how many weapon types it uses past 256, or how
many sides past five. Either one means the match can be played only on
Open Annihilation. What the mod happens to load does not decide it: the
profile does. A room at 256 weapon types and five sides plays as it
always has, including with Total Annihilation 3.1c.

The host refuses a player on Total Annihilation 3.1c as that player
arrives. When the profile raises weapon types, the room says "*** {player}
plays Total Annihilation 3.1c, which cannot play a mod with more than 256
weapon types." When it raises only the sides, the room says "*** {player}
plays Total Annihilation 3.1c, which cannot play a mod with more than five
sides." When both are raised, the weapon-types line is the one shown.
The line stays on the machines in the room. It is not sent out as chat.

An Open Annihilation player is seated first. When that player's presence
record arrives and does not name limits high enough for the mod, the host
refuses them once: "*** {player} needs a newer Open Annihilation for this
mod." A later record from the same player does not say it again. A build
that sends no such word, such as Open Annihilation 0.7.4, is not refused.

**START** stays grey while a 3.1c player is in the room, while an Open
Annihilation player has not yet shown limits that cover the mod, or while
a build that sends no limits word is still seated. It stays grey until
that player leaves. A press while it is grey selects nothing.

A player on Open Annihilation whose mod is one of these, seated in a room
a Total Annihilation 3.1c host is playing, leaves. The game list says
"This mod needs every player on Open Annihilation. The host plays Total
Annihilation 3.1c." A direct join ends with that same sentence.

Each of these sentences is also written to the game's log, whole, on one
line, with the player's name filled in.
