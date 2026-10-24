# Capturing video

Two options of the game, `open-annihilation`, are developer's tools for
making showcase videos of the engine: `--capture-video PATH.mp4` writes what
the game's window shows, with the game's sound, as an MP4 file ready to
upload to YouTube, and `--showcase NAME` plays a scripted run of the game in
place of a player. They work apart or together.

**Capture needs the `ffmpeg` program on `PATH`**, built with `libx264`
(Homebrew's `ffmpeg` is). The capture starts it for the frames and again to
join them to the sound; when it cannot be started, the game stops at once
and says so.

## The capture

`--capture-video PATH.mp4` starts once the window is open, after the
opening movies, and makes the video when the game closes or the showcase
ends. While it runs:

- **Frames.** Every frame the window presents is read back from the
  renderer just before it is presented, at the window's size: the frontend's
  screens as the window shows them, letterboxed, and the match at its full
  size. `--resolution WxH` opens the window at that size; both must be even.
  The window must keep its size until the capture ends. The main menu shows
  the OA button in its bottom-right corner, as players see it, and so do
  `--snapshot` pictures of the main menu.
- **Pace.** The video has 30 frames a second of the sound device's clock:
  frame *n* shows the screen as it was *n* / 30 seconds after the start.
  When the game is late, the frame before is written again; when it draws
  faster, the frames between are left out. Because frames follow the same
  clock the sound is played on, the video stays in step with the sound
  however fast the game or the machine runs. The last line the capture
  prints says how many frames were repeated while the game was late.
- **Sound.** The capture takes the game's sound: before SDL starts, it
  chooses SDL's disk audio driver, which plays the mix into a file instead
  of the speakers, so **a capture is silent**. The capture then opens a
  silent sound device of its own, before the game plays anything, so the
  mix is 32-bit float, stereo, at 48 kHz, and starts with the video. That
  device's postmix callback counts the sample frames the device plays:
  that count is the capture's clock. By itself the disk driver keeps only
  roughly to time (on the Mac it was measured on, about 6% slow), so the
  capture sets it to wait three quarters of each buffer's length, and the
  callback holds each buffer back until its moment on the wall clock: the
  mix plays in real time, and the frames at 30 a second of it are 30 a
  second of the wall clock. While a buffer is held back, which is a few
  milliseconds of every 21, the game waits for it when it starts or stops
  a sound. A sound can reach the mix up to one device buffer (1024 sample
  frames, about 21 ms) after the frame that started it.
- **Encoding.** The frames go as raw RGB through a pipe to `ffmpeg`, which
  encodes them as H.264 (libx264, High profile, 4:2:0, BT.709 colour,
  constant quality 18, two B-frames and closed groups of 15 frames). When
  the capture ends, `ffmpeg` joins them to the mix, encoded as AAC at 48 kHz
  and 384 kbit/s and cut to the frames' length, with the file's index at its
  start (`+faststart`): the settings YouTube recommends for uploads. While
  the capture runs, the frames and the mix are kept beside the video, as
  `NAME.capture-frames.mp4` and `NAME.capture-mix.f32`; both are removed
  once the video is made, and a capture that stops on an error leaves them
  for a look.

A capture does not include movies (the opening movies and the campaign's
ending), which a movie player shows, nor any headless run, check or
benchmark: those options cannot be used with `--capture-video`.

A capture keeps to the wall clock, so what it shows depends on how fast the
machine runs. To make a video of a recorded game that is the same on every
run, at any size and frame rate, from a camera that follows the game's
moments, use a director script instead ([director.md](director.md)):
`--generate-script` plans one from the recording and `--render-script`
renders it frame by frame, headless, with its sound mixed offline.

The code is `src/app/video_capture.cpp` (`oa-app-video-capture`), with its
header `video_capture.hpp`. The runtime hands it each frame from
`Runtime::capture_render_target()`, which every presented frame passes
through. `app-video-capture` tests its pacing and the `ffmpeg` arguments.

## The showcase

`--showcase arm-first-mission` plays from the main menu into the first
mission of the Arm campaign and through its victory. Its input goes through
the same event path as the player's mouse and keyboard: pointer motion,
button and key events, as SDL delivers them, sent to the runtime's event
dispatch on the frames of the game's own loop, so the pointer glides to what
it clicks and each button shows pressed. In order:

1. The main menu for three seconds, then SINGLE and New Campaign.
2. On the New Campaign screen, the Arm side; the Arm Campaign must be the
   one chosen. Start opens the first mission's briefing, which shows with
   its narration for nine seconds before its Start begins the mission.
3. Three seconds into the mission, Ctrl+A selects every unit of the player,
   and MOVE ORDERS in the orders panel is clicked until every unit that
   moves shows Hold Position. Fire orders stay as the units start: Fire At
   Will, so they answer the Core units they meet.
4. MOVE, then a click on the radar where it shows the point of the
   mission's MoveUnitToRadius victory condition (in the first Arm mission,
   the Galactic Gate), sends them all there.
5. The camera follows the unit of the group nearest that point: the T key,
   which follows the next selected unit, is pressed until it follows that
   one. The showcase looks again every second, and changes to another unit
   when the one followed is destroyed or another is 160 pixels nearer.
6. When a unit reaches the point, the mission is won: the VICTORY title,
   the darkened last frame, and the glamour picture, which a click advances
   after five seconds; then the score screen, held six seconds before the
   game closes.

The showcase prints a `showcase:` line for each step. It stops with an error
when a screen does not open within a minute, when the units do not reach
the point within fifteen minutes or when the mission is lost. Unless `--seed`
names another, it seeds the mission with the same value every run, so a run
plays out much the same way again; it follows the wall clock, so no two runs
are identical. It is written in `src/app/runtime_showcase.cpp`.

`--showcase skirmish-battle` measures how the game keeps up in a fight on
the machine it runs on. After three seconds of the main menu it starts the
two-player skirmish `--benchmark` starts, its game continuing after a
commander's death (the player's own Commander Death choice is put back once
it has started), with `--combat`'s two armies (50 units a side without it)
and `--busy-combat`'s additions, always, and says the frame size, the frame
rate cap and the drawing threads it plays with. The battle then plays for a
minute on the game's own loop, clock, sound and window (a match that ends
sooner ends it, and says so), the first frames that took longer than five
ticks are listed, and the run ends with one line:

```text
showcase: battle 60.0 s: 1792 ticks (29.9 a second), 2683 frames (44.7 a second),
game speed 10 of 10; frame 22.4 ms mean, 529.1 longest (3 ticks in 425.8 ms,
draw 97.3 ms, present 5.2 ms); 2 frames slower than 167 ms; work 22.4 ms a frame;
tick 1.32 ms mean; draw 16.3 ms and present 4.9 ms a frame
```

(one line in the output), then the memory report, with the largest working
set the system saw. 30 ticks a second at game speed 10 is the full rate.

## Construction in the constrained 3D view

`--showcase base-construction --content totala:core-contingency
--hardware-acceleration=full --experimental-3d` builds an Arm base on Town &
Country. The commander constructs a solar collector, three metal extractors
and a Kbot plant. The plant produces a construction Kbot, which builds three
Defenders and two Guardians while the commander assists. Sites must be legal;
extractors prefer metal-bearing ground. The sequence waits for actual
construction completion and stops if a stage cannot finish.

This is a funded showcase: resource stores are replenished, the opponent is
idle and game speed is increased. Models, nanoframes, unit scripts and build
orders use the running simulation. The camera opens in the classic view,
then orbits and pans within the normal constrained range, without unlocking.
Once the perimeter is complete, two staged waves of two Core Raiders attack
it using ordinary combat orders. The camera varies zoom, pan and elevation
during the fighting, then holds on the cooling wrecks. The showcase requires
visible projectile lights, hot wrecks and heat sources before completing.
It returns to the default top-down view and builds another solar collector
to show construction lighting with the same Graphics settings.
Use `--capture-video` to record the sequence and its game audio.

## Making the showcase video

A preferences file of its own keeps the player's preferences out of the
run and turns the movies off; music and sound effects are on by default:

```text
open-annihilation-preferences 1
"Total Annihilation|PlayMovie" "0"
"Total Annihilation|musicmode" "1"
"Total Annihilation|Sound Mode" "1"
```

With SDL's dummy video driver the game opens no window on the screen, and
the capture plays nothing through the speakers. On macOS, SDL's offscreen
video driver cannot open the game's window, so the dummy driver is the one
to use. From a build tree configured as
[CONTRIBUTING.md](../CONTRIBUTING.md#build-and-test) describes
(`RelWithDebInfo` runs at full speed):

```sh
SDL_VIDEO_DRIVER=dummy SDL_RENDER_DRIVER=software \
  build/open-annihilation.app/Contents/MacOS/open-annihilation \
  --game-dir "/path/to/Total Annihilation" --skip-intro \
  --resolution 1280x1024 --preferences-file showcase.conf \
  --showcase arm-first-mission \
  --capture-video open-annihilation-showcase-arm-mission-1.mp4
```

On Windows and Linux the executable is `build/open-annihilation.exe` or
`build/open-annihilation`. Captures are local files: keep them out of Git.
