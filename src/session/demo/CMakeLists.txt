# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# TA Demo (.tad) playback through the match's network receive path, and the
# recording of a network game: its stream on disk and its replay (.oademo).
add_library(oa-session-demo src/demo_clock.cpp src/demo_keyframe.cpp src/demo_playback.cpp
  src/demo_recording.cpp src/demo_summary.cpp src/imported_recording.cpp src/recording_stream.cpp
  src/replay_record.cpp)
add_library(oa::session::demo ALIAS oa-session-demo)
target_include_directories(oa-session-demo PUBLIC include)
target_compile_features(oa-session-demo PUBLIC cxx_std_20)
target_link_libraries(oa-session-demo
  PUBLIC oa-formats-tad oa-formats-oademo oa-base-threads oa-netgame-match-runtime oa-netgame
    oa-ui-frontend-multiplayer
  PRIVATE oa-base-text oa-base-sha256 oa-base-zstd oa-options oa-formats-sqsh oa-data-mutators
    oa-data-brain-library oa-present)

if(BUILD_TESTING)
  add_executable(oa-session-demo-test tests/demo_playback_test.cpp)
  target_link_libraries(oa-session-demo-test PRIVATE oa-session-demo oa-formats-oademo oa-formats-zip oa-options
    oa-test-match oa-test-support)
  add_test(NAME demo-playback COMMAND oa-session-demo-test)
  set_tests_properties(demo-playback PROPERTIES TIMEOUT 120)
  add_executable(oa-session-demo-clock tests/demo_clock_test.cpp)
  target_link_libraries(oa-session-demo-clock PRIVATE oa-session-demo)
  add_test(NAME session-demo-clock COMMAND oa-session-demo-clock)
  set_tests_properties(session-demo-clock PROPERTIES TIMEOUT 30)
endif()
