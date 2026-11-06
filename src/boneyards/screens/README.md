# Screens

The screen models the consoles draw: login, lobby, profile, galaxy, chat
and the message box.

## Entry points

The headers under `include/oa/boneyards/screens/`. The library is
`oa-boneyards-screens`.

## State

Reads the session and the player's profile record. Writes the model the
console paints. It does not talk to the lobby itself.

## Invariants

Text is UTF-8. The registration address baked into the QR code is
`OA_BONEYARDS_REGISTRATION_URL`, which is `https://coreprime.net/register`.

## Tests

`boneyards-screens-*`. Widget checks that need an install are marked as
game-data tests.

## Limitations

The in-game registration form stays behind its release option, which is off.
