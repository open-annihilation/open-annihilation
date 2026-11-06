// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's local people lists: named lists of Boneyards user ids (the chat
// console's "Silenced" list). A list is kept in boneyards/peoplelists/<name>.lst
// under the player's folder. It is read from there when the player has a
// copy, and from the same path in the game folder otherwise. A list is read
// the first time it is used, and every list is written back when the set is
// released. Nothing is written while the player's folder is not chosen yet.
#pragma once

#include <cstdint>

namespace oa::boneyards {

// Directory the list files live in, under the player's folder on write and
// under the game folder when the player has no copy.
inline constexpr const char* kPeopleListDirectory = "./boneyards/peoplelists";
inline constexpr const char* kPeopleListExtension = ".lst";
// Longest list name kept, terminator included.
inline constexpr uint32_t kPeopleListNameCapacity = 32;
// Lists one set holds; the game names only "Silenced".
inline constexpr uint32_t kMaxPeopleLists = 8;
// Ids one list holds, which bounds what a list file can make it allocate.
inline constexpr uint32_t kMaxPeopleListIds = 0x10000;

struct PeopleList {
    char name[kPeopleListNameCapacity]{};
    int32_t* ids{}; // insertion order; an id inserted twice is kept twice
    uint32_t count{};
    uint32_t capacity{};
    uint8_t loaded{}; // its file has been read (or found missing)
};

// The lists, each found by its name's text.
struct PeopleLists {
    PeopleList lists[kMaxPeopleLists]{};
    uint32_t count{};
};

/// Constructs an empty set of people lists.
///
/// @param[out] lists set to reset
void people_lists_init(PeopleLists* lists);

/// Writes every list back to its file, then frees the set's lists.
///
/// @param[in,out] lists set to save and empty
void people_lists_release(PeopleLists* lists);

/// Finds the named list, adding it unloaded and empty when the set has none by that name.
///
/// @param[in,out] lists set to search and extend
/// @param name list name, matched by its text
/// @return the list, or null when the name is too long or the set is full
PeopleList* people_lists_find_or_add(PeopleLists* lists, const char* name);

/// Tells whether the named list holds a user id, reading its file first if unread.
///
/// @param[in,out] lists set holding the list
/// @param name list name
/// @param user_id Boneyards user id to look for
/// @return true when the list holds the id
bool people_list_contains(PeopleLists* lists, const char* name, int32_t user_id);

/// Appends a user id to the named list, reading its file first if unread.
///
/// An id inserted twice is kept twice.
///
/// @param[in,out] lists set holding the list
/// @param name list name
/// @param user_id Boneyards user id to append
void people_list_insert(PeopleLists* lists, const char* name, int32_t user_id);

/// Removes every copy of a user id from the named list, reading its file first if unread.
///
/// @param[in,out] lists set holding the list
/// @param name list name
/// @param user_id Boneyards user id to remove
/// @return false when the list does not hold the id
bool people_list_erase(PeopleLists* lists, const char* name, int32_t user_id);

/// Marks the named list read, then appends the decimal ids of its file.
///
/// Reading stops at the end of the file or at the first text that is not a
/// number. The player's copy is read when there is one, and the game
/// folder's otherwise.
///
/// @param[in,out] lists set holding the list
/// @param name list name; the file is boneyards/peoplelists/<name>.lst
/// @return false when the file cannot be opened
bool people_list_load(PeopleLists* lists, const char* name);

/// Rewrites the named list's file under the player's folder, one id per CRLF-ended line.
///
/// The list's directories are created there. A list that was never read
/// leaves its file empty. Nothing is written while the player's folder is
/// not chosen yet.
///
/// @param[in,out] lists set holding the list
/// @param name list name
/// @return false when the file cannot be created or the list was never read
bool people_list_save(PeopleLists* lists, const char* name);

/// Writes every list of the set back to its file.
///
/// @param[in,out] lists set to save
/// @return true always
bool people_lists_save_all(PeopleLists* lists);

} // namespace oa::boneyards
