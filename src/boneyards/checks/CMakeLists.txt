# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The checks that drive a running game through the engine's check host.
# The extension links this library; the sources are not compiled into it.
# cmake/OaBoneyards.cmake adds this directory only when self-checks are built.
add_library(oa-boneyards-checks STATIC
  runtime_boneyards_check.cpp
  runtime_boneyards_chat_check.cpp)
add_library(oa::boneyards::checks ALIAS oa-boneyards-checks)
target_include_directories(oa-boneyards-checks PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oa-boneyards-checks
  PUBLIC oa-boneyards-console oa-boneyards-client oa-boneyards-screens
    oa-boneyards-extension-options oa-boneyards-console-private
  PRIVATE oa::app::headers oa-platform-preferences oa-formats-hpi SDL3::SDL3 oa-options
    oa-platform-shims)
target_compile_features(oa-boneyards-checks PUBLIC cxx_std_20)
