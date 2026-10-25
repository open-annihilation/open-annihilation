# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The signed engine feed: the releases one channel offers. The signature is
# checked before any byte is parsed. Nothing here fetches or opens a file.
add_library(oa-data-engine-feed src/feed.cpp src/check.cpp src/choose.cpp src/write.cpp)
add_library(oa::data::engine_feed ALIAS oa-data-engine-feed)
target_include_directories(oa-data-engine-feed PUBLIC include)
target_compile_features(oa-data-engine-feed PUBLIC cxx_std_20)
target_link_libraries(oa-data-engine-feed
  PUBLIC oa-data-registry oa-formats-oamod oa-formats-url oa-base-sha256 oa-options
  PRIVATE oa-data-catalogue oa-formats-json oa-base-signing)

if(BUILD_TESTING)
  add_executable(oa-data-engine-feed-test tests/feed_test.cpp)
  target_link_libraries(oa-data-engine-feed-test PRIVATE oa-data-engine-feed oa-data-catalogue
    oa-base-signing oa-test-support)
  add_test(NAME data-engine-feed COMMAND oa-data-engine-feed-test)
  set_tests_properties(data-engine-feed PROPERTIES TIMEOUT 30)
endif()
