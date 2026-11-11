# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Writes the tests a build registers, as ctest --show-only=json-v1 lists
# them, to OUT, for check-native-online. CTEST is the build's ctest, BUILD
# the build folder and CONFIG the configuration (may be empty). ctest runs
# this with the build's own CMake, so the listing is written wherever the
# tests run, also where the check's Python cannot start ctest, as Windows
# Python under Wine cannot start a Linux ctest.

if(NOT DEFINED CTEST OR CTEST STREQUAL "" OR NOT DEFINED BUILD OR BUILD STREQUAL "" OR
   NOT DEFINED OUT OR OUT STREQUAL "")
  message(FATAL_ERROR "write_test_listing: -DCTEST=, -DBUILD= and -DOUT= are required")
endif()

file(REMOVE "${OUT}" "${OUT}.partial")
get_filename_component(oa_listing_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${oa_listing_dir}")
set(oa_listing_command "${CTEST}" --test-dir "${BUILD}" --show-only=json-v1)
if(NOT "${CONFIG}" STREQUAL "")
  list(APPEND oa_listing_command -C "${CONFIG}")
endif()
execute_process(COMMAND ${oa_listing_command}
  OUTPUT_FILE "${OUT}.partial" ERROR_VARIABLE oa_listing_error RESULT_VARIABLE oa_listing_result)
if(NOT oa_listing_result EQUAL 0)
  file(REMOVE "${OUT}.partial")
  string(STRIP "${oa_listing_error}" oa_listing_error)
  message(FATAL_ERROR "write_test_listing: ${CTEST} --show-only=json-v1 failed (${oa_listing_result}): ${oa_listing_error}")
endif()
file(RENAME "${OUT}.partial" "${OUT}")
message(STATUS "write_test_listing: ${OUT}")
