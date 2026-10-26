# Cheat codes

A line that starts with `+` in the [chat line](console.md) is a command.
The name ignores capitals, so `+atm` and `+ATM` are the same command.
This page is every code from Total Annihilation's cheat list, what it
does, and where it works in Open Annihilation.

## Where a code works

Open Annihilation runs every code on this page in a skirmish and in a
campaign mission. No passphrase is required. In 3.1c, `+atm` and
`+nowisee` did not work in a campaign, and `+ai`, `+control` and a unit's
name worked only in a skirmish after the developer passphrase.

In a multiplayer game the **universal** commands always work. The
**cheat** commands work only when the battle room's Cheats option is on,
and then for every player alike. The host turns that option on. The Game
Settings sheet's Cheat Codes row shows Allowed or Disallowed.

Typing `+now Film Chris Include Reload Assert` still turns developer mode
on, and typing anything else after `+now` turns it off. Those five words
are case-sensitive. The command names on this page are not. Developer mode
does not allow the cheat commands. An unknown name, once developer mode is
on, reads a debug script of that name. With Cheats off, the passphrase does
not make `+atm`, `+ai`, `+control` or a unit name work. It still opens
the debug commands that are not on this list, such as `+kill` and
`+reload`, and the F11 debug keys.

## Universal commands

These work in every game, including a multiplayer game with Cheats off.
Unless the table says the line is shared, the command runs on your
machine and the echo stays on your machine.

| Command | What it does |
| --- | --- |
| `+noshake` | Blasts stop shaking the screen, or start again. |
| `+shootall` | Your units fire at every enemy, or only at the ones they were told to. Shared: every machine runs it. |
| `+setsharemetal <amount>` | You share metal once your store is above this much. |
| `+setshareenergy <amount>` | You share energy once your store is above this much. |
| `+sharemetal` | You share metal, or stop. A live multiplayer game announces it. |
| `+shareenergy` | You share energy, or stop. |
| `+sharemapping` | You share what you have mapped, or stop. |
| `+shareradar` | You share radar, or stop. |
| `+shareall` | All four shares switch together. |
| `+give <player> <amount> metal` | You give that player metal. `energy` gives energy. In a network game the gift is sent, so both machines agree. |
| `+clock` | The game clock shows, or hides. |
| `+switchalt` | The number keys select squads without Alt, or need Alt again. `+switchalt 1` and `+switchalt 0` set it. |
| `+bps` | In a network game, the traffic readout shows or hides. It does nothing in a skirmish or a campaign. |
| `+dither` | Dither switches. |
| `+logo <logo> <player>` | That player's logo becomes the numbered logo. A logo or a player that is not there is announced and nothing changes. |
| `+screenchat` | Chat is drawn on the battlefield, or only in the log. |
| `+contour <spacing> [phase]` | Contour lines use that spacing. |
| `+showranges` | Weapon ranges show, or hide. |
| `+sfx` | New particle effects are refused, or allowed again. |
| `+compression` | In a network game, outgoing packets are compressed or sent whole. It does nothing in a skirmish or a campaign. |
| `+selectable` | Every live unit can be selected. Your machine only. |
| `+bigbrother` | The observer camera turns on or off. |
| `+gamma <tenths>` | The palette gamma becomes that many tenths. |
| `+shadow` | Unit shadows switch. |
| `+fshadow` | Feature shadows switch. |
| `+tshadow` | Vehicle shadows switch. |
| `+antialias` | Anti-aliasing switches. |
| `+shading` | Shading switches. |
| `+light <a> <b> <c>` | The model light uses those three values. |
| `+scrollspeed <speed>` | The map scrolls at that speed. |
| `+sound3d` | 3D sound switches. |
| `+cdstop` | The music track stops. |
| `+cdplay <track>` | That music track plays. |
| `+musicmode [1]` | The music mode changes. |
| `+lostype` | Line of sight is drawn the other way. A mod can move this into the cheat commands; then it follows the Cheats option instead. |
| `+sing` | The novelty voice switches. |
| `+iface <mode>` | The interface mode changes. |
| `+nometal [player] [amount]` | That player's metal becomes the amount, or 0. With no player, it is yours. Shared: every machine sets the same store. |
| `+noenergy [player] [amount]` | The same for energy. Shared. |
| `+makeposter [width height]` | A picture of the map around the view is written to the screenshots folder. `+makeposter all` takes the whole map. |

## Cheat commands

These work in every skirmish and every campaign mission. In a multiplayer
game they work only while Cheats is allowed, and the line is sent to
every player. What then runs on the other machines is in the table.

| Command | What it does |
| --- | --- |
| `+atm` | The player you are viewing gains 1000 metal and 1000 energy, or the amount a mod sets. Your machine only. A player another machine controls keeps the store that machine sends. |
| `+nowisee` | Mapping and line of sight turn off, so you see every unit. Your machine only. |
| `+radar` | The minimap shows every unit, or only those in radar contact again. |
| `+view <player>` | You watch that player: their sight, their stores and which units speak. `+view` back returns to you. |
| `+los` | Line of sight switches. |
| `+mapping` | Mapping switches. |
| `+doubleshot` | Every shot deals double damage, or ordinary damage again. |
| `+halfshot` | Every shot deals half damage, or ordinary damage again. |
| `+meteor` | A meteor falls on the next tick. `+meteor 1` turns storms on and `+meteor 0` turns them off. |
| `+ai [player]` | A player your machine runs is handed to the computer, or handed back: you, or a computer player you added in the battle room. With no player, it is you. It changes nothing on the other machines; each player runs `+ai` on their own machine for the players it drives, and a player another machine runs is refused: "That player is run from another machine". A player who is already a computer is announced, "Already a computer player", and left as they are; handing someone else back does not change a computer seated beside them. Handing a human to the computer starts the computer on the next orders tick. |
| `+control <player>` | That player becomes the one you play and the one you view, on your machine only. The line is shown in that player's name, because the change happens before the line is posted. The other machines keep their own player. A player that is not in the game is ignored. |

`+doubleshot`, `+halfshot`, `+radar`, `+view`, `+los`, `+mapping` and
`+meteor` run on the machine that typed them. They are not run again when
the line arrives elsewhere.

## A unit's name

`+unitname` creates that unit for player 0 at the pointer. `+unitname
<player>` creates it for that player. `?` matches one character and `*`
matches any run, so `+arm*` creates every unit whose name starts with
`arm`. Each one is placed in a row from the pointer. A building stands
where a builder would put it there: on its build grid, at the height its
footprint takes, so a shipyard on the water floats with its lower part
under the surface. Any other unit settles on the ground or the water as a
unit a factory makes does.

A player number that is not a slot is announced, "No such player", and no
unit is created. A slot nobody is using still gets the unit on this
machine. In a network game a seated player's unit is created on the
machine that plays that player, at that machine's pointer. The other
machines do not create a second one.

This is a cheat: a skirmish and a campaign always, a multiplayer game
only while Cheats is allowed. A unit whose name is itself a command, such
as a unit named `AI`, is the command and is not created.

## Other codes

These are not typed on the chat line.

- **DRDEATH**, in capitals, on the single-player screen shows the control
  that opens any campaign mission. Typing it again hides that control.
- **\*III** through **\*X**, in capitals, on the skirmish setup sets the
  number of slots from 3 to 10. A setting can choose the same numbers.
- **\\** repeats the last command, under the same rule as typing it
  again. A cheat repeats only while cheats are allowed; an option such as
  `+clock` repeats without that. With the key-remap option, Insert
  repeats and **\\** does nothing. F11 still needs the passphrase, and so
  does F10 when it stands in for F11.
