# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Fetches catalogues and packages. HTTPS uses the system's TLS where this
# system has it. OA_HTTP_TLS=OFF compiles the stub and https is read as http.
option(OA_HTTP_TLS "Fetch HTTPS with the system's TLS" ON)
add_library(oa-netgame-http
  src/url_request.cpp
  src/response_reader.cpp
  src/gzip.cpp
  src/lookup.cpp
  src/client.cpp
  src/form.cpp
  src/tls.cpp)
add_library(oa::netgame::http ALIAS oa-netgame-http)
target_include_directories(oa-netgame-http PUBLIC include)
target_compile_features(oa-netgame-http PUBLIC cxx_std_20)
target_link_libraries(oa-netgame-http
  PUBLIC oa-options
  PRIVATE oa-netgame oa-formats-url oa-base-threads ZLIB::ZLIB)
set_property(SOURCE src/client.cpp APPEND
  PROPERTY COMPILE_DEFINITIONS "OA_ENGINE_VERSION=\"${PROJECT_VERSION}\"")

if(OA_HTTP_TLS)
  target_compile_definitions(oa-netgame-http PRIVATE OA_HTTP_TLS=1)
  if(APPLE)
    enable_language(OBJCXX)
    target_sources(oa-netgame-http PRIVATE src/tls_apple.mm)
    set_source_files_properties(src/tls_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_target_properties(oa-netgame-http PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_link_libraries(oa-netgame-http PRIVATE "-framework Foundation")
  elseif(WIN32)
    # Loaded at run time. No import library, so a 95-era toolchain without
    # winhttp.h still compiles, and a machine without WinHTTP still starts.
    target_sources(oa-netgame-http PRIVATE src/tls_winhttp.cpp)
  elseif(UNIX)
    target_sources(oa-netgame-http PRIVATE src/tls_openssl.cpp)
    target_link_libraries(oa-netgame-http PRIVATE ${CMAKE_DL_LIBS})
  else()
    target_sources(oa-netgame-http PRIVATE src/tls_none.cpp)
  endif()
else()
  target_compile_definitions(oa-netgame-http PRIVATE OA_HTTP_TLS=0)
  target_sources(oa-netgame-http PRIVATE src/tls_none.cpp)
endif()

if(BUILD_TESTING)
  add_executable(oa-netgame-http-test tests/client_test.cpp)
  target_link_libraries(oa-netgame-http-test
    PRIVATE oa-netgame-http oa-netgame-http-fixture oa-test-support oa-netgame oa-base-threads)
  add_test(NAME net-http COMMAND oa-netgame-http-test)
  set_tests_properties(net-http PROPERTIES TIMEOUT 120)
  # Where the client loads libssl at run time, the test checks that a system
  # with libssl gets HTTPS.
  if(OA_HTTP_TLS AND UNIX AND NOT APPLE)
    target_compile_definitions(oa-netgame-http-test PRIVATE OA_HTTP_TLS_OPENSSL=1)
    target_link_libraries(oa-netgame-http-test PRIVATE ${CMAKE_DL_LIBS})
  endif()

  # The reader is compiled into this program so the test does not link the
  # client library, which would define the reader twice.
  add_executable(oa-netgame-http-response-test
    tests/response_reader_test.cpp
    src/response_reader.cpp)
  target_include_directories(oa-netgame-http-response-test PRIVATE src include)
  target_link_libraries(oa-netgame-http-response-test PRIVATE oa-test-support oa-options)
  target_compile_features(oa-netgame-http-response-test PRIVATE cxx_std_20)
  add_test(NAME net-http-response COMMAND oa-netgame-http-response-test)
  set_tests_properties(net-http-response PROPERTIES TIMEOUT 30)

  add_executable(oa-netgame-http-form-test tests/form_test.cpp src/form.cpp)
  target_include_directories(oa-netgame-http-form-test PRIVATE include)
  target_link_libraries(oa-netgame-http-form-test PRIVATE oa-test-support oa-options)
  target_compile_features(oa-netgame-http-form-test PRIVATE cxx_std_20)
  add_test(NAME net-http-form COMMAND oa-netgame-http-form-test)
  set_tests_properties(net-http-form PROPERTIES TIMEOUT 30)

  if(WIN32)
    foreach(target oa-netgame-http-test oa-netgame-http-response-test oa-netgame-http-form-test)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} "-DDLLS=$<TARGET_RUNTIME_DLLS:${target}>"
          "-DDEST=$<TARGET_FILE_DIR:${target}>" -P "${PROJECT_SOURCE_DIR}/tools/copy_runtime.cmake")
    endforeach()
  endif()
endif()
