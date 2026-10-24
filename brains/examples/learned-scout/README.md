<!--
SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
SPDX-License-Identifier: GPL-3.0-only
-->

# Learned Scout

Learned Scout sends each of its seat's units that can move, and that has no
order, to the centre of one sector of the map. It does this every 30 ticks.
It builds nothing. It is a small example of a recorded rule, a trained
network, and a brain that plays with that network.

The manifest lists `works-with: ["ta-3.1c"]`. A works-with list cannot hold
both that id and a star, and a star alone does not fit this game.

## Levels

`learned` is the first level, and the one a match uses when no level is
named. It loads `nets/scout.oann` and sends the unit to the sector the
network picks.

`rule` plays the written rule below and records each choice with
`brain.record`. The recorded file is what the trainer reads. Both levels
are difficulty `easy`.

## The rule

The map is cut into 8 by 8 sectors. Rows run north to south and columns
west to east. A sector's index is `row * 8 + column`, counting from 0.
Its centre is the middle of that cell of the explored grid.

Among sectors whose explored share is below one half, the choice is the
sector whose centre is nearest the unit. Distance is the squared distance
of the floored world coordinates, in whole numbers. The lowest index wins
a tie. When every sector is at least half explored, the choice is the
farthest sector, and again the lowest index wins a tie.

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
1a1e39e299131827ba9295aec1f7dc4f6d9bac414139ae2adb3b0402f5f0908e  N05-records.jsonl
```

Each match records one choice. A unit is sent only while it has no order,
and the order from that first choice is still in place at 9,000 ticks, so
later visits record nothing more. The fifth sample is the one the trainer
holds out.

Ashap Plateau, seed 20261023:

```
brain match: map Ashap Plateau seed 20261023 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 850274 status playing
brain match: seat 2 units 10 built 10 lost 0 kills 0 metal 885 energy 901 metal-produced 599 energy-produced 7381 instructions - status built-in
brain match: digest 1ae0783ae8952174
```

Acid Foursome, seed 20261024:

```
brain match: map Acid Foursome seed 20261024 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 948100 status playing
brain match: seat 2 units 10 built 10 lost 0 kills 0 metal 741 energy 1000 metal-produced 578 energy-produced 7805 instructions - status built-in
brain match: digest 4f84aa8f429001f8
```

Painted Desert, seed 20261025:

```
brain match: map Painted Desert seed 20261025 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 1166927 status playing
brain match: seat 2 units 9 built 9 lost 0 kills 0 metal 880 energy 734 metal-produced 567 energy-produced 7192 instructions - status built-in
brain match: digest fe509b1c80888d8b
```

Show Down, seed 20261026:

```
brain match: map Show Down seed 20261026 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 821680 status playing
brain match: seat 2 units 9 built 9 lost 0 kills 0 metal 992 energy 160 metal-produced 468 energy-produced 5564 instructions - status built-in
brain match: digest 654cef572ae48405
```

Coast To Coast, seed 20261027:

```
brain match: map Coast To Coast seed 20261027 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 801212 status playing
brain match: seat 2 units 10 built 10 lost 0 kills 0 metal 68 energy 1100 metal-produced 528 energy-produced 12243 instructions - status built-in
brain match: digest 89a9497ccf85e7b6
```

Caldera's Rim, seed 20261028:

```
brain match: map Caldera's Rim seed 20261028 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 1782472 status playing
brain match: seat 2 units 7 built 7 lost 0 kills 0 metal 915 energy 674 metal-produced 306 energy-produced 6452 instructions - status built-in
brain match: digest 6d4fead659501d80
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
held-out agreement: 100.0% (1/1)
```

```
fabee4791220419a21ea598efa27d0bf368b71faa14c8250599972309e4f777d  N05-scout.onnx
```

The converter prints `agreement: choice 100.0% (6 of 6)`. The network
file's SHA-256, which `oabrain.yaml` names, is:

```
cd75bbf83542373634c24077cd319f97ab3235aeb84a626a2e70f1e318062a3c  nets/scout.oann
```

The `.onnx` and the records file are not part of the package. The game
data make the records, and the commands above make the same bytes again.

## Pack and check

```sh
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
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 853342 status playing
brain match: seat 2 units 10 built 10 lost 0 kills 0 metal 885 energy 901 metal-produced 599 energy-produced 7381 instructions - status built-in
brain match: digest 1ae0783ae8952174
```

The seat stays `playing`. The second run prints that same report and the
same digest.

Level `rule` uses `--brain-levels rule,easy` and prints:

```
brain match: map Ashap Plateau seed 20261023 ticks 9000
brain match: seat 1 learned-scout 1.0 level rule side 0
brain match: seat 2 built-in level easy side 1
brain match: end tick 9000 winner none reason time
brain match: seat 1 units 1 built 1 lost 0 kills 0 metal 10000 energy 10000 metal-produced 150 energy-produced 3762 instructions 850274 status playing
brain match: seat 2 units 10 built 10 lost 0 kills 0 metal 885 energy 901 metal-produced 599 energy-produced 7381 instructions - status built-in
brain match: digest 1ae0783ae8952174
```

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
