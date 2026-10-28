# Job pool

A small fixed pool of worker threads that runs a job's bands with the calling
thread. Target `oa-platform-job-pool`, header `oa/platform/job_pool.hpp`,
namespace `oa::platform::job_pool`.

The drawing passes that work row by row hand their rows to it in bands: the
terrain fill (`fill_scaled_viewport`) and the fog (`draw_fog_grid`) in
[the world renderer](../../present/world-renderer/README.md), the
battlefield's units, features and effects (`draw_world_band`), the
conversion of each frame to the window's pixel format before it is shown,
the effects' light and heat (`renderer_effects::compose`) and the 3D view's
depth under them (`live_scene_visibility`) ([src/app](../../app/README.md)).

## Results do not depend on the threads

Each band writes only its own rows and reads nothing another band writes,
so the order bands run in, and the threads they run on, never change a byte
of the result. A pool of any size gives the frame one thread gives.

The terrain fill, the fog, the conversion and the effects split their work
into bands by its data, never by the number of threads: the terrain fill,
the conversion and the effects by 32 rows, the fog by one row of its grid,
and the 3D view's depth by 16 rows, or one band without a pool. Each band
of the effects takes every light in turn, and each band of the 3D view's
depth the triangles that reach it in their order, so each pixel takes them
in the order one band gives. The battlefield's
draws take one band for each thread instead: the model bridge splits into
that many bands of whole tile rows, only where its rows and the frame's
split alike (`bridge_split` in
[the model module](../../present/model/README.md)), and each band draws
every draw of the frame with its own rows alone, each draw over a band's
edge worked out as over the whole frame. Any number of those bands gives
the frame one band gives, so these results do not depend on the threads
either.

## Entry points

- `Pool(threads)` starts `threads - 1` workers; the calling thread is the
  last. `Pool(1)` starts none, so a job on it runs its bands in order on the
  calling thread, with no memory or start-up cost beyond the object.
- `run(bands, entry, context)` and the `run_bands(pool, bands, band)`
  template run every band and return once all have run. `run_bands` with a
  null pool runs the bands in order on the calling thread, which is what the
  drawing passes do when they are given no pool.
- `default_threads(processors)` is the policy the game uses: 1 on a machine
  of one or two logical processors; otherwise one fewer than the processors,
  at most `default_thread_limit` (4). The game's `--draw-threads N` and the
  `OA_DRAW_THREADS` environment variable override it, up to `max_threads`.
- `bands_of_rows(rows, band_rows)` counts the bands that cover a pass's rows.

## How a job runs

Each worker waits on its own `WakeEvent` (`oa/platform/lock.hpp`), started
by `start_thread` (`oa/platform/system.hpp`) with a 64 KiB stack request; the
pool uses no other threads or locks. `run` publishes the job, wakes one
worker for every band beyond the first, and takes bands itself from a shared
counter until none is left. Workers take bands from the same counter. A
worker counts itself in before it looks for the job, so once the job is
withdrawn and the count is back to zero no worker still reads it; the
calling thread checks the count a few thousand times and then waits for
the worker that leaves last to signal. A worker woken late finds no job, or
the next one, and joins that.

One thread runs a job on a pool at a time. A job started while another runs,
from inside a band or from another thread, runs its bands in order on the
thread that started it, with the same result.

The destructor tells the workers to end, wakes them and waits until each has
counted itself out, the last thing a worker does with the pool.

## Limits

- `start_thread` treats the stack size as advisory; where the host ignores
  it a worker gets the system's default stack. The bands themselves keep no
  scratch of their own beyond a few local variables.
- A band must not throw.
- The pool is for passes that take a millisecond or so: waking a worker
  costs some microseconds, so a pass of a few bands gains nothing on it.

## Tests

`platform-job-pool` checks the policy, that a one-thread pool runs bands in
order on the calling thread, that pools of 2, 3, 4 and 8 threads fill the same
rows as no pool over 200 jobs each, that every band runs exactly once for
band counts from 0 to 513, that a job started from a band runs inline, and
that pools start, run and stop twenty times over and stop without having run
a job.
