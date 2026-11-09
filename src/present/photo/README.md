# Photo

Names a photo of the battlefield, builds the text the file carries
about the map, the players and the moment, decides which sizes the
battlefield can offer, and filters a picture for a look. Later tickets
add the tiles and the depth cues. The module opens no file and no font:
a caption is drawn with a face stack the caller passes.

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

`oa/present/photo/photo_sizes.hpp`, same namespace:

- `plan_sizes` offers Screen, 2×, 3× and 4×, and smooth edges at Off, 2×
  and 4×, from the battlefield, the render mode's pixel limit, the memory
  still free and whether the match is shared.
- A shared match offers Screen and smooth edges at 2× only.
- `box_reduce` turns a smoother draw into the picture, one whole block a
  pixel, rounded half up.

`oa/present/photo/looks.hpp`, same namespace:

- `apply_look` runs a colour grade, a vignette, grain and a frame over
  packed RGB or RGBA. It touches only the colour channels. An idle look
  (None, vignette 0, grain 0, no frame) returns without reading the pool
  or writing a byte.
- The grade is one of None, 1997, Dusk, Arctic, Noir and Bleach: three
  tone curves, a colour matrix and a saturation, kept as constants.
- Grain is seeded by the caller. The same seed writes the same grain.
- `photo_grain_size` scales the panel's grain size by a photo's size
  factor, so a photo two to four times the battlefield draws grain the
  size the preview shows.
- The work runs in bands of 32 rows on `oa::platform::job_pool::run_bands`.
  The bands follow the rows, so the picture does not depend on how many
  threads the pool has. A null pool runs on the caller.
- A caption bar is drawn with the interface face on the stack the caller
  passes. A null stack still paints the bar, with no letters.

## State

None. Each call reads its arguments and returns a name or writes the
picture it was given.

## Invariants

- An idle look (None, vignette 0, grain 0, no frame) writes no pixel and
  runs no band.
- The same picture, look and seed write the same bytes on any number of
  threads, and with no pool. The bands follow the rows.
- A caption is drawn on the caller's thread, before the bands. On a short
  picture the face shrinks, and a line that still does not fit is clipped
  to the bar.
- Grain size is a count of pixels of the picture `apply_look` is given.
  The caller passes `photo_grain_size` for a photo larger than the
  battlefield, so its grain covers the same part of the map as the
  preview's. Box-reduced by its factor, a whole-pixel picture with the
  scaled grain is the preview's look.
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

`present-photo-looks` checks each grade, the vignette, the grain and each
frame on a fixed 64×36 picture. The digest of each result is pinned. One,
two and eight threads, and no pool, write the same bytes. None and an
amount of 0 leave the picture unchanged. The same seed writes the same
grain. The picture drawn two, three and four times larger in whole-pixel
blocks, with a grade and the grain size scaled by `photo_grain_size`,
box-reduces to the look on the picture itself; with the grain left at the
panel's size it does not.

`present-photo-record` checks the chunk list for a fixed moment, with
names and without, the keywords and their order, the JSON record of
version 1, a player who has no team, a name that is not UTF-8, a replay
file whose name holds a player's name while names are off, and a folder
path that must not reach the output. The list's bytes are pinned.

`present-photo-sizes` checks the sizes offered for several battlefields,
pixel limits, memory figures and a shared match, and an exact box
reduction of a known picture.

## Limitations

The module does not choose a folder, write a file, or encode a picture.
The caller decides the path budget, which is the system's longest path
minus nothing: the runtime passes that limit. The caller also decides
whether player names are on, and supplies the version, the tick and
the camera.

The look does not open a font. The caller passes the face stack, and one
thread at a time uses it. Grain size is in pixels of the picture it is
given, not in a fraction of that picture, so the caller scales it for a
photo saved larger than the preview (`photo_grain_size`). A caption that does not fit the bar is
clipped, not wrapped.
