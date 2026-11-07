# Colour vision

Chooses the ten player colours for a colour-vision deficiency, and says when a
shape should sit on each colour. It is presentation only. A mode never tints
the whole picture, and it never changes a saved game, a recording or a byte
another machine receives. Another player's machine uses that player's own
preference.

With the preference off, nothing here is applied: units, logos, minimap blips
and dots keep the game's own colours.

## Entry points

Namespace `oa::present::colour_vision`, header
`include/oa/present/colour_vision/colour_vision.hpp`.

- `parse_mode` and `mode_name` read and write
  `open-annihilation.access.colour-vision`: `off`, `protanopia`,
  `deuteranopia`, `tritanopia` or `achromatopsia`. Any other text is `off`.
- `shape_cues_preference` reads `open-annihilation.access.shape-cues`. A
  whole number above 0 is on, and anything else is off. `shape_cues_in_effect`
  is on when that preference is on, and always for achromatopsia. Both apply
  together, on the menus and in a match.
- `set_active`, `active_mode` and `shape_cues_active` are the process-wide
  choice. The default is off, with no shapes.
- `anchor_srgb` and `stock_anchor_index` are each mode's ten anchor colours.
  Achromatopsia repeats four lightness levels across the ten players.
  `semantic_colours` is the health, war-map, resource, chat and kit colours
  of a mode, including the background each group is drawn on. `off` is the
  game's own set.
- `match_index` finds the nearest palette entry the way the palette remap
  does (the smallest sum of absolute channel differences, the first index on
  a tie). `paint_mark_slots` writes a mode's good, mid and bad indices into
  UI slots 10, 14, 12 and 4, matched in the palette in use. Off writes
  nothing. Produced and consumed resource figures are not those slots.
  `match_resource_figures`, `resource_produced_index` and
  `resource_consumed_index` keep them for the active mode, matched the same
  way, and the HUD asks for them only while a mode is on. With the mode off
  the figures stay on slots 10 and 12. `match_player_dots` and `player_dot`
  are the dot indices for the active mode. With the mode off, callers keep
  the game's own dot table.
- `simulate` is the colour as that eye sees it, and `ciede2000` is the
  distance. The three dichromacies use Machado, Oliveira and Fernandes
  (2009) at severity 1.0 on linear sRGB. Achromatopsia is CIE relative
  luminance Y. `simulate_vienot` is Viénot, Brettel and Mollon (1999) for
  protanopia and deuteranopia, and `simulate_brettel` is Brettel, Viénot and
  Mollon (1997) for tritanopia.
- `team_shades` and `team_maps` build the index map for each player colour.
  Each old dot index goes to the anchor, and the other old shades keep their
  step in lightness from that dot. `paint_team_archive` applies the map to
  the ten-frame logo sequences, or writes the originals back when the mode
  is off. `blip_maps` sends only a minimap blip's body to the anchor.
  `prepare_original`, `remap_row_rle` and `blip_bodies` handle the blips,
  which are stored row-compressed.

## State

One process-wide choice: the mode, whether the shape preference is on, the
ten dot indices, the produced and consumed figure indices, the blip bodies
last read, the team maps last built, and the original bytes of any frame a
mode has recoloured. It starts as off, with no originals held, so the first
paint of an off preference writes nothing.

## Invariants

- Off does not rewrite a frame that has not been recoloured.
- A second apply remaps from the saved originals, never from indices a mode
  already wrote.
- Each player colour's map is applied only to that colour's frame.
- A frame's transparency index is left as it is.
- Each old dot index is sent to the anchor, so the anchor is the middle of
  that army's new ramp. The other old shades move by the same step in L*.
- A minimap blip's map sends its body index to the anchor and changes no
  other, so the outline the frames share stays the game's own index. When
  the body already is the anchor, the map changes nothing.
- Equal distance keeps the lower palette index.
- With the mode off, `paint_mark_slots` writes no slot. Resource figure
  indices are not written into those slots, a save or the simulation.
- No anchor is one of the 32 palette indices the stock maps' terrain draws
  most often, and none is index 171. Each protanopia, deuteranopia and
  tritanopia anchor is at L* 15 or lighter. The least gap from those indices,
  under both eyes, is as large as the palette then allows: protanopia 5.59
  (Machado) and 5.75 (Viénot), deuteranopia 4.82 and 4.80, tritanopia 5.42
  (Machado) and 4.65 (Brettel). Achromatopsia keeps four levels, one below
  L* 15, and a gap of about 1.6.
- Each dichromat mode's ten colours stay at least 20 apart under Machado
  and under Viénot (protanopia and deuteranopia) or Brettel (tritanopia).
  The least pairs are 20.07 and 20.93 for protanopia, 21.11 and 20.80 for
  deuteranopia, and 20.59 and 20.15 for tritanopia. Achromatopsia's four
  levels stay at least 20 apart under luminance Y.
- No mode is written into a save, a recording or a byte another machine
  receives. A whiteboard mark records the game's own dot index.

## Tests

`tests/colour_vision_test.cpp` (`present-colour-vision`) checks the distance
against Sharma, Wu and Dalal's first pair, that each dichromat mode's anchors
stay at least 20 apart under Machado and under Viénot or Brettel, that the
achromatopsia levels do the same under luminance Y, that no anchor is a
terrain index, and that the semantic groups reach the distances for that
eye. With the installed game (`present-colour-vision-data`) it checks that
`PALETTE.PAL` matches the stock anchor indices, that each dichromat anchor is
at L* 15 or lighter, that each anchor keeps its recorded gap from the terrain
indices, that each old dot index maps to the anchor, and that a blip map
changes no index but the body.

`native-colour-vision` plays a staged ten-player battle in each mode and
checks the anchor colour on each player's units and minimap dot.

## Limitations

The war-map and chat colours are published here and not yet drawn. Health,
selection, build sites and radar marks take their palette indices from
`paint_mark_slots`. Produced and consumed resource figures take
`resource_produced` and `resource_consumed`, matched when the slots are
painted. The kit draws its state colours from these tables. Shape marks are
not drawn; only whether they are in effect is answered. Boneyards chat colours are unchanged. There is no whole-screen
filter. Achromatopsia shares four greys across the ten players, and the
shapes tell those players apart. The simulation is severity 1.0 only. The
mode is this machine's presentation: another player sees the colours that
player's own preference chooses. Off leaves the picture alone.
