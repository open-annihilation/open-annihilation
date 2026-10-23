# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Integer network runner. A checked .oann model becomes buffers, and a run
# uses only integer arithmetic.
add_library(oa-data-neural src/network.cpp src/kernels.cpp)
add_library(oa::data::neural ALIAS oa-data-neural)
target_include_directories(oa-data-neural PUBLIC include)
target_compile_features(oa-data-neural PUBLIC cxx_std_20)
target_link_libraries(oa-data-neural PUBLIC oa-formats-oann PRIVATE oa-options)

if(BUILD_TESTING)
  add_executable(oa-data-neural-test tests/neural_test.cpp)
  target_link_libraries(oa-data-neural-test PRIVATE
    oa-data-neural oa-formats-oann oa-base-sha256 oa-test-support oa-options)
  add_test(NAME data-neural COMMAND oa-data-neural-test)
  set_tests_properties(data-neural PROPERTIES TIMEOUT 60)
endif()
