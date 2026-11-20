# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# A test whose command is the game must not use the shared data folder or the
# default online checks. add_test is wrapped so each such command gains
# --data-dir on a folder of its own and preferences that set
# open-annihilation.update-check to never and open-annihilation.content-updates
# to never, or to library when the command enters the Boneyards with -y.
# The mod entry check stores the update check as when-asked, because never
# asks to turn the online features on. A preferences file the test already names,
# under this build tree, gains the two lines. The test's name and its other
# arguments stay as they were.

function(oa_command_is_game cmd out_var)
  list(LENGTH cmd oa_count)
  if(oa_count EQUAL 0)
    set("${out_var}" FALSE PARENT_SCOPE)
    return()
  endif()
  list(GET cmd 0 oa_exe)
  if(oa_exe STREQUAL "oa-game" OR oa_exe MATCHES "(^|/)open-annihilation(\\.exe)?$"
      OR oa_exe MATCHES "TARGET_FILE:oa-game>")
    set("${out_var}" TRUE PARENT_SCOPE)
  else()
    set("${out_var}" FALSE PARENT_SCOPE)
  endif()
endfunction()

function(oa_list_has_flag cmd flag out_var)
  set(oa_found FALSE)
  foreach(oa_arg IN LISTS cmd)
    if(oa_arg STREQUAL flag)
      set(oa_found TRUE)
    endif()
  endforeach()
  set("${out_var}" "${oa_found}" PARENT_SCOPE)
endfunction()

function(oa_flag_value cmd flag out_var)
  set(oa_take FALSE)
  set(oa_value "")
  foreach(oa_arg IN LISTS cmd)
    if(oa_take)
      set(oa_value "${oa_arg}")
      set(oa_take FALSE)
    elseif(oa_arg STREQUAL flag)
      set(oa_take TRUE)
    endif()
  endforeach()
  set("${out_var}" "${oa_value}" PARENT_SCOPE)
endfunction()

# Seconds since the epoch at the start of this configure. A preferences file
# written earlier is a game's save from a previous run, not the text this
# test authors. The seed must not put that save back.
string(TIMESTAMP oa_configure_stamp "%s")

# update_mode is never, when-asked, or on. on is a check that serves its own
# release feed and must leave the update check able to run. when-asked does
# not look for a release at startup, and is not the off word, so the Boneyards
# mod entry reaches its mod question instead of asking to turn the checks on.
function(oa_ensure_offline_preferences path library update_mode)
  if(EXISTS "${path}")
    file(TIMESTAMP "${path}" oa_mtime "%s")
    if(oa_mtime LESS oa_configure_stamp)
      file(WRITE "${path}" "open-annihilation-preferences 1\n")
    endif()
  endif()
  if(NOT EXISTS "${path}")
    file(WRITE "${path}" "open-annihilation-preferences 1\n")
  endif()
  file(READ "${path}" oa_body)
  if(NOT oa_body MATCHES "\n$")
    string(APPEND oa_body "\n")
  endif()
  if(update_mode STREQUAL "on")
    string(REGEX REPLACE
      "\"open-annihilation.update-check\" \"(never|when-asked)\"\n" "" oa_body "${oa_body}")
  elseif(update_mode STREQUAL "when-asked")
    string(REGEX REPLACE "\"open-annihilation.update-check\" \"(never|automatically)\""
      "\"open-annihilation.update-check\" \"when-asked\"" oa_body "${oa_body}")
    if(NOT oa_body MATCHES "open-annihilation.update-check")
      string(APPEND oa_body "\"open-annihilation.update-check\" \"when-asked\"\n")
    endif()
  else()
    string(REGEX REPLACE "\"open-annihilation.update-check\" \"(automatically|when-asked)\""
      "\"open-annihilation.update-check\" \"never\"" oa_body "${oa_body}")
    if(NOT oa_body MATCHES "open-annihilation.update-check")
      string(APPEND oa_body "\"open-annihilation.update-check\" \"never\"\n")
    endif()
  endif()
  # A file left by an earlier run may already say never or library. The
  # value this launch needs replaces either.
  if(library)
    string(REGEX REPLACE "\"open-annihilation.content-updates\" \"(never|automatically)\""
      "\"open-annihilation.content-updates\" \"library\"" oa_body "${oa_body}")
  else()
    string(REGEX REPLACE "\"open-annihilation.content-updates\" \"(library|automatically)\""
      "\"open-annihilation.content-updates\" \"never\"" oa_body "${oa_body}")
  endif()
  file(WRITE "${path}" "${oa_body}")
  file(READ "${path}" oa_body)
  if(NOT oa_body MATCHES "open-annihilation.content-updates")
    if(library)
      file(APPEND "${path}" "\"open-annihilation.content-updates\" \"library\"\n")
    else()
      file(APPEND "${path}" "\"open-annihilation.content-updates\" \"never\"\n")
    endif()
  endif()
  # The seed is what this registration intends. A fixture may remove the
  # preferences file, and the game may rewrite it; the seed is copied back
  # and is what check-native-online reads. A language-notice start file is
  # the copy the fixture puts over the live file, so it gains the same lines.
  file(READ "${path}" oa_body)
  file(WRITE "${path}.offline-seed" "${oa_body}")
  if(path MATCHES "\\.conf$" AND NOT path MATCHES "\\.start\\.conf$")
    string(REGEX REPLACE "\\.conf$" ".start.conf" oa_start "${path}")
    if(EXISTS "${oa_start}")
      oa_ensure_offline_preferences("${oa_start}" "${library}" "${update_mode}")
    endif()
  endif()
endfunction()

# prefs_out and data_out are paths to append, or empty when that flag is
# already on the command (a preferences file under this build tree is patched
# in place). The caller appends the flags to add_test. Setting the COMMAND
# property afterwards does not change the command ctest runs.
function(oa_offline_suffix oa_name cmd prefs_out data_out)
  if(NOT BUILD_TESTING)
    set("${prefs_out}" "" PARENT_SCOPE)
    set("${data_out}" "" PARENT_SCOPE)
    return()
  endif()
  oa_command_is_game("${cmd}" oa_is_game)
  if(NOT oa_is_game)
    set("${prefs_out}" "" PARENT_SCOPE)
    set("${data_out}" "" PARENT_SCOPE)
    return()
  endif()
  set(oa_cmd "${cmd}")
  set(oa_library FALSE)
  set(oa_update_mode "never")
  foreach(oa_arg IN LISTS oa_cmd)
    if(oa_arg STREQUAL "-y" OR oa_arg MATCHES "enter-boneyards")
      set(oa_library TRUE)
    endif()
    # never on the update check asks to turn the online features on, and the
    # mod question this check waits for never opens. when-asked does not
    # fetch a release.
    if(oa_arg STREQUAL "--check-boneyards-mod-entry")
      set(oa_library TRUE)
      set(oa_update_mode "when-asked")
    endif()
    if(oa_arg STREQUAL "--check-engine-feed" OR oa_arg STREQUAL "--check-update-offer")
      set(oa_update_mode "on")
    endif()
    # The welcome setup starts with no preferences file. A file here skips it.
    if(oa_arg STREQUAL "--check-first-run" OR oa_arg STREQUAL "--check-setup-window")
      set(oa_skip_prefs TRUE)
    endif()
  endforeach()
  if(NOT oa_skip_prefs)
    set(oa_skip_prefs FALSE)
  endif()
  set(oa_dir "${CMAKE_BINARY_DIR}/offline-preferences/${oa_name}")
  file(MAKE_DIRECTORY "${oa_dir}/data")
  set(oa_add_prefs "")
  if(NOT oa_skip_prefs)
    oa_list_has_flag("${oa_cmd}" "--preferences-file" oa_has_prefs)
    if(oa_has_prefs)
      oa_flag_value("${oa_cmd}" "--preferences-file" oa_prefs)
      string(FIND "${oa_prefs}" "${CMAKE_BINARY_DIR}/" oa_in_build)
      if(oa_prefs MATCHES "\\$<" OR oa_prefs STREQUAL "" OR NOT oa_in_build EQUAL 0)
        set(oa_prefs "${oa_dir}/preferences.conf")
        set(oa_add_prefs "${oa_prefs}")
      endif()
    else()
      set(oa_prefs "${oa_dir}/preferences.conf")
      set(oa_add_prefs "${oa_prefs}")
    endif()
    oa_ensure_offline_preferences("${oa_prefs}" "${oa_library}" "${oa_update_mode}")
  endif()
  set(oa_add_data "")
  oa_list_has_flag("${oa_cmd}" "--data-dir" oa_has_data)
  if(NOT oa_has_data)
    set(oa_add_data "${oa_dir}/data")
  endif()
  set("${prefs_out}" "${oa_add_prefs}" PARENT_SCOPE)
  set("${data_out}" "${oa_add_data}" PARENT_SCOPE)
endfunction()

# A fixture that deletes a preferences file runs this instead of cmake -E rm.
# It removes the same paths, then copies each .conf back from the seed the
# game test's registration wrote beside it. No seed means the file stays
# gone, which is what the welcome setup needs.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/offline-preferences")
file(WRITE "${CMAKE_BINARY_DIR}/offline-preferences/restore-preferences.cmake" [=[
foreach(path IN LISTS REMOVALS)
  if(EXISTS "${path}")
    file(REMOVE_RECURSE "${path}")
  endif()
endforeach()
foreach(pair IN LISTS RESTORES)
  string(FIND "${pair}" "|" oa_bar)
  if(oa_bar EQUAL -1)
    continue()
  endif()
  string(SUBSTRING "${pair}" 0 "${oa_bar}" oa_dest)
  math(EXPR oa_rest "${oa_bar} + 1")
  string(SUBSTRING "${pair}" "${oa_rest}" -1 oa_seed)
  if(EXISTS "${oa_seed}")
    file(READ "${oa_seed}" oa_body)
    file(WRITE "${oa_dest}" "${oa_body}")
  endif()
endforeach()
]=])

include("${CMAKE_CURRENT_LIST_DIR}/OaOfflineAddTest.cmake")
