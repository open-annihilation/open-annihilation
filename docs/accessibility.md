# Accessibility

Colour vision, the focus ring, the keyboard and screen readers. The rows
are in Settings › Accessibility. Each one takes effect at once, and Cancel
puts the section back to what it held when Settings opened.
[Settings](settings.md#accessibility) lists every row, including the input
helpers and the practice pad. [Keys](keys.md#accessibility) lists the
helpers a layout can ask for.

The lobby has its own page for its regions, its consoles, and the colours
and marks drawn there: [Colours, keys and speech in the lobby](boneyards/accessibility.md).

## Contents

- [Colour vision](#colour-vision)
- [What each mode changes](#what-each-mode-changes)
- [What the check does not cover](#what-the-check-does-not-cover)
- [Shape cues](#shape-cues)
- [The focus ring](#the-focus-ring)
- [The keyboard](#the-keyboard)
- [Screen readers](#screen-readers)
- [What is spoken](#what-is-spoken)

## Colour vision

**Colour vision** is Off, Protanopia, Deuteranopia, Tritanopia or
Achromatopsia. Off is the default. It changes the colours this machine
uses to tell players, health and status apart. It does not change the
match. A saved game, a recording and the bytes another machine receives
stay as they are, and another player sees the colours that player's own
setting chooses. With Off, the picture is the one the game has always
drawn.

The preview below Shape cues shows the ten player colours, then OK,
caution, danger and information, in the mode you chose. With Off it shows
the stock palette's colours, even when the mod you play has its own
palette. In the game, each colour of a mode is matched to the nearest
colour of the palette in use.

Choosing any mode other than Off turns Shape cues on. Choosing Off does
not turn them off. In Achromatopsia the row stays on and is locked, and it
says "Achromatopsia keeps shape cues on". Tab skips a locked row, but a
screen reader still reads it: its name, its value, that it cannot be
changed, and the reason as its description.

## What each mode changes

Protanopia, deuteranopia and tritanopia each give the ten players a new
set of colours, on units, logos, the radar and the dots on the minimap and
the megamap. Protanopia and deuteranopia share the status colours below.
Their player colours differ. Tritanopia has its own of both.

Achromatopsia cannot give ten different colours. It uses four levels of
lightness, shared across the ten players, and the markers of Shape cues
tell the players who share a level apart. The four are palette colours,
from a very dark red to a light green, picked to differ in lightness
alone, so that to an eye with achromatopsia they are four greys. Health
and build bars are white, grey and dark grey.

| | Protanopia and deuteranopia | Tritanopia | Achromatopsia |
| --- | --- | --- | --- |
| Health and build bars | light blue, cream, red | blue, yellow, red | white, grey, dark grey |
| Selection and a legal build site | the "good" colour | the "good" colour | white |
| A refused build site, and the range blink | the "bad" colour | the "bad" colour | dark grey |
| Resource figures | produced and consumed recoloured | produced and consumed recoloured | a light blue and a dark grey, with + and − |
| Status (OK, caution, danger, information) | a blue, a cream, a red-orange and a grey | a blue, a gold, a red-orange and a grey | four greys |

The ground, unit bodies, the original menus' pictures and the gold of the
OA button stay as they are. The lobby's links, chat and player rows are
on [its page](boneyards/accessibility.md).

## What the check does not cover

The player colours were checked against a model of each kind of colour
vision. The goal was that every pair a player must tell apart differs by
at least 20 under that eye. Units are small on screen, so the goal sits
above the point at which two colours merely look different side by side.
The ten colours of protanopia, deuteranopia and tritanopia meet that goal
under the model of that eye, and achromatopsia's four shades do too. The
check has three limits.

- The model is complete colour blindness, the worst case. Partial colour
  blindness, which is the common kind, sits between ordinary vision and
  that. Both ends pass. The check does not try every step between them.
- The tritanopia model used first (Machado, Oliveira and Fernandes, 2009)
  is the least well checked of the three. Tritanopia is rare. The same
  colours were also checked with Brettel, Viénot and Mollon (1997), where
  the closest pair differs by 20.15. Protanopia and deuteranopia were
  checked again with Viénot, Brettel and Mollon (1999).
- Lightness matters on the battlefield. Each protanopia, deuteranopia and
  tritanopia colour is at L* 15 or lighter, and the darkest of them is
  L* 15.6. Achromatopsia's darkest shade stays at L* 9. Four shades that
  stay 20 apart cannot all sit further than that from the greys of the
  ground.

Under achromatopsia some status colours stay closer than 20. The marks of
Shape cues, the + and − on the resource bar, and the notches in a bar
carry that difference. The check does not claim those pairs meet 20 on
their own.

## Shape cues

**Shape cues** is Off or On, Off by default. On, and always in
Achromatopsia:

- Each player has a shape and a number. Players 1 to 5 are a filled
  circle, square, triangle, diamond and star. Players 6 to 10 are the
  same shapes hollow. The number sits beside the shape, except on the
  minimap, the megamap and the battle room's colour cells, which show the
  shape alone. The shape replaces the dot on the minimap, the megamap and
  the radar, sits on a unit while it is selected or under the pointer, and
  stands in front of the name in the side panel, the team panels, the kill
  board and the chat line.
- A health bar and a build bar gain a notch at one third and at two
  thirds.
- A refused build site shows a cross at the pointer. An enemy's weapon
  range is a dashed ring. Yours, and an ally's, stays solid.
- A chip that states something, and the warning dot on the OA button,
  carry a mark for that state. A chip that is already an icon, a filter
  or a tag does not gain a second mark.

The lobby's side symbols, role tags and latency bars are on
[its page](boneyards/accessibility.md).

## The focus ring

A screen opens on the control it has always opened on, and no ring is
drawn. The main menu reports Single player as focused and draws nothing
new. The Library is the exception: it opens with the ring already on its
search, so you can type at once.

Tab moves to the next stop and draws the ring there. Shift+Tab moves to
the previous stop and draws it there. The first Tab leaves the control the
screen opened on. Return presses the focused control and hides the ring.
Moving the pointer hides it too. The next Tab or Shift+Tab shows it on the
control it moves to. While the ring is hidden, Return, Space, the arrows
and a button's underlined letter do what they have always done.

**Always show focus** is Off or On, Off by default. On, the ring is drawn
on the focused control when the screen opens, including the battle room's
chat line.

The ring is the one the game already draws on its own screens: one point
of the focus colour. It is drawn above anything that covers the control.

A row of tabs, a row of chips, a radio group and a list are each one stop.
The arrows move inside that stop. Tab leaves it. Up and Down step a list
once that list is focused, and they do not move to another control. Left
and Right step a slider, a row of choices and the battle room's colour.
Page Up and Page Down take a large step, and Home and End go to the ends.

The focus keys follow your layout. On the original menus, the multiplayer
screens, the battle room and the lobby they are Next button, Previous
button, Focus left and Focus right, List up and List down, Page up, Page
down, Go to start and Go to end. On Open Annihilation's own screens, such
as Settings, the Library and the key editor, they are Next control and
Previous control, Up, Down, Left and Right, with the same page and end
keys, under Window in the key editor. With the game's own layout, and with
a layout saved by 0.9.0, those are Tab and Shift+Tab, the arrows, Home,
End and the page keys. A key you move in the editor moves here too, and
the key it left does not. On the multiplayer screens and in the lobby, a
letter you move a focus key onto still types in a text field, and shows no
ring there.

On the multiplayer screens and in the lobby's consoles, a held focus key
repeats at your Key repeat delay and Key repeat rate. Left at System, it
repeats at the system's pace. On every other screen a held focus key
repeats at the system's pace.

## The keyboard

The paths below are the ones the game walks. A control that is greyed is
not a stop. Tab wraps at the ends.

[The original menus](controls.md#the-original-menus) has the main menu,
Single player, a new campaign, Any mission, Load game, the skirmish, map
selection, Options and its pages, the briefing and the campaign's end.
On the main menu, Tab walks Single player, Multiplayer, Intro and Exit,
then the Library and Settings. While the update pill shows, it comes after
Settings. While the menu's strip is drawn, its buttons stand in for those
four and Enter sits between the columns, with Credits after Enter. The
letter under Enter still enters.

[Keys on the multiplayer screens](multiplayer/keyboard.md) has the provider
list, the address, the game list, the screen where a game is named, and
the battle room. In the battle room the chat line has the keyboard when
the room opens. The first Next button leaves it and draws the ring. The
order is your own Ready, side, colour and team, then the options down the
column, Select Map, Mutators when that button is shown, the Metal, Energy
and Max Units sliders, the chat log, the chat line, Start and Previous
Menu. Start is not a stop until the room can start. Another player's row
is not in that order. Once the ring is showing, the arrows move among the
other players' rows, and Return on a row opens that player's card when the
row shows one.

Settings opens on the open section, with no ring. The section list is one
stop. Tab walks that stop, then that section's rows, Restore defaults,
Cancel and OK, and the close mark. The first Tab shows the ring on the
section's first row. The first Shift+Tab shows it on the close mark.
Until the ring shows, Up moves to the close mark, and Down, Left, Right
and Space move to the section's first row. Once the ring is on the
section list, the arrows change section.

[The Library](library.md#keys-and-controllers) walks its tabs as one stop,
then the rows of the open tab. At a small window the search, the keys
control, Unload mod and the map links sit below the fold. They are not
stops until the page scrolls. Page Down, or Down from the expansion
pills, scrolls it, and Tab then includes the ones that have come into
view. The Library still opens with the ring on its search.

The other screens:

- **Map browser.** The source, the player and size rows, the search, the
  cards, Cancel and Close. Select joins, between Cancel and Close, once
  the chosen card can be played. The cards are one stop, and the arrows
  move among them.
- **Mutators.** The filter pills, the tag pills, Tags when some tags do
  not fit as pills, the search, the list, the chosen mutator's own
  controls, Cancel, Use these and Close. The list is one stop. At a small
  window the filter is one drop-down, and Details, once a mutator is
  chosen, takes the place of its controls.
- **Brain picker.** The list, the level strip, the options and the
  every-seat switch, Cancel, Use this and Close. It has no filter and no
  search. At a small window Details takes the place of the level strip,
  the options and the switch.
- **Key editor.** The view, the search, Find, the category, the command
  list, Undo, More, Cancel, Save and Close.
- **Welcome.** Skip, Back, Next, Previous, the language cards and the
  control that moves to the next card. The cards are one stop.
- **Keys panel.** Edit keys, Close and the list. Its key is Ctrl+/ (Cmd+/
  on a Mac), and it moves in the editor.
- **In-game menu.** Load Game, Save Game, Options, the briefing, Help,
  Exit and Resume. Escape resumes. Options opens on the Sound page. Tab
  walks Cancel, Undo, that page's settings, the restore button, the Sound,
  Music, Speeds and Visuals buttons, and Previous.
- **Arrange overlays.** The handles, Reset, Cancel and Done. An arrow
  moves the focused handle.

Two things a match does not do on its own. **Focus the panel's buttons**
(`ui.hud-focus`) is unbound. During a match F6 is Go to bookmark 2, and
there is no free chord, so bind this in the editor if you want it. While
it is on, Tab and Shift+Tab move a ring over the order buttons and the
build page, and Return presses the focused button. Escape, or the action
again, returns to the battlefield. F6 moves between regions only outside a
match, on the lobby's frame, its map and an open console. The twelve
build cells are unbound too. Bind a cell in the editor to press it from
the keyboard. [Pause and the game menu](controls.md#pause-and-the-game-menu)
says the same.

## Screen readers

**Screen reader** is Automatic, On or Off. Automatic is the default.

- **Automatic** works with a screen reader while the system says one is
  running. Narrator counts, as do NVDA and JAWS on Windows, VoiceOver on
  a Mac, an iPhone or an iPad, and Orca on Linux.
- **On** also works with a reader the system does not report.
- **Off** never does. Nothing is read, and nothing is spoken.

Where the support runs, the game offers the same controls the keyboard
walks. It builds that tree while a reader is running and has asked for
the window:

- On Windows 10 and later, through UI Automation. Narrator, NVDA and JAWS
  read that tree.
- On a Mac, an iPhone or an iPad, through VoiceOver.
- On Linux, through AT-SPI, which Orca reads.

On Windows older than Windows 10 the game does not load that support. It
speaks with the system's own voice instead, where that voice is installed
(SAPI). That is Windows 8, 7, Vista, XP and the Windows 95 package, and
any later Windows whose support did not load. Under Automatic this speaks
while the system says a screen reader is running. Under On it speaks
whether or not one is. A move speaks the control's name, what it is and
its value, and cuts off whatever was being spoken. A message that comes
with the move, such as the text of a message box, is spoken first and is
not cut off. A later change of only the value, such as a slider moving
while it stays focused, is not spoken again. Changing Screen reader itself
is spoken, because the row is read again. macOS, iOS and Linux do not use
this voice. The reader speaks there.

The Windows 95 package leaves the support out. When that package also has
no voice, the row says "Nothing on this system can speak." A machine with
no voice stays quiet: no dialog, and no error.
[Speech on an older Windows](troubleshooting.md#speech-on-an-older-windows)
is the same account.

A password is read as its length, never as the text it holds. A control
that is hidden, under a console or behind a dialog, is left out. A greyed
control stays, and it cannot be pressed.

## What is spoken

Spoken lines use the screen reader when one is attached, and the system's
voice on the older Windows above. They are quiet while Screen reader is
Off, and while Automatic finds no reader running.

| Row | Default | What it speaks |
| --- | --- | --- |
| Speak chat | All | All speaks chat lines, whispers, pages and someone joining or leaving a chat room, in the lobby and in a battle room or a match. Pages only speaks pages, the short messages one player sends another, and leaves the rest quiet. Off speaks none of them. A burst of lines is spoken as one count, "5 new lines in Main". Your own lines are not spoken, and neither is a player you have muted. |
| Speak lobby changes | On | The battle room only: a player joining or leaving, a change of ready, side, colour, team, map or option, the countdown, and the host leaving. Your own changes are not spoken. It does not cover the lobby's consoles. |
| Speak the ticker | Off | The ticker repeats, so it stays quiet until this is on. |

These are spoken whenever a reader or the fallback voice is in use. They
have no row of their own:

- "Commander under attack", when your commander is reported under attack.
  The game's own voice already speaks unit alerts.
- Load Game, when it has no saved games: "There are no saved games to
  choose from".

Opening a console, login progress, a chosen planet and a message box are
spoken too. They are listed on
[the lobby's page](boneyards/accessibility.md). Pages only means the pages
there.
