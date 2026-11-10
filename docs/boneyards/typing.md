# Typing

When a console or pane opens to ask for text, that field has the keyboard.
A refresh of the same screen does not take the keyboard back if the player
has moved to another field. Return runs the button the layout names as its
default.

The focus keys follow the player's layout, in the consoles, on the main
frame and on the war map. Next button, Previous button, Focus left, Focus
right, List up, List down, Page up, Page down, Go to start and Go to end
are the commands. With the default layout, and with a layout saved by
0.9.0, those commands are Tab, Shift+Tab, the arrows, Home, End and the
page keys, and every step below is unchanged. A key the player moves in
the key editor moves here too, and the key it left does not. A character
typed into a field stays in that field when a focus command was moved
onto that character. F6 and Shift+F6 are not those commands: they still
move between the main frame, an open console and the war map, unless the
player has moved one of the commands above onto F6. Nor is Tab, an arrow,
Home, End or a page key held with Ctrl: whatever the layout, it does what
it does with the default layout. Ctrl+Left and Ctrl+Right still move a
field's caret by a word. A focus key held down repeats its move at the
player's repeat delay and rate. Left at the system's, it repeats at the
system's pace.

| Screen | Field |
| --- | --- |
| Chat | the entry line |
| Locator player, planet, battle and frequency searches | the name |
| Locator Server Transfer | none |
| Create Frequency | the frequency name |
| Change Frequency | the password, when the frequency needs one |
| Host | the game name |
| Battle Intelligence of a forming battle | the password, when the battle needs one |
| Enlist in the game | User, the first field. Return runs Register |
| Registration unavailable | Return runs Ok |
| Login | the name, or the password when a name is already saved |
| Profile Account | none. CHANGE focuses the password |
| Contents Ladders | none. The console opens on a page, not on its search |
| QuickPlay | none. The button that opened the console keeps the keyboard |
| Recon, Help, News, Scoreboard, a player's pane, the other Profile tabs | none |

A dialog keeps the keyboard where it was when it opened. The message box
keeps it, and the side prompt's root keeps it. Matchmaking, QuickPlay and
the registration page leave it on the button that opened them, so the white
frame stays where it was. The dialog confines Tab, the arrows and F6 from
the moment it opens. The first Tab, Shift+Tab or arrow, while the focus is
still outside the dialog or on its root, lands on the default answer:
Choose, Ok, No, Activate, Open, or else Back. Return before that key does
what it does today.

Matchmaking therefore leaves the keyboard on Minimize. The first Tab or
arrow lands on Activate, and Return then runs Activate. Escape runs Close
and hands the keyboard back. A key that closes matchmaking or the
registration page hands the keyboard back to the button that opened it; a
click on Close or Back leaves it where the click put it, as before, so no
white frame appears.

After the first Tab, Shift+Tab or F6, the arrows and Return follow the
console. In Chat, Tab visits the frequencies, Create, the players, Minimize
when it is shown, the log and the entry, each once. The arrows choose a
frequency or a player. Return on a frequency opens its pane, where GO joins
it, and Return on a player opens that player's pane.
There the arrows move between General, Ladder and Awards, and Tab continues
to Whisper Lock, Muzzle, Page and the page. Recon's description is reached
the same way. In Help, News and the Ladders, the arrows move through the
list, Right opens a heading and Left closes it, and Return opens the chosen
page. Tab reaches a link on that page, and Return follows it and brings the
keyboard back to the list. The Ladders
name and SEARCH are reached too. Until that first Tab, Shift+Tab or F6,
the arrows and Return do what they did before. Return, or moving the
pointer to a new place, puts them back to that. A move that stays put
leaves them.

F6 and Shift+F6 move between the main frame, the open console and the war
map, also while nothing has the keyboard yet (the war map just after
login). Tab and Shift+Tab stay in the one that has the keyboard. A dialog, the
side prompt, matchmaking and the registration page keep Tab until they
close. Escape presses the screen's Escape control, and closes a console
that has none.

A radio group is one stop: the choice that is showing. The arrows along
the group's line change that choice, and they stop at the end of the line.
A row, whose choices share one top, steps with Left and Right, and Up and
Down leave it. A stack, whose choices share one left edge, steps with Up
and Down, and Left and Right leave it. For the column order a row stands
at its first choice, so Up and Down step from one row to the next whichever
choice is showing.

Any other arrow uses that same column order. It wraps, in the frame and on
the war map, and an arrow there crosses from one to the other. It never
lands on a control an open console covers. Inside an open console the arrow
stays, and at the last stop that way it stays put. A pad leaves the console
with Back, which is Escape, and the keyboard returns to the control it came
from.

Once Tab, Shift+Tab or F6 has moved into a console, the stops are these.

| Screen | Stops |
| --- | --- |
| Battle | the battle list (Up and Down choose the row once Tab or F6 has shown the ring; with no ring, Down after a click reopens the clicked battle), HOST, the planet intelligence list, then the open pane. A forming battle's pane is its password, when the battle has one, and JOIN |
| Host | the game name, the password, the game types (Up and Down choose one once Tab or F6 has shown the ring; with no ring, the clicked type stays), one stop for each of Player Skill, Number of Players and On Commander Death, then START. Right on the skill row chooses the next skill. Down from the skill row reaches the players row, and Down again the death row |
| Profile | the categories (Up and Down choose one once Tab or F6 has shown the ring; with no ring, the clicked category stays), that category's fields, APPLY, then RESET. Escape closes the console |
| Matchmaking | one stop for the game, the skill, the side and the planets, then Activate, then Close. The first Tab from outside the dialog is still Activate. The game row steps with Left and Right |
| QuickPlay | the chosen game type, where, skill, and side (Arm, Core or Either), Favorite, then FIND BATTLE. The side row steps with Left and Right. The console still does not take the keyboard when it opens. Escape closes it |
| Locator | the five find rows, the name, Search, each result, then Relocate and Page. Return in the name still runs the search. Return on a result keeps the keyboard on that result |
