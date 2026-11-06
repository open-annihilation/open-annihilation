# Rover: the Boneyards lobby client

The client the game talks to for everything on the Boneyards service: login,
chat rooms, battle lists, hosting and joining games, profiles, the Galactic
War feeds and HTTP downloads. The library is `oa-boneyards-rover`, linked
into the game through the module's extension. The 32-bit drop-in library
and its loader are not built.

## Entry points

The game reaches the client through the table in
[`include/oa/rover/interface.h`](include/oa/rover/interface.h). The library
is `oa-boneyards-rover`.

## The interface table

The game reaches the client only through the table in
[`include/oa/rover/interface.h`](include/oa/rover/interface.h): an exported
entry point fills a pointer with an 82-entry table of functions, and the game
calls those slots by number. The header compiles as C11 and C++20; on 32-bit
Windows its layouts are the 3.1c ABI, elsewhere their native equivalents.
[`src/slots.h`](src/slots.h) documents every slot, and each lives in the
source file of its subject (`session.cpp`, `rooms.cpp`, `battles.cpp`,
`games.cpp`, `profiles.cpp`, `lobby.cpp`, `servers.cpp`, `http_fetch.cpp`,
...). Slots the game never calls report failure, so a host sees a lobby that
is unavailable rather than one that half works. Strings, property bags and
record-file readers cross the table as the client's own objects
([`include/oa/rover/strings.h`](include/oa/rover/strings.h), `src/bags.h`,
`src/records.h`), allocated from the client's own heap (`src/heap.h`,
`src/small_block_heap.h`), which the 32-bit library lays out as 3.1c does
because the game may free those buffers itself.

## The links

A session holds three TCP links: the yard master, which lists the servers
and hands the client over, and two boneyards. `src/connect.h` lists the
login stages the game's progress callback sees. A link carries message
records, each one or more bracketed entries of typed fields
(`{[Service<T:key=value>...]...}`, at most 0x400 bytes; `src/record.h`
gives the escaping rules). Every byte on a link passes through the link
cipher (`include/oa/rover/cipher.h`): it is XORed with the low byte of a
32-bit counter that starts at 1 when the link connects and advances by one
per byte, one counter per direction, so a stream deciphers the same however
it was split into sends. The cipher hides the text from casual view and
gives no security; the login password travels as an MD5 digest
(`src/md5.h`).

Three threads do the work. The link thread connects, sends queued records
and parses what arrives; the select thread turns socket readiness into
events for it (`src/sockets.h`); the fetch thread serves HTTP downloads
(`src/http_fetch.h`). Received records wait in a queue until slot 5 pumps
them on the game's thread, where armed handlers (`src/handlers.h`) take each
entry by command name and call the game back.

`get_sector` is an addition of this client, not one of the interface slots.
The area id is sent with the request and does not choose the map. While a
fetch is open the pump keeps `Map` records, in the order sector, then that
sector's planets, then its connectors, and the WarManager replies
`sector_end` and `sector_unchanged`. The same records at any other time are
ignored. Each kept `Map` record is stored with a CRLF ending, and that
ending is part of the file a SHA-256 covers. A map line still has to fit in
one record (0x400 bytes). The client's idle tick bounds the wait. The
dossier callback does not pump or sleep.

## Platform

The core runs on the platform table of
[`include/oa/rover/platform.h`](include/oa/rover/platform.h): sockets,
threads, events, locks and files. `src/platform_posix.cpp` and
`src/platform_win32.cpp` provide it; `oa-game` selects one that finds the
Boneyards files under the game directory.

## Tests

`ctest -R '^rover-'` runs them. `rover-cipher`, `rover-record`,
`rover-strings`, `rover-bags`, `rover-small-block-heap`, `rover-records`,
`rover-sector-map`, `rover-sector-fetch` and `rover-file-server` check the byte-level pieces;
`rover-link` and `rover-link-lifecycle` the links over a scripted in-memory
server; `rover-session-unit`, `rover-lobby` and `rover-interface` the slots;
`rover-platform` the native platform table (threads, events, locks, files,
the clock and loopback sockets) and the table a host selects in its place.
`rover-records-data` and `rover-sector-map-data` read the Boneyards files of
the install `OA_GAME_DIR` names and skip without it. `rover-login-reference`
logs in against a local bycomms server when one was built, and
`rover-login-live` against a live server named by `OA_BY_LIVE_SERVER`,
`OA_BY_USER` and `OA_BY_PASSWORD`; both skip otherwise.

## Limitations

The 32-bit drop-in library and its loader are not built. The session's links and
records are the client's own; a console does not read them except through
the table.
