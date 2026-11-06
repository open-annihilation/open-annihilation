# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The installs the module's tests read: network play's test installs, with
# the multiplayer install and the files of the 3.1c install
# (support/include/oa/boneyards_test).
add_library(oa-boneyards-tests-installs INTERFACE)
add_library(oa::boneyards::tests_installs ALIAS oa-boneyards-tests-installs)
target_include_directories(oa-boneyards-tests-installs INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/support/include")
target_link_libraries(oa-boneyards-tests-installs INTERFACE oa-netgame-test-installs
  oa-platform-shims)
