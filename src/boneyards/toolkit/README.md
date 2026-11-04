# Toolkit

Windows, rows, edits, the bitmap faces and the modern chat line.

## Entry points

`oa/boneyards/toolkit.hpp`. The library is `oa-boneyards-toolkit`. The chat
line that calls the engine is `oa-boneyards-toolkit-chat`
(`src/modern_chat_engine.cpp`), which the toolkit library does not compile.

## State

Reads the installed services and the font atlases. Writes the windows'
pixels. The chat line reads the engine's modern faces when modern fonts
are on.

## Invariants

Release options are compile definitions on this library, all off. The chat
engine is not part of the toolkit archive, so a test that wants it links
`oa-boneyards-toolkit-chat`.

## Tests

`boneyards-font-atlases`, `boneyards-modern-chat`, and the window, edit and
resource checks. Names that contained the word "test" drop that word.

## Limitations

The modern chat faces are the engine's extension faces. They are not safe to
use from two threads at once; the engine locks its own calls.
