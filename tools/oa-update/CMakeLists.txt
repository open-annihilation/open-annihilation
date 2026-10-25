# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# oa-update's steps. The executable stays in the root CMakeLists.txt, where
# the Windows manifest, the DLL copy and the packaged notices attach to it.
add_library(oa-update-core STATIC
  src/journal.cpp
  src/lock.cpp
  src/log.cpp
  src/routes.cpp
  src/run.cpp
  src/start.cpp
  src/unpack.cpp
  src/verify.cpp
  src/wait.cpp
  src/window.cpp)
add_library(oa::update::core ALIAS oa-update-core)
target_include_directories(oa-update-core PUBLIC include)
target_include_directories(oa-update-core PRIVATE ${PROJECT_SOURCE_DIR}/src/app/include)
target_compile_features(oa-update-core PUBLIC cxx_std_20)
# The root's per-source definitions do not reach a subdirectory target.
# Only a self-check build accepts --feed-key and the plain-folder route.
target_compile_definitions(oa-update-core PUBLIC "OA_SELF_CHECKS_BUILT=$<BOOL:${OA_SELF_CHECKS}>")
target_link_libraries(oa-update-core
  PUBLIC oa-app-update
  PRIVATE oa-data-engine-feed oa-base-sha256 oa-base-signing oa-formats-zip oa-formats-json
    oa-ui-kit oa-ui-paint oa-platform-text-font oa-platform-shims oa-options SDL3::SDL3)

if(BUILD_TESTING)
  add_executable(oa-update-test tests/update_test.cpp)
  target_link_libraries(oa-update-test
    PRIVATE oa-update-core oa-app-update oa-data-engine-feed oa-data-catalogue oa-base-signing
      oa-base-sha256 oa-formats-zip oa-formats-url oa-ui-kit oa-test-support oa-options)
  add_test(NAME tools-oa-update COMMAND oa-update-test)
  set_tests_properties(tools-oa-update PROPERTIES TIMEOUT 60)
  if(WIN32)
    add_custom_command(TARGET oa-update-test POST_BUILD
      COMMAND ${CMAKE_COMMAND} "-DDLLS=$<TARGET_RUNTIME_DLLS:oa-update-test>"
        "-DDEST=$<TARGET_FILE_DIR:oa-update-test>" -P "${PROJECT_SOURCE_DIR}/tools/copy_runtime.cmake")
  endif()
endif()
