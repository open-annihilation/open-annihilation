# Photo

Names a photo of the battlefield, and builds the text the file carries
about the map, the players and the moment. Later tickets add the sizes,
the tiles, the looks and the depth cues. This module draws nothing and
opens no file.

## Entry points

`oa/present/photo/photo_file_name.hpp`, namespace `oa::present::photo`:

- `LocalMoment` is the local date and time the name uses.
- `path_characters` counts a text the way a path counts it: bytes, or
  UTF-16 units on Windows.
- `photo_file_name` builds `Photo <YYYY-MM-DD> <HH.MM.SS> <map>.png`.
  Characters a file system refuses in the map become a space. The map is
  shortened by whole characters so the path fits a budget the caller gives.
  The second photo of a second is numbered `2`, then `3`.

`oa/present/photo/photo_texts.hpp`, same namespace:

- `photo_texts` builds the six text chunks from facts the caller fills in.
  Title and Description are UTF-8. Software, Creation Time and Source are
  Latin-1. OpenAnnihilation is a JSON record of version 1. The caller keeps
  the returned strings until the PNG write returns.
- Game text is read as UTF-8, and a byte that is not UTF-8 is read in the
  game's code page.
- With player names off, no player's name is written. The description then
  gives each side, and a team only when that player has one.
- A team is numbered from 1. The game's first team is team 1. A player with
  no team has `"team": null`, and the description leaves the team out, with
  names on and with names off.
- A folder, an account, a place a machine can be reached, and a replay's
  directory are not written. `replay` and `look` are written only when the
  facts carry them. With names on, `replay` keeps the file's own name. With
  names off it keeps the digest and the tick.

## State

None. Each call reads its arguments and returns a name.

## Invariants

- The date, the time, a collision's number and `.png` are never shortened.
- A UTF-8 character is kept whole or dropped whole.
- Refused characters are replaced with a space, not removed. A trailing run
  of dots and spaces on the map becomes spaces.
- The name holds no folder.
- The chunk list never holds a folder, an account, a place a machine can
  be reached, or a player's name while names are off.
- Creation Time's weekday is computed from the date. The clock is thirty
  ticks a second.

## Tests

`present-photo-names` checks refused characters, control characters,
trailing dots and spaces, a UTF-8 map kept whole, the `2` and `3` of a
collision, shortening to a path budget, and the date and time.

`present-photo-record` checks the chunk list for a fixed moment, with
names and without, the keywords and their order, the JSON record of
version 1, a player who has no team, a name that is not UTF-8, a replay
file whose name holds a player's name while names are off, and a folder
path that must not reach the output. The list's bytes are pinned.

## Limitations

The module does not choose a folder, write a file, or encode a picture.
The caller decides the path budget, which is the system's longest path
minus nothing: the runtime passes that limit. The caller also decides
whether player names are on, and supplies the version, the tick and
the camera.
