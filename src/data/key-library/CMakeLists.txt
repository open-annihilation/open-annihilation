# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Installed key layouts: each folder's oakeys.yaml and, when it reads, its
# origin record. It opens those two files and never a package.
add_library(oa-data-key-library src/library.cpp)
add_library(oa::data::key_library ALIAS oa-data-key-library)
target_include_directories(oa-data-key-library PUBLIC include)
target_compile_features(oa-data-key-library PUBLIC cxx_std_20)
target_link_libraries(oa-data-key-library PUBLIC oa-formats-oakeys PRIVATE oa-options)

if(BUILD_TESTING)
  # A scratch Keys folder listed from its manifests: good layouts, an own
  # layout, a broken and an oversized manifest, a staging folder, a link,
  # two folders with one id, a missing folder, and an own layout written
  # whole.
  add_executable(oa-data-key-library-test tests/library_test.cpp)
  target_link_libraries(oa-data-key-library-test PRIVATE oa-data-key-library oa-test-support
    oa-options)
  add_test(NAME data-key-library COMMAND oa-data-key-library-test)
  set_tests_properties(data-key-library PROPERTIES TIMEOUT 30)
endif()
