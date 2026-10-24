<!--
SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
SPDX-License-Identifier: GPL-3.0-only
-->

# Learned Scout

Learned Scout sends each of its seat's units that can move, when that unit
is free, to the centre of one sector of the map. It does this every 30
ticks. It builds nothing. It is a small example of a recorded rule, a
trained network, and a brain that plays with that network.

A unit is free when it has no order. It is also free when it is standing
within its footprint of the centre it was last sent to, even when that move
order is still reported. The footprint reach is the longer side of the
unit's footprint, in cells, times 16 world units.

The manifest lists `works-with: ["ta-3.1c"]`. A works-with list cannot hold
both that id and a star, and a star alone does not fit this game.

## Levels

`learned` is the first level, and the one a match uses when no level is
named. It loads `nets/scout.oann`. When the unit is free it sends the unit
to the sector the network picks. When that centre is one the unit cannot
stand on, it follows the written rule instead.

`rule` plays the written rule below. Every 30 ticks it records the choice
for each unit that can move, and it sends the unit when the unit is free.
The recorded file is what the trainer reads. Both levels are difficulty
`easy`, and both use the same standable centres and the same free test.

## The rule

The map is cut into 8 by 8 sectors. Rows run north to south and columns
west to east. A sector's index is `row * 8 + column`, counting from 0.
Its centre is the middle of that cell of the explored grid.

A sector counts only when the unit can stand on its centre. The cell comes
from `map:anchor` for the unit's movement class. `map:passable` is 0 when
that cell is blocked or off the map, and 1 or 3 when the class can enter
it. A centre counts when that value is greater than 0.

Among the sectors that remain whose explored share is below one half, the
choice is the sector whose centre is nearest the unit. Distance is the
squared distance of the floored world coordinates, in whole numbers. The
lowest index wins a tie. When every standable sector is at least half
explored, the choice is the farthest, and again the lowest index wins a
tie.

## The network

Each choice is a tensor named `sectors`, shape `{2, 8, 8}`. Channel 1 is
the explored grid. Channel 2 is each sector centre's distance from the
unit, divided by the map's diagonal. The network is a conv2d of 1 by 1
from 2 channels to 8, a relu, a conv2d of 1 by 1 from 8 channels to 1, a
flatten, and an argmax. That is 24 weights and 9 biases. The output is
named `choice`.

The trainer starts from scores that follow the rule: below half explored,
a nearer centre scores higher; at or past half, a farther centre scores
higher; an open sector outranks a covered one. It then runs one epoch of
gradient descent at rate `1e-16`. The step is small so those scores stay
on the grid the integer network uses. The same records produce the same
model bytes.

## Records

Run the commands from the repository root, with `OA_GAME_DIR` set to the
3.1c game data. Scratch files stay under `/tmp`. Each match is 9,000
ticks. The six record files, joined in the order below, are
`/tmp/N05-scratch/N05-records.jsonl`.

```sh
scratch=/tmp/N05-scratch
mkdir -p "$scratch" /tmp/N05-user
game=build/open-annihilation.app/Contents/MacOS/open-annihilation
maps=(
  "Ashap Plateau"
  "Acid Foursome"
  "Painted Desert"
  "Show Down"
  "Coast To Coast"
  "Caldera's Rim"
)
i=0
for map in "${maps[@]}"; do
  seed=$((20261023 + i))
  "$game" --skip-intro --mute --headless-check --game-dir "$OA_GAME_DIR" \
    --user-folder /tmp/N05-user \
    --brain-match brains/examples/learned-scout,builtin \
    --brain-levels rule,easy \
    --brain-map "$map" --seed "$seed" --match-ticks 9000 \
    --brain-record "$scratch/N05-rec-$i.jsonl" \
    > "$scratch/N05-rec-$i.out"
  i=$((i + 1))
done
cat "$scratch"/N05-rec-0.jsonl "$scratch"/N05-rec-1.jsonl \
  "$scratch"/N05-rec-2.jsonl "$scratch"/N05-rec-3.jsonl \
  "$scratch"/N05-rec-4.jsonl "$scratch"/N05-rec-5.jsonl \
  > "$scratch/N05-records.jsonl"
shasum -a 256 "$scratch/N05-records.jsonl"
```

```
984b97abf871ffddeeb775ae428f7076abf1dbad2f38ac98c9fcbfee28618f0b  N05-records.jsonl
```

Each match records a choice every 30 ticks for each unit that can move.
The seat's commander is that unit on these maps, so a 9,000-tick match
records 301 samples. The joined file holds 1,806 samples. The trainer
holds out every fifth sample, 361 of them. A move is issued on a look only
when the unit is free, so the file keeps a sample between orders as well
as on each new order.

Ashap Plateau, seed 20261023:

```
brain match: map Ashap Plateau seed 20261023 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3792695 status playing
brain match: seat 2 units 5 built 5 lost 0 kills 0 metal 1000 energy 9 metal-produced 650 energy-produced 3762 instructions - status built-in
brain match: digest 539bef379b1bb576
```

Acid Foursome, seed 20261024:

```
brain match: map Acid Foursome seed 20261024 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3893533 status playing
brain match: seat 2 units 9 built 9 lost 0 kills 0 metal 845 energy 386 metal-produced 580 energy-produced 6225 instructions - status built-in
brain match: digest 9b22be83a2ad1108
```

Painted Desert, seed 20261025:

```
brain match: map Painted Desert seed 20261025 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 4117251 status playing
brain match: seat 2 units 10 built 10 lost 0 kills 0 metal 657 energy 1000 metal-produced 493 energy-produced 10146 instructions - status built-in
brain match: digest e06d630f365af60e
```

Show Down, seed 20261026:

```
brain match: map Show Down seed 20261026 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3763436 status playing
brain match: seat 2 units 12 built 12 lost 0 kills 0 metal 133 energy 1100 metal-produced 678 energy-produced 11549 instructions - status built-in
brain match: digest 4aae969f90d4f899
```

Coast To Coast, seed 20261027:

```
brain match: map Coast To Coast seed 20261027 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3743890 status playing
brain match: seat 2 units 7 built 7 lost 0 kills 0 metal 676 energy 1000 metal-produced 161 energy-produced 6912 instructions - status built-in
brain match: digest a0bf40e0c137685a
```

Caldera's Rim, seed 20261028:

```
brain match: map Caldera's Rim seed 20261028 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 4728584 status playing
brain match: seat 2 units 7 built 7 lost 0 kills 0 metal 939 energy 524 metal-produced 313 energy-produced 6352 instructions - status built-in
brain match: digest 923c326f4f920edd
```

## Train and convert

```sh
python3 -I tools/train_learned_scout.py "$scratch/N05-records.jsonl" \
  --out "$scratch/N05-scout.onnx"
shasum -a 256 "$scratch/N05-scout.onnx"
rm -f brains/examples/learned-scout/nets/scout.oann
build/oa-tool brain net "$scratch/N05-scout.onnx" \
  --calibrate "$scratch/N05-records.jsonl" \
  --out brains/examples/learned-scout/nets/scout.oann
shasum -a 256 brains/examples/learned-scout/nets/scout.oann
```

The converter will not replace an existing `nets/scout.oann`, so the
remove comes before it. The trainer prints:

```
held-out agreement: 92.5% (334/361)
```

```
22a1008aae5234680bf07adadd9ceca828d34ff5af72ea0a985bd2badad2a790  N05-scout.onnx
```

The converter prints `agreement: choice 97.9% (1768 of 1806)`. The network
file's SHA-256, which `oabrain.yaml` names, is:

```
fcccef0c3dccbf8aac16a80266efcaf1013b5cc8f568dbcd4f8b17d53fe1c601  nets/scout.oann
```

The `.onnx` and the records file are not part of the package. The game
data make the records, and the commands above make the same bytes again.

## Pack and check

```sh
rm -f "$scratch/N05-learned-scout.oabrain"
build/oa-tool brain pack brains/examples/learned-scout \
  --out "$scratch/N05-learned-scout.oabrain"
build/oa-tool brain check "$scratch/N05-learned-scout.oabrain" \
  --game-dir "$OA_GAME_DIR"
```

Check exits 0 and reports:

```
fits: ok, 0 instructions
start: ok, 2 instructions
result: ok
```

## Play

The first map above is Ashap Plateau. One match at level `learned`, and
the same match again, then one at level `rule`. The seed is 20261023 and
the length is 9,000 ticks.

```sh
"$game" --skip-intro --mute --headless-check --game-dir "$OA_GAME_DIR" \
  --user-folder /tmp/N05-user \
  --brain-match brains/examples/learned-scout,builtin \
  --brain-levels learned,easy \
  --brain-map "Ashap Plateau" --seed 20261023 --match-ticks 9000
```

Both `learned` runs print:

```
brain match: map Ashap Plateau seed 20261023 ticks 9000
brain match: seat 1 learned-scout 1.0 level learned side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 1068031 status playing
brain match: seat 2 units 5 built 5 lost 0 kills 0 metal 1000 energy 9 metal-produced 650 energy-produced 3762 instructions - status built-in
brain match: digest 7a1bfdc075170065
```

The seat stays `playing`. The second run prints that same report and the
same digest.

Level `rule` uses `--brain-levels rule,easy` and prints the Ashap Plateau
report from the records section above, including digest
`539bef379b1bb576`.

## Teaching something else

Change the choice in `lua/rule.lua`. If the features change, change
`lua/sectors.lua` as well and keep a tensor named `sectors` of shape
`{2, 8, 8}`: the trainer and `nets/scout.oann` read that name and that
shape. Change the starting scores in `tools/train_learned_scout.py` so
they follow the new rule. The descent step does not replace those scores
on its own.

Record again at level `rule`, run the trainer, remove
`nets/scout.oann`, convert, and put the new SHA-256 in `oabrain.yaml`.
The network stays two 1 by 1 convolutions, from 2 channels to 8 to 1.
