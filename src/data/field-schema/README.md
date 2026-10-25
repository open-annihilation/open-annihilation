# Field schema

`oa-data-field-schema` types every key the unit, weapon and movement-class
loaders read. Each key has a type, the width the loader stores, the line
that reads it, the value used when a file leaves the key out, and the
values that are refused or only warned about. Unknown keys stay free text,
so a mod's own tags still work. The table is a `constexpr` array in
`src/fields.cpp`, the same in every build.

The weapon flag table also reads `shellweapon` (`weapons.cpp:49`). The
Context's comma list of weapon names omits it; the schema has 73 weapon
keys, that one included.

## Entry points

`oa/data/field_schema.hpp` (namespace `oa::data::field_schema`):

- `fields` returns the specs of one record kind, in table order.
- `find_field` finds a key, ignoring ASCII case. A `[DAMAGE]` unit name is
  any key other than `default`.
- `check_text` reads a written value the way the loader would and returns
  ok, warn or refuse, with a sentence that names the key, the value, the
  limit and the reason. `check_number` does the same for a number already
  parsed. A key the schema does not hold is ok.
- `read_value` returns the value a script sees: an integer after the width
  cast, a fixed or decimal number unscaled, a flag's low bit, or the text.
- `default_value` returns what a record holds for a key the block leaves
  out, including a default derived from another key. A key the schema does
  not hold is absent. A `[DAMAGE]` unit entry left out is 0.

## Types and storages

`FieldType` is how the text is read. `integer` is `get_int`. `fixed` is
`get_fixed` (16.16). `decimal` is `get_double`. `flag` is an integer whose
low bit is kept (`with_bit`, `unit_def_loader.cpp:26`). `text` is kept as
written. `name_ref` names another record (weapons, corpse, object, model,
sounds, explosion art, movement class, sound category). `token_list` is
category words. `yard_map` is yard-map cells.

`Storage` is how wide the result is. `none` is a flag bit or text. `int8`,
`uint8`, `int16`, `uint16`, `int32` and `uint32` are the integer widths. `float32`
keeps a decimal in a float. `fixed32` keeps a 16.16 value, or a weapon
speed scaled into 32 bits. `ticks16` and `ticks32` are seconds times 30
(`weapon_ticks_per_second`, `weapons.hpp:52`).

Where a name is read on two records, the record kind tells them apart.
Unit `turnrate` is an integer stored in 16 bits (`unit_def_loader.cpp:237`).
Weapon `turnrate` is a decimal, divided by 30 and then stored in 16 bits
(`weapons.cpp:341`).

## Defaults that are not zero or empty

Every other key is 0, false or empty. These are the getters' values, before
the movement loader's later slope clamps (`move_classes.cpp:53`).

| Key | Record | Left-out value | Source |
| --- | --- | --- | --- |
| `bankscale` | unit | 1.0 (`fixed_unit_scale`) | `unit_def_loader.cpp:230`, `unit_def_loader.hpp:24` |
| `damagemodifier` | unit | 1.0 (`fixed_unit_scale`) | `unit_def_loader.cpp:232`, `unit_def_loader.hpp:24` |
| `moverate1` | unit | twice the 16.16 value of `maxvelocity` (0 when that key is left out) | `unit_def_loader.cpp:233` |
| `moverate2` | unit | the same as `moverate1` | `unit_def_loader.cpp:236` |
| `standingmoveorder` | unit | 2 (`standing_order_default`) | `unit_def_loader.cpp:266`, `unit_def_loader.hpp:23` |
| `standingfireorder` | unit | 2 | `unit_def_loader.cpp:270`, `unit_def_loader.hpp:23` |
| `cloakcostmoving` | unit | the whole number `cloakcost` reads as | `unit_def_loader.cpp:283` |
| `wpri_badtargetcategory` | unit | `none` | `unit_def_loader.cpp:205`, `unit_def_loader.hpp:25` |
| `wsec_badtargetcategory` | unit | `none` | `unit_def_loader.cpp:209` |
| `wspe_badtargetcategory` | unit | `none` | `unit_def_loader.cpp:213` |
| `nochasecategory` | unit | `none` | `unit_def_loader.cpp:217` |
| `selfdestructcountdown` | unit | 5 (`self_destruct_countdown_default`) | `unit_def_loader.cpp:418`, `unit_def_loader.hpp:22` |
| `objectname` | unit | the text of `unitname` | `unit_def_loader.cpp:220`, `unit_header.cpp:171` |
| `mincloakdistance` | unit | 80 when `cloakcost` reads above 0, else 0 (`min_cloak_distance_default`) | `unit_def_loader.cpp:483`, `unit_def_loader.hpp:21` |
| `copyright` | unit | `(no Copyright line)` (`unit_copyright_default`) | `unit_header.cpp:200`, `unit_header.hpp:82` |
| `maxwaterdepth` | unit and movement class | 10000 | `move_classes.cpp:33` (unit via `unit_def_loader.cpp:447`) |
| `minwaterdepth` | unit and movement class | −10000 | `move_classes.cpp:36` |
| `maxslope` | unit and movement class | 255 | `move_classes.cpp:39` |
| `maxwaterslope` | unit and movement class | 255 | `move_classes.cpp:45` |
| `badslope` | unit and movement class | the low byte of `maxslope` as read, halved (127 when `maxslope` is left out too) | `move_classes.cpp:42` |
| `badwaterslope` | unit and movement class | the same of `maxwaterslope` | `move_classes.cpp:49` |
| `id` | weapon | −1, which the loader then refuses | `weapons.cpp:310` |
| `range` | weapon | 32767 (`weapon_default_range`) | `weapons.cpp:326`, `weapons.hpp:59` |
| `minbarrelangle` | weapon | −11.25 degrees (`weapon_default_min_barrel_angle`) | `weapons.cpp:354`, `weapons.hpp:56` |

A `[DAMAGE]` entry left out is 0 (`weapons.cpp:403` for `default`,
`weapons.cpp:410` for a unit name).

`DefaultRule` says how a derived default is found. `constant` uses the
spec's own number or text. `twice_fixed` shifts the 16.16 value of
`default_from` left one place (`moverate1` and `moverate2` from
`maxvelocity`). `whole_of` takes what `default_from` reads as a whole
number (`cloakcostmoving` from `cloakcost`). `half_low_byte` takes the low
byte of `default_from`, using that key's own default when it is left out
too, and halves it (`badslope`, `badwaterslope`). `text_of` copies the text
of `default_from` (`objectname` from `unitname`). `cloaker_distance` is 80
when `default_from` (`cloakcost`) reads above 0, else 0. `mincloakdistance`
does not store 80 as its own constant.

## Refusals

A refusal is a value that can crash a match or drop a record. Each reason
below is the `why` stored on that key.

- A non-finite number in any numeric field. `parse_double` can return an
  infinity from `1e999` (`tdf.cpp`), and truncating that infinity is not
  defined. An integer text `1e999` is the integer 1 (`parse_int` stops at
  the first character that is not a digit, `tdf.cpp:450`), so the number is
  not refused. The letters after the 1 are a warning.
- `footprintx` and `footprintz` below 1 or above 1024, on a unit
  (`move_classes.cpp:31` and `move_classes.cpp:32`, reached from
  `unit_def_loader.cpp:447`) and on a movement class (`move_classes.cpp:31`
  and `move_classes.cpp:32`). A unit's bounds
  are the footprint times 0x100000 and wrap at 2048 cells
  (`unit_def_loader.cpp:23`, `unit_def_loader.cpp:104`,
  `unit_def_loader.cpp:108`, applied at `unit_def_loader.cpp:470`). Yard maps
  hold at most 1,048,576 cells (`unit_def_loader.cpp:67`). 1 and 1024 are
  kept.
- `transportsize` below 0 (`unit_def_loader.cpp:239`) and
  `transportcapacity` below 0 (`unit_def_loader.cpp:240`). A negative count
  breaks boarding.
- Weapon `id` past the weapon types the caller numbers (`weapons.cpp:310`),
  255 unless that caller read a profile's `limits.weapon-types`. The loader
  drops the section, and every unit armed with it loses the weapon. 0 and the
  last numbered id are kept. The left-out value −1 is refused.
- `burst` below 0 (`weapons.cpp:342`).
- On a key the schema holds, text that contains `;`, a line break, a zero
  byte, `//` or `/*`. That text cannot be written back as one property.
  The same markers on an unknown key, such as `tedclass`, are ok.
- A numeric field whose text holds no number (`abc`, an empty value, or
  only spaces). The loader would read 0, and `read_value` still reports
  that 0, but the check refuses it: `footprintx: "abc" is not a number`.
  Integer and flag fields read digits only (`parse_int`). Fixed and decimal
  fields read the same grammar as `parse_double`, including an exponent
  (`1e3` is 1000). `0x10` is the digit 0; the loader does not read it as 16.

## Warnings

A warning is kept. Strange values and the wraps below are not refusals.
A non-whole number in an integer field is not a warning: the loader keeps
the whole part (`workertime` 67.5 and `buildcostmetal` 412.5 stay ok).

A number with other text after it is a warning that names the text the
loader ignores (`12abc` is 12 and ignores `abc`). On an integer or a flag
the fractional part is not that text: the loader keeps the whole part, so
`workertime` `67.5` stays ok. Anything after the fraction is ignored
(`67.5abc` ignores `abc`). `1e3` on an integer or a flag is 1 and ignores
`e3`; on a fixed or decimal field it is 1000. The warning is not added when
that number is itself refused (`0x10` on `footprintx` is 0, which is
refused). Shipped 3.1c files may hold a leading number and then other text;
refusing those would drop records the game already loads.

Named thresholds:

- `maxdamage` above 32767 or below 1 (`unit_def_loader.cpp:255`). Health is
  taken from the low 16 bits (`spawn.cpp:143`). 32767 and 1 are kept. The
  stored width is 32 bits, so this warning is the health cast, not that
  width.
- `buildtime` above 600000 (`unit_def_loader.cpp:252`). 600000 is kept.
  Shipped units sit above it, so the warning is information.
- Weapon `range` above 32000 (`weapons.cpp:326`). 32000 is kept. The
  left-out value 32767 warns.
- `[DAMAGE]` `default` above 32767 (`weapons.cpp:403`, stored in 16 bits)
  and a `[DAMAGE]` unit name above 32767 (`weapons.cpp:410`, stored in 32
  bits). 32767 is kept.
- Every flag other than 0 and 1. The loader keeps only the low bit, so 2
  reads as off. Unit flags, each with the line that reads it:
  `activatewhenbuilt` (`unit_def_loader.cpp:315`),
  `amphibious` (`unit_def_loader.cpp:324`),
  `antiweapons` (`unit_def_loader.cpp:337`),
  `armoredstate` (`unit_def_loader.cpp:311`),
  `builder` (`unit_def_loader.cpp:281`),
  `canattack` (`unit_def_loader.cpp:359`),
  `cancapture` (`unit_def_loader.cpp:389`),
  `candgun` (`unit_def_loader.cpp:393`),
  `canfly` (`unit_def_loader.cpp:317`),
  `canguard` (`unit_def_loader.cpp:362`),
  `canhover` (`unit_def_loader.cpp:319`),
  `canload` (`unit_def_loader.cpp:371`),
  `canmove` (`unit_def_loader.cpp:368`),
  `canpatrol` (`unit_def_loader.cpp:365`),
  `canreclamate` (`unit_def_loader.cpp:376`),
  `canresurrect` (`unit_def_loader.cpp:381`),
  `canstop` (`unit_def_loader.cpp:356`),
  `cantbetransported` (`unit_def_loader.cpp:415`),
  `commander` (`unit_def_loader.cpp:411`),
  `digger` (`unit_def_loader.cpp:339`),
  `downloadable` (`unit_def_loader.cpp:279`),
  `firestandorders` (`unit_def_loader.cpp:353`),
  `floater` (`unit_def_loader.cpp:321`),
  `hidedamage` (`unit_def_loader.cpp:306`),
  `hoverattack` (`unit_def_loader.cpp:334`),
  `immunetoparalyzer` (`unit_def_loader.cpp:331`),
  `init_cloaked` (`unit_def_loader.cpp:276`),
  `isairbase` (`unit_def_loader.cpp:297`),
  `isfeature` (`unit_def_loader.cpp:325`),
  `istargetingupgrade` (`unit_def_loader.cpp:301`),
  `kamikaze` (`unit_def_loader.cpp:399`),
  `mobilestandorders` (`unit_def_loader.cpp:348`),
  `norestrict` (`unit_def_loader.cpp:403`, `unit_header.cpp:179`),
  `noshadow` (`unit_def_loader.cpp:327`),
  `onoffable` (`unit_def_loader.cpp:343`),
  `shootme` (`unit_def_loader.cpp:308`),
  `showplayername` (`unit_def_loader.cpp:408`),
  `stealth` (`unit_def_loader.cpp:282`),
  `teleporter` (`unit_def_loader.cpp:304`),
  `upright` (`unit_def_loader.cpp:320`),
  `wacky` (`unit_header.cpp:183`),
  `zbuffer` (`unit_def_loader.cpp:295`).
  Weapon flags: `ballistic` (`weapons.cpp:31`),
  `beamweapon` (`weapons.cpp:48`), `burnblow` (`weapons.cpp:41`),
  `commandfire` (`weapons.cpp:44`), `cruise` (`weapons.cpp:43`),
  `dropped` (`weapons.cpp:50`), `endsmoke` (`weapons.cpp:56`),
  `groundbounce` (`weapons.cpp:33`), `guidance` (`weapons.cpp:28`),
  `interceptor` (`weapons.cpp:47`), `lineofsight` (`weapons.cpp:30`),
  `meteor` (`weapons.cpp:52`), `noautorange` (`weapons.cpp:340`),
  `noexplode` (`weapons.cpp:40`), `noradar` (`weapons.cpp:53`),
  `paralyzer` (`weapons.cpp:54`), `propeller` (`weapons.cpp:39`),
  `selfprop` (`weapons.cpp:38`), `shellweapon` (`weapons.cpp:49`),
  `smoketrail` (`weapons.cpp:36`), `soundtrigger` (`weapons.cpp:27`),
  `startsmoke` (`weapons.cpp:55`), `stockpile` (`weapons.cpp:45`),
  `targetable` (`weapons.cpp:46`), `toairweapon` (`weapons.cpp:35`),
  `tracks` (`weapons.cpp:29`), `turret` (`weapons.cpp:37`),
  `twophase` (`weapons.cpp:42`), `unitsonly` (`weapons.cpp:32`),
  `vlaunch` (`weapons.cpp:51`), `waterweapon` (`weapons.cpp:34`).
- `bmcode` other than 0 and 1 (`unit_def_loader.cpp:263`). It is also stored
  in 8 bits, so a value outside −128 to 127 warns for that wrap as well.

Wraps, by the width the loader keeps. `parse_int` wraps modulo 2^32
(`tdf.cpp:450`). `get_fixed` keeps the low 32 bits of the value times 65536
(`tdf.cpp:539`), so a 16.16 value beyond ±32767.99998 wraps.

- Signed 8 bits, outside −128 to 127: unit `makesmetal`
  (`unit_def_loader.cpp:246`), `waterline` (`unit_def_loader.cpp:238`);
  weapon `firestarter` (`weapons.cpp:358`), `rendertype` (`weapons.cpp:359`).
  `transportsize` and `transportcapacity` refuse below 0 and warn above 127.
- Unsigned 8 bits, outside 0 to 255: `maxslope` (`move_classes.cpp:39`) and
  `maxwaterslope` (`move_classes.cpp:45`), and `badslope`
  (`move_classes.cpp:42`) and `badwaterslope` (`move_classes.cpp:49`), on a
  unit and on a movement class. Weapon `color` (`weapons.cpp:360`, read
  back at `combat_state.cpp:108`) and `color2` (`weapons.cpp:361`, read back
  at `combat_state.cpp:109`).
- Unsigned 16 bits, outside 0 to 65535: unit `buildangle`
  (`unit_def_loader.cpp:291`, read back at `unit-spawn/src/runtime.cpp:90`); weapon
  `tolerance` (`weapons.cpp:365`, read back at `combat_state.cpp:100`) and
  `pitchtolerance` (`weapons.cpp:366`, read back at `combat_state.cpp:101`).
- Signed 16 bits, outside −32768 to 32767: unit `attackrunlength`
  (`unit_def_loader.cpp:397`), `builddistance` (`unit_def_loader.cpp:292`), `cruisealt`
  (`unit_def_loader.cpp:294`), `healtime` (`unit_def_loader.cpp:254`),
  `kamikazedistance` (`unit_def_loader.cpp:400`), `maneuverleashlength`
  (`unit_def_loader.cpp:395`), `mincloakdistance`
  (`unit_def_loader.cpp:289`), `radardistance` (`unit_def_loader.cpp:257`),
  `radardistancejam` (`unit_def_loader.cpp:259`), `sightdistance`
  (`unit_def_loader.cpp:256`), `sonardistance` (`unit_def_loader.cpp:258`),
  `sonardistancejam` (`unit_def_loader.cpp:261`), `sortbias`
  (`unit_def_loader.cpp:293`), `turnrate` (`unit_def_loader.cpp:237`),
  `workertime` (`unit_def_loader.cpp:253`), and the movement depths
  `maxwaterdepth` (`move_classes.cpp:33`) and `minwaterdepth`
  (`move_classes.cpp:36`). Weapon `accuracy` (`weapons.cpp:364`),
  `areaofeffect` (`weapons.cpp:335`) and `sprayangle` (`weapons.cpp:345`).
  `burst` refuses below 0 and warns above 32767. Footprints refuse outside
  1 to 1024 before this wrap applies.
- Signed 32 bits, outside ±2147483647: unit `buildcostenergy`
  (`unit_def_loader.cpp:225` and `unit_header.cpp:175`), `buildcostmetal`
  (`unit_def_loader.cpp:226` and `unit_header.cpp:176`), `cloakcost`
  (`unit_def_loader.cpp:283`), `cloakcostmoving` (`unit_def_loader.cpp:287`),
  `selfdestructcountdown` (`unit_def_loader.cpp:418`), `standingfireorder`
  (`unit_def_loader.cpp:270`), `standingmoveorder`
  (`unit_def_loader.cpp:266`); weapon `coverage` (`weapons.cpp:327`),
  `shakemagnitude` (`weapons.cpp:368`). `buildtime` warns above 600000
  first. Weapon `id` is refused past the weapon types the caller numbers
  (255 unless that caller read a profile). `range` warns above 32000
  first. `maxdamage` warns outside 1 to 32767 before its unsigned 32-bit
  wrap (0 to 4294967295).
- 16.16, outside ±32767.99998: unit `acceleration`
  (`unit_def_loader.cpp:229`), `bankscale` (`unit_def_loader.cpp:230`),
  `brakerate` (`unit_def_loader.cpp:228`), `damagemodifier`
  (`unit_def_loader.cpp:232`), `maxvelocity` (`unit_def_loader.cpp:227`),
  `moverate1` (`unit_def_loader.cpp:235`), `moverate2`
  (`unit_def_loader.cpp:236`), `pitchscale` (`unit_def_loader.cpp:231`).
- Ticks in 16 bits, when the whole number of seconds times 30 falls outside
  −32768 to 32767 (about 1092 seconds): weapon `burstrate`
  (`weapons.cpp:344`), `duration` (`weapons.cpp:346`), `flighttime`
  (`weapons.cpp:352`), `holdtime` (`weapons.cpp:353`), `randomdecay`
  (`weapons.cpp:348`), `reloadtime` (`weapons.cpp:329`), `smokedelay`
  (`weapons.cpp:350`), `weapontimer` (`weapons.cpp:339`). `reloadtime` 1100
  is 33000 ticks and warns. Ticks in 32 bits warn only outside the signed
  32-bit tick count: `shakeduration` (`weapons.cpp:370`).
- Weapon speeds are scaled before the width check, not compared as a raw
  16.16 value. `weaponvelocity` (`weapons.cpp:321`) and `startvelocity`
  (`weapons.cpp:323`) are multiplied by 65536/30 (`weapons.hpp:53`) and
  warned when that product falls outside signed 32 bits (near ±983040).
  `weaponacceleration` (`weapons.cpp:325`) uses 65536/900 (`weapons.hpp:54`),
  which wraps near ±29491200. Weapon `turnrate` (`weapons.cpp:341`) is
  divided by 30 (`weapons.hpp:55`) and warned when that whole number falls
  outside signed 16 bits, at 983040 and below about −983070.

## Adding a key

When a loader learns a key, add one row to the `constexpr` table in
`src/fields.cpp`, in that record's group, in key order, with its type,
storage, bounds, default and the loader file and line. Add the same name
to the list in `tests/key_lists.inc`. Map the stored member in
`tests/defaults_test.cpp`, or list the key there with the reason it has no
member and keep its default at 0 or empty. Add any default that is not 0
or empty, and any refusal or warning, to this file.

## State

None. The table is fixed for the process. Pointers from `find_field` and
`fields` address it.

## Invariants

- The table holds 120 unit keys, 73 weapon keys, two `[DAMAGE]` kinds
  (`default` and any other name) and 9 movement-class keys.
- A key is matched without regard to ASCII case. The stored spelling is
  lower case.
- An infinite bound is no bound. A constant default of 0 or empty text is
  omitted from the row.
- `check_text` and `check_number` agree on a finite number the text is, in
  full. Integer and flag checks use the whole part. Text after that number
  is a warning, unless the number itself is refused.
- `default_value` returns the left-out value even when the key is present
  on the block. A derived rule reads the other keys.

## Tests

`data-field-schema` checks the key lists against this table, the sample
types and storages, the refusals and warnings above (including values that
must stay ok), case-insensitive lookup, `read_value` and `default_value`.

`data-field-schema-data` (run with `--data`) checks every key of every
`units/*.fbi` `[UNITINFO]`, every section of `weapons/*.tdf` including
`[DAMAGE]`, and every section of `gamedata/moveinfo.tdf`. A refusal fails
the run. Warnings are printed once each and do not fail it.

`data-field-schema-defaults` loads a made-up unit `RIDGEBARE` whose
`[UNITINFO]` holds only `UnitName=RIDGEBARE;`, a second unit that sets
`MaxVelocity`, `CloakCost`, `MaxSlope` and `MaxWaterSlope`, a weapon
section `RIDGE_BARE`, and empty and partial movement blocks. Each stored
member is compared with `default_value` on that same block. Keys the
loaders turn into an index (`category`, `defaultmissiontype`,
`soundcategory`, `version`) are listed in the test; their schema default
is 0 or empty.

## Limitations

The profile-bound data keys in
`src/data/defs/include/oa/data/defs/rule_keys.hpp` are not typed here.
Feature, side and OTA keys are not typed yet. This module does not apply
the checks to rule results or to hook writes, and it does not check one
record against another. The library does not depend on the script host,
the rules engine or the asset store. The movement defaults here are the
getters' values; they do not include the slope clamps at
`move_classes.cpp:53`.
