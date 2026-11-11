# Accessibility in the Boneyards

The Boneyards uses the same Settings › Accessibility rows as the rest of
the game. Colour vision, shape cues, the focus ring, screen readers and
what is spoken are in [Accessibility](../accessibility.md). This page is
the part that is only here: the regions, the war map, the consoles, and
the colours and marks drawn on them.

[Typing](typing.md) says which field has the keyboard when a console
opens, and how the arrows move once they follow the console. The focus
keys follow your layout here too, on the consoles, the main frame and the
war map. A held one repeats at your Key repeat delay and Key repeat rate.

## Contents

- [Regions](#regions)
- [The war map](#the-war-map)
- [The consoles](#the-consoles)
- [Colours and cues](#colours-and-cues)
- [What is spoken](#what-is-spoken)

## Regions

Three regions can take the keyboard: the main frame, an open console and
the war map. F6 moves to the next of those that is showing, and Shift+F6
to the previous. The ring is drawn on the stop it lands on. Tab and
Shift+Tab stay inside the region that has the keyboard.

Opening a console with a click leaves the keyboard on the frame's button.
F6 then enters the console. Just after login nothing has the keyboard, so
the first F6 lands on the frame and the next on a planet.

A dialog, the side prompt, matchmaking and the registration page confine
Tab, the arrows and F6 from the moment they open. The first Tab, Shift+Tab
or arrow, while the focus is still outside, lands on the default answer.
Escape closes a console, and returns the keyboard to the control it came
from.

A greyed control is not a stop. A radio group is one stop, and the arrows
along its line change the choice.

## The war map

[The galactic map](war-map.md#the-keyboard) is the longer account. On the
map the ring is drawn on a planet. The arrows move to the nearest planet
in that direction, and a planet joined to this one by a line is taken
before the others. When no planet lies that way, the keyboard stays.
Return chooses the planet the ring is on, and Recon, Chat and Battle
light. Home moves to the capitol of your side. + and − change the
magnification, as the slider does.

The ring is not drawn until F6, Tab or Shift+Tab. Until then an arrow
moves as it does on the main frame. The eight edge-scroll buttons stay on
the frame. Tab walks the planets of the map that is showing. It does not
use one fixed list of names.

## The consoles

On the main frame, once a planet is chosen, Tab walks QuickPlay, Recon,
Chat, Battle, News, Ladders, Locator, Profile, Help, the zoom slider, the
eight scrolls and Exit. Recon, Chat and Battle are stops only once a
planet is chosen. Release is a stop only while the map is held. A screen
reader names the scrolls "Scroll the map up", "Scroll the map up and
right", "Scroll the map right", "Scroll the map down and right", "Scroll
the map down", "Scroll the map down and left", "Scroll the map left" and
"Scroll the map up and left". The slider is Zoom.

Until the first Tab, Shift+Tab or F6, the arrows and Return do what they
did before. After that, the stops are these. A name in brackets is the
window's own name, where a reader says a shorter one.

| Screen | Stops |
| --- | --- |
| Login | Password, REGISTER, CANCEL, Server List, Status, Registered User, Name. Login joins after Password once a password is typed. Password is always a stop |
| Chat | Chat rooms, CREATE, Chat players, Minimize when it is shown, the log (Chat), the entry (Chat line) |
| A player's pane | those, then General, Whisper Lock, Muzzle, PAGE and the page's scroll bar. The arrows move between General, Ladder and Awards |
| Side prompt | Choose, then EXIT, Arm and Core. The first Tab from outside lands on Choose |
| Battle | HOST, the planet list (Planets), JOIN and the battle list (Battle). When the battle has a password, that field joins in front of JOIN |
| Host | the game name, the password, the game types, one stop each for Player Skill, Number of Players and On Commander Death, the battle list, HOST and the planet list. START joins between On Commander Death and the battle list once the form can start |
| Profile | CHANGE, then the categories (Profile). That category's fields, APPLY and RESET join once the profile has a change. On a clean profile APPLY and RESET are greyed, so they are not stops |
| Recon | the description (Recon) |
| News and Help | the contents list (Contents). On an open Help page, that page's links, then the list. Return on a link follows it and brings the keyboard back to the list |
| Ladders | the contents list, the name (Ladder search) and SEARCH. SEARCH is greyed while the name is empty, and it is not a stop until a letter is typed |
| Locator | Search, Locate Player, Locate Planet, Find Battle, Find Frequency, Server Transfer and the name. Return in the name runs the search. Each result, then Relocate and Page, join after that search |
| QuickPlay | the chosen game type, where, skill, side (Arm, Core or Either), Favorite (Prefer my favorite, and the map's name) and FIND BATTLE. The console does not take the keyboard when it opens. Escape closes it |
| Matchmaking | ACTIVATE, Close, then one stop each for the game, the skill band, the side and the planet. The first Tab from outside is ACTIVATE. Escape is Close |

## Colours and cues

[Colour vision](colour-vision.md) is the full account of which pictures
move. In short: a colour-vision mode recolours the war map's Arm, Core
and contested links, chat roles and the coloured part of a latency
grade, matched to the palette in use. Planets, rings and the star field
stay as they are. Lines already in the chat log keep the colour they
were printed in.

Shape cues, while they are on, and always in Achromatopsia:

- A planet a side holds carries that side's filled mark, a triangle for
  Arm and a square for Core. A capitol also carries the outline of the
  side that founded it. A filled mark means a side holds the planet.
- In Achromatopsia, Arm's links are dashed. Every other link stays solid.
- The planet summary writes the side's name in front of each win count.
- A chat line gains a bracketed role tag, such as [SYS], [NEWS], [ARM]
  or [CORE], or that language's own bracketed side name.
- A latency picture is one, two or three bars. A name whispers are
  locked onto carries a lock, and a muzzled name a cross.

## What is spoken

Pages only means Boneyards pages. A page is the short message one player
sends another, spoken as "Page from {name}" or "Page from {name}: {text}".
Speak chat, on All, also speaks a whisper ("Whisper from {name}: {text}",
and the echo of one you send) and someone joining or leaving a chat room
("{name} joined {room}", "{name} left {room}"). A burst of lines is one
count, "5 new lines in Main". Your own lines are not spoken, and neither
is a player you have muted. Speak lobby changes does not cover these
consoles. It covers the battle room, on
[Accessibility](../accessibility.md#what-is-spoken).

These are spoken whenever a reader or the fallback voice is in use. They
have no row of their own:

- Login: "Offline", "Connecting", "Disconnected", or "Logged in as {name}".
- "Planet {name}", when you choose a planet, and "Summary of {name}:
  {owner}, {battles} battles, {forming} forming".
- "Opened {console}", when a console opens. Battle says "Battle, {place}"
  once a planet is chosen.
- "Entered {room}", when you enter a chat room.
- The text of a message box.
