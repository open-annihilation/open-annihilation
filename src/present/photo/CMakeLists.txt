# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The name of a photo, and the text it carries. Later tickets add the sizes,
# the tiles, the looks and the depth cues.
add_library(oa-present-photo src/photo_file_name.cpp src/photo_texts.cpp)
add_library(oa::present::photo ALIAS oa-present-photo)
target_include_directories(oa-present-photo PUBLIC include)
target_compile_features(oa-present-photo PUBLIC cxx_std_20)
target_link_libraries(oa-present-photo PRIVATE oa-present oa-formats-json)

if(BUILD_TESTING)
  add_executable(oa-present-photo-test tests/photo_names_test.cpp)
  target_link_libraries(oa-present-photo-test PRIVATE oa-present-photo)
  add_test(NAME present-photo-names COMMAND oa-present-photo-test)

  add_executable(oa-present-photo-record-test tests/photo_record_test.cpp)
  target_link_libraries(oa-present-photo-record-test PRIVATE oa-present-photo oa-present
    oa-formats-json oa-options oa-base-game-math oa-base-bytes oa-base-text oa-base-threads)
  add_test(NAME present-photo-record COMMAND oa-present-photo-record-test)
endif()
