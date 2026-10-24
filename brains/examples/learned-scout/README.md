<!--
SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
SPDX-License-Identifier: GPL-3.0-only
-->

# Learned Scout

Learned Scout sends each of its seat's units that can move, when that unit
is free, to the centre of one sector of the map. It does this every 30
ticks. It builds nothing. It is a small example of a recorded rule, a
trained network, and a brain that plays with that network.

A unit is free when `head_order()` returns nil, or when the third value
from that call is true. The third value is the idle bit. A finished move
stays at the head of the queue with that bit set, and the unit is free
then.

The manifest lists `works-with: ["ta-3.1c"]`. A works-with list cannot hold
both that id and a star, and a star alone does not fit this game.

## Levels

`learned` is the first level, and the one a match uses when no level is
named. It loads `nets/scout.oann`. When the unit is free it sends the unit
to the sector the network picks.

`rule` plays the written rule below. Every 30 ticks it records the choice
for each unit that can move, and it sends the unit when the unit is free.
The recorded file is what the trainer reads. Both levels are difficulty
`easy`. Both write a blocked centre the same way, and both use the same
free test.

## The rule

The map is cut into 8 by 8 sectors. Rows run north to south and columns
west to east. A sector's index is `row * 8 + column`, counting from 0.
Its centre is the middle of that cell of the explored grid.

A centre the unit cannot stand on is written as fully explored in channel
1, the count 65536. The cell comes from `map:anchor` for the unit's
movement class. `map:passable` is 0 when that cell is blocked or off the
map, and 1 or 3 when the class can enter it. A centre the class cannot
enter is the one written full.

Among sectors whose explored share is below one half, the choice is the
sector whose centre is nearest the unit. Distance is the squared distance
of the floored world coordinates, in whole numbers. The lowest index wins
a tie. When every sector is at least half explored, the choice is the
farthest, and again the lowest index wins a tie. A blocked centre is
already fully explored, so the nearest-open choice skips it.

## The network

Each choice is a tensor named `sectors`, shape `{2, 8, 8}`. Channel 1 is
the explored grid, with a blocked centre written as fully explored.
Channel 2 is each sector centre's distance from the unit, divided by the
map's diagonal. The network is a conv2d of 1 by 1 from 2 channels to 8, a
relu, a conv2d of 1 by 1 from 8 channels to 1, a flatten, and an argmax.
That is 24 weights and 9 biases. The output is named `choice`.

The trainer draws each starting weight from `random.Random(20261023)`,
uniform in ±0.1. It then runs plain gradient descent on a margin: for
each sample, the recorded sector's score should be at least 1 above the
best other sector's, and a sample that misses it adds the difference to
the loss. It runs for 2000 epochs at rate `0.3`, which takes a few minutes.
Squared error between the 64 scores and the one-hot choice is the obvious
first loss, but it stops near 80%: it spends its effort on the 63 sectors
not chosen. The margin looks only at the choice and its nearest rival.
The same records produce the same model bytes. The trainer also prints a
closed-form agreement. That ranking is a comparison only. It is not the
network's weights.

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
d8cbe31964e8c00bea748ff46f8b9daa33407f3051481361c1761cdd86186859  N05-records.jsonl
```

Each match records a choice every 30 ticks for each unit that can move.
The seat's commander is that unit on these maps, so a 9,000-tick match
records 301 samples. The joined file holds 1,806 samples. The trainer
holds out every fifth sample, 361 of them. A move is issued on a look only
when the unit is free. The unit is free when its head order is absent or
the idle bit is set. The file keeps a sample between orders as well as on
each new order.

Ashap Plateau, seed 20261023:

```
brain match: map Ashap Plateau seed 20261023 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3787819 status playing
brain match: seat 2 units 11 built 11 lost 0 kills 0 metal 954 energy 637 metal-produced 558 energy-produced 7345 instructions - status built-in
brain match: digest c917b1dc16a53a0d
```

Acid Foursome, seed 20261024:

```
brain match: map Acid Foursome seed 20261024 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3897284 status playing
brain match: seat 2 units 5 built 5 lost 0 kills 0 metal 1000 energy 6 metal-produced 781 energy-produced 3762 instructions - status built-in
brain match: digest 6e0cac2096e235d8
```

Painted Desert, seed 20261025:

```
brain match: map Painted Desert seed 20261025 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 4111929 status playing
brain match: seat 2 units 10 built 10 lost 0 kills 0 metal 657 energy 1000 metal-produced 493 energy-produced 10146 instructions - status built-in
brain match: digest e06d630f365af60e
```

Show Down, seed 20261026:

```
brain match: map Show Down seed 20261026 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3758630 status playing
brain match: seat 2 units 11 built 11 lost 0 kills 0 metal 805 energy 1000 metal-produced 615 energy-produced 9624 instructions - status built-in
brain match: digest 925c1fd3eaf026cd
```

Coast To Coast, seed 20261027:

```
brain match: map Coast To Coast seed 20261027 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 3739020 status playing
brain match: seat 2 units 9 built 9 lost 0 kills 0 metal 522 energy 790 metal-produced 210 energy-produced 7037 instructions - status built-in
brain match: digest 026671dc8053c885
```

Caldera's Rim, seed 20261028:

```
brain match: map Caldera's Rim seed 20261028 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 4723514 status playing
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
held-out agreement: 95.8% (346/361)
closed form agreement: 87.8% (317/361)
```

```
036a4a855fa8641330299d026a3b2465d8082e90e6f22cafbf518ab29e764c53  N05-scout.onnx
```

The converter prints `agreement: choice 99.8% (1802 of 1806)`. The network
file's SHA-256, which `oabrain.yaml` names, is:

```
49c1902a2ced00d0a010ed3e6def535bb087d74a6ab9d7cf7bd8ce753175a85f  nets/scout.oann
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
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 1049307 status playing
brain match: seat 2 units 11 built 11 lost 0 kills 0 metal 954 energy 637 metal-produced 558 energy-produced 7345 instructions - status built-in
brain match: digest c917b1dc16a53a0d
```

The seat stays `playing`. The second run prints that same report and the
same digest. It is also the `rule` match's digest below: on this map the
network sends the commander everywhere the written rule does.

Level `rule` uses `--brain-levels rule,easy` and prints the Ashap Plateau
report from the records section above, including digest
`c917b1dc16a53a0d`.

## Teaching something else

Change the choice in `lua/rule.lua`. If the features change, change
`lua/sectors.lua` as well and keep a tensor named `sectors` of shape
`{2, 8, 8}`: the trainer and `nets/scout.oann` read that name and that
shape. Record again at level `rule`, run the trainer, remove
`nets/scout.oann`, convert, and put the new SHA-256 in `oabrain.yaml`.
The network stays two 1 by 1 convolutions, from 2 channels to 8 to 1.
The trainer keeps drawing its start from the fixed seed.
