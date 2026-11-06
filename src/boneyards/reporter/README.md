# Reporter

The match report posted after a battle the overlay launched.

## Entry points

`oa/boneyards/reporter.hpp`. The library is `oa-boneyards-reporter`. The
extension's `boneyards_report.cpp` feeds it.

## State

Reads the match events network play reports. Writes the report the lobby
receives. A battle the overlay did not launch is not reported here.

## Invariants

The report follows network play's extension API, version 2. Text in the
report is UTF-8.

## Tests

`boneyards-reporter`.

## Limitations

It does not open a connection of its own. The client sends what this builds.
