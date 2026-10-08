// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Included before every C++ source of a build for Windows 95 whose
// cross-compiler uses the Windows thread backend (mingw-w64-common.cmake),
// so that the C++ run-time library's thread support declares the condition
// variables it needs.
//
// GCC's gthr-win32.h gives the run-time library condition variables only where
// the version compiled for has them, and Windows has them from Vista on. A
// build whose sources are compiled against the declarations of Windows 95 would
// therefore not compile <mutex>, which <atomic> reaches, at all. The two macros
// below tell it to have them here. mingw-w64 declares the type and the calls
// either way; oa-platform-xp-runtime defines the calls over the ones Windows 95
// does have. Nothing else is taken from a later Windows: a call one later added
// still does not compile.

#if defined(_WIN32) && !defined(OA_WINDOWS_95_GTHR_HPP)
#define OA_WINDOWS_95_GTHR_HPP 1

// Read while <bits/gthr.h> is included, which the first C++ header of a
// translation unit does.
#ifndef __GTHREAD_HAS_COND
#define __GTHREAD_HAS_COND 1
#endif
#ifndef __GTHREADS_CXX0X
#define __GTHREADS_CXX0X 1
#endif

#endif // OA_WINDOWS_95_GTHR_HPP
