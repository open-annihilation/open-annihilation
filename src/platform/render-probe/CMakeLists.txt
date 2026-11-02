# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# What the game can learn about the renderer SDL made and the adapter behind
# it. The pure tables build everywhere; each system builds the readers of
# its own graphics interfaces, which reach them through SDL's objects or
# functions looked up at run time, so no graphics library is linked.
if(NOT TARGET SDL3::SDL3)
  return()
endif()
add_library(oa-platform-render-probe src/names.cpp src/probe.cpp src/probe_gl.cpp src/probe_gpu.cpp)
add_library(oa::platform::render_probe ALIAS oa-platform-render-probe)
if(WIN32)
  target_sources(oa-platform-render-probe PRIVATE src/probe_windows.cpp)
endif()
if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(oa-platform-render-probe PRIVATE src/probe_vulkan.cpp)
endif()
if(APPLE)
  enable_language(OBJCXX)
  target_sources(oa-platform-render-probe PRIVATE src/probe_apple.mm)
  set_target_properties(oa-platform-render-probe PROPERTIES OBJCXX_STANDARD 20
    OBJCXX_STANDARD_REQUIRED ON)
  target_link_libraries(oa-platform-render-probe PRIVATE "-framework Foundation")
endif()
target_include_directories(oa-platform-render-probe PUBLIC include)
target_compile_features(oa-platform-render-probe PUBLIC cxx_std_20)
target_link_libraries(oa-platform-render-probe PRIVATE SDL3::SDL3 oa-options)

if(BUILD_TESTING)
  # The software and virtual rasteriser names, the drivers that report a
  # fixed texture limit or need their adapter read, the class the
  # accelerated tier has been run on, the classification and cleaning of
  # what a driver reports, what describe reads through a stand-in reader,
  # the video drivers whose windows have a framebuffer of their own, SDL's
  # software renderer on the dummy video driver described, the adapter of a
  # graphics device where the video driver makes one, and on Windows no
  # graphics library among the executable's imports.
  add_executable(oa-platform-render-probe-test tests/render_probe_test.cpp)
  target_link_libraries(oa-platform-render-probe-test PRIVATE oa-platform-render-probe SDL3::SDL3
    oa-test-support oa-options)
  add_test(NAME platform-render-probe COMMAND oa-platform-render-probe-test)
  set_tests_properties(platform-render-probe PROPERTIES TIMEOUT 60
    ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=dummy")
endif()
