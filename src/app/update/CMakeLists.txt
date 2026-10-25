# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# This build's version and package id, when a new release is due, and the
# worker that fetches the signed engine feed. The verified bytes are kept
# under the data folder. No SDL.
add_library(oa-app-update src/build.cpp src/check.cpp src/checker.cpp src/offer.cpp src/plan.cpp)
add_library(oa::app::update ALIAS oa-app-update)
target_include_directories(oa-app-update PUBLIC include)
target_compile_features(oa-app-update PUBLIC cxx_std_20)
target_link_libraries(oa-app-update
  PUBLIC oa-data-engine-feed oa-data-registry oa-formats-oamod oa-platform-preferences oa-options
    oa-ui-library
  PRIVATE oa-app-headers oa-netgame-http oa-base-threads oa-platform-shims oa-base-sha256
    oa-formats-json oa-ui-engine-settings)
# The root's per-source definitions do not reach a subdirectory target.
set_property(SOURCE src/build.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\""
  "OA_ENGINE_VERSION_LABEL=\"${OA_VERSION_LABEL}\"")

if(BUILD_TESTING)
  add_executable(oa-app-update-build-test tests/build_test.cpp)
  target_link_libraries(oa-app-update-build-test PRIVATE oa-app-update oa-app-headers oa-test-support)
  set_property(SOURCE tests/build_test.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\""
    "OA_ENGINE_VERSION_LABEL=\"${OA_VERSION_LABEL}\"")
  add_test(NAME app-update-build COMMAND oa-app-update-build-test)
  set_tests_properties(app-update-build PROPERTIES TIMEOUT 60)

  add_executable(oa-app-update-check-test tests/check_test.cpp)
  target_link_libraries(oa-app-update-check-test
    PRIVATE oa-app-update oa-netgame-http oa-netgame-http-fixture oa-base-signing oa-base-sha256
      oa-data-catalogue oa-base-threads oa-test-support)
  add_test(NAME app-update-check COMMAND oa-app-update-check-test)
  set_tests_properties(app-update-check PROPERTIES TIMEOUT 60)

  add_executable(oa-app-update-offer-test tests/offer_test.cpp)
  target_link_libraries(oa-app-update-offer-test PRIVATE oa-app-update oa-app-headers oa-test-support)
  add_test(NAME app-update-offer COMMAND oa-app-update-offer-test)
  set_tests_properties(app-update-offer PROPERTIES TIMEOUT 30)

  add_executable(oa-app-update-plan-test tests/plan_test.cpp)
  target_link_libraries(oa-app-update-plan-test PRIVATE oa-app-update oa-test-support oa-options)
  add_test(NAME app-update-plan COMMAND oa-app-update-plan-test)
  set_tests_properties(app-update-plan PROPERTIES TIMEOUT 30)

  if(WIN32)
    foreach(target oa-app-update-build-test oa-app-update-check-test oa-app-update-offer-test
        oa-app-update-plan-test)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DDLLS=$<TARGET_RUNTIME_DLLS:${target}>"
          "-DDEST=$<TARGET_FILE_DIR:${target}>" -P "${PROJECT_SOURCE_DIR}/tools/copy_runtime.cmake")
    endforeach()
  endif()
endif()
