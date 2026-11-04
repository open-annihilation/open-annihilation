# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Rover: the Boneyards lobby client behind the 82-slot interface table.
# oa-boneyards-rover is the static library the extension links; 0.9 does
# not build the 32-bit drop-in library.

if(WIN32)
  set(oa_rover_platform_source src/platform_win32.cpp)
else()
  set(oa_rover_platform_source src/platform_posix.cpp)
endif()

# The master server list baked into Rover, used when neither serv_sav.byd
# nor server.byd holds a server. The file is in server.byd's format.
# cmake/OaBoneyards.cmake sets OA_BONEYARDS_MASTER_SERVERS before this
# directory is added.
if(NOT OA_BONEYARDS_MASTER_SERVERS)
  message(FATAL_ERROR "OA_BONEYARDS_MASTER_SERVERS is not set")
endif()
if(NOT IS_ABSOLUTE "${OA_BONEYARDS_MASTER_SERVERS}")
  get_filename_component(OA_BONEYARDS_MASTER_SERVERS "${OA_BONEYARDS_MASTER_SERVERS}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(NOT EXISTS "${OA_BONEYARDS_MASTER_SERVERS}" OR IS_DIRECTORY "${OA_BONEYARDS_MASTER_SERVERS}")
  message(FATAL_ERROR
    "OA_BONEYARDS_MASTER_SERVERS (\"${OA_BONEYARDS_MASTER_SERVERS}\") does not name a server list file")
endif()
set(oa_baked_master_servers_source "${CMAKE_CURRENT_BINARY_DIR}/generated/baked_master_servers.cpp")
add_custom_command(
  OUTPUT "${oa_baked_master_servers_source}"
  COMMAND ${CMAKE_COMMAND}
    "-DINPUT=${OA_BONEYARDS_MASTER_SERVERS}"
    "-DOUTPUT=${oa_baked_master_servers_source}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedMasterServers.cmake"
  DEPENDS "${OA_BONEYARDS_MASTER_SERVERS}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedMasterServers.cmake"
  COMMENT "Baking the master server list"
  VERBATIM
)

add_library(oa-boneyards-rover STATIC
  src/bags.cpp
  src/battles.cpp
  src/cipher.cpp
  src/connect.cpp
  src/file_server.cpp
  src/games.cpp
  src/handlers.cpp
  src/heap.cpp
  src/http_fetch.cpp
  src/interface.cpp
  src/link.cpp
  src/lobby.cpp
  src/md5.cpp
  src/platform_select.cpp
  src/profiles.cpp
  src/record.cpp
  src/records.cpp
  src/requests.cpp
  src/rooms.cpp
  src/sector_fetch.cpp
  src/sector_map.cpp
  src/servers.cpp
  "${oa_baked_master_servers_source}"
  src/session.cpp
  src/small_block_heap.cpp
  src/sockets.cpp
  src/state.cpp
  src/strings.cpp
  src/wire_text.cpp
  ${oa_rover_platform_source}
)
add_library(oa::boneyards::rover ALIAS oa-boneyards-rover)
target_include_directories(oa-boneyards-rover PUBLIC include)
target_compile_features(oa-boneyards-rover PUBLIC c_std_11 cxx_std_20)
if(TARGET oa-options)
  target_link_libraries(oa-boneyards-rover PRIVATE oa-options oa-platform-shims)
elseif(MSVC)
  target_compile_options(oa-boneyards-rover PRIVATE /W4 /permissive-)
  target_link_libraries(oa-boneyards-rover PRIVATE oa-platform-shims)
else()
  target_compile_options(oa-boneyards-rover PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(oa-boneyards-rover PRIVATE oa-platform-shims)
endif()
if(WIN32)
  target_link_libraries(oa-boneyards-rover PUBLIC ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(oa-boneyards-rover PUBLIC Threads::Threads)
endif()

# Record and queue headers the client reads. They stay beside the sources.
add_library(oa-boneyards-rover-detail INTERFACE)
add_library(oa::boneyards::rover_detail ALIAS oa-boneyards-rover-detail)
target_include_directories(oa-boneyards-rover-detail INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(oa-boneyards-rover-detail INTERFACE oa-boneyards-rover)

if(BUILD_TESTING)
  add_executable(oa-boneyards-rover-platform-test tests/rover_platform_test.cpp)
  target_link_libraries(oa-boneyards-rover-platform-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-platform
           COMMAND oa-boneyards-rover-platform-test "${CMAKE_CURRENT_SOURCE_DIR}/tests/rover_platform_test.cpp")
  add_executable(oa-boneyards-rover-record-test tests/rover_record_test.cpp)
  target_link_libraries(oa-boneyards-rover-record-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-record COMMAND oa-boneyards-rover-record-test)
  add_executable(oa-boneyards-rover-session-unit-test tests/rover_session_test.cpp)
  target_link_libraries(oa-boneyards-rover-session-unit-test PRIVATE oa-boneyards-rover)
  target_compile_definitions(oa-boneyards-rover-session-unit-test PRIVATE
    "OA_BAKED_MASTER_SERVERS_PATH=\"${OA_BONEYARDS_MASTER_SERVERS}\"")
  add_test(NAME rover-session-unit COMMAND oa-boneyards-rover-session-unit-test)
  add_executable(oa-boneyards-rover-reach-test tests/rover_reach_test.cpp)
  target_link_libraries(oa-boneyards-rover-reach-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-reach COMMAND oa-boneyards-rover-reach-test)
  add_executable(oa-boneyards-rover-lobby-test tests/rover_lobby_test.cpp)
  target_link_libraries(oa-boneyards-rover-lobby-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-lobby COMMAND oa-boneyards-rover-lobby-test)
  add_executable(oa-boneyards-rover-cipher-test tests/rover_cipher_test.cpp)
  target_link_libraries(oa-boneyards-rover-cipher-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-cipher COMMAND oa-boneyards-rover-cipher-test)
  add_executable(oa-boneyards-rover-link-test tests/rover_link_test.cpp)
  target_link_libraries(oa-boneyards-rover-link-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-link COMMAND oa-boneyards-rover-link-test)
  add_executable(oa-boneyards-rover-link-lifecycle-test tests/rover_link_lifecycle_test.cpp)
  target_link_libraries(oa-boneyards-rover-link-lifecycle-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-link-lifecycle COMMAND oa-boneyards-rover-link-lifecycle-test)
  # Needs OA_BY_LIVE_SERVER, OA_BY_USER and OA_BY_PASSWORD; skipped otherwise.
  add_executable(oa-boneyards-rover-login-live-test tests/rover_login_live_test.cpp)
  target_link_libraries(oa-boneyards-rover-login-live-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-login-live COMMAND oa-boneyards-rover-login-live-test)
  set_tests_properties(rover-login-live PROPERTIES LABELS rover-live SKIP_RETURN_CODE 77 TIMEOUT 120)
  if(NOT WIN32)
    # The bycomms reference server (github.com/coreprime/bycomms), built into
    # the main checkout's local/bin: go build -o local/bin/bycomms ./cmd/bycomms
    if(NOT DEFINED OA_BYCOMMS_BINARY)
      execute_process(
        COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --path-format=absolute --git-common-dir
        OUTPUT_VARIABLE oa_rover_git_common_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      get_filename_component(oa_rover_checkout "${oa_rover_git_common_dir}" DIRECTORY)
      set(oa_rover_bycomms_default "${oa_rover_checkout}/local/bin/bycomms")
    endif()
    set(OA_BYCOMMS_BINARY "${oa_rover_bycomms_default}" CACHE FILEPATH
        "bycomms reference server for the rover-login-reference test")
    add_executable(oa-boneyards-rover-login-reference-test tests/rover_login_reference_test.cpp)
    target_link_libraries(oa-boneyards-rover-login-reference-test PRIVATE oa-boneyards-rover)
    add_test(NAME rover-login-reference COMMAND oa-boneyards-rover-login-reference-test ${OA_BYCOMMS_BINARY})
    set_tests_properties(rover-login-reference PROPERTIES LABELS rover-reference TIMEOUT 60)
    # Skipped only where the server was never built; once it was, a missing
    # binary fails the test.
    if(NOT EXISTS "${OA_BYCOMMS_BINARY}")
      message(STATUS "rover-login-reference will be skipped: no bycomms at '${OA_BYCOMMS_BINARY}'")
      set_tests_properties(rover-login-reference PROPERTIES SKIP_RETURN_CODE 77)
    endif()
  endif()
  foreach(data_test IN ITEMS small-block-heap strings bags records sector-map file-server wire-text)
    string(REPLACE "-" "_" data_source "${data_test}")
    add_executable(oa-boneyards-rover-${data_test}-test tests/rover_${data_source}_test.cpp)
    target_link_libraries(oa-boneyards-rover-${data_test}-test PRIVATE oa-boneyards-rover)
    add_test(NAME rover-${data_test} COMMAND oa-boneyards-rover-${data_test}-test)
  endforeach()
  # The record and sector-map readers over the 3.1c Boneyards files of the
  # BY1117a install OA_GAME_DIR names.
  foreach(data_test IN ITEMS records sector-map)
    target_link_libraries(oa-boneyards-rover-${data_test}-test PRIVATE oa-boneyards-tests-installs)
    add_test(NAME rover-${data_test}-data COMMAND oa-boneyards-rover-${data_test}-test --data)
    oa_boneyards_install_tests(rover-${data_test}-data)
  endforeach()
  add_executable(oa-boneyards-rover-interface-test tests/rover_interface_test.cpp)
  target_link_libraries(oa-boneyards-rover-interface-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-interface COMMAND oa-boneyards-rover-interface-test)
  # Chat to a server that takes UTF-8, and to one that does not.
  add_executable(oa-boneyards-rover-language-test tests/rover_language_test.cpp)
  target_link_libraries(oa-boneyards-rover-language-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-language COMMAND oa-boneyards-rover-language-test)
  set_tests_properties(rover-language PROPERTIES TIMEOUT 60)
  add_executable(oa-boneyards-rover-sector-fetch-test tests/rover_sector_fetch_test.cpp)
  target_link_libraries(oa-boneyards-rover-sector-fetch-test PRIVATE oa-boneyards-rover)
  add_test(NAME rover-sector-fetch COMMAND oa-boneyards-rover-sector-fetch-test)
  # Inside the engine's build the tests compile with its options, as the
  # library does.
  if(TARGET oa-options)
    get_property(oa_rover_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    list(FILTER oa_rover_targets INCLUDE REGEX "^oa-boneyards-rover-.+-test$")
    foreach(test_target IN LISTS oa_rover_targets)
      target_link_libraries(${test_target} PRIVATE oa-options oa-platform-shims)
    endforeach()
  endif()
endif()
