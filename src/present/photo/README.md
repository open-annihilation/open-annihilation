# Photo

Names a photo of the battlefield. Later tickets add what a photo records
besides the picture: the details, the sizes, the tiles, the looks and the
depth cues. This module draws nothing and opens no file.

## Entry points

`oa/present/photo/photo_file_name.hpp`, namespace `oa::present::photo`:

- `LocalMoment` is the local date and time the name uses.
- `path_characters` counts a text the way a path counts it: bytes, or
  UTF-16 units on Windows.
- `photo_file_name` builds `Photo <YYYY-MM-DD> <HH.MM.SS> <map>.png`.
  Characters a file system refuses in the map become a space. The map is
  shortened by whole characters so the path fits a budget the caller gives.
  The second photo of a second is numbered `2`, then `3`.

## State

None. Each call reads its arguments and returns a name.

## Invariants

- The date, the time, a collision's number and `.png` are never shortened.
- A UTF-8 character is kept whole or dropped whole.
- Refused characters are replaced with a space, not removed. A trailing run
  of dots and spaces on the map becomes spaces.
- The name holds no folder.

## Tests

`present-photo-names` checks refused characters, control characters,
trailing dots and spaces, a UTF-8 map kept whole, the `2` and `3` of a
collision, shortening to a path budget, and the date and time.

## Limitations

The module does not choose a folder, write a file, or encode a picture.
It does not add the text a photo can carry. The caller decides the path
budget, which is the system's longest path minus nothing: the runtime
passes that limit.
