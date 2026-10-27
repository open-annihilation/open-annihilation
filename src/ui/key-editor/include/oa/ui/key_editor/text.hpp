// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every word the key editor shows, in English. A pattern's places are written
// {name}. A translation keeps them and may move them. The texts are looked up
// whole before their places are filled, so a translator finds every one of
// them here. Action names and descriptions are the registry's English words,
// looked up through the same hook.
#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace oa::ui::key_editor {

/// How the screen looks a word up. `shown` null returns the English.
struct TextHooks {
    /// Returns the text to show for an English word. The view need only last
    /// until the next lookup.
    std::string (*shown)(void* user, std::string_view english) = nullptr;
    void* user = nullptr; ///< passed to shown
};

/// Looks a word up. A null hook returns the English.
///
/// @param hooks the lookup
/// @param english the English word, as written in this file
/// @return the text to show
[[nodiscard]] std::string show(const TextHooks& hooks, std::string_view english);

/// Fills the places of a pattern that has already been looked up.
///
/// Each place is a name without braces and the text that replaces `{name}`.
/// A name the pattern does not use is ignored.
///
/// @param pattern the looked-up pattern
/// @param places the name and the text of each place
/// @return the filled text
[[nodiscard]] std::string fill(
    std::string_view pattern,
    std::initializer_list<std::pair<std::string_view, std::string_view>> places
);

namespace words {

/// The screen's name, and Compact's title.
inline constexpr std::string_view keys = "Keys";
/// The word beside the layout drop-down.
inline constexpr std::string_view layout = "LAYOUT";
/// The layout's author and version, under its name.
inline constexpr std::string_view by_author = "by {author} · {version}";
/// How many of the player's own changes sit on the layout. `{n}` is a number.
inline constexpr std::string_view own_changes = "and {n} changes of your own";
/// The same line when the player has made one change.
inline constexpr std::string_view own_change_one = "and 1 change of your own";
/// Large's search field, while it is empty.
inline constexpr std::string_view search_actions = "Search actions, or press a key";
/// Regular and Compact's search field, while it is empty.
inline constexpr std::string_view search_short = "Search, or press a key";
/// The filter that keeps every action, and the first category.
inline constexpr std::string_view filter_all = "All";
/// The filter that keeps actions the draft names.
inline constexpr std::string_view filter_changed = "Changed";
/// The filter that keeps actions in a clash.
inline constexpr std::string_view filter_clashes = "Clashes";
/// The filter that keeps actions with no chord.
inline constexpr std::string_view filter_unbound = "Unbound";
/// The filter that keeps actions that can latch.
inline constexpr std::string_view filter_latchable = "Latchable";
/// A filter pill. `{filter}` is the filter's name and `{count}` how many it keeps.
inline constexpr std::string_view filter_count = "{filter} ({count})";
/// Regular and Compact's category drop-down while every category is shown.
inline constexpr std::string_view all_categories = "All categories";
/// The heading over the hacks that are on.
inline constexpr std::string_view hacks_on = "Hacks on";
/// The mod's Category words.
inline constexpr std::string_view tags = "Tags";
/// A tag a layout names that the loaded units do not carry.
inline constexpr std::string_view not_in_loaded_units = "not in the loaded units";
/// The mod's menu keys.
inline constexpr std::string_view menus = "Menus";
/// Developer keys. Listed only in Developer Mode.
inline constexpr std::string_view developer = "Developer";
/// The kit's keys on Open Annihilation's screens.
inline constexpr std::string_view oa_screens = "Open Annihilation's screens";
/// Actions this version does not know. Read only, and written back.
inline constexpr std::string_view not_in_version = "Not in this version";
/// The built-in layout's name.
inline constexpr std::string_view oa_default = "OA default";
/// Shown in the header while a match is paused behind the editor.
inline constexpr std::string_view game_paused = "game paused";
/// Compact's header context. `{layout}` is the layout's name and `{n}` a number.
inline constexpr std::string_view compact_context = "{layout} · {n} changes";
/// Compact's header context when the player has made one change.
inline constexpr std::string_view compact_context_one = "{layout} · 1 change";
/// A category drop-down's row. `{name}` is the category and `{count}` how many it holds.
inline constexpr std::string_view category_count = "{name} ({count})";
/// Extra chords a Compact row does not draw. `{n}` is how many more.
inline constexpr std::string_view more_chords = "+{n}";
/// The details, while no row is chosen.
inline constexpr std::string_view choose_action = "Choose an action";

/// Category letters. A translator may change them. U is the Menus area.
inline constexpr std::string_view letter_orders = "O";
inline constexpr std::string_view letter_pointer = "P";
inline constexpr std::string_view letter_selection = "S";
inline constexpr std::string_view letter_groups = "G";
inline constexpr std::string_view letter_camera = "C";
inline constexpr std::string_view letter_building = "B";
inline constexpr std::string_view letter_game = "M";
inline constexpr std::string_view letter_chat = "T";
inline constexpr std::string_view letter_window = "W";
inline constexpr std::string_view letter_menus = "U";
inline constexpr std::string_view letter_hacks = "L";
inline constexpr std::string_view letter_mod = "D";
inline constexpr std::string_view letter_developer = "V";

/// Area titles, in the registry's order. Mouse is shown as Pointer.
inline constexpr std::string_view area_orders = "Orders";
inline constexpr std::string_view area_pointer = "Pointer";
inline constexpr std::string_view area_selection = "Selection";
inline constexpr std::string_view area_groups = "Groups";
inline constexpr std::string_view area_camera = "Camera";
inline constexpr std::string_view area_building = "Building";
inline constexpr std::string_view area_game = "Game";
inline constexpr std::string_view area_chat = "Chat";
inline constexpr std::string_view area_watching = "Watching";
inline constexpr std::string_view area_window = "Window";
inline constexpr std::string_view area_menus = "Menus";
inline constexpr std::string_view area_developer = "Developer";
inline constexpr std::string_view area_megamap = "Megamap";
inline constexpr std::string_view area_whiteboard = "Whiteboard";

/// Where an action works, after its area.
inline constexpr std::string_view while_playing = "while playing";
inline constexpr std::string_view while_watching = "while watching";
/// Context titles, as the registry names them.
inline constexpr std::string_view context_everywhere = "Everywhere";
inline constexpr std::string_view context_oa = "Open Annihilation";
inline constexpr std::string_view context_menus = "Menus";
inline constexpr std::string_view context_match = "Match";
inline constexpr std::string_view context_playing = "Playing";
inline constexpr std::string_view context_watching = "Watching";
inline constexpr std::string_view context_chat = "Chat line";
inline constexpr std::string_view context_panels = "Panels";
inline constexpr std::string_view context_unit_info = "Unit info";
inline constexpr std::string_view context_megamap = "Megamap";
inline constexpr std::string_view context_whiteboard = "Whiteboard";
inline constexpr std::string_view context_placing = "Placing";
inline constexpr std::string_view context_box = "Selection box";

/// A quiet aside.
inline constexpr std::string_view aside_held = "held";
inline constexpr std::string_view aside_latches = "latches";
inline constexpr std::string_view aside_confirm = "hold to confirm";

/// What a chord is.
inline constexpr std::string_view letter_key = "The letter {key}, wherever your keyboard has it";
inline constexpr std::string_view place_key = "The key in the {key} place";
inline constexpr std::string_view mouse_button = "Mouse button {button}";
inline constexpr std::string_view named_key = "The {key} key";
/// Appended when the chord is one of the player's own changes.
inline constexpr std::string_view your_change = "· your change";

/// Facts in the details.
inline constexpr std::string_view fact_kind = "Kind";
inline constexpr std::string_view fact_base = "Base";
inline constexpr std::string_view fact_works = "Works with";
inline constexpr std::string_view keys_heading = "KEYS";
/// How an action fires.
inline constexpr std::string_view kind_press = "Press";
inline constexpr std::string_view kind_repeat = "Repeat";
inline constexpr std::string_view kind_hold = "Hold";
inline constexpr std::string_view kind_modifier = "Modifier";
inline constexpr std::string_view kind_pointer = "Pointer";
inline constexpr std::string_view kind_wheel = "Wheel";
/// Where a default chord comes from.
inline constexpr std::string_view source_31c = "3.1c";
inline constexpr std::string_view source_oa = "Open Annihilation";
inline constexpr std::string_view source_data = "the mod's data";
inline constexpr std::string_view from_source = "{chords}, from {source}";
inline constexpr std::string_view from_order_panel =
    "{chords}, from the mod's order panel ({gadget})";
/// Condition titles for Works with.
inline constexpr std::string_view when_single = "Single player";
inline constexpr std::string_view when_multi = "Multiplayer";
inline constexpr std::string_view when_seat = "A seat";
inline constexpr std::string_view when_selection = "A selection";
inline constexpr std::string_view when_developer = "Developer mode";
inline constexpr std::string_view when_debug = "Debug keys";
inline constexpr std::string_view when_wheel = "Wheel zoom";
inline constexpr std::string_view when_no_wheel = "No wheel zoom";
inline constexpr std::string_view when_pointer = "Pointer keys";

/// The details' reset. `{layout}` is the layout the action returns to.
inline constexpr std::string_view reset_to = "RESET TO {layout}";
/// A latchable action's switch.
inline constexpr std::string_view latch = "Latch";
/// The line under Latch.
inline constexpr std::string_view latch_hint = "Tap to turn it on, tap again to turn it off.";
/// A confirmable action's switch.
inline constexpr std::string_view hold_to_confirm = "Hold to confirm";
/// Shown under Hold to confirm while that Settings row is off.
inline constexpr std::string_view confirm_off = "Off in Settings › Accessibility";
/// The switch that heads Groups, and the Large footer's aside.
inline constexpr std::string_view number_keys = "Number keys select groups";
inline constexpr std::string_view number_keys_state = "Number keys select groups: {state}";
inline constexpr std::string_view state_on = "On";
inline constexpr std::string_view state_off = "Off";

/// The footer.
inline constexpr std::string_view restore = "RESTORE DEFAULTS";
inline constexpr std::string_view more = "MORE";
inline constexpr std::string_view back = "BACK";
inline constexpr std::string_view reset = "RESET";
inline constexpr std::string_view cancel = "CANCEL";
inline constexpr std::string_view save = "SAVE";
/// The footer while a clash is left. `{n}` is how many.
inline constexpr std::string_view settle = "Settle {n} clashes to save";
/// The footer while one clash is left.
inline constexpr std::string_view settle_one = "Settle 1 clash to save";
/// The layout menu.
inline constexpr std::string_view duplicate = "DUPLICATE…";
inline constexpr std::string_view rename = "RENAME…";

/// Choosing another layout while the draft has changes.
inline constexpr std::string_view keep_question = "Keep your {n} changes on top of {layout}?";
/// The same question when the draft holds one change.
inline constexpr std::string_view keep_question_one = "Keep your 1 change on top of {layout}?";
inline constexpr std::string_view keep_them = "KEEP THEM";
inline constexpr std::string_view set_aside = "SET THEM ASIDE";
/// Leaving while the draft has changes.
inline constexpr std::string_view leave_question = "Leave without saving {n} changes?";
/// The same question when the draft holds one change.
inline constexpr std::string_view leave_question_one = "Leave without saving 1 change?";
inline constexpr std::string_view leave = "LEAVE";
inline constexpr std::string_view keep_editing = "KEEP EDITING";
/// Duplicate and rename ask this. The field is the layout's name.
inline constexpr std::string_view name_layout = "Name this layout";
/// A layout could not be written. `{reason}` is the sentence the writer gave.
inline constexpr std::string_view write_failed = "The layout was not saved: {reason}";
/// The name question's accept, and a notice's OK. The catalogue already has this word.
inline constexpr std::string_view ok = "OK";

/// The dashed chip that starts a capture. The kit draws this English on the chip.
inline constexpr std::string_view add_key = "+ ADD KEY";
/// The capture prompt's title. `{action}` is the action's name.
inline constexpr std::string_view press_keys = "Press the keys for {action}";
/// The line under the well.
inline constexpr std::string_view hold_modifiers =
    "Hold the modifiers you want, then press a key or a mouse button, or turn the wheel.";
/// The well, before a key has finished the chord.
inline constexpr std::string_view press_a_key = "Press a key…";
/// The means heading.
inline constexpr std::string_view means = "Means";
/// A letter, shorter than the details' explanation. `{key}` is the character.
inline constexpr std::string_view means_letter = "The letter {key}";
/// The gesture heading.
inline constexpr std::string_view starts_on = "Starts on";
/// A plain press.
inline constexpr std::string_view a_press = "A press";
/// Two presses.
inline constexpr std::string_view a_double_press = "A double press";
/// A hold.
inline constexpr std::string_view a_hold = "A hold";
/// The chord is free. `{chord}` is how it is drawn and `{context}` where it is read.
inline constexpr std::string_view chord_free = "{chord} is free {context}.";
/// The chord is taken. `{name}` is every other action, `{description}` what the
/// first of them does, and `{context}` where it is read.
inline constexpr std::string_view clash_line =
    "{name}: {description} {context}. OK asks what to do.";
/// A fixed chord. `{description}` is that action's description, with a small first letter.
inline constexpr std::string_view always_fixed =
    "Always {description} in Open Annihilation. Choose another key.";
/// A letter no key on this keyboard types.
inline constexpr std::string_view not_on_keyboard = "Not on this keyboard";
/// The capture prompt's footer.
inline constexpr std::string_view escape_hint = "Escape cancels · hold Escape to bind Escape";
/// The clash prompt's title. `{chord}` is the chord.
inline constexpr std::string_view chord_taken = "{chord} is taken";
/// Giving a chord in place of another. `{old}` is the chord being changed.
inline constexpr std::string_view giving_instead =
    "You are giving {action} the chord {chord} in place of {old}.";
/// Giving a chord that is new.
inline constexpr std::string_view giving_chord = "You are giving {action} the chord {chord}.";
/// Where the chord is already taken. `{where}` starts with a capital.
inline constexpr std::string_view already_does = "{where}, {chord} already does what {other} does.";
/// What an action keeps when Replace takes its only chord.
inline constexpr std::string_view nothing_unbound = "nothing: unbound";
/// The line under the clash table. `{action}` is the action Replace chooses.
inline constexpr std::string_view after_replace =
    "After Replace, {action} is chosen in the list so you can give it another key.";
/// Replace, on the clash prompt.
inline constexpr std::string_view replace = "REPLACE";
/// Swap, on the clash prompt. Shown only when a chord is being changed.
inline constexpr std::string_view swap = "SWAP";
/// The draft card's title. `{chord}` is the shared chord.
inline constexpr std::string_view does_two = "{chord} does two things";
/// The draft card's place. `{name}` is the other action and `{context}` where both are read.
inline constexpr std::string_view here_and_in = "Here, and in {name}. Both are read {context}.";
/// The card keeps the chord on this action.
inline constexpr std::string_view keep_here = "KEEP IT HERE";
/// The card keeps the chord on the other action. `{name}` is that action's name.
inline constexpr std::string_view keep_for = "KEEP IT FOR {name}";
/// The card starts a capture on the shared chord.
inline constexpr std::string_view choose_another = "CHOOSE ANOTHER KEY…";
/// What the other action is left with. `{chord}` is that, or "nothing: unbound".
inline constexpr std::string_view keep_leaves = "Keep it here leaves {name} with {chord}.";

/// The view tabs.
inline constexpr std::string_view view_list = "Commands";
inline constexpr std::string_view view_keyboard = "Keyboard";
inline constexpr std::string_view view_mouse = "Mouse";
/// The layer strip. Nothing is the catalogue's word.
inline constexpr std::string_view holding = "Holding";
inline constexpr std::string_view holding_nothing = "Nothing";
inline constexpr std::string_view layer_shift = "Shift";
inline constexpr std::string_view layer_ctrl = "Ctrl";
inline constexpr std::string_view layer_alt = "Alt";
inline constexpr std::string_view layer_cmd = "Cmd";
/// The shape drop-down. `{shape}` is ANSI, ISO or JIS.
inline constexpr std::string_view shape_from_system = "{shape} · from your system";
inline constexpr std::string_view shape_system = "From your system";
inline constexpr std::string_view shape_ansi = "ANSI";
inline constexpr std::string_view shape_iso = "ISO";
inline constexpr std::string_view shape_jis = "JIS";
/// The legend. Game and window share the game letter. The dashed sentence is gone.
inline constexpr std::string_view legend_game_window = "Game and window";
inline constexpr std::string_view legend_hacks = "The mod's hacks";
inline constexpr std::string_view legend_data = "The mod's data";
inline constexpr std::string_view grey_keys =
    "Grey keys are free while playing. Click a free key to give it the action chosen in the "
    "list. Hover a key to see everything on it.";
/// Card contexts. These start with a capital, unlike "while playing".
inline constexpr std::string_view card_while_playing = "While playing";
inline constexpr std::string_view card_while_watching = "While watching";
inline constexpr std::string_view card_placing = "Placing a building";
inline constexpr std::string_view card_chat_line = "In the chat line";
inline constexpr std::string_view card_close_line = "Close the line";
inline constexpr std::string_view card_always = "always";
inline constexpr std::string_view card_oa_screens = "On Open Annihilation's screens";
/// The catalogue's Back, not the footer's BACK.
inline constexpr std::string_view card_back = "Back";
inline constexpr std::string_view in_place_of = "in place of";
/// A release, beside A press, A double press and A hold.
inline constexpr std::string_view a_release = "A release";
/// Named keys the picture spells out.
inline constexpr std::string_view key_space = "Space";
inline constexpr std::string_view key_enter = "Enter";
inline constexpr std::string_view key_tab = "Tab";
/// The details' button that outlines an action's keys.
inline constexpr std::string_view find_on_keyboard = "FIND ON KEYBOARD";
/// The mouse view.
inline constexpr std::string_view clicks_give_orders = "Clicks give orders with";
inline constexpr std::string_view scheme_right_click = "Right-click";
inline constexpr std::string_view scheme_left_click = "Left-click";
inline constexpr std::string_view button_left = "Left";
inline constexpr std::string_view button_right = "Right";
inline constexpr std::string_view button_middle = "Middle";
inline constexpr std::string_view button_wheel = "Wheel";
inline constexpr std::string_view button_forward = "Forward";
/// Free is the catalogue's word.
inline constexpr std::string_view free_role = "Free";
inline constexpr std::string_view place_minimap = "On the minimap";
inline constexpr std::string_view place_build = "On a build button";
inline constexpr std::string_view place_whiteboard = "Whiteboard held";
inline constexpr std::string_view mouse_note =
    "Click a button on the picture, or a role in the table, to change it. Buttons 6 to 8 of a "
    "gaming mouse appear here when the game sees them pressed.";
inline constexpr std::string_view button_6 = "Button 6";
inline constexpr std::string_view button_7 = "Button 7";
inline constexpr std::string_view button_8 = "Button 8";
/// Short names a key shows. The card keeps the action's full name.
inline constexpr std::string_view cap_store = "Store";
inline constexpr std::string_view cap_set = "Set";
inline constexpr std::string_view cap_group = "Group";
inline constexpr std::string_view cap_page = "Page";
inline constexpr std::string_view cap_cat = "Cat.";
inline constexpr std::string_view cap_idle_bld = "Idle bld";
inline constexpr std::string_view cap_idle_fac = "Idle fac";
inline constexpr std::string_view cap_sel_bld = "Sel bld";
inline constexpr std::string_view cap_sel_fac = "Sel fac";
inline constexpr std::string_view cap_cmdr = "Cmdr";
inline constexpr std::string_view cap_same = "Same";
inline constexpr std::string_view cap_armed = "Armed";
inline constexpr std::string_view cap_box_armed = "Box: armed";
inline constexpr std::string_view cap_box_fact = "Box: fact.";
inline constexpr std::string_view cap_box_bld = "Box: bld";
inline constexpr std::string_view cap_destruct = "Destruct";
inline constexpr std::string_view cap_on_off = "On/off";
inline constexpr std::string_view cap_fire = "Fire";
inline constexpr std::string_view cap_moves = "Moves";
inline constexpr std::string_view cap_force = "Force";
inline constexpr std::string_view cap_build = "Build";
inline constexpr std::string_view cap_shot = "Shot";
inline constexpr std::string_view cap_resources = "Resources";
inline constexpr std::string_view cap_kills = "Kills";
inline constexpr std::string_view cap_macro = "Macro";
inline constexpr std::string_view cap_clear = "Clear";
inline constexpr std::string_view cap_bars = "Bars";
inline constexpr std::string_view cap_faster = "Faster";
inline constexpr std::string_view cap_slower = "Slower";
inline constexpr std::string_view cap_pin = "Pin";
inline constexpr std::string_view cap_status = "Status";
inline constexpr std::string_view cap_marker = "Marker";
inline constexpr std::string_view cap_prev_pg = "Prev pg";
inline constexpr std::string_view cap_next_pg = "Next pg";
inline constexpr std::string_view cap_follow_back = "Follow back";
inline constexpr std::string_view cap_space_up = "Space +";
inline constexpr std::string_view cap_space_down = "Space -";
/// The wheel and the middle button, above the mouse picture.
inline constexpr std::string_view cap_wheel_middle = "Wheel and middle";

/// The footer's IMPORT….
inline constexpr std::string_view import_keys = "IMPORT…";
/// The footer's SHARE….
inline constexpr std::string_view share_keys = "SHARE…";
/// The footer's PASTE….
inline constexpr std::string_view paste_keys = "PASTE…";
/// The share prompt's title.
inline constexpr std::string_view share_title = "Share your keys";
/// The name field's label.
inline constexpr std::string_view share_name_label = "Name";
/// The base line's label.
inline constexpr std::string_view starts_from = "Starts from";
/// The changes line's label. The catalogue already has this word.
inline constexpr std::string_view changes_label = "Changes";
/// How many keys and helpers the share text carries.
inline constexpr std::string_view keys_and_helpers = "{keys} keys, {helpers} helpers";
/// The same line when the text carries one key and one helper.
inline constexpr std::string_view keys_one_helper_one = "1 key, 1 helper";
/// The same line when the text carries one key.
inline constexpr std::string_view keys_one_helpers = "1 key, {helpers} helpers";
/// The same line when the text carries one helper.
inline constexpr std::string_view keys_helpers_one = "{keys} keys, 1 helper";
/// Where SAVE FILE… wrote the package. `{place}` is the path.
inline constexpr std::string_view saved_to = "Saved to {place}";
/// Opens the folder a package was written to.
inline constexpr std::string_view open_folder = "OPEN FOLDER";
/// What the paste text would change. `{n}` is the count and `{layout}` the base.
inline constexpr std::string_view text_changes = "The text changes {n} keys of {layout}.";
/// The same line when the text changes one key.
inline constexpr std::string_view text_changes_one = "The text changes 1 key of {layout}.";
/// A row whose chord is a key's place.
inline constexpr std::string_view place_word = "place";
/// APPLY on the paste prompt. `{n}` is how many rows it writes.
inline constexpr std::string_view apply_changes = "APPLY {n} CHANGES";
/// APPLY when it writes one row.
inline constexpr std::string_view apply_change_one = "APPLY 1 CHANGE";
/// The line under an imported layout. `{file}` is the file's name.
inline constexpr std::string_view imported_from_file = "Imported from {file}";
/// A legacy key Open Annihilation has no action for.
inline constexpr std::string_view not_used = "not used by Open Annihilation";
/// The import summary. `{n}` were read, `{m}` differ, `{layout}` is the base.
inline constexpr std::string_view found_differ = "Found {n} keys · {m} differ from {layout}";
/// The summary when one key was read and it differs.
inline constexpr std::string_view found_one_differs_one = "Found 1 key · 1 differs from {layout}";
/// The summary when one key was read.
inline constexpr std::string_view found_one_key = "Found 1 key · {m} differ from {layout}";
/// The summary when one key differs.
inline constexpr std::string_view found_differs_one = "Found {n} keys · 1 differs from {layout}";
/// IMPORT on the import prompt. `{n}` is how many keys it writes.
inline constexpr std::string_view import_n_keys = "IMPORT {n} KEYS";
/// IMPORT when it writes one key.
inline constexpr std::string_view import_one_key = "IMPORT 1 KEY";
/// Asks for another legacy file.
inline constexpr std::string_view choose_another_file = "CHOOSE ANOTHER FILE…";
/// A left-hand or right-hand modifier. `{modifier}` is Shift, Ctrl or Alt.
inline constexpr std::string_view hands_are_one =
    "Left and right {modifier} are one key in Open Annihilation";
/// The install question, above each problem.
inline constexpr std::string_view layout_has_problems = "This layout has problems:";
/// The paste prompt's title.
inline constexpr std::string_view paste_title = "Paste keys";
/// The import prompt's title.
inline constexpr std::string_view import_title = "Import keys from an older setup";
/// Under the share text.
inline constexpr std::string_view share_file_note =
    "The file is a .oakeys package anyone can open in Open Annihilation. The text carries the keys "
    "alone, for a forum or a chat.";
/// Under the paste summary.
inline constexpr std::string_view nothing_until_apply = "Nothing changes until you apply it.";
/// COPY AS TEXT.
inline constexpr std::string_view copy_as_text = "COPY AS TEXT";
/// SAVE FILE….
inline constexpr std::string_view save_file = "SAVE FILE…";
/// CLOSE. The catalogue already has this word.
inline constexpr std::string_view close_word = "CLOSE";
/// A paste clash. `{chord}` is the key and `{action}` the action that keeps it.
inline constexpr std::string_view paste_clash =
    "One clash: {chord} already belongs to {action}. Apply keeps yours and lists the clash.";
/// An unknown action a paste keeps.
inline constexpr std::string_view kept_for_later =
    "{action} is kept in your layout for a version that has it.";
/// The import prompt's first source.
inline constexpr std::string_view from_eye = "A mod's Ctrl+F2 keys";
/// The aside under that source.
inline constexpr std::string_view from_eye_aside = "The keys OA keeps for TA Zero, ESC or Twilight";
/// The import prompt's tdraw source.
inline constexpr std::string_view from_tdraw = "tdraw's Settings.ini";
/// The aside under that source.
inline constexpr std::string_view from_tdraw_aside = "Mayhem, Twilight and older patches";
/// The import prompt's cnc-ddraw source.
inline constexpr std::string_view from_ddraw = "cnc-ddraw's ddraw.ini";
/// The aside under that source.
inline constexpr std::string_view from_ddraw_aside = "Full screen, screenshot and pointer keys";
/// The file the import is showing. `{file}` is its name.
inline constexpr std::string_view chosen_file = "Chosen file: {file}";
/// What an import does to a chord another action holds.
inline constexpr std::string_view import_moves =
    "Importing gives the key to this action and leaves {action} without it.";
/// Under the import rows.
inline constexpr std::string_view numbers_are_places =
    "Numbers are Windows key codes, read as key places. The changes go on top of your layout as "
    "\"Imported from {file}\". Nothing else changes.";

// Commands first: the list, Find by key, the capture's outcomes, the one way
// a clash is settled, the draft and its Undo, and the keyboard inspector.

/// The category the editor opens on: the commands most players change.
inline constexpr std::string_view common_commands = "Common commands";
/// The last category: every action, the registry's whole list.
inline constexpr std::string_view all_actions = "All actions";
/// The search field, while it is empty. Typing searches commands only.
inline constexpr std::string_view search_commands = "Search commands";
/// The button that listens for a key and lists what uses it.
inline constexpr std::string_view find_by_key = "Find by key";
/// Find by key's line before a key is pressed.
inline constexpr std::string_view find_press =
    "Press a key or a mouse button to see what it does. Nothing is changed.";
/// Find by key's line for a chord in use. `{chord}` is the chord and `{n}` a number.
inline constexpr std::string_view find_uses = "{chord} is used in {n} places. Nothing is changed.";
/// The same line for a chord used once.
inline constexpr std::string_view find_uses_one = "{chord} is used in 1 place. Nothing is changed.";
/// Find by key's line for a chord nothing uses.
inline constexpr std::string_view find_free = "{chord} is free: nothing uses it.";
/// Under Find by key's list: how to look up another chord.
inline constexpr std::string_view find_again = "Click the key, then press another to look it up.";
/// Find by key's footer.
inline constexpr std::string_view find_escape = "Escape closes · hold Escape to look it up";
/// A Find by key row's link: choose that action in the list.
inline constexpr std::string_view open_action = "Open ›";
/// Closes Find by key.
inline constexpr std::string_view close_title = "Close";
/// The capture's eyebrow while a chord is changed. `{context}` is where it is read.
inline constexpr std::string_view change_eyebrow = "Change a key · {context}";
/// The capture's eyebrow while a key is added.
inline constexpr std::string_view add_eyebrow = "Add a key · {context}";
/// The capture's title. `{action}` is the action.
inline constexpr std::string_view new_key_for = "New key for {action}";
/// Under a finished chord: the well takes a press to listen again.
inline constexpr std::string_view try_another = "Click the key to try another.";
/// The capture's and the details' disclosure.
inline constexpr std::string_view more_options = "More options";
/// The capture's switch: the key's place rather than its letter.
inline constexpr std::string_view same_place = "Same place on every keyboard";
/// The capture's gesture label.
inline constexpr std::string_view how_it_acts = "How it acts";
/// The capture's primary button for a free chord. `{chord}` and `{action}`.
inline constexpr std::string_view use_for = "Use {chord} for {action}";
/// The capture's badge for a chord in use. `{chord}` and `{context}`.
inline constexpr std::string_view in_use = "{chord} is in use {context}";
/// Under that badge. `{name}` is every other action.
inline constexpr std::string_view in_use_by = "{name} uses it. Nothing changes until you choose.";
/// The capture's primary button for a chord in use.
inline constexpr std::string_view choose_outcome = "Choose what happens…";
/// The clash question's title. `{chord}` is the chord.
inline constexpr std::string_view already_in_use = "{chord} is already in use";
/// The clash question's sentence. `{name}` and `{context}`.
inline constexpr std::string_view uses_it =
    "{name} uses it {context}. Choose what happens; nothing changes until you confirm.";
/// The draft clash's sentence. `{name}` is the other action and `{context}` where both are read.
inline constexpr std::string_view both_use =
    "Here and {name} both use it {context}. Choose which keeps it; nothing changes until you "
    "confirm.";
/// An outcome: the chord leaves the other action. `{action}` takes it.
inline constexpr std::string_view move_it_to = "Move it to {action}";
/// An outcome: the two actions trade chords.
inline constexpr std::string_view swap_them = "Swap them";
/// An outcome: one action keeps the shared chord. `{action}` keeps it.
inline constexpr std::string_view keep_it_for = "Keep it for {action}";
/// The confirm button of Move it. `{action}` takes the chord.
inline constexpr std::string_view move_to = "Move to {action}";
/// What an action is left with when it gives up its only chord.
inline constexpr std::string_view no_key = "no key";
/// Back to the capture from the clash question.
inline constexpr std::string_view choose_another_key = "Choose another key";
/// A use of the chord that never meets this action's. `{chord}`, `{name}`,
/// `{context}` (where that one is read) and `{where}` (where this one is).
inline constexpr std::string_view also_used =
    "{chord} also does {name} {context}. That never meets {where}, so it stays.";
/// The details' button that opens the clash question for a draft clash.
inline constexpr std::string_view settle_button = "Settle…";
/// A chord's button in the details: capture another for it.
inline constexpr std::string_view change_button = "Change…";
/// The details' button that adds a chord.
inline constexpr std::string_view add_a_key = "+ Add a key";
/// The details' foot while the action has no change of the player's. `{layout}`.
inline constexpr std::string_view from_layout = "From {layout}";
/// The details' foot while the player changed the action.
inline constexpr std::string_view your_change_on = "Your change, over {layout}";
/// The details' button that drops the player's change.
inline constexpr std::string_view reset_title = "Reset";
/// The details' fact that names the action's id and its 3.1c word.
inline constexpr std::string_view known_as = "Known as";
/// The footer's draft line. `{n}` is a number.
inline constexpr std::string_view unsaved_edits = "{n} unsaved edits";
/// The same line for one edit.
inline constexpr std::string_view unsaved_edit_one = "1 unsaved edit";
/// The footer's draft line while nothing is unsaved.
inline constexpr std::string_view no_unsaved = "No unsaved edits";
/// Compact's draft line. `{n}` is a number.
inline constexpr std::string_view unsaved_short = "{n} unsaved";
/// The footer's button that takes back the last change.
inline constexpr std::string_view undo = "Undo";
/// The footer's menu of the rarer layout actions.
inline constexpr std::string_view more_title = "More";
/// The footer's button that closes without saving.
inline constexpr std::string_view cancel_title = "Cancel";
/// The footer's primary button.
inline constexpr std::string_view save_changes = "Save changes";
/// More's items.
inline constexpr std::string_view restore_title = "Restore defaults";
inline constexpr std::string_view import_button = "Import…";
inline constexpr std::string_view share_button = "Share…";
inline constexpr std::string_view paste_button = "Paste…";
/// The close question. `{n}` is a number.
inline constexpr std::string_view close_question = "Save your {n} edits before closing?";
/// The same question for one edit.
inline constexpr std::string_view close_question_one = "Save your edit before closing?";
/// The close question's buttons.
inline constexpr std::string_view save_word = "Save";
inline constexpr std::string_view discard = "Discard";
inline constexpr std::string_view keep_editing_title = "Keep editing";
/// The keyboard's switch to the list of every key, and back.
inline constexpr std::string_view show_as_list = "Show as list";
inline constexpr std::string_view show_keyboard = "Show keyboard";
/// The inspector's heading. `{key}` is the key.
inline constexpr std::string_view what_key_does = "What {key} does";
/// The inspector's line for a context the key does nothing in.
inline constexpr std::string_view does_nothing = "Nothing";
/// The inspector's button for the key's action while playing. `{action}`.
inline constexpr std::string_view change_action_key = "Change {action}'s key…";
/// The inspector's button for the action chosen in the list. `{key}`, `{action}`.
inline constexpr std::string_view use_key_for = "Use {key} for {action}…";
/// The inspector while no key is chosen.
inline constexpr std::string_view select_a_key = "Select a key to see everything on it.";
/// Under the keyboard.
inline constexpr std::string_view grey_keys_free =
    "Grey keys are free while playing. Select a key to see what it does in each place.";
/// A keycap's mark for the player's own change, in the legend.
inline constexpr std::string_view your_change_mark = "Your change";
/// The details' button, under More options, that outlines the action's keys.
inline constexpr std::string_view find_on_keyboard_title = "Find on keyboard";
/// The Library's notice after USE, for every game. `{layout}` is the layout.
inline constexpr std::string_view using_everywhere = "Using {layout} for every game.";
/// The same notice for one mod. `{layout}` and `{mod}`.
inline constexpr std::string_view using_with_mod = "Using {layout} with {mod}.";
/// The details' draft clash, beside Settle…. `{name}` and `{context}`.
inline constexpr std::string_view here_and_there = "Here and {name} both use it {context}.";

/// The keys overlay's title while a match is being played.
inline constexpr std::string_view keys_while_playing = "Keys while playing";
/// The keys overlay's title while a match is being watched.
inline constexpr std::string_view keys_while_watching = "Keys while watching";
/// The keys overlay's title while a building is being placed.
inline constexpr std::string_view keys_while_placing = "Keys while placing a building";
/// The layout's name and how many of the player's own changes it carries.
/// `{name}` is the layout and `{n}` is a number, including 0 and 1.
inline constexpr std::string_view overlay_changes = "{name} · {n} changes";
/// The button that opens the key editor from the overlay.
inline constexpr std::string_view edit_keys = "EDIT KEYS…";
/// The label before the latches and sticky modifiers that are on.
inline constexpr std::string_view on_line = "On:";
/// The group of actions that exist only because a mod's hack is on.
inline constexpr std::string_view this_mod = "This mod";
/// A run of one numbered family. `{first}` is the first chord, `{last}` the last key.
inline constexpr std::string_view chord_range = "{first}…{last}";
/// How the overlay closes. `{modifier}` is Primary's name, Ctrl or Cmd.
inline constexpr std::string_view overlay_footer =
    "{modifier}+/ or Escape closes this. More in EDIT KEYS…";
/// A help line with the chord that fires it. `{description}` is the sentence, `{chord}` the key.
inline constexpr std::string_view help_chord = "{description} · {chord}";
/// The rotate hint when a key and the wheel both turn a building.
inline constexpr std::string_view rotate_both = "Press {key}, or {wheel}, to rotate";
/// The rotate hint when only a key turns a building.
inline constexpr std::string_view rotate_key = "Press {key} to rotate";
/// The rotate hint when only the wheel turns a building.
inline constexpr std::string_view rotate_wheel = "{wheel} rotates";
/// The wheel, in a composed chord such as Alt and this word.
inline constexpr std::string_view wheel_word = "wheel";

} // namespace words

} // namespace oa::ui::key_editor
