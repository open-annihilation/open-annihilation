# Checks

The launch check and the chat check. They run inside the game and are not
compiled into the extension library. The directory is built only when the
game's self-checks are built, the same way the engine's own checks are.

## Entry points

`runtime_boneyards_check.cpp` and `runtime_boneyards_chat_check.cpp`. The
library is `oa-boneyards-checks`.

## State

Reads the session, the consoles and the installed game files. Writes the
check's own report. It does not change a match the player did not launch
from the overlay.

## Invariants

The library links the consoles, the client and the screens. It does not link
the game. Symbols the game provides are resolved when the game links the
extension.

## Tests

The native launch checks (`--check-boneyards-launch host|join`) use this
code. They are registered and are not run on this machine.

## Limitations

A configure that did not build the console application cannot build this
library, because it links that application.
