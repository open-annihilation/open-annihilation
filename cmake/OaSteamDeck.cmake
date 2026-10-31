# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The Steam Deck's files for the Steam Deck package
# (docs/installation/steam-deck.md, docs/development/releasing.md).
#
# oa-steam-artwork makes Steam's library pictures from the engine's icon in
# branding/ at packaging time, so no copy of the art is kept in the
# repository. oa-steam-deck-files, which only a build that names it runs,
# writes those pictures into steam-deck/artwork in the build tree and copies
# the files of platforms/linux/steam-deck beside them (the two Steam Input
# templates and the folder's README), giving the steam-deck folder that goes
# into the Steam Deck package next to open-annihilation.
include_guard(GLOBAL)

add_executable(oa-steam-artwork "${PROJECT_SOURCE_DIR}/tools/steam-artwork/steam_artwork.cpp")
target_link_libraries(oa-steam-artwork PRIVATE oa-formats-png oa-platform-shims oa-platform-text-font
  oa-ui-game-files oa-options)

set(oa_steam_deck_folder "${CMAKE_BINARY_DIR}/steam-deck")
set(oa_steam_deck_sources "${PROJECT_SOURCE_DIR}/platforms/linux/steam-deck")
# The files of the folder, besides the artwork.
set(oa_steam_deck_files
  "${oa_steam_deck_sources}/README.md"
  "${oa_steam_deck_sources}/open-annihilation.vdf"
  "${oa_steam_deck_sources}/open-annihilation-keyboard-mouse.vdf")
add_custom_target(oa-steam-deck-files
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${oa_steam_deck_folder}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${oa_steam_deck_folder}/artwork"
  COMMAND oa-steam-artwork --icon "${PROJECT_SOURCE_DIR}/branding/open-annihilation-icon.png"
    --fonts "${OA_TEXT_FONTS_DIR}" --output "${oa_steam_deck_folder}/artwork"
  COMMAND "${CMAKE_COMMAND}" -E copy ${oa_steam_deck_files} "${oa_steam_deck_folder}"
  COMMENT "Writing the Steam Deck's files into ${oa_steam_deck_folder}"
  VERBATIM)
add_dependencies(oa-steam-deck-files oa-steam-artwork)
