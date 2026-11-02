# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The values a Windows laptop's graphics drivers read from the game's
# executable to start it on the discrete graphics card. The drivers read the
# executable's own export table, so the game links the object itself, as an
# object library: a static library's object that nothing calls would be left
# out of the link.
if(WIN32)
  add_library(oa-platform-graphics-preference OBJECT src/graphics_preference.cpp)
  add_library(oa::platform::graphics_preference ALIAS oa-platform-graphics-preference)
  target_compile_features(oa-platform-graphics-preference PUBLIC cxx_std_20)
  target_link_libraries(oa-platform-graphics-preference PRIVATE oa-options)
endif()
