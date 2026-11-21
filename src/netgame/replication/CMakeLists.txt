# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-netgame-replication
  deltas.cpp
  replication.cpp
  route_batches.cpp)
add_library(oa::netgame::replication ALIAS oa-netgame-replication)
target_include_directories(oa-netgame-replication PUBLIC include)
target_link_libraries(oa-netgame-replication PUBLIC oa-netgame oa-core-types oa-options)

if(BUILD_TESTING)
  add_executable(oa-netgame-replication-test tests/replication_test.cpp)
  target_link_libraries(oa-netgame-replication-test PRIVATE oa-netgame-replication oa-formats-tad)
  add_test(NAME net-replication COMMAND oa-netgame-replication-test)
  set_tests_properties(net-replication PROPERTIES TIMEOUT 120)
endif()
