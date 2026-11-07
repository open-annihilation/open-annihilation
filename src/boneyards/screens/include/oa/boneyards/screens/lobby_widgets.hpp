// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Lobby widgets built on the Boneyards toolkit: function buttons, the
// rank/latency icons of a player row, and the Boneyards list box with its
// scroll bar.
#pragma once

#include "oa/boneyards/screens/chat_room.hpp"
#include "oa/boneyards/screens/widgets.hpp"
#include "oa/boneyards/toolkit.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

inline constexpr const char* kWidgetsGaf = "Widgets.gaf";
inline constexpr int32_t kWidgetKey = 9; // transparent palette index of Widgets.gaf

inline constexpr int32_t kLobbyEditHeight = 0x15;
inline constexpr int32_t kLobbyEditSelectionHeight = 0x11;

struct HatFonts {
    Handle small_light{k_no_handle};
    Handle small{k_no_handle};
    Handle small_dim{k_no_handle};
    Handle small_green{k_no_handle};
    Handle small_orange{k_no_handle};
    Handle small_blue{k_no_handle};
    Handle small_red{k_no_handle};
    Handle medium{k_no_handle};
    Handle button_pressed{k_no_handle};
    Handle button_normal{k_no_handle};
    Handle button_hover{k_no_handle};
    Handle big{k_no_handle};
};

/// Installs the fonts function buttons draw their captions in.
///
/// @param fonts the Hat fonts; null draws captions in each button's own font
void function_button_fonts(const HatFonts* fonts);

// Lobby label: a Static that keeps the caption style its placement chose for
// later texts.
struct LobbyLabel {
    Window base{};
    int32_t style{};
};

/// Creates a lobby label measured in its font and anchored relative to its parent.
///
/// @param[in,out] parent window the label is placed in
/// @param x anchor x relative to the parent, in pixels
/// @param y anchor y relative to the parent, in pixels
/// @param text caption
/// @param font caption font
/// @param alignment caption alignment 0..8 (rows top to bottom, columns left to right); an edge
///     applies only along a given width or height
/// @param anchor corner at (x, y): 0 top left, 1 top right, 2 bottom left, 3 bottom right
/// @param width label width in pixels; 0 takes the caption's measured width
/// @param height label height in pixels; 0 takes the caption's measured height
/// @return the label; allocation failure aborts
Window* lobby_label_create(
    Window* parent,
    int32_t x,
    int32_t y,
    const char* text,
    Handle font,
    int32_t alignment,
    int32_t anchor,
    int32_t width = 0,
    int32_t height = 0
);

/// Replaces the caption of a label made by lobby_label_create, in the style its placement chose.
///
/// @param[in,out] label the label
/// @param text new caption
void lobby_label_set_text(Window* label, const char* text);

// Gap between a labelled field's caption and its value.
inline constexpr int32_t kLobbyFieldGap = 3;
// Alignment both labels of a labelled field take (left, centred vertically).
inline constexpr int32_t kLobbyFieldAlignment = 3;

// Labelled value field: a container at (x, y) inside its
// parent holding a caption in the dim small
// font and, beside it, a value label in the small font that the setters
// rewrite. Both labels are left-aligned and as tall as the field.
struct LobbyField {
    Window base{};
    Window* value{}; // a LobbyLabel; null without a value label
};

/// Creates a labelled value field.
///
/// A caption sits at the field's left edge at its measured width and pushes
/// the value kLobbyFieldGap pixels past it; the value label, first showing
/// `value`, takes the rest of the width.
///
/// @param[in,out] parent window the field is placed in
/// @param x left edge relative to the parent, in pixels
/// @param y top edge relative to the parent, in pixels
/// @param w field width in pixels
/// @param h field height in pixels
/// @param caption caption in the dim small font; null for none
/// @param fonts the Hat fonts
/// @param value first value text; null, or no width left, creates no value label
/// @return the field; allocation failure aborts
LobbyField* lobby_field_create(
    Window* parent,
    int32_t x,
    int32_t y,
    int32_t w,
    int32_t h,
    const char* caption,
    const HatFonts* fonts,
    const char* value = ""
);

/// Sets a field's value text; a field without a value label ignores it.
///
/// @param[in,out] field the field
/// @param text value text
void lobby_field_set_text(LobbyField* field, const char* text);

/// Sets a field's value to a count shown as signed decimal text.
///
/// @param[in,out] field the field
/// @param count value, read as signed 32-bit
void lobby_field_set_count(LobbyField* field, uint32_t count);

// Radio members a lobby radio button can deselect on.
inline constexpr int32_t kLobbyRadioMembers = 5;

// Lobby radio button (BYRadioButton): a toolkit RadioButton that only lets go
// of its selection when one of its listed members is selected.
struct LobbyRadio {
    Window base{};
    int32_t member_count{};
    Window* members[kLobbyRadioMembers]{};
};

/// Creates a lobby radio button sized and skinned beside its caption, which also becomes its name.
///
/// @param[in,out] parent window the radio is placed in
/// @param x left edge relative to the parent, in pixels
/// @param y top edge relative to the parent, in pixels
/// @param text caption and name
/// @param font caption font
/// @return the radio button, unpressed; allocation failure aborts
LobbyRadio* lobby_radio_create(Window* parent, int32_t x, int32_t y, const char* text, Handle font);

/// Lists a radio whose selection deselects this one.
///
/// @param[in,out] radio the lobby radio button
/// @param member radio button listed; members past the record's five are ignored
void lobby_radio_add_member(LobbyRadio* radio, Window* member);

/// Creates a lobby check box (BYCheckBox) sized to its caption, unchecked.
///
/// The box is the caption's width plus 0xb pixels, at least 9 high, with the
/// caption two pixels past the box and the Check_Box frames.
///
/// @param[in,out] parent window the box is placed in
/// @param x left edge relative to the parent, in pixels
/// @param y top edge relative to the parent, in pixels
/// @param text caption
/// @param font caption font
/// @return the check box
CheckBox*
lobby_check_box_create(Window* parent, int32_t x, int32_t y, const char* text, Handle font);

namespace function_button {
inline constexpr int32_t large_width = 0x63;
inline constexpr int32_t small_width = 0x39;
inline constexpr int32_t height = 0x15;
inline constexpr int32_t state_count = 4;
} // namespace function_button

/// Places a button as a function button with four Function_Button_Large or _Small states all
/// captioned alike.
///
/// @param[in,out] button button to set up
/// @param small whether to use the small face
/// @param dx left edge relative to the parent, in pixels
/// @param dy top edge relative to the parent, in pixels
/// @param caption caption of every state
/// @param[in,out] parent window the button is placed in
void function_button_setup(
    Window* button, bool small, int32_t dx, int32_t dy, const char* caption, Window* parent
);

/// Creates a lobby text field (BYEdit): a SingleEdit 0x15 high with a 0x11 selection band.
///
/// Framed entries share a keyed outline in both states.
///
/// @param[in,out] parent window the field is placed in
/// @param dx left edge relative to the parent, in pixels
/// @param dy top edge relative to the parent, in pixels
/// @param width field width in pixels
/// @param font text font
/// @param max_length most characters the field takes
/// @param framed whether to draw the outline
/// @return the text field
SingleEdit* lobby_edit_create(
    Window* parent,
    int32_t dx,
    int32_t dy,
    int32_t width,
    Handle font,
    int32_t max_length,
    bool framed = false
);

/// Returns the lobby text field's dispatch table: the SingleEdit table whose caption slot replaces
/// the text.
///
/// @return the table
const WindowOps* lobby_edit_ops();

/// Writes the same text into the four button state slots.
///
/// @param[in,out] window the button
/// @param text caption of every state
void window_set_four_state_texts(Window* window, const char* text);

// Boneyards scroll bar: a toolkit Slider that builds its
// own arrow and thumb buttons from the
// Widgets.gaf "scrollbar" frames and draws its track into a blank image.
struct BySlider {
    Slider slider;
    Window* thumb;     // "sbutton"
    Window* increment; // "incbutton", at the top
    Window* decrement; // "decbutton", at the bottom
};

// Boneyards list box: a toolkit ListBox that creates its
// "slider" on placement and shows a transparent
// blank image instead of its background colour.
struct ByListBox {
    ListBox list;
    uint8_t framed;          // outline the list and inset the slider by a pixel
    uint8_t forward_changes; // pass `changed` notifications to the list
    Window* selected_item;
    BySlider* slider;
};

// Head shared by the Boneyards list rows. A click on a row, or a child
// passing one on, makes it the list's selected_item; drawing a selectable
// row first applies its class's ListRowLook.
struct ByTextRow {
    Window base{};
    ByListBox* list{};
    Window* label{}; // player and frequency rows keep none here
    const HatFonts* fonts{};
    bool selectable{};
};

/// Appends a fixed-height row with its inset label to a Boneyards list.
///
/// @param[in,out] list list receiving the row
/// @param text label text in the small font; null makes no label
/// @param fonts the Hat fonts
/// @param selectable whether the row takes the selected look
/// @param indent label inset in pixels
/// @return the row; allocation failure aborts
ByTextRow* by_text_row_create(
    ByListBox* list, const char* text, const HatFonts* fonts, bool selectable, int32_t indent = 3
);

/// Gives the row's label a font; a row without a label is left alone.
///
/// @param[in,out] row list row
/// @param font label font
void by_text_row_set_font(ByTextRow* row, Handle font);

// The two look handlers of the list-row classes: the look of a row while it
// is its list's selected row, and while it is not. Null where the class
// leaves the look alone.
struct ListRowLook {
    void (*selected)(Window* row);
    void (*unselected)(Window* row);
};

/// Draws a list row and its children (the list-row classes' draw handler).
///
/// A row that follows the selection first gets the look for whether it is
/// its list's selected row.
///
/// @param row the row
/// @param highlight whether the row follows the selection
/// @param selected_row its list's selected row
/// @param look the class's selected and unselected looks
/// @return true always
bool list_row_draw(
    Window* row, bool highlight, const Window* selected_row, const ListRowLook& look
);

/// Wraps text into fixed-height rows of the list, keeping the list scrolled to its end when it was.
///
/// Leading whitespace of each row is skipped. The first row is inset 4
/// pixels and continuation rows by the width of `indent - 1` spaces.
///
/// @param[in,out] list list receiving the rows
/// @param text text to wrap
/// @param fonts the Hat fonts
/// @param indent inset of the continuation rows, in space widths plus one
/// @param font row font; k_no_handle takes the small font
void by_list_wrap_text(
    ByListBox* list, const char* text, const HatFonts* fonts, int32_t indent, Handle font
);

/// Returns the width of a list's rows: the list less its scroll bar.
///
/// @param list the list window
/// @return row width in pixels
int32_t by_list_row_width(const Window* list);

/// Allocates a Boneyards scroll bar; placing it builds its buttons.
///
/// @return the scroll bar; allocation failure aborts
BySlider* by_slider_create();

/// Allocates a Boneyards list box; placing it creates its scroll bar.
///
/// @param framed outline the list and inset the scroll bar by a pixel
/// @param forward_changes pass `changed` notifications to the list
/// @return the list box; allocation failure aborts
ByListBox* by_list_box_create(bool framed, bool forward_changes);

/// Forgets the selected row, then removes every row.
///
/// @param[in,out] list the list box
void by_list_box_clear(ByListBox* list);

/// Returns the selected row's text through the row's own text slot; LOGIN takes the server name
/// this way.
///
/// @param list the list box
/// @return the text, or "" when no row is selected
const char* by_list_selected_text(const ByListBox* list);

// Player-row icons, children of the BYPlayerListItem row.
struct PlayerRowIcons {
    Window* user_icon;
    Window* latency_icon;
    int32_t user_frame;
    int32_t latency_frame;
};

/// Stores a player row's latency frame and shows it from Latency_Icons.
///
/// @param[in,out] row the row's icons
/// @param frame Latency_Icons frame
void player_row_set_latency(PlayerRowIcons* row, int32_t frame);

/// Stores a player row's rank icon frame and shows it from User_Icons.
///
/// @param[in,out] row the row's icons
/// @param frame User_Icons frame
void player_row_set_user_icon(PlayerRowIcons* row, int32_t frame);

/// What a rank or latency icon is, for the words a screen reader speaks.
enum class PlayerIconKind : uint8_t {
    none,    ///< not one of those icons
    rank,    ///< the rank picture
    latency, ///< the latency picture
};

/// The rank or latency a player-row icon is showing.
struct PlayerIconWords {
    PlayerIconKind kind = PlayerIconKind::none;
    /// Short rank title. Empty when the frame has none. Static storage.
    const char* rank = "";
    /// 1, 2 or 3 for a latency grade. 1 is the red square and 3 the green.
    /// 0 when the frame is not one of those.
    int32_t latency_grade = 0;
};

/// The words a latency grade speaks, before translation.
///
/// Grade 1 is the red square and speaks "Latency poor", grade 2 the amber
/// square and "Latency fair", and grade 3 the green square and "Latency good".
/// Any other grade speaks "Latency unknown".
///
/// @param grade the latency grade, or 0 when the frame is not a grade
/// @return the catalogue key; never null
const char* latency_grade_phrase(int32_t grade) noexcept;

/// Reads the rank or latency a player-row icon is showing.
///
/// The picture itself is unchanged. A window that is not that row's rank or
/// latency icon leaves `out` empty and returns false.
///
/// @param icon the icon window; may be null
/// @param[out] out the rank or the latency grade; null is ignored
/// @return true when `icon` is one of those icons
bool player_icon_words(const Window* icon, PlayerIconWords* out);

/// Who a chat line is speaking for, when a role tag is drawn in front of it.
enum class ChatRole : uint8_t {
    none,   ///< an ordinary line, no tag
    system, ///< a status line
    news,   ///< a news line
    arm,    ///< an Arm name
    core,   ///< a Core name
};

/// The role a chat font is, matched against the session's role fonts.
///
/// The comparison is the font handle at the moment the line is printed.
/// A handle below zero, or one that matches none of the four, is none.
/// When two roles share a handle, system wins, then news, then Arm, then Core.
///
/// @param font the line's font handle
/// @param system the session's status font
/// @param news the session's news font
/// @param arm the session's Arm font
/// @param core the session's Core font
/// @return the role, or none
[[nodiscard]] ChatRole chat_role_for_handles(
    int32_t font, int32_t system, int32_t news, int32_t arm, int32_t core
) noexcept;

/// The English tag drawn before a line of that role.
///
/// The words are [SYS], [NEWS], [ARM] and [CORE]. None has no tag.
///
/// @param role the line's role
/// @return the tag, or null for none
[[nodiscard]] const char* chat_role_tag(ChatRole role) noexcept;

/// Copies a chat line, and while cues are in effect puts its role tag in front.
///
/// The tag is the Boneyards catalogue's line for that role, in the player's
/// language. The sections are SYS, NEWS, ARM and CORE. The game's own
/// translation of those names is not used. With no catalogue line the English
/// tag is used. A role of none, or cues off, copies the line alone. A null
/// line is empty. The tag stands alone when the line is empty.
///
/// @param[out] out the line to print; null writes nothing
/// @param capacity size of `out`, including the terminator
/// @param text the line; null is empty
/// @param role the line's role
/// @param cues true while shape cues are in effect
void write_role_line(char* out, std::size_t capacity, const char* text, ChatRole role, bool cues);

/// Light palette entry a cue draws with, so the mark reads when colour does not.
inline constexpr uint8_t kCueInk = 80;
/// Dark palette entry a cue clears to. A latency bar's ground is this entry.
inline constexpr uint8_t kCueGround = 93;
/// Pixels on a side of a lock or mute mark.
inline constexpr int32_t kNameMarkSide = 7;
/// Columns a name moves right to leave room for its mark.
inline constexpr int32_t kNameMarkRoom = 8;

/// A mark drawn on a player name while shape cues are in effect.
enum class NameMark : uint8_t {
    none, ///< no mark
    lock, ///< whispers are locked onto this name
    mute, ///< the name is muzzled
};

/// Draws a lock or a mute mark into an indexed picture.
///
/// The picture is filled with `ground`, then the mark in `ink`. A lock is a
/// shackle over a body. A mute mark is a cross. None, a null picture, or a
/// non-positive size draws nothing.
///
/// @param pixels the picture; null draws nothing
/// @param width the picture's width in pixels
/// @param height the picture's height in pixels
/// @param pitch bytes from one row to the next
/// @param mark which mark
/// @param ink the mark's colour
/// @param ground the colour behind the mark
void paint_name_mark(
    uint8_t* pixels,
    int32_t width,
    int32_t height,
    int32_t pitch,
    NameMark mark,
    uint8_t ink,
    uint8_t ground
) noexcept;

/// Draws a latency grade as that many vertical bars.
///
/// Grades 1, 2 and 3 draw that many bars, from the left, on `ground`. Any
/// other grade draws nothing and leaves the picture as it was. A null
/// picture or a non-positive size draws nothing.
///
/// @param pixels the picture; null draws nothing
/// @param width the picture's width in pixels
/// @param height the picture's height in pixels
/// @param pitch bytes from one row to the next
/// @param grade the latency grade, 1 to 3
/// @param ink the bars' colour
/// @param ground the colour behind the bars
void paint_latency_bars(
    uint8_t* pixels,
    int32_t width,
    int32_t height,
    int32_t pitch,
    int grade,
    uint8_t ink,
    uint8_t ground
) noexcept;

inline constexpr int32_t kByRowHeight = 0x11;
inline constexpr int32_t kByRowIconSize = 0xb;
inline constexpr int32_t kByRowNameX = 0x11; // name label, past the row's icon

inline constexpr const char* kByPlayerRowClass = "BYPlayerListItem";
inline constexpr size_t kByPlayerNameCapacity = 0x100;
inline constexpr int32_t kByPlayerRowFirstUserIcon = 0x13;
inline constexpr int32_t kByPlayerRowFirstLatency = 1;

// Player row (BYPlayerListItem): the rank icon, the name
// and the latency icon just past it. The name takes
// the light font on the selected row; otherwise green while whispers are
// locked onto the player, dim while muzzled, else the small font. Callers
// set the two flags directly; the next draw applies them.
struct ByPlayerRow {
    ByTextRow row;
    char name[kByPlayerNameCapacity];
    uint32_t user_id;
    PlayerRowIcons icons; // RankIcon, LatencyIcon
    bool whisper_locked;
    bool silenced;
    Window* name_label;
};

/// Appends a player row: the rank icon, the name and the latency icon just past it.
///
/// The row shows User_Icons frame 0x13 and Latency_Icons frame 1 until the
/// setters change them.
///
/// @param[in,out] list list receiving the row
/// @param name player name
/// @param user_id player's user id
/// @param fonts the Hat fonts
/// @return the row; allocation failure aborts
ByPlayerRow*
by_player_row_create(ByListBox* list, const char* name, uint32_t user_id, const HatFonts* fonts);

inline constexpr const char* kByChatRoomRowClass = "BYChatRoomListItem";
inline constexpr int32_t kByRoomCountWidth = 0xe;
inline constexpr int32_t kByRoomCurrentFrame = 1; // Icon_Circle on the current frequency
inline constexpr int32_t kByRoomOtherFrame = 2;

// Frequency row (BYChatRoomListItem): an Icon_Circle, the
// name and the occupant count. The circle
// shows the current-frequency frame while `current_room` names the row's
// frequency; the name and count take the light font on the selected row.
struct ByChatRoomRow {
    ByTextRow row;
    char name[kRoomNameCapacity];
    char description[kRoomDescriptionCapacity];
    char tag[kRoomTagCapacity];
    uint32_t room_id;
    bool needs_password;
    int32_t users;
    int32_t type;        // room_type
    Handle current_icon; // Icon_Circle kByRoomCurrentFrame
    Handle other_icon;   // Icon_Circle kByRoomOtherFrame
    Window* icon;        // FrequencyIcon
    Window* name_label;
    Window* count_label;
    const uint32_t* current_room; // the session's current frequency; may be null
};

/// Adds a frequency row where its room type places it, with a count of 0.
///
/// A known type gets the circle, the name and the count; an unlisted type
/// gets a bare row outside the list.
///
/// @param[in,out] list list receiving the row
/// @param name frequency name
/// @param description frequency description
/// @param tag frequency tag
/// @param room_id frequency id
/// @param needs_password whether entering needs a password
/// @param type room_type value
/// @param fonts the Hat fonts
/// @param current_room the session's current frequency; may be null
/// @return the row; allocation failure aborts
ByChatRoomRow* by_chat_room_row_create(
    ByListBox* list,
    const char* name,
    const char* description,
    const char* tag,
    uint32_t room_id,
    bool needs_password,
    int32_t type,
    const HatFonts* fonts,
    const uint32_t* current_room
);

/// Shows a number of users in a frequency row's count label.
///
/// @param[in,out] row frequency row
/// @param users occupant count
void by_chat_room_row_set_count(ByChatRoomRow* row, uint32_t users);

/// Adds a delta to a frequency row's shown count.
///
/// @param[in,out] row frequency row
/// @param delta amount added (wraps around)
void by_chat_room_row_add_count(ByChatRoomRow* row, int32_t delta);

inline constexpr const char* kByCircleRowClass = "BYCircleLstItem";
inline constexpr int32_t kByCircleSelectedFrame = 1; // Icon_Circle on the selected row
inline constexpr int32_t kByCircleOtherFrame = 2;

// Profile category row (BYCircleLstItem): an Icon_Circle
// and the category's name. The selected row
// shows the circle's first frame and its name in the light font.
struct ByCircleRow {
    ByTextRow row;
    int32_t tab;          // the ProfileTab the row lists
    Handle selected_icon; // Icon_Circle kByCircleSelectedFrame
    Handle other_icon;    // Icon_Circle kByCircleOtherFrame
    Window* icon;         // ProfileCategoryIcon
    Window* name_label;
};

/// Appends a profile category row: the ProfileCategoryIcon circle, centred three pixels in, and the
/// name past it.
///
/// @param[in,out] list list receiving the row
/// @param name category name
/// @param tab ProfileTab the row lists
/// @param fonts the Hat fonts
/// @return the row; allocation failure aborts
ByCircleRow*
by_circle_row_create(ByListBox* list, const char* name, int32_t tab, const HatFonts* fonts);

// Row of two labels (the battle Scoreboard's message rows): a name in the
// dim small font at its measured width, then a text in the small font over
// the rest of the row. The row follows the selection with no look of its
// own.
struct ByTwoLabelRow {
    ByTextRow row;
    Window* first_label;  // null without a first text
    Window* second_label; // null without a second text
};

/// Appends a two-label row, the labels 3 pixels in (0x11 with `indent`) and 3 apart.
///
/// @param[in,out] list list receiving the row
/// @param first name in the dim small font; null makes no first label
/// @param second text in the small font; null makes no second label
/// @param fonts the Hat fonts
/// @param indent whether the labels start 0x11 in instead of 3
/// @return the row; allocation failure aborts
ByTwoLabelRow* by_two_label_row_create(
    ByListBox* list, const char* first, const char* second, const HatFonts* fonts, bool indent
);

// Radio group (RadioGroup): its caption in the medium font over a row of
// lobby radio buttons in the small font, each 5 pixels past the last and
// each listing the others as its members. A member picked sounds
// GREEN_BUTTON, becomes the selection and is reported to the group's parent
// with code 200 and its index. Member values are the radio buttons' states
// (0 disabled, 1 selected, 2 enabled).
inline constexpr int32_t kByRadioGroupHeight = 0x1f;
inline constexpr int32_t kByRadioGroupNotify = 200;
inline constexpr const char* kByRadioGroupClick = "GREEN_BUTTON";

struct ByRadioGroup {
    Window base{};
    LobbyRadio* members[kLobbyRadioMembers]{};
    int32_t count{};
    int32_t selected{-1}; // index of the selected member, -1 none
    // Plays a named sound; null plays none.
    void (*play_sound)(void* user, const char* name){};
    void* sound_user{};
};

/// Creates a radio group: the caption at its top left and a radio button per name 0xf below it,
/// the first selected.
///
/// The group is 0x1f high and as wide as its buttons (each its name's width
/// plus 0xb) with 5 pixels between them.
///
/// @param[in,out] parent window the group is placed in
/// @param x left edge relative to the parent, in pixels
/// @param y top edge relative to the parent, in pixels
/// @param caption caption over the buttons
/// @param names the buttons' names, which are also their captions
/// @param count number of names; past kLobbyRadioMembers are dropped
/// @param fonts the Hat fonts
/// @return the group; allocation failure aborts
ByRadioGroup* by_radio_group_create(
    Window* parent,
    int32_t x,
    int32_t y,
    const char* caption,
    const char* const* names,
    int32_t count,
    const HatFonts* fonts
);

/// Makes a member the selection: it shows selected and every other enabled member enabled; a
/// window that is no member leaves nothing selected.
///
/// @param[in,out] group radio group
/// @param member the member to select
void by_radio_group_select(ByRadioGroup* group, const Window* member);

/// Selects the member at an index.
///
/// @param[in,out] group radio group
/// @param index member index
void by_radio_group_set_index(ByRadioGroup* group, int32_t index);

/// Selects the member whose name matches exactly; no match changes nothing.
///
/// @param[in,out] group radio group
/// @param name member name
void by_radio_group_select_name(ByRadioGroup* group, const char* name);

/// Shows the selection selected and every other member enabled.
///
/// @param[in,out] group radio group
void by_radio_group_refresh(ByRadioGroup* group);

/// Disables the member whose name matches exactly.
///
/// @param[in,out] group radio group
/// @param name member name
void by_radio_group_disable_name(ByRadioGroup* group, const char* name);

/// Disables every member but the selection.
///
/// @param[in,out] group radio group
void by_radio_group_disable_others(ByRadioGroup* group);

/// Returns the selected member's name.
///
/// @param group radio group
/// @return the name, or "" with nothing selected
const char* by_radio_group_selected_name(const ByRadioGroup* group);

/// Shows a radio group model's members and selection on its window.
///
/// @param[in,out] group the window
/// @param model the model whose values the members take
void by_radio_group_show(ByRadioGroup* group, const RadioGroup& model);

/// Reads a radio group window's selection and member values into its model.
///
/// @param group the window
/// @param[in,out] model the model taking them
void by_radio_group_read(const ByRadioGroup* group, RadioGroup* model);

/// Counts the characters of a line that fit a width in a font.
///
/// Breaks at the last space, tab, '-', '/' or '\' before the glyph that
/// overflows (that glyph's position when no break precedes it); a line that
/// fits runs to a newline or the end.
///
/// @param text text to measure
/// @param font font the text is drawn in
/// @param available width in pixels
/// @return number of characters on the line
int32_t wrapped_line_length(const char* text, Handle font, int32_t available);

} // namespace oa::boneyards::screens
