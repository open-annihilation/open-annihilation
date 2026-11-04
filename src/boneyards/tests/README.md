# Tests

Install paths shared by the module's tests, and the check that
`coreprime.ccx` matches the archive the build writes.

## Entry points

`oa/boneyards_test/installs.hpp`. The interface library is
`oa-boneyards-tests-installs`. `coreprime/coreprime_ccx.cpp` is the
`coreprime-ccx` test, built from `cmake/OaBoneyards.cmake`.

## State

Reads `OA_GAME_DIR` and `OA_LAUNCHER_GAME_DIR`. Writes nothing.

## Invariants

The header path stays `oa/boneyards_test/installs.hpp`. A missing install
skips the test that needs it.

## Tests

`coreprime-ccx`, and every test that links this library.

## Limitations

This directory is not a library the game links.
