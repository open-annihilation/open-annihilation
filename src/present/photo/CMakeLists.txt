# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The name of a photo, the text it carries, the sizes it can offer, the tiles
# of a Full photo past the card, and a look over its picture, including a
# tilt-shift and haze by ground height.
add_library(oa-present-photo src/photo_file_name.cpp src/photo_sizes.cpp src/photo_tiles.cpp
  src/photo_poster.cpp src/photo_texts.cpp src/looks.cpp)
add_library(oa::present::photo ALIAS oa-present-photo)
target_include_directories(oa-present-photo PUBLIC include)
target_compile_features(oa-present-photo PUBLIC cxx_std_20)
target_link_libraries(oa-present-photo PRIVATE oa-present oa-formats-json oa-platform-job-pool
  oa-platform-text-font)

if(BUILD_TESTING)
  add_executable(oa-present-photo-test tests/photo_names_test.cpp)
  target_link_libraries(oa-present-photo-test PRIVATE oa-present-photo)
  add_test(NAME present-photo-names COMMAND oa-present-photo-test)

  add_executable(oa-present-photo-record-test tests/photo_record_test.cpp)
  target_link_libraries(oa-present-photo-record-test PRIVATE oa-present-photo oa-present
    oa-formats-json oa-options oa-base-game-math oa-base-bytes oa-base-text oa-base-threads)
  add_test(NAME present-photo-record COMMAND oa-present-photo-record-test)

  add_executable(oa-present-photo-sizes-test tests/photo_sizes_test.cpp)
  target_link_libraries(oa-present-photo-sizes-test PRIVATE oa-present-photo)
  add_test(NAME present-photo-sizes COMMAND oa-present-photo-sizes-test)

  add_executable(oa-present-photo-poster-test tests/photo_poster_test.cpp)
  target_link_libraries(oa-present-photo-poster-test PRIVATE oa-present-photo)
  add_test(NAME present-photo-poster COMMAND oa-present-photo-poster-test)

  add_executable(oa-present-photo-tiles-test tests/photo_tiles_test.cpp)
  target_link_libraries(oa-present-photo-tiles-test PRIVATE oa-present-photo)
  add_test(NAME present-photo-tiles COMMAND oa-present-photo-tiles-test)

  add_executable(oa-present-photo-looks-test tests/looks_test.cpp)
  target_link_libraries(oa-present-photo-looks-test PRIVATE oa-present-photo oa-platform-job-pool
    oa-platform-text-font oa-base-sha256)
  oa_copy_text_fonts(oa-present-photo-looks-test
    "$<TARGET_FILE_DIR:oa-present-photo-looks-test>")
  add_test(NAME present-photo-looks COMMAND oa-present-photo-looks-test)

  add_executable(oa-present-photo-depth-test tests/depth_test.cpp)
  target_link_libraries(oa-present-photo-depth-test PRIVATE oa-present-photo oa-platform-job-pool
    oa-base-sha256)
  add_test(NAME present-photo-depth COMMAND oa-present-photo-depth-test)
endif()
