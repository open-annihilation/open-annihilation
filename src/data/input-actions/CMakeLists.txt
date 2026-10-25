# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The action registry as a constant table. Nothing in the game reads it yet.
add_library(oa-data-input-actions src/actions.cpp)
add_library(oa::data::input_actions ALIAS oa-data-input-actions)
target_include_directories(oa-data-input-actions PUBLIC include)
target_compile_features(oa-data-input-actions PUBLIC cxx_std_20)
target_link_libraries(oa-data-input-actions PRIVATE oa-options)
