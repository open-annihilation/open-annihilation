# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# A registry's catalogue: the list of packages it offers, and the check that
# the bytes are the ones a trusted key signed. Nothing here fetches.
add_library(oa-data-catalogue src/catalogue.cpp src/references.cpp src/times.cpp
  src/signature_file.cpp src/check.cpp)
add_library(oa::data::catalogue ALIAS oa-data-catalogue)
target_include_directories(oa-data-catalogue PUBLIC include)
target_compile_features(oa-data-catalogue PUBLIC cxx_std_20)
target_link_libraries(oa-data-catalogue PUBLIC oa-base-sha256 oa-base-signing oa-data-registry
  oa-formats-url oa-formats-oamod oa-formats-oamut oa-formats-oabrain oa-options
  PRIVATE oa-formats-json
  oa-data-content-bundles)

if(BUILD_TESTING)
  # Catalogue entries, and the signature, sequence and expiry check.
  add_executable(oa-data-catalogue-test tests/catalogue_test.cpp)
  target_link_libraries(oa-data-catalogue-test PRIVATE oa-data-catalogue oa-test-support)
  add_test(NAME data-catalogue COMMAND oa-data-catalogue-test)
  add_executable(oa-data-catalogue-check-test tests/check_test.cpp)
  target_link_libraries(oa-data-catalogue-check-test PRIVATE oa-data-catalogue oa-test-support)
  add_test(NAME data-catalogue-check COMMAND oa-data-catalogue-check-test)
  set_tests_properties(data-catalogue data-catalogue-check PROPERTIES TIMEOUT 60)
endif()
