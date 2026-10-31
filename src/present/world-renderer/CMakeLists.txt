# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

add_library(oa-present-world-renderer src/world_renderer.cpp src/unit_renderer.cpp src/camera.cpp src/radar.cpp src/display_modes.cpp
  src/overlays.cpp src/fog.cpp src/terrain_view.cpp src/draw_order.cpp src/scene_filter.cpp src/scene_filter_reference.cpp)
add_library(oa::present::world_renderer ALIAS oa-present-world-renderer)
target_link_libraries(oa-present-world-renderer PRIVATE oa-base-game-math)
target_include_directories(oa-present-world-renderer PUBLIC include)
target_link_libraries(oa-present-world-renderer PUBLIC oa-formats-hpi oa-formats-tnt oa-sim-model-runtime oa-options oa-formats-gaf
  oa-core-types oa-present oa-sim-visibility-state oa-platform-job-pool)

add_executable(oa-render-terrain EXCLUDE_FROM_ALL tools/render_crop.cpp)
target_link_libraries(oa-render-terrain PRIVATE oa-present-world-renderer oa-platform-shims)

add_executable(oa-render-unit EXCLUDE_FROM_ALL tools/render_unit.cpp)
target_link_libraries(oa-render-unit PRIVATE oa-present-world-renderer oa-platform-shims)

if(BUILD_TESTING)
  add_executable(oa-world-renderer-test tests/world_renderer_test.cpp)
  target_link_libraries(oa-world-renderer-test PRIVATE oa-present-world-renderer oa-test-support)
  add_test(NAME world-renderer COMMAND oa-world-renderer-test)
  add_executable(oa-world-screen-span-test tests/world_screen_span_test.cpp)
  target_link_libraries(oa-world-screen-span-test PRIVATE oa-present-world-renderer)
  add_test(NAME world-screen-span COMMAND oa-world-screen-span-test)
  add_executable(oa-world-camera-test tests/world_camera_test.cpp)
  target_link_libraries(oa-world-camera-test PRIVATE oa-present-world-renderer oa-test-support)
  add_test(NAME world-camera COMMAND oa-world-camera-test)
  add_executable(oa-world-overlays-test tests/world_overlays_test.cpp)
  target_link_libraries(oa-world-overlays-test PRIVATE oa-present-world-renderer oa-test-support)
  add_test(NAME world-overlays COMMAND oa-world-overlays-test)
  add_executable(oa-world-fog-test tests/world_fog_test.cpp)
  target_link_libraries(oa-world-fog-test PRIVATE oa-present-world-renderer oa-test-support)
  add_test(NAME world-fog COMMAND oa-world-fog-test)
  # The terrain fill and the fog drawn in bands on pools of 2, 3, 4 and 8
  # threads give the bytes the calling thread alone draws.
  add_executable(oa-world-draw-bands-test tests/world_draw_bands_test.cpp)
  target_link_libraries(oa-world-draw-bands-test PRIVATE oa-present-world-renderer oa-platform-job-pool)
  add_test(NAME world-draw-bands COMMAND oa-world-draw-bands-test)
  # The area pass against its references, across band splits and pools of
  # 1 to 8 threads, with a pinned digest; it logs its time at 1080p. The
  # nearest resample against its mapping, and a scene of the map drawn 1:1
  # and resampled at a zoom against the terrain filled at that zoom, on
  # pools of 2, 3, 4 and 8 threads.
  add_executable(oa-world-scene-filter-test tests/world_scene_filter_test.cpp)
  target_link_libraries(oa-world-scene-filter-test PRIVATE oa-present-world-renderer oa-platform-job-pool
    oa-test-support)
  add_test(NAME world-scene-filter COMMAND oa-world-scene-filter-test)
  # How steady the zoomed battlefield stays as the view moves: a thin line
  # through the area pass, today's point sampling and the thin-line rule,
  # and the shimmer of the area pass and of the card's filters' references
  # against point sampling and NEAREST.
  add_executable(oa-world-zoom-stability-test tests/world_zoom_stability_test.cpp)
  target_link_libraries(oa-world-zoom-stability-test PRIVATE oa-present-world-renderer oa-test-support)
  add_test(NAME world-zoom-stability COMMAND oa-world-zoom-stability-test)
  add_executable(oa-world-draw-order-test tests/world_draw_order_test.cpp)
  target_link_libraries(oa-world-draw-order-test PRIVATE oa-present-world-renderer)
  add_test(NAME world-draw-order COMMAND oa-world-draw-order-test)
  add_executable(oa-world-radar-test tests/world_radar_test.cpp)
  target_link_libraries(oa-world-radar-test PRIVATE oa-present-world-renderer oa-test-game-data oa-test-support)
  add_test(NAME world-radar COMMAND oa-world-radar-test)
  # The radar picture of an installed map.
  oa_add_game_data_test(world-radar-data oa-world-radar-test --data)
  add_executable(oa-world-terrain-view-test tests/world_terrain_view_test.cpp)
  target_link_libraries(oa-world-terrain-view-test PRIVATE oa-present-world-renderer oa-test-game-data)
  add_test(NAME world-terrain-view COMMAND oa-world-terrain-view-test)
  # The terrain view over the installed game's maps.
  oa_add_game_data_test(world-terrain-view-data oa-world-terrain-view-test --data)
  set_tests_properties(world-terrain-view-data PROPERTIES TIMEOUT 300)
endif()

if(BUILD_TESTING)
  add_executable(oa-world-camera-clamp-test tests/world_camera_clamp_test.cpp)
  target_link_libraries(oa-world-camera-clamp-test PRIVATE oa-present-world-renderer)
  add_test(NAME world-camera-clamp COMMAND oa-world-camera-clamp-test)
endif()
