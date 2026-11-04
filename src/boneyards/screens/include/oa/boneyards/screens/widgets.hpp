// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Small Boneyards controls used across the lobby screens: radio groups,
// numeric count labels, list-item class tests and the <$tag|default$>
// template expansion used by HTML views.
#pragma once

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// Widget value states shared by buttons, check boxes and radio buttons.
namespace widget_value {
inline constexpr int32_t disabled = 0;
inline constexpr int32_t selected = 1;
inline constexpr int32_t enabled = 2;
inline constexpr int32_t icon_off = 3; // two-state icon, first frame
inline constexpr int32_t icon_on = 4;
} // namespace widget_value

inline constexpr std::size_t kRadioGroupCapacity = 5;
inline constexpr std::size_t kRadioNameCapacity = 0x20;

struct RadioMember {
    char name[kRadioNameCapacity]; // widget name
    int32_t value;                 // widget value
};

// RadioGroup widget.
struct RadioGroup {
    RadioMember members[kRadioGroupCapacity];
    int32_t count;
    int32_t selected; // -1 when nothing is selected
};

/// Appends a member to a radio group.
///
/// @param[in,out] group group to extend
/// @param name widget name, copied
/// @param value widget value (widget_value)
/// @return false when the group is full
bool radio_add(RadioGroup* group, const char* name, int32_t value);

/// Selects one member and greys every other enabled member.
///
/// @param[in,out] group radio group
/// @param member member index; a negative or out-of-range index selects none
void radio_select_member(RadioGroup* group, int32_t member);

/// Stores a selection index and selects that member.
///
/// @param[in,out] group radio group
/// @param index member index, -1 for none
void radio_set_index(RadioGroup* group, int32_t index);

/// Selects the member whose name matches exactly.
///
/// @param[in,out] group radio group
/// @param name widget name; no member matching leaves the group unchanged
void radio_select_by_name(RadioGroup* group, const char* name);

/// Rewrites every member's value from the stored selection: the selected member 1, every other 2.
///
/// @param[in,out] group radio group
void radio_refresh(RadioGroup* group);

/// Disables the member whose name matches exactly.
///
/// @param[in,out] group radio group
/// @param name widget name
void radio_disable_by_name(RadioGroup* group, const char* name);

/// Disables every member except the selected one.
///
/// @param[in,out] group radio group
void radio_disable_others(RadioGroup* group);

/// Clears the selection and disables every member.
///
/// @param[in,out] group radio group
void radio_clear(RadioGroup* group);

inline constexpr std::size_t kCountLabelCapacity = 12;

// Numeric label: a count and its text.
struct CountLabel {
    int32_t count;
    char text[kCountLabelCapacity];
};

/// Formats a value as signed decimal label text.
///
/// @param[out] out text buffer
/// @param capacity size of `out` in bytes
/// @param value value, read as signed 32-bit
void format_signed_label(char* out, std::size_t capacity, uint32_t value);

/// Stores a count and formats it as by_chat_room_row_set_count shows it.
///
/// @param[out] label count label
/// @param value new count, read as signed 32-bit
void count_label_set(CountLabel* label, uint32_t value);

/// Adds a delta to a count label and reformats it as by_chat_room_row_add_count does.
///
/// @param[in,out] label count label
/// @param delta amount added (wraps around)
void count_label_add(CountLabel* label, int32_t delta);

// List-item classes recognised by the lobby lists.
enum class ListItemClass : int32_t {
    player,       // BYPlayerListItem
    circle,       // BYCircleLstItem
    chat_room,    // BYChatRoomListItem
    toc,          // BYTOCListItem
    toc_category, // BYTOCCategoryListItem
};

/// Tells whether a class name is one of the lobby list-item classes.
///
/// @param item list-item class
/// @param class_name class name compared exactly and case-sensitively; null never matches
/// @return true on a match
bool is_list_item_class(ListItemClass item, const char* class_name);

// Looks up a template tag's value; returns false when the tag is unknown.
using TemplateLookup = bool (*)(void* context, const char* tag, const char** value);

/// Expands <$TAG|default$> template spans in two passes (measure, then copy).
///
/// Unknown tags, or every tag when `lookup` is null, use their default text.
/// A buffer too short for the result is left untouched.
///
/// @param[out] out expanded text; null to only measure
/// @param[in,out] size with `out` null receives the size needed (terminator included); otherwise
///     the capacity of `out`
/// @param text template text; null fails
/// @param lookup tag lookup; may be null
/// @param context context handed to `lookup`
/// @return false when `text` is null or `*size` is too small (the buffer is then untouched)
bool expand_template_tags(
    char* out, std::size_t* size, const char* text, TemplateLookup lookup, void* context
);

} // namespace oa::boneyards::screens
