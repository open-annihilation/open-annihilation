# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Options for the pinned HarfBuzz that tools/bootstrap_text_fonts.py,
# tools/bootstrap_macos_deps.py and tools/bootstrap_windows_deps.py build,
# read after HarfBuzz's project() call.
#
# With Visual Studio's compiler, HarfBuzz's objects name no C library (/Zl),
# as FreeType's do (freetype_build_options.cmake beside this file), and they
# leave out the standard library's marks of the C library and the iterator
# debug level they were built with. The release build of HarfBuzz then links
# into a Debug program too: HarfBuzz's interface is C, and no standard
# library object crosses it.
if(MSVC)
  add_compile_options(/Zl)
  add_compile_definitions(_ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH _ALLOW_RUNTIME_LIBRARY_MISMATCH)
endif()
