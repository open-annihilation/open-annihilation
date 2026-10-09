# Game audio

This component holds the sound registry and selection policy of Total
Annihilation 3.1c and supplies a portable SDL3 WAV playback boundary.

The registry is loaded from `gamedata/allsound.tdf`: each section registers its
32-byte section name and `sound` value, up to 255 entries. A configured value
resolves below `sounds` with any existing extension replaced by `wav`; no
localized copy of the file is looked for. Name lookup is case-insensitive and
returns `0xffff` when absent. Selection applies the playback gates, carries the
request to announce the sound to the other players' simulations, and supports a
temporary alternate route. The `s`/`S` command switch sets the
playback-suppressed flag and leaves the diagnostic-direct flag unchanged. The
request to announce a sound is exposed as a flag; telling the other players is
left to the caller.

3.1c's own `gamedata/sound.tdf` names thirteen sounds its files never held,
among them `untdone`, the plants' finished-unit sound, and the game plays
nothing for them: `known_missing_sounds` lists them, and
`known_missing_sound` matches a resource or configured name against the list
whatever its folder, extension or case. A failure to play one is never
reported, and a mod that ships one plays it. `sound_resource_key` gives the
key a sound is known by from one call to the next: its path in lower case
with slashes. `game-audio-data` checks that the list is exactly the sounds
the installed game's `sound.tdf` names and lacks.

The SDL backend reads WAV data through `oa::AssetStore`, so loose-file and HPI
precedence remain centralized in the asset layer.

The WAV player's effects (`play_placed`, `play_resource`) keep the game's
voice policy, as the offline mix below does: at most eight voices
(`default_voice_limit`); a start that finds them all taken stops the voice
that started first; a sound plays on at most four of its buffers
(`sample_buffers`) at once, a start taking an idle one, else making one
while fewer than four exist (the highest free index first), else restarting
the one that has played furthest, which then holds two voices until it
stops. Each sound file is read and decoded on its first start, converted
once to the mixer's rate (`convert_for_mixer`), and kept, with up to 8 MiB
of others in that form, the least recently started that no voice
plays giving way first; a buffer plays on a stream it holds while it plays,
and up to eight idle streams are kept for the next starts. A placed start
weighs the stream's sides with `spatial_stereo_gain`'s levels
(`set_side_gains`) rather than converting its samples. The loop and the
stream are converted once in the same way. A file whose converted samples
would take more than 8 MiB as an effect or the loop, or more than 32 MiB as
the stream, is refused before it is converted (`converted_bytes`). `effect_voices` counts the
voices.

`hold_loop` silences the looping sound where it is and lets it play on from
there; the game holds the menu's loop while a movie plays and while the
application is inactive. The hold is the player's: a loop started while it
lasts starts silent.

Besides its effects and its one looping sound, the WAV player keeps one
stream, as the game keeps one: the briefing's narration and the end screen's
glamour sound. `SdlWavPlayer::play_stream` plays a sound once, after a delay
of silence, in place of any stream still playing or waiting;
`stop_stream` silences it at once, during its delay too; and `stream_busy`
holds from the request until the sound has played out.

Unit announcements are queued only for the viewpoint player's units whose owner
is allowed to announce; the viewpoint is the player the match views, which
"+View" moves. While the novelty voice "+Sing" toggles is on, an
announcement whose sound plays plays the first of the gates' two novelty
sounds on the first 30-tick window of every eight and the second on the
other seven, instead of the unit's own sound: 3.1c's honk and sing, or the
two a mod profile names (`AnnouncementPresentationGates::novelty_sounds`).
The caption stays the unit's own, and every gate that keeps a unit's own
sound silent keeps the novelty sound silent. Categories use a fixed descriptor table: the queue
suppresses a duplicate category, orders its eight records by priority, and
drops slot seven when full. `gamedata/SOUND.TDF` maps the unit FBI
`SoundCategory` to one or more direct sound names. Presentation uses the game's
15-bit `rand()` scaling, priority/volume gates, unit text flag, speech-mode bit,
and per-category cooldown in 30 Hz ticks. The selected name is resolved beneath
`sounds` and handed to the SDL player's direct-resource boundary, bypassing the
ALLSOUND registry as the game does.

Effects volume: `fxvol` is shifted left by 10 before the device clamps it at
65535, so preference 64 reaches full scale. `WaveOutVolume` is a scalar restored
at startup; the device clamps it to 16 bits and applies it to both channels.
SDL multiplies these two normalized gains and applies the result to each
active stream.

Sound-screen RESTORE stores fxvol 27, sets the ackfx, buildfx, and speechfx
bits of `Game.sound_flags`, forces that word's low three mode bits to 1, and
stores unitchat 10. Other bits of the word, including its high byte, are kept.
It also clears the sound device's 3D mode word. The saved volumes are then
reapplied, the effects volume through the path above.

Clips the match plays at a map point: with the sound mode's 3D switch on
(set by Sound Mode 2, the sound screen's 3D mode and the Sound3D console
command, and passed to `voice_spatial` as its `spatial_enabled` argument) they
play at -585 placed from the middle of the view, within a minimum distance of
the view's mean size and a maximum of the map's width plus height; otherwise
unplaced at -585 inside the view and -1585 outside it. `Mixer::spatial_enabled`
is a separate switch, set by `mixer_enable_spatial`, for the voices the mixer
itself plays with a position.
`SdlWavPlayer::play_placed` hears a placement through `spatial_stereo_gain`,
which attenuates and pans as a 3D sound buffer does: level min/d beyond the
minimum distance (held from the maximum) and a pan by the source's bearing.
Its other sounds play at the level that stands for -585.

Registered names are kept to 32 characters, and a full 32-character name is
compared on all 32 characters; every comparison stays within the name.

CD music runs the CD player (`cd_music`, `music_mood`) against a music-file
disc: `music/<n>.mp3` (or .ogg/.wav/.flac) is disc track n, track 1 is the data
track, so tracks 2..17 of the game's music are the sixteen playable tracks.
A mod's profile may lay the disc out otherwise (ui.audio):
`music_disc_scan_numbered` takes `1.mp3`, `2.mp3` and on, up to the first
missing number, as disc tracks 1 and on (track 1 still data, and no disc at
all without `1.mp3`; `01.mp3` is not track 1); `music_disc_scan_folder`
takes every MP3 file of the folder, sorted by name without case, as tracks
2 and on, whatever the names say (`10.mp3` before `2.mp3`).
`audio-music-session` checks both, and `audio-music-session-mod-install`
the numbered files of the game folder `OA_MOD_GAME_DIR` names.
An unknown 16-track data disc gets the game's default kinds: tracks 1-7 battle,
8-16 building (calm). `sdl_music` decodes each track with `MusicDecoder` on
the sound output's thread and maps the CD mixer line to the stream gain. `music_session` holds the game-side
music events: startup, main menu (kind 4), game loading (calm), teardown, the
per-frame mood update fed by local hits (+1) and kills (+5), ARMOPT pause, the
CDPlay/CDStop/MusicMode console commands and the MUSIC.GUI transport. The
whole mood (activity ring, applied kind, switch timer and last update tick)
resets at match start and end, so each game's music is independent of the
last; in 3.1c the applied kind, switch timer and last update tick carry over
from one game to the next.

## Music decoder

`oa-audio-music-decoder` (`oa/audio/music_decoder.hpp`,
`oa/audio/resampler.hpp`) decodes a music file to interleaved stereo float
at 44100 Hz, a block of 4096 source frames per `decode` call. It recognises
the encoding from the file's first bytes: RIFF WAVE (8-, 16-, 24- and 32-bit
integer and 32-bit float PCM), Ogg Vorbis, FLAC (native or in Ogg) and,
failing those, MPEG audio. The compressed formats are decoded by stb_vorbis,
dr_mp3 and dr_flac, kept in [`third_party/`](../../third_party/) and built
in `src/audio/src/music_codecs.c` with the options
`src/audio/src/music_codecs.h` sets; MP3
files with an encoder-delay tag play without the encoder's padding.

Samples are scaled to -1..1 by 2^-(bits-1), and other channel counts are
mixed to stereo as the header lists (mono at 1/sqrt(2) on both sides, a
centre at 1/sqrt(2) to each side, back and side channels at 1/sqrt(2) to
their own side, a back centre at 1/2 to each, no low-frequency channel).
`Resampler` converts other rates with a Kaiser-windowed sinc kernel (16 zero
crossings each side of the lower rate, cutoff 0.97, window 9), mirroring
the input about its ends, and gives ceil(frames * 44100 / rate) frames; it
is also the rate converter of the sound output's software mixer.

Against the previous decoding of the same files: WAVE and FLAC at 44100 Hz
give the same samples bit for bit; MP3 and Vorbis differ by float rounding
only (about -128 dB and -135 dB); a converted rate agrees to -67..-115 dB
with the same frame count. A final MP3 frame cut short by the end of the
file is dropped rather than decoded from missing bytes; in the game's own
tracks that frame is silence, so each track ends 1152 samples (26 ms)
sooner.

Tests: `audio-music-decoder` (the resampler's lengths, splits and accuracy,
every WAVE depth and channel count, the encoded tones in `src/audio/tests/data`
described in [tests/README.md](tests/README.md), and damaged files) and
`audio-music-decoder-data`, which decodes every file of the installed
game's music folder.

## Sound output

Every sound the game plays goes through `oa-audio-output`
(`oa/audio/sound_output.hpp`): `sound_output()` is the process's
`SoundOutput`, which opens `OutputStream`s, each with its own sample format
(8-bit unsigned, 16- or 32-bit signed, 32-bit float), channel count and
rate. A stream opens paused; it plays what is `put` to it, or asks its
`StreamFeed` for more on the output's thread with the stream locked, and has
its own gain, the gains of its two sides, pause, `clear`, `flush` and the
queued and converted byte counts. `SdlWavPlayer`, the sound-device sink
(`sdl_audio_sink`) and the music device (`sdl_music`) play through it, and
start and stop it where they used to start and stop SDL's audio.

Two outputs exist (`oa/audio/sound_output_backends.hpp`):

- SDL's (`sdl_sound_output.cpp`), in every build with SDL: `SoftwareMixer`
  mixes the streams into one SDL audio stream of 16-bit stereo at 44100 Hz
  on the default playback device, which SDL plays as it is where the device
  takes that format. The device stream opens with the first `start` or
  stream and closes with the last `stop` once no stream is open; `start`
  and `stop` are SDL's audio subsystem, counted as `SDL_InitSubSystem`
  counts.
- The wave-out mixer (`wave_out_output.cpp`), on Windows: a
  `BufferedOutput` that mixes the streams with `SoftwareMixer` into four
  buffers of 1024 frames (16-bit stereo, 44100 Hz, about 93 ms) on the
  Windows wave-out device of
  [src/platform/sound-device](../platform/sound-device/README.md), refilled
  in ring order by the device's thread as each one plays out. Windows
  before Vista plays through it unless the environment variable
  `OA_SOUND_OUTPUT` is `sdl`: SDL's output there wakes its thread every
  millisecond to ask how far the device has played, which takes a single
  processor more of its time. Any other Windows plays through it when
  `OA_SOUND_OUTPUT` is `waveout`, and a build without SDL always
  (`choose_sound_output`).

`SoftwareMixer` mixes in integer arithmetic, so that sound costs little on a
processor with slow floating point. It widens each stream's samples to 16
bits (8-bit unsigned ones around 128 times 256, 32-bit ones without their
low 16 bits, float ones times 32768, rounded and held to 16 bits), plays one
channel on both sides (more than two play their first two), converts other
rates with `PcmResampler` as the samples arrive, multiplies each side by the
stream's gain times that side's gain (16 fraction bits), sums the streams
with 8 fraction bits kept, and rounds the sum to 16 bits, held to
-32768..32767. A stream at 44100 Hz and a gain of one plays its 16-bit
samples unchanged. `PcmResampler` (`oa/audio/resampler.hpp`) is a
Kaiser-windowed sinc of 6 zero crossings each side (window 0.1102 * (80 -
8.7), zeros on the lower rate's frames) with 14-bit taps whose phases each
sum to one; the input is silent beyond its ends, N frames give
ceil(N * 44100 / rate), and its 32-bit output keeps a band-limited signal's
overshoot past full scale. `convert_for_mixer` keeps a converted sound at
half its level in 16 bits, for that overshoot, and the WAV player plays it
at twice the gain (`converted_sample_gain`). For the game's 361 sound files,
SDL's own stream conversion and mixing of the same sounds at the same gains
gives the same samples to within 4 steps of 16 bits (10 for the one file at
22254 Hz) and within 4 when eight play at once, under one step root mean
square; float samples agree within one. `BufferedOutput` takes the device
through its hooks (`oa::platform::sound_device::Hooks`), so its ring is
tested on a device the test plays by hand.

Tests: `audio-output` (the integer resampler's limits, lengths, splits,
accuracy and alignment; the formats, gains, side gains, pausing, clearing,
feeds and rate conversion of the mixer, and `convert_for_mixer`; the
buffered output's ring, its start count and a refused buffer),
`audio-sdl-player` (the WAV player's stream, voices and a placed start's
two sides) and on Windows `audio-output-wave-out`, which plays a tone on
the system's device, or is skipped where there is none.

## Offline mix

`oa-audio-offline-mix` (`oa/audio/offline_mix.hpp`, namespace
`oa::audio::offline_mix`) mixes the game's sound effects for director
renders: clips started at exact sample frames, mixed on demand into 16-bit
stereo at 48 kHz, with no sound device, no clock and no SDL. It reads sound
files only through its `ClipFileHooks` table and writes nothing but the
samples it returns; it never reads or writes simulation state.

Entry points: `decode_clip` (a sound file to a 48 kHz mono clip),
`centibel_gain` and `voice_gains` (a volume and a placement to Q15 gains),
`OfflineMix` (`start` queues a clip at a sample frame, `render` mixes the
next sample frames, `position`, `voices_playing`, `errors`) and
`wave_header` (the 44-byte header of a 48 kHz 16-bit stereo WAVE file).

Every step after decoding is integer arithmetic, so the same starts give the
same samples on every platform, and a mix rendered in several calls equals
one rendered in one call:

- Decoding takes the raw, DIGI and RIFF layouts `describe_wave` finds: 8-bit
  unsigned or 16-bit signed, mono or stereo, 4000 to 96000 Hz. 8-bit samples
  widen as `(x - 128) * 256`; stereo averages its two channels, rounding
  down. The clip is resampled to 48 kHz by exact rational phase: output
  sample n reads input position `n * rate / 48000` and interpolates linearly
  to the next input sample (0 past the end), rounding down; a clip of N
  input frames gives `ceil(N * 48000 / rate)` samples. A file larger than
  `max_clip_file_bytes`, a data span that runs past the file's end, no whole
  sample frame, or a clip of more than `max_clip_file_bytes` samples at
  48 kHz is refused with a message.
- `centibel_gain` turns hundredths of a decibel below `full_scale_volume`
  (0, the device's full volume, so the game's near volume of -585 plays
  5.85 dB below full scale, as in the game) into a Q15 gain from two tables typed into
  the source: `10^(-h/20)` for whole decibels 0 to 19 and `10^(-u/2000)` for
  hundredths 0 to 99, both Q30 and rounded to nearest; each further 20 dB
  divides by ten. Louder volumes play at full scale. `voice_gains` takes
  `spatial_stereo_gain`'s levels (float, with + - * / and square root only),
  rounds each down to Q15 and multiplies it by that gain.
- Each output sample is the sum over playing clips of
  `(clip sample * gain) >> 15`, times the master gain (Q15; by default the
  game's default effects volume, 27 << 10 over 65535) `>> 15`, saturated to
  16 bits.

The voice policy is the game's mixer's: at most eight voices; a start that
finds them all taken stops the voice that started first; each clip has up
to four buffers, made as starts need them, and a start reuses an idle one,
else makes one while fewer than four exist (the highest free index first),
else restarts the one that has played furthest (the lowest index among
equals). A restarted buffer keeps the voice it had and takes a second one,
so it counts twice toward the eight until it stops; when the older of its
two voices is the one evicted, the buffer stops, and its other voice is
freed at the next start. A clip that plays to its end frees its voices
before any start on the sample frame it ends on. Starts apply in the order
of their sample frames, and starts on one frame in the order they were
queued; a start queued for a frame already mixed applies at `position()`.

Clips are decoded once and cached under their path, with backslashes turned
into slashes and ASCII letters lowered; the hooks are asked for the path
with slashes and its letters as given. The cache holds `max_cached_clips`
clips; a full cache drops the least recently started clip that no voice
plays. A clip that cannot be read or decoded plays nothing, and its message
is kept once in `errors()`.

Tests: `audio-offline-mix` decodes synthetic 8-bit, 16-bit, stereo, DIGI,
headerless, 11025 Hz and 22254 Hz files to exact samples, refuses malformed
ones (a data span past the end, no data, a four-gigabyte count, unsupported
widths, channels and rates, oversized files and clips), checks the decibel
tables against a power function to one Q15 step, pins voice gains, the
voice limit, the restart bookkeeping, start order, saturation, errors and
the cache, checks that rendering in pieces (with starts queued ahead or just
in time) equals one render, pins the SHA-256 of a two-second synthetic
scene, and pins `wave_header`'s bytes. `audio-offline-mix-data` decodes
every sound file of the installed game.

Limitations: the mix plays each clip once. The looping sound, the stream and
the music are not mixed, so no voice is ever exempt from eviction.
