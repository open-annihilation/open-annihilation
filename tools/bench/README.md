# The frame bench

The frame bench measures how fast the game draws a big battle in the Full
tier's turned 3D view, the same way on every build, so that a change to the
renderer can be measured against the build before it. The game runs it
itself with `--bench VIEW` ([the option](../../docs/command-line.md#--bench));
the files here set up the battle, run the game in a real window and turn
the runs into tables.

| File | What it is |
|---|---|
| `battle.stage` | The battle: about 550 units on Lava Highground's central plateau, armies patrolling into each other with reinforcements, factories with long queues, builders and aircraft. |
| `idle.stage` | The same match with nothing added: the two commanders. |
| `low-across.bench`, `turning.bench`, `far-out.bench`, `close.bench`, `idle.bench` | The views: low across the battle with the camera still, the same view turning 6 degrees a second, the farthest the view zooms out, zoom 2, and the low view over the idle match. |
| `preferences.conf` | The preferences every run starts from: the Full tier at 4x anti-aliasing, Full 3D Mode and Unlocked camera on, the +stats overlay shown, a unit limit of 1000, and a two-player skirmish on Lava Highground, Arm against a computer Core, the game going on after a commander's death. |
| `run.sh` | One run in a window, in a folder of its own. |
| `campaign.sh` | Runs of several builds back to back, each view in turn. |
| `summarise.py` | The runs as Markdown tables. |
| `windowid.swift` | On macOS, finds the game's window for the screenshot. |

## Running it

Build the game, then run one view (`OA_GAME_DIR` names the TA 3.1c
installation):

```sh
tools/bench/run.sh build/open-annihilation.app tools/bench/turning.bench runs/turning-1
```

The game opens its window, plays the main menu for a second, starts the
skirmish, plays the stage into it, warms up to tick 600 and measures. It
prints a line each second and closes when it is done. `runs/turning-1`
then holds `bench.txt`, the bench's lines, the game's `log.txt` and, on
macOS, `shot.png`, the window halfway through. Keep the machine otherwise
idle while it runs, and leave the window uncovered.

To compare builds, give each a name; the views run in turn, the builds back
to back, two rounds unless `--runs` says otherwise:

```sh
tools/bench/campaign.sh runs before=old/build/open-annihilation.app after=build/open-annihilation.app
```

and to see the runs again, with the stage timers and the scene's counts:

```sh
tools/bench/summarise.py --stages --counts runs
```

## What it reports

The bench's lines are described with [the option](../../docs/command-line.md#--bench).
In short: the frame rate and the 1% low frame rate (the frame rate of the
slowest 1% of the frames), the frame's time at its median, 95th and 99th
percentiles, and how long each part of the frame took (the stage timers).
The low, far-out, close and idle views measure the same 750 match ticks of
the battle on every build, however long they take; the turning view
measures 25 seconds. The stage timers' names stay the same from one release
to the next, so that runs of different builds compare. They cost nothing in
an ordinary game: they measure only during a bench, and a view with
`stage_timers 0` measures the frames with them off.
