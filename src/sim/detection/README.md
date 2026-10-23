# Detection

Radar, sonar and line-of-sight contacts, and the sightings target searches
read. The routines here are the per-unit steps. The match (`src/sim/match-runtime`)
decides when a scan runs and which player is the viewer.

The viewpoint's contact scan writes `Unit.flags`. A player's picture is the
same facts in a byte the caller owns, so a seat can keep its own radar
without moving the flags the simulation reads.

## Entry points

The viewpoint scan, for each unit it walks:

- `stamp_contact` marks sonar at or below sea level inside the sonar reach,
  and radar when the unit's top is at or above sea level inside the radar
  reach. Typeless units, the viewpoint's own units and stealth types are
  skipped.
- `jam_radar` and `jam_sonar` clear that contact and mark the unit jammed.
- `expose_cloaker` holds a cloak off while an enemy the owner sees stands
  inside mincloakdistance.
- `shares_radar` is the first pass's test: the owner's alliance row names
  the viewer and the owner's player-info role shares radar.

Sightings, rebuilt every 30 ticks:

- `sightings_due`, `clear_sightings` and `file_sighting` fill the seen list,
  the radar list and the player's own repair pads. `finished_unit` and
  `sighting_candidate` are the gates on that walk.
- `diff_sighted` compares two slot-ordered lists of known units. An entry
  matches only when its slot and its generation both match, so a slot a new
  unit has taken is one lost and one gained. It allocates nothing.
- `pads_in_reach` and `pad_in_reach` are the pads a damaged aircraft may
  choose. `sighted_within` is the cloak distance test.

A per-player picture uses the same tests and writes a byte instead of
`Unit.flags`:

- `stamp_picture` is `stamp_contact` with the picture's player in place of
  the viewpoint.
- `jam_picture_radar` and `jam_picture_sonar` are the jam steps.
- `seat_jammer_jams` is the jammer rule for one pass: every jammer, unless
  allied jammers are ignored and the pass's player owns or allies the
  jammer's owner, one way by that player's row.
- `created_picture_bits` is how a new unit starts: sonar when its owner is
  the picture's player, otherwise clear.

`squared_distance_high` is the horizontal distance the stamp and the pad
reach share.

## State

None. Each function works on the records it is given. The caller owns the
picture bytes and the sighting lists.

## Invariants

The picture functions never write `Unit`. They do not draw from a random
stream, and they do not expose a cloaker.

## Tests

`sim-detection` (`tests/detection_test.cpp`) covers distance, the stamp
grid, jammers, shared radar, sightings, repair pads and the picture
functions: `stamp_picture` against `stamp_contact` when the viewer is the
viewpoint, another viewer's own units left unstamped, the jammer rule's
truth table and `created_picture_bits`. `diff_sighted` is checked with
empty lists, gains, losses, interleaved slots, a reused slot's generation,
identical lists, and a list entry that carries a field past the slot and
the generation.

## Limitations

A picture is not saved, recorded or hashed. After a load it is rebuilt from
`created_picture_bits`. Jammed bits on a player's own units are kept, as
the flags keep them: the first pass sets radar and sonar without clearing
jammed.
