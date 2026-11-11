# Keys on the multiplayer screens

The keys follow the player's layout. The focus ring, and the same keys on
the rest of the game, are [Accessibility](../accessibility.md). Next button
moves to the next control and Previous button moves to the previous one,
on the provider list, the
address, the game list, the screen where a game is named, and the battle
room. With the game's own layout those are Tab and Shift+Tab, and a layout
from 0.9.0 does the same. Up and Down are the list keys. They change the
selected row of a list once that list is the focused control, and they do
not move to another control. A held arrow, and a held Next button, repeat
at the player's repeat delay and rate. Left at the system's, they repeat
at the system's pace. Return presses the focused control, the same
way a click does. A key moved onto a letter still types in the address, the
game name and the chat line, and shows no ring there. Pressed anywhere else,
it moves the focus and types nothing, not even into the field it lands on.

In the battle room the chat line has the keyboard when the room opens. Until
Next button or Previous button is pressed, the arrow keys move as they have:
through the lists, then each other player's row, then back to the chat line.
Up and Down stay inside a list. Left and Right stay in the chat line. Nothing
is drawn around the focused control.

Next button and Previous button then move through the player's own Ready,
side, colour and team, then the options down the column as it stands:
commander, fixed location, mapping, line of sight, cheating, watching,
game open and restrictions. A greyed option is skipped. Select Map
comes next, then Mutators when that button is shown, then the Metal, Energy
and Max Units sliders, the chat log, the chat line, Start and Previous Menu.
The first Next button leaves the current control for the next of those and
draws a ring around it. Another player's row is not in that order. A control
the room has greyed, such as the host's options on a client's machine, stays
on the screen for the pointer and is left out of that order.

Once that ring is showing, Left and Right each move the player's colour to
the next one, the same as a click on the colour. Left and Right step a
slider. The arrow keys move among the other players' rows. Return presses
the focused control. Return on another player's row opens that player's card
when the row shows one, and otherwise does nothing; it does not press the
row. Return, or a move of the pointer, hides the ring. The next Next button
shows it on the control it moves to. With Always show focus on, the ring is
there when the screen opens. The room reads that ring from the layer, so a
button that took Return, Next button or Escape, such as Mutators, leaves the
arrows walking as they do when the ring is hidden.

A button a mod adds for the host is used with the pointer.
