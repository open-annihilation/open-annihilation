# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The sign-in window: WebView2 on Windows when OA_WEBVIEW2_DIR names the
# unpacked Microsoft.Web.WebView2 package, and a window that never opens
# elsewhere. Visual Studio builds only: the package's loader is a library of
# its linker, and WebView2 needs Windows 10 or later, so a Windows XP build
# goes without.
set(OA_WEBVIEW2_DIR "" CACHE PATH "The unpacked Microsoft.Web.WebView2 NuGet package, for the GOG sign-in window")
set(oa_webview2_arch "")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(oa_webview2_arch arm64)
  else()
    set(oa_webview2_arch x64)
  endif()
else()
  set(oa_webview2_arch x86)
endif()
set(oa_webview2_loader "${OA_WEBVIEW2_DIR}/build/native/${oa_webview2_arch}/WebView2LoaderStatic.lib")
if(WIN32 AND MSVC AND NOT OA_WINDOWS_XP AND OA_WEBVIEW2_DIR AND EXISTS "${oa_webview2_loader}"
   AND EXISTS "${OA_WEBVIEW2_DIR}/build/native/include/WebView2.h")
  add_library(oa-platform-web-sign-in src/windows_web_sign_in.cpp)
  target_include_directories(oa-platform-web-sign-in PRIVATE "${OA_WEBVIEW2_DIR}/build/native/include")
  target_link_libraries(oa-platform-web-sign-in PRIVATE "${oa_webview2_loader}" version ole32 shell32
    user32 gdi32 advapi32)
  message(STATUS "GOG sign-in window: WebView2 from ${OA_WEBVIEW2_DIR}")
else()
  add_library(oa-platform-web-sign-in src/web_sign_in.cpp)
  if(WIN32 AND NOT OA_WINDOWS_XP)
    message(STATUS "GOG sign-in window: none; set OA_WEBVIEW2_DIR to build it")
  endif()
endif()
add_library(oa::platform::web_sign_in ALIAS oa-platform-web-sign-in)
target_include_directories(oa-platform-web-sign-in PUBLIC include)
target_compile_features(oa-platform-web-sign-in PUBLIC cxx_std_20)
