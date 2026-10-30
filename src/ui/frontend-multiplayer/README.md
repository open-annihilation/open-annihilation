# Multiplayer screens

The multiplayer connect, lobby and battle room. Library `oa-ui-frontend-multiplayer`, headers `include/oa/ui/frontend_multiplayer/`, namespace `oa::ui::frontend_multiplayer`. The battle room's own screens are described beside the host in `src/app/README.md`. This page is the mutator lines that room posts.

## Entry points

`mutator_room` builds the words the battle room posts, the line beside START, and what a guest's GET MUTATORS shows. It does not read the lobby and it posts nothing itself.

`mutator_chat_lines` is what every machine posts when the room's list changes. The same list posts nothing. A list that becomes empty posts "*** {host} turned mutators off." A list that was empty posts "*** {host} turned on mutators: {list}" and then "*** This game is Open Annihilation only, and unranked." Any other change posts the turned-on lines only. Names are joined with ", " and split at those boundaries so no line passes 71 bytes and no name is cut. Each continuation is "*** {list}". A first name that does not fit beside the host's words starts the next line, and the turned-on line goes alone, so a change always says who made it.

`fit_chat_line` breaks one of these lines, when it is wider than the battle room's chat list as the caller measures it or longer than the 71 bytes the chat ring keeps of a line, at spaces into lines that fit; each further line starts "*** " when the first does, and a single word wider than the list stays whole. A line holding Chinese or Japanese characters, kana or Hangul breaks as the text kit wraps (`first_row`): also between two Chinese or Japanese characters, never inside a UTF-8 sequence, and inside a Latin or Hangul word only when it is wider than the list.

`lobby_post_classic_line` posts the two lines about a 3.1c player, "*** {player} plays Total Annihilation 3.1c, which cannot play with mutators." and "*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.", from the interface catalogue. With the chat list's measure bound (`multiplayer_bind_chat_measure`, `Lobby::chat_measure`) it fits them with `fit_chat_line`; unbound, they are cut by bytes alone.

The host posts "*** {player} chose a side this game does not have." once for each player and side when a playing seat's side is past the room's side count. START waits while a playing seat's side is past that count.

`room_limits` keeps a room whose profile raises weapon types past 256, or sides past five, to Open Annihilation. The host refuses a 3.1c player with "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than 256 weapon types." or "*** {player} plays Total Annihilation 3.1c, which cannot play a mod with more than five sides.", and an Open Annihilation player whose presence record does not cover those limits with "*** {player} needs a newer Open Annihilation for this mod.", once, when the record arrives. START stays grey while either is seated, including a build that sends no limits word. A player in such a room leaves a 3.1c host, and the game list shows "This mod needs every player on Open Annihilation. The host plays Total Annihilation 3.1c." Each of those lines is written whole to the log. A room at 3.1c's limits refuses no one for them. The side check beside START is unchanged.

`lacking_lines` is one "*** {player} does not have {name}." for each player the host has just found lacking a mutator. `start_reason` is "Waiting for {player}: {name}", the line beside START. `reason_box` places that waiting line: its right edge 4 points before START's left, its top 4 points under START's top, shortened from the left by any rectangle it meets.

`guest_lack_lines` is what a guest posts, in its own chat only, when the mutators it lacks change. The same keys post nothing. A lack's key is its id, or its name when it has no id, so a name that arrives after the id was posted does not post the line again. Nothing lacking posts nothing. One name posts "*** You do not have {name}. GET MUTATORS downloads it from {registry}." Several post "*** You do not have {names}. GET MUTATORS downloads them from {registry}." with the first lack's registry. When no registry offers the first lack, "*** You do not have {name}." or "*** You do not have {names}." posts alone; each form is a catalogue word of its own, so every language reads it whole. Names are joined with ", " and split at those boundaries so no line passes 71 bytes and no name is cut. A sentence that does not fit splits after its first full stop, ". " or the Chinese "。", and the rest starts "*** ". `guest_ready_line` is "Ready once {name} is here", the line in READY's place while a mutator is missing. It does not enable or disable READY.

`get_mutators_state` reads each item of the host's list in the host's order. The first item that is not already here decides: `get` and `other_release_get` are `get`, `gone` and `other_release_gone` are `gone`, `waiting_for_catalogue` is `none` until that catalogue is read, and `downloading`, `failed` and `downloads_off` keep their names. An empty list, or one this machine already has, is `none`. `get_mutators_text` is the line for that state: "Getting {name}: {percent}%", "{name}: {problem}" with a known failure as a short phrase such as "unreachable", "{name}: not offered.", or "{registry}: downloads off." ("Downloads are off." with no registry). `none` and `get` have no line of their own. `get_mutators_button_shown` is true for `get` and `failed`.

The host's Mutators button lives with the skirmish button in `runtime_mutator_button.cpp`. The guest's VIEW… and GET MUTATORS, the bar and the ready line live in `runtime_get_mutators.cpp`. VIEW… opens the picker read-only on the host's set.

A difference line on the presence card wraps when it is wider than the card, onto at most three lines (`presence_card_difference_lines`), and the last is cut with "..." when the rest does not fit. The presence card, after Rules, has one row a mutator of the host's set when the room has one. The first row is labelled "Mutators". "{item} ✓" when that player has the host's release. "{item}: not installed" or "{item}: another release here", in amber, when not. The host's own card marks every one ✓. A player who lacks one then reads, after the content difference lines, "Different from the host: no {name}." or "Different from the host: another release of {name}.", and "The game cannot start until {player} gets it, or the host turns it off."

## State

The helpers keep none. The caller passes the names and the rectangles and posts the lines. The room's list, the chat ring and the lock on the host's options stay with the battle room.

## Invariants

- A line is at most 71 bytes, and a mutator's name is never cut to make it fit.
- The same list twice posts nothing. The Open Annihilation line is posted only when the list goes from none to some, and the off line only when it goes to none.
- An edge that only meets the waiting line does not shorten it.
- The waiting line does not enable or disable START, and the guest's ready line does not enable or disable READY.
- The same keys of what a guest lacks post nothing. A sentence is split after its period rather than cutting a name.

## Tests

`ui-multiplayer-mutator-room` places the host's button and the guest's two-button row over the battle room's rectangles at 640×480 and at 1.5 points a frame pixel, with GET MAP's box present and absent, and checks the chat lines, the guest's lack line once per change, each fetch state and its line, the lacking lines, the waiting line and a translated lookup. `ui-multiplayer-presence-card` checks a host card and a guest card for each mutator, and the words through the translation. `ui-multiplayer-lobby` checks the rules dot for a guest that lacks one mutator of the host's list and for a guest that has them all. `ui-multiplayer-room-mutators` checks the 3.1c lines from a German and a French catalogue, in the code page, and fitted to a bound measure. `native-mutator-room` hosts a battle room alone and checks that the guest's controls stay hidden, and that at 640x480 the 3.1c lines and the waiting line read whole in English, German, French, Spanish and Italian.

## Limitations

The chat ring holds text only, so the lines carry the words and not a colour. The second line of the host's block is left out when the frame has no room for it. A guest's row can sit above the player rows when GET MAP's box leaves no clear place under them.
