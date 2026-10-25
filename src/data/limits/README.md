# Capacities a mod may raise

`oa-data-limits` holds `Limits`, one plain record of every capacity of the
engine that a mod may raise, each field defaulting to its 3.1c value. A
default `Limits` plays the game exactly as 3.1c does; a mod's profile fills
the record before any game data loads, and the engine hands each part to the
code that sizes its tables from it.

| Field | 3.1c value | What reads it |
| --- | --- | --- |
| `units_per_player` | 250, chosen from 20 to 500; 101 on the unit-limit screen without a stored limit | the session's clamp, the Open Annihilation settings' range and default, the match's unit slots |
| `unit_types.bitset_bits` | 512 | the same-type selection's type bitset (`sim::selection`) |
| `category_masks.types` | 512 | the category registry's mask width (`data::defs`), and from it the target-category masks |
| `effects` | 400 emitters a layer before eviction; 1,000 in the shared pool | the effect world's layers and pool (`sim::effect_particles`) |
| `path_search.nodes` | 1,333 path nodes a tick | the path search's tick credit (`sim::ground_orders`), times the player's Pathfinding cycles |
| `build_lists` | 30 CANBUILD entries, the rest dropped | the build lists of the unit table (`data::defs`) and of the computer players (`sim::ai`) |
| `model_composite` | 600 by 600, growing to fit a larger model | the model renderer's composite buffer (`present::model`) |
| `weapon_types.types` | 256, ids 0 to 255 | kept for the profile; nothing reads it yet |
| `sides.count` | 5, SIDE0 to SIDE4 | kept for the profile; nothing reads it yet |

The Open Annihilation settings already offer unit limits up to 1,500 per
player for every game; a profile's `units_per_player.maximum` raises that
highest stop when it is larger, and its `default_limit` replaces 250 as the
limit a player starts with.

`check_limits` checks every value against the range a profile may name, the
`highest_*` and `lowest_*` constants, and returns the first one out of range
as a `LimitsError`. The highest values are the largest each capacity's
storage holds: 6,553 units per player (65,531 unit slots, within 16-bit unit
ids), 65,536 type ids, weapon types from 256 to 65,536, sides from 5 to 32,
a million emitters a layer and ten million in the pool, ten million path
nodes a tick, 1,024 build-list entries and an 8,192 pixel composite side.

`unit_types.partial_widening` and `units_per_player.limits_screen_fallback`
are kept for the profile; nothing reads them yet.

The test, `data-limits`, checks the 3.1c defaults, that the highest values
pass, and that each value just outside its range is refused.
