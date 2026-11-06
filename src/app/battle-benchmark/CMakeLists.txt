# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-app-battle-benchmark src/battle_benchmark.cpp)
add_library(oa::app::battle_benchmark ALIAS oa-app-battle-benchmark)
target_include_directories(oa-app-battle-benchmark PUBLIC include)
target_compile_features(oa-app-battle-benchmark PUBLIC cxx_std_20)
target_link_libraries(oa-app-battle-benchmark PRIVATE oa-options)

if(BUILD_TESTING)
  add_executable(oa-app-battle-benchmark-test tests/battle_benchmark_test.cpp)
  target_link_libraries(oa-app-battle-benchmark-test PRIVATE oa-app-battle-benchmark oa-options)
  add_test(NAME app-battle-benchmark COMMAND oa-app-battle-benchmark-test)
  set_tests_properties(app-battle-benchmark PROPERTIES TIMEOUT 30)
endif()
