# app/update

This build's version and package id, when a check for a new release is due,
and the worker that fetches the signed engine feed. The verified bytes are
kept under the data folder. The main thread reads a snapshot. Nothing is
downloaded or installed here.

[build.hpp](include/oa/app/update/build.hpp),
[check.hpp](include/oa/app/update/check.hpp),
[checker.hpp](include/oa/app/update/checker.hpp) and
[plan.hpp](include/oa/app/update/plan.hpp) are the entry points
(`oa::app::update`).

## Entry points

- `this_build` names the version, the release number and the package id
  compiled into this module. `package_id` is the same rule for a platform
  name: `windows-95` when the build is a Windows 95 build, `steam-deck`
  when the platform is `linux-x86_64` and the program folder holds a
  `steam-deck` directory, and the platform name otherwise.
- `read_check_mode`, `write_check_mode`, `read_checked` and `write_checked`
  are the preference values. `check_due` is true only for Automatically,
  when the last check that reached the server is absent, at least a day
  old, or in the future.
- `Checker` fetches one feed and its signature on a worker thread. `start`
  does nothing while a check is running. `cancel` asks it to stop, and a
  cancelled check is not a finished one. `snapshot` is the latest result.
- `read_plan` and `plan_text` are the plan the game writes for the helper.
  `read_state`, `state_text` and `write_state` are the record of what was
  installed and what was kept. This module does not install a package.

## State

The worker keeps one job and one snapshot. The verified feed and its
signature are files under `<data>/updates`. The mode and the time of the
last check that reached the server are preference values. The runtime
holds the worker for the run.

## Invariants

- The feed's address is `feed_address`. A test feed replaces it for one
  run. An empty key list means the engine's pinned keys.
- The worker sends the game's own user agent and no other header, and no
  query. It does not fetch the signature unless the feed came back whole.
- `check_feed` runs only on those two responses. `choose` runs only on an
  accepted feed. The files are written only after the feed verifies, and
  not when the check was cancelled first.
- A check that reached the server reports `checked_at`. A failure to
  resolve, connect or finish in time does not, and neither does a cancel.
- One automatic check a run. Never makes no request. A match cancels a
  check that is still running.

## Tests

`app-update-build` covers the package id, including Windows 95 and the
Steam Deck folder, and `this_build` against the version compiled into the
test. `app-update-check` covers when a check is due, the preference values,
and a `Checker` against a feed served on 127.0.0.1: an offer, a release
already running, a missing feed, a refused feed, a check that runs out of
time, and a cancel. `app-update-plan` covers the plan and state texts and
the refusals: an unknown format or route, a relative path, an empty
program, and a title or line that is too long.

## Limitations

- The channel is `stable`. A preview channel is later work.
- `rollout` and the oldest version that may play online are not acted on.
  That waits for 0.9.
- A verified feed is only offered. Downloading the package, and the
  settings page that asks for a check, are later work.
