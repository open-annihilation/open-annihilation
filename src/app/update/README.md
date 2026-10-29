# app/update

This build's version and package id, when a check for a new release is due,
the worker that fetches the signed engine feed, the package download, the
update centre's one state with its words and actions, the player's choices,
and the plan, state and receipt files the game and `oa-update` share. The
verified bytes are kept under the data folder. The main thread reads
snapshots. Nothing here installs a package.

[build.hpp](include/oa/app/update/build.hpp),
[centre.hpp](include/oa/app/update/centre.hpp),
[check.hpp](include/oa/app/update/check.hpp),
[checker.hpp](include/oa/app/update/checker.hpp),
[choices.hpp](include/oa/app/update/choices.hpp),
[download.hpp](include/oa/app/update/download.hpp),
[offer.hpp](include/oa/app/update/offer.hpp),
[plan.hpp](include/oa/app/update/plan.hpp) and
[receipt.hpp](include/oa/app/update/receipt.hpp) are the entry points
(`oa::app::update`).

## Entry points

- `this_build` names the version, the release number and the package id
  compiled into this module. `package_id` is the same rule for a platform
  name: `windows-95` when the build is a Windows 95 build, `steam-deck`
  when the platform is `linux-x86_64` and the program folder holds a
  `steam-deck` directory, and the platform name otherwise.
- `read_check_mode`, `write_check_mode`, `read_checked` and `write_checked`
  are the preference values. `check_due` is true only for Automatically:
  at the first check of a run, whatever the age of the last check that
  reached the server, and again an hour after the last automatic check of
  this run began, or when that time is in the future. `automatic_check_starts`
  is that hour at the main menu first and then anywhere, and
  `update_notice_shows` is the main menu, never a match.
- `offer_text` writes the question's title, the size of the offered
  package's zip, and the reassurance. The notes are the release's own
  lines and are not looked up. `notes_paragraph` joins those lines under
  "New in {version}:", one bulleted line each. `offer_wanted` is false at
  or below the release `read_skipped` names. `write_skipped` remembers
  that number.
- `Checker` fetches one feed and its signature on a worker thread. `start`
  does nothing while a check is running. `cancel` asks it to stop, and a
  cancelled check is not a finished one. `snapshot` is the latest result,
  and a failed one says why as `CheckFailure`: the server could not be
  reached, the feed was refused, or this machine could not store it.
  `remember` publishes the offer of the feed an earlier check stored, after
  checking its signature again, with no request.
- `PackageDownload` fetches one package in steps of a mebibyte, publishing
  the bytes and the rate (`RateMeter`) after each. A match, or the
  player's `pause`, stops it at the next step; `resume` carries on.
  `discard` stops it and deletes what it stored. A package already
  downloaded is checked again and used without a fetch
  (`package_downloaded`). A failure says why as `DownloadFailure`.
- `centre_state` decides the update centre's `Stage` from what the runtime
  gathers in `CentreInputs`, with the `Act`s that stage allows.
  `centre_words` writes its status, detail, the short form for the menu
  pill and the headers, a note, and each action's caption; the install
  action names the route (`install_way`). `step_words` and
  `recovery_steps` write a receipt's steps and the steps that put an
  unfinished recovery right.
- `read_scheduled`, `write_scheduled` and `clear_scheduled` keep the
  release that installs when the game quits; `installs_at_quit` is the
  only test a quit makes. `read_held`, `write_held`, `clear_held` and
  `release_held` keep a release that did not install until the player
  tries again. `read_install_policy` reads Install updates. `clear_skipped`
  takes a skip back.
- `read_plan` and `plan_text` are the plan the game writes for the helper.
  `helper_inherits_open_files` is false: the helper is not given the files
  this process still has open, so it can rename the fonts folder once this
  process has left. `windows_helper_command` is the command line that
  starts it.
  `read_state`, `state_text` and `write_state` are the record of what was
  installed and what was kept. `count_failed_start` counts a start that has
  not reached the main menu, `uncount_quit_start` takes back one the player
  quit, and `start_end` tells how a watched start ended.
- `read_receipt`, `receipt_text`, `write_receipt` and `load_receipt` are
  the receipt the helper leaves: its steps, its `Outcome`, the versions and
  the times. `failed_outcome`, `holds_release` and `failure_outcome` read
  and decide outcomes. This module does not install a package.

## State

Each worker keeps one job and one snapshot. The verified feed and its
signature, the downloaded package, the state file and the receipt are
files under `<data>/updates`. The mode, the time of the last check that
reached the server, the skipped, scheduled and held releases and Install
updates are preference values. The runtime holds the workers for the run.

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
- The first automatic check of a run is at the settled main menu, whatever
  the age of the last check that reached the server. Another starts an hour
  after that one began, including during a match, and a release found there
  waits for the next main menu. A failed check is tried again at the next
  hour. Never makes no request. Starting a match still cancels a check that
  is waiting, because the load can outlast it.
- Quitting installs only a release scheduled for the quit
  (`installs_at_quit`). Keeping a package ready schedules nothing.
- A quit before the main menu is not a failed start; only a start that
  crashed, or stopped with an error, stays counted.

## Tests

`app-update-build` covers the package id, including Windows 95 and the
Steam Deck folder, and `this_build` against the version compiled into the
test. `app-update-check` covers when a check is due, including the first
menu and the hour, the preference values, and a `Checker` against a feed
served on 127.0.0.1: an offer, a release already running, a remembered
stored feed, a missing feed, a refused feed, a check that runs out of
time, and a cancel, with each failure's kind.
`app-update-plan` covers the plan and state texts and the refusals: an
unknown format or route, a relative path, an empty program, and a title or
line that is too long. `app-update-download` covers the download against a
local server: a resume, a match's pause, the player's pause and resume, a
cancel that keeps nothing, a package already downloaded, and each
failure's kind. `app-update-centre` covers the state each set of facts
gives and its actions, the English words, the route's install words,
scheduling and taking it back, Later kept apart from Skip, a held release,
each failure's words, the time left and the rate. `app-update-receipt`
covers the receipt's names, outcomes, round trip, refusals and file, and
the state file's quit marker.

## Limitations

- The channel is `stable`. A preview channel is later work.
- `rollout` and the oldest version that may play online are not acted on.
  That waits for 0.9.
- The update centre's state is decided here, and the menu pill and the
  redrawn Settings › Updates that show it with Pause, Cancel and the
  scheduling buttons are later work; the screens that exist read it where
  they can.
