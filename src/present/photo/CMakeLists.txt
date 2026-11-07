# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The name of a photo. Later tickets add the details, the sizes, the tiles,
# the looks and the depth cues.
add_library(oa-present-photo src/photo_file_name.cpp)
add_library(oa::present::photo ALIAS oa-present-photo)
target_include_directories(oa-present-photo PUBLIC include)
target_compile_features(oa-present-photo PUBLIC cxx_std_20)

if(BUILD_TESTING)
  add_executable(oa-present-photo-test tests/photo_names_test.cpp)
  target_link_libraries(oa-present-photo-test PRIVATE oa-present-photo)
  add_test(NAME present-photo-names COMMAND oa-present-photo-test)
endif()
