# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Installed brains: each folder's oabrain.yaml and, when it reads, its
# origin record. It opens those two files and never a script, a network or
# a table.
add_library(oa-data-brain-library src/library.cpp)
add_library(oa::data::brain_library ALIAS oa-data-brain-library)
target_include_directories(oa-data-brain-library PUBLIC include)
target_compile_features(oa-data-brain-library PUBLIC cxx_std_20)
target_link_libraries(oa-data-brain-library PUBLIC oa-formats-oabrain
  PRIVATE oa-formats-oamod oa-options)

if(BUILD_TESTING)
  # A scratch Brains folder listed from its manifests: good brains, broken
  # and missing manifests, the installer's own folders, a link, two folders
  # with one id, origin records, network sizes and a missing folder.
  add_executable(oa-data-brain-library-test tests/library_test.cpp)
  target_link_libraries(oa-data-brain-library-test PRIVATE oa-data-brain-library
    oa-test-support oa-options)
  add_test(NAME data-brain-library COMMAND oa-data-brain-library-test)
  set_tests_properties(data-brain-library PROPERTIES TIMEOUT 30)
endif()
