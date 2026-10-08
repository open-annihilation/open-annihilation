# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The functions Windows 95 lacks that the C++ run-time library and SDL call.
# oa-platform-xp-runtime's counterparts cover the ones Windows XP lacks, and the
# two are separate modules because the lists barely overlap: what XP wants is
# what Vista added, and what Windows 95 wants is the wide half of the Win32 and
# C run-time surfaces, which that system exports as stubs that fail.
#
# They are objects of the program rather than a library on the link line. An
# import library SDL names is linked before any library of ours, so a function
# SDL calls has been linked from it by the time ours arrives and a second
# definition is what the linker sees rather than a replacement it takes. An
# object of the program is linked before all of them.
if(MINGW AND OA_WINDOWS_95)
  add_library(
    oa-platform-win95-runtime OBJECT src/windows_95_crt.cpp src/windows_95_kernel.cpp src/windows_95_user.cpp
                                         src/windows_95_shell.cpp
  )
  add_library(oa::platform::win95_runtime ALIAS oa-platform-win95-runtime)
  target_compile_features(oa-platform-win95-runtime PUBLIC cxx_std_20)
  target_link_libraries(oa-platform-win95-runtime PRIVATE oa-options)
  target_link_libraries(
    oa-options INTERFACE
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:$<TARGET_OBJECTS:oa-platform-win95-runtime>>"
  )
endif()
