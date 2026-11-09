# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The player's replays, as the Library lists them: the .oademo and .tad files
# below Recordings, each read for its description or its header, and the
# index under Recordings/.replays that keeps what was read (a campaign's
# mission name, a player's number, the index pass's timeline and the tick a
# Watch was left at), and the replays the player keeps, in a file of their
# own beside it.
add_library(oa-data-replays src/replays.cpp src/index.cpp src/kept.cpp src/kinds.cpp src/days.cpp
  src/content.cpp src/import.cpp src/retention.cpp src/feed.cpp src/share.cpp)
add_library(oa::data::replays ALIAS oa-data-replays)
target_include_directories(oa-data-replays PUBLIC include)
target_compile_features(oa-data-replays PUBLIC cxx_std_20)
target_link_libraries(oa-data-replays
  PUBLIC oa-options
  PRIVATE oa-formats-oademo oa-formats-tad oa-formats-json oa-formats-oamod oa-base-sha256
    oa-data-saves oa-platform-shims)

if(BUILD_TESTING)
  # A scratch Recordings folder of made-up replays: a scan lists only the
  # replays (not the partial games, the hidden folders, the content notes or
  # what lies too deep) and reads each; the index round trip; a second scan
  # that takes unchanged files from the index, timeline and watched tick with
  # them; a campaign mission name kept with its trailing space; an index
  # deleted, damaged or junk and rebuilt with every replay still kept; the
  # kept marks by
  # recording, surviving a move, a rename and a damaged file; a .tad's header
  # with a Windows-1252 name; damaged files listed as damaged; the kinds
  # extensions register; grouping by day; an import into Imported that leaves
  # sector 6 out, selects the same identity again and refuses a file that
  # does not parse; which replays the retention rule takes, on either side of
  # 30, 90 and 365 days; the replays a kept file set aside spares, those
  # recorded before it was set aside; and a Core Prime feed: a page, one
  # replay with its needs, an empty next, an unknown member, a damaged feed,
  # an empty feed, an error, and an id that is refused.
  add_executable(oa-data-replays-test tests/replays_test.cpp tests/content_test.cpp
    tests/feed_test.cpp tests/share_test.cpp)
  target_link_libraries(oa-data-replays-test PRIVATE oa-data-replays oa-formats-oademo
    oa-formats-tad oa-formats-zip oa-base-sha256 oa-test-support oa-options)
  add_test(NAME data-replays COMMAND oa-data-replays-test)
  set_tests_properties(data-replays PROPERTIES TIMEOUT 60)
endif()
