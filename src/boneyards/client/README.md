# Client

The session the consoles drive: login, rooms, battles, profiles and the
files the session places.

## Entry points

`oa/boneyards/rover_session.hpp` and the calls beside it. The library is
`oa-boneyards-client`. Rover is reached through `oa-boneyards-rover` and
`oa-boneyards-rover-detail`.

## State

Reads the player's folder and the lobby replies. Writes the session and the
files it places. A console that is not open does not call it.

## Invariants

Strings that cross into Rover use Rover's own objects. The client does not
include Rover's sources by a relative path.

## Files

A relative path is read from the player's folder when a copy is there, then
from the game's files, then from the expansion, then from the game folder.
`boneyards/server.byd` (any spelling) is read only from the player's folder.
The expansion's copy and the game folder's copy are the 1998 list, so they
are absent and Rover uses the list baked into the build. The player's own
`server.byd` and the saved `serv_sav.byd` still come first. What the session
writes still goes under the player's folder.

## Tests

`boneyards-client-*`. Replays live under `tests/data/`.

## War map

When the dossier arrives the client asks for the current sector and returns.
The session stays logged in. Its idle tick, the one that already pumps
Rover, checks the reply each pass and gives it a few seconds: three seconds
of real time, or ninety steps of the frontend clock when the overlay has
reported that clock. The frame is
not held. The request names the logged-in area and, when the player's
`Boneyards/<area>_map.gwm` is already there, the SHA-256 of that file. A
reply that matches its record count, its SHA-256 and Rover's sector reader
replaces that file, and only then does the session report the dossier.
Silence, an error, a record out of place or a failed check writes nothing, and the
galactic map then loads as before: the player's copy first, then the
expansion, then the game folder. The archive beside the program does not
hold the map. `Map` records that arrive outside this
exchange are ignored. The area id is sent with the request and does not
choose which map comes back.

## Limitations

A live login is a skipped test unless the maintainer names a server. This
code never opens a connection to one on its own.
