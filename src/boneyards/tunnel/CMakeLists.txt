# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Relay tunnel: the frame codec, the handshake and the resumable stream.
add_library(oa-boneyards-tunnel
  src/hmac.cpp
  src/stream.cpp
  src/tunnel.cpp
  src/wire.cpp)
add_library(oa::boneyards::tunnel ALIAS oa-boneyards-tunnel)
target_include_directories(oa-boneyards-tunnel PUBLIC include)
target_compile_features(oa-boneyards-tunnel PUBLIC cxx_std_20)
target_link_libraries(oa-boneyards-tunnel
  PUBLIC oa-options oa-base-sha256
  PRIVATE oa-netgame)

if(BUILD_TESTING)
  add_executable(oa-boneyards-tunnel-wire-test tests/wire_test.cpp)
  target_link_libraries(oa-boneyards-tunnel-wire-test PRIVATE oa-boneyards-tunnel oa-test-support)
  add_test(NAME boneyards-tunnel-wire COMMAND oa-boneyards-tunnel-wire-test)

  add_executable(oa-boneyards-tunnel-hmac-test tests/hmac_test.cpp)
  target_link_libraries(oa-boneyards-tunnel-hmac-test PRIVATE oa-boneyards-tunnel oa-test-support)
  add_test(NAME boneyards-tunnel-hmac COMMAND oa-boneyards-tunnel-hmac-test)

  add_executable(oa-boneyards-tunnel-stream-test tests/stream_test.cpp)
  target_link_libraries(oa-boneyards-tunnel-stream-test PRIVATE oa-boneyards-tunnel oa-test-support)
  add_test(NAME boneyards-tunnel-stream COMMAND oa-boneyards-tunnel-stream-test)

  add_executable(oa-boneyards-tunnel-session-test tests/session_test.cpp)
  target_link_libraries(oa-boneyards-tunnel-session-test PRIVATE
    oa-boneyards-tunnel oa-netgame oa-test-support)
  add_test(NAME boneyards-tunnel-session COMMAND oa-boneyards-tunnel-session-test)
  set_tests_properties(boneyards-tunnel-session PROPERTIES TIMEOUT 60)

  # The launch check runs this on 127.0.0.1. It is not a test of its own.
  add_executable(oa-boneyards-test-relay tests/relay_main.cpp)
  target_link_libraries(oa-boneyards-test-relay PRIVATE oa-boneyards-tunnel oa-netgame)
endif()
