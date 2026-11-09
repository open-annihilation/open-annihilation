// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Builds an AccessKit tree update through the C API and reads a node's role
// and label back. On Windows the functions are loaded from accesskit.dll
// beside this program. A Windows 95 build checks that the bridge is left out.

#include "oa/platform/screen_reader.hpp"
#include "oa/platform/screen_reader/announce.hpp"

#include "oa/test/check.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#if !defined(OA_SCREEN_READER_COMPILED_OUT)
#include "oa/platform/screen_reader/api.hpp"
#include "oa/platform/screen_reader/tree.hpp"
#endif

#if defined(_WIN32) && !defined(OA_SCREEN_READER_COMPILED_OUT)
#include <windows.h>
#endif

namespace {

namespace screen = oa::platform::screen_reader;

#if !defined(OA_SCREEN_READER_COMPILED_OUT)
/// The test node is a label, and the window node owns it, both read back
/// before the tree update takes the nodes.
void role_and_label_round_trip() {
    const screen::Calls* table = screen::calls();
    if (table == nullptr) {
        OA_CHECK(table != nullptr);
        return;
    }
    accesskit_node* label = table->node_new(ACCESSKIT_ROLE_LABEL);
    if (label == nullptr) {
        OA_CHECK(label != nullptr);
        return;
    }
    table->node_set_label(label, screen::test_label);
    OA_CHECK(table->node_role(label) == ACCESSKIT_ROLE_LABEL);
    char* text = table->node_label(label);
    OA_CHECK(text != nullptr && std::strcmp(text, screen::test_label) == 0);
    if (text != nullptr)
        table->string_free(text);

    accesskit_node* window = table->node_new(ACCESSKIT_ROLE_WINDOW);
    if (window == nullptr) {
        OA_CHECK(window != nullptr);
        table->node_free(label);
        return;
    }
    OA_CHECK(table->node_role(window) == ACCESSKIT_ROLE_WINDOW);
    table->node_push_child(window, screen::test_node);

    accesskit_tree_update* update =
        table->tree_update_with_capacity_and_focus(2, screen::test_node);
    if (update == nullptr) {
        OA_CHECK(update != nullptr);
        table->node_free(label);
        table->node_free(window);
        return;
    }
    accesskit_tree_info* info = table->tree_info_new(screen::window_node);
    OA_CHECK(info != nullptr);
    if (info != nullptr)
        table->tree_update_set_tree_info(update, info);
    table->tree_update_push_node(update, screen::test_node, label);
    table->tree_update_push_node(update, screen::window_node, window);
    table->tree_update_free(update);
}

/// Reads a string AccessKit returned, and frees it.
///
/// @param table the bound C functions
/// @param text the string; null is an empty result
/// @return the characters
std::string take_text(const screen::Calls& table, char* text) {
    if (text == nullptr)
        return {};
    const std::string copy(text);
    table.string_free(text);
    return copy;
}

/// One control of the fixture, with the bounds and the actions the reader sees.
///
/// @param id the id
/// @param role the role
/// @param label the name
/// @return the control
screen::Node control_of(uint64_t id, screen::Role role, const char* label) {
    screen::Node node;
    node.id = id;
    node.role = role;
    node.label = label;
    node.x = 10.0;
    node.y = 20.0;
    node.width = 30.0;
    node.height = 40.0;
    return node;
}

/// Maps a fixture that holds each role the reader is shown, reads every node
/// back through AccessKit's C API, and turns each action into the game's event.
void roles_actions_and_silence() {
    const screen::Calls* table = screen::calls();
    if (table == nullptr) {
        OA_CHECK(table != nullptr);
        return;
    }

    std::vector<screen::Node> nodes;
    screen::Node window =
        control_of(screen::window_node, screen::Role::window, "Open Annihilation");
    screen::Node button = control_of(10, screen::Role::button, "Single");
    button.shortcut = "S";
    screen::Node colour = control_of(11, screen::Role::button, "Colour");
    colour.value_text = "Blue, player 1";
    colour.steps = true;
    screen::Node box = control_of(12, screen::Role::check_box, "Ready");
    box.toggled_known = true;
    box.toggled = true;
    screen::Node toggle = control_of(13, screen::Role::switch_control, "Chat");
    toggle.toggled_known = true;
    toggle.toggled = false;
    screen::Node radios = control_of(14, screen::Role::radio_group, "Side");
    screen::Node radio = control_of(15, screen::Role::radio, "Arm");
    radio.selected_known = true;
    radio.selected = true;
    radio.place = 1;
    radio.set_size = 2;
    radios.children.push_back(radio.id);
    screen::Node tabs = control_of(16, screen::Role::tab_list, "Sections");
    screen::Node tab = control_of(17, screen::Role::tab, "Graphics");
    tab.selected_known = true;
    tab.selected = true;
    tab.place = 2;
    tab.set_size = 9;
    tabs.children.push_back(tab.id);
    screen::Node row_list = control_of(18, screen::Role::list, "Games");
    screen::Node row = control_of(19, screen::Role::list_item, "Metal");
    row.selected_known = true;
    row.selected = true;
    row.place = 3;
    row.set_size = 40;
    row_list.children.push_back(row.id);
    screen::Node contents = control_of(20, screen::Role::tree, "Contents");
    screen::Node contents_row = control_of(21, screen::Role::tree_item, "Maps");
    contents_row.expanded_known = true;
    contents_row.expanded = true;
    contents.children.push_back(contents_row.id);
    screen::Node field = control_of(22, screen::Role::text_field, "Name");
    field.value_text = "Ada";
    screen::Node secret = control_of(23, screen::Role::password, "Password");
    secret.value_text = "4";
    screen::Node search = control_of(24, screen::Role::search, "Search");
    search.value_text = "tank";
    screen::Node slider = control_of(25, screen::Role::slider, "Gamma");
    slider.value_text = "Gamma 1.0";
    slider.has_value = true;
    slider.value = 3.0;
    slider.has_range = true;
    slider.least = 0.0;
    slider.most = 10.0;
    slider.step = 1.0;
    screen::Node log = control_of(26, screen::Role::log, "Chat log");
    log.live = screen::LiveSetting::polite;
    screen::Node dialog = control_of(27, screen::Role::dialog, "Settings");
    dialog.modal = true;
    screen::Node alert = control_of(28, screen::Role::alert_dialog, "The disc is missing");
    alert.modal = true;
    alert.live = screen::LiveSetting::assertive;
    screen::Node group = control_of(29, screen::Role::group, "Recon");
    screen::Node picture = control_of(30, screen::Role::image, "Latency good");
    screen::Node link = control_of(31, screen::Role::link, "Manual");
    screen::Node caption = control_of(32, screen::Role::label, "Commander");
    caption.description = "The player's name";
    screen::Node covered = control_of(33, screen::Role::button, "Covered");
    covered.hidden = true;
    screen::Node decoration = control_of(34, screen::Role::none, "Frame");
    screen::Node grayed = control_of(35, screen::Role::button, "Start");
    grayed.disabled = true;
    screen::Node grayed_words = control_of(36, screen::Role::label, "No Mod");
    grayed_words.disabled = true;

    window.children = {button.id, colour.id,   box.id,      toggle.id,  radios.id,
                       tabs.id,   row_list.id, contents.id, field.id,   secret.id,
                       search.id, slider.id,   log.id,      dialog.id,  alert.id,
                       group.id,  picture.id,  link.id,     caption.id, grayed.id};

    nodes.push_back(window);
    nodes.push_back(button);
    nodes.push_back(colour);
    nodes.push_back(box);
    nodes.push_back(toggle);
    nodes.push_back(radios);
    nodes.push_back(radio);
    nodes.push_back(tabs);
    nodes.push_back(tab);
    nodes.push_back(row_list);
    nodes.push_back(row);
    nodes.push_back(contents);
    nodes.push_back(contents_row);
    nodes.push_back(field);
    nodes.push_back(secret);
    nodes.push_back(search);
    nodes.push_back(slider);
    nodes.push_back(log);
    nodes.push_back(dialog);
    nodes.push_back(alert);
    nodes.push_back(group);
    nodes.push_back(picture);
    nodes.push_back(link);
    nodes.push_back(caption);
    nodes.push_back(covered);
    nodes.push_back(decoration);
    nodes.push_back(grayed);

    const std::vector<uint64_t> kept = screen::kept_ids(nodes);
    OA_CHECK(kept.size() == nodes.size() - 2);
    for (const uint64_t id : kept)
        OA_CHECK(id != covered.id && id != decoration.id);
    const uint64_t same = screen::fingerprint(nodes, button.id);
    OA_CHECK(screen::fingerprint(nodes, button.id) == same);
    std::vector<screen::Node> renamed = nodes;
    renamed[1].label = "Skirmish";
    OA_CHECK(screen::fingerprint(renamed, button.id) != same);
    OA_CHECK(screen::fingerprint(nodes, radio.id) != same);

    const screen::Role roles[] = {
        screen::Role::window,         screen::Role::button,      screen::Role::check_box,
        screen::Role::switch_control, screen::Role::radio_group, screen::Role::radio,
        screen::Role::tab_list,       screen::Role::tab,         screen::Role::list,
        screen::Role::list_item,      screen::Role::tree,        screen::Role::tree_item,
        screen::Role::text_field,     screen::Role::password,    screen::Role::search,
        screen::Role::slider,         screen::Role::log,         screen::Role::dialog,
        screen::Role::alert_dialog,   screen::Role::group,       screen::Role::image,
        screen::Role::link,           screen::Role::label,
    };
    const accesskit_role expected[] = {
        ACCESSKIT_ROLE_WINDOW,       ACCESSKIT_ROLE_BUTTON,
        ACCESSKIT_ROLE_CHECK_BOX,    ACCESSKIT_ROLE_SWITCH,
        ACCESSKIT_ROLE_RADIO_GROUP,  ACCESSKIT_ROLE_RADIO_BUTTON,
        ACCESSKIT_ROLE_TAB_LIST,     ACCESSKIT_ROLE_TAB,
        ACCESSKIT_ROLE_LIST,         ACCESSKIT_ROLE_LIST_ITEM,
        ACCESSKIT_ROLE_TREE,         ACCESSKIT_ROLE_TREE_ITEM,
        ACCESSKIT_ROLE_TEXT_INPUT,   ACCESSKIT_ROLE_PASSWORD_INPUT,
        ACCESSKIT_ROLE_SEARCH_INPUT, ACCESSKIT_ROLE_SLIDER,
        ACCESSKIT_ROLE_LOG,          ACCESSKIT_ROLE_DIALOG,
        ACCESSKIT_ROLE_ALERT_DIALOG, ACCESSKIT_ROLE_GROUP,
        ACCESSKIT_ROLE_IMAGE,        ACCESSKIT_ROLE_LINK,
        ACCESSKIT_ROLE_LABEL,
    };
    for (std::size_t index = 0; index < sizeof roles / sizeof roles[0]; ++index) {
        const screen::Node* found = nullptr;
        for (const screen::Node& node : nodes) {
            if (node.role == roles[index] && !node.hidden && !node.disabled) {
                found = &node;
                break;
            }
        }
        OA_CHECK(found != nullptr);
        if (found == nullptr)
            continue;
        accesskit_node* made = screen::make_node(*found);
        OA_CHECK(made != nullptr);
        if (made == nullptr)
            continue;
        OA_CHECK(table->node_role(made) == expected[index]);
        OA_CHECK(take_text(*table, table->node_label(made)) == found->label);
        if (!found->value_text.empty())
            OA_CHECK(take_text(*table, table->node_value(made)) == found->value_text);
        if (!found->description.empty())
            OA_CHECK(take_text(*table, table->node_description(made)) == found->description);
        if (!found->shortcut.empty())
            OA_CHECK(take_text(*table, table->node_keyboard_shortcut(made)) == found->shortcut);
        const accesskit_opt_rect bounds = table->node_bounds(made);
        OA_CHECK(bounds.has_value);
        if (bounds.has_value) {
            OA_CHECK(bounds.value.x0 == found->x);
            OA_CHECK(bounds.value.y0 == found->y);
            OA_CHECK(bounds.value.x1 == found->x + found->width);
            OA_CHECK(bounds.value.y1 == found->y + found->height);
        }
        table->node_free(made);
    }

    accesskit_node* gamma = screen::make_node(slider);
    OA_CHECK(gamma != nullptr);
    if (gamma != nullptr) {
        const accesskit_opt_double number = table->node_numeric_value(gamma);
        const accesskit_opt_double least = table->node_min_numeric_value(gamma);
        const accesskit_opt_double most = table->node_max_numeric_value(gamma);
        const accesskit_opt_double step = table->node_numeric_value_step(gamma);
        OA_CHECK(number.has_value && number.value == 3.0);
        OA_CHECK(least.has_value && least.value == 0.0);
        OA_CHECK(most.has_value && most.value == 10.0);
        OA_CHECK(step.has_value && step.value == 1.0);
        OA_CHECK(take_text(*table, table->node_value(gamma)) == "Gamma 1.0");
        table->node_free(gamma);
    }

    accesskit_node* ready = screen::make_node(box);
    OA_CHECK(ready != nullptr);
    if (ready != nullptr) {
        const accesskit_opt_toggled toggled = table->node_toggled(ready);
        OA_CHECK(toggled.has_value && toggled.value == ACCESSKIT_TOGGLED_TRUE);
        table->node_free(ready);
    }
    accesskit_node* chat = screen::make_node(toggle);
    OA_CHECK(chat != nullptr);
    if (chat != nullptr) {
        const accesskit_opt_toggled toggled = table->node_toggled(chat);
        OA_CHECK(toggled.has_value && toggled.value == ACCESSKIT_TOGGLED_FALSE);
        table->node_free(chat);
    }

    accesskit_node* arm = screen::make_node(radio);
    OA_CHECK(arm != nullptr);
    if (arm != nullptr) {
        const accesskit_opt_bool selected = table->node_is_selected(arm);
        const accesskit_opt_index place = table->node_position_in_set(arm);
        const accesskit_opt_index size = table->node_size_of_set(arm);
        OA_CHECK(selected.has_value && selected.value);
        // AccessKit counts from 0: the first of two.
        OA_CHECK(place.has_value && place.value == 0);
        OA_CHECK(size.has_value && size.value == 2);
        table->node_free(arm);
    }

    accesskit_node* maps = screen::make_node(contents_row);
    OA_CHECK(maps != nullptr);
    if (maps != nullptr) {
        const accesskit_opt_bool expanded = table->node_is_expanded(maps);
        OA_CHECK(expanded.has_value && expanded.value);
        OA_CHECK(table->node_supports_action(maps, ACCESSKIT_ACTION_EXPAND));
        OA_CHECK(table->node_supports_action(maps, ACCESSKIT_ACTION_COLLAPSE));
        table->node_free(maps);
    }

    accesskit_node* chat_log = screen::make_node(log);
    OA_CHECK(chat_log != nullptr);
    if (chat_log != nullptr) {
        const accesskit_opt_live live = table->node_live(chat_log);
        OA_CHECK(live.has_value && live.value == ACCESSKIT_LIVE_POLITE);
        table->node_free(chat_log);
    }
    accesskit_node* missing = screen::make_node(alert);
    OA_CHECK(missing != nullptr);
    if (missing != nullptr) {
        OA_CHECK(table->node_is_modal(missing));
        const accesskit_opt_live live = table->node_live(missing);
        OA_CHECK(live.has_value && live.value == ACCESSKIT_LIVE_ASSERTIVE);
        table->node_free(missing);
    }
    accesskit_node* settings = screen::make_node(dialog);
    OA_CHECK(settings != nullptr);
    if (settings != nullptr) {
        OA_CHECK(table->node_is_modal(settings));
        table->node_free(settings);
    }

    accesskit_node* single = screen::make_node(button);
    OA_CHECK(single != nullptr);
    if (single != nullptr) {
        OA_CHECK(table->node_supports_action(single, ACCESSKIT_ACTION_CLICK));
        OA_CHECK(!table->node_supports_action(single, ACCESSKIT_ACTION_FOCUS));
        OA_CHECK(!table->node_supports_action(single, ACCESSKIT_ACTION_SET_VALUE));
        const accesskit_node_ids children = table->node_children(single);
        OA_CHECK(children.length == 0);
        table->node_free(single);
    }
    accesskit_node* side = screen::make_node(radios);
    OA_CHECK(side != nullptr);
    if (side != nullptr) {
        const accesskit_node_ids children = table->node_children(side);
        OA_CHECK(
            children.length == 1 && children.values != nullptr && children.values[0] == radio.id
        );
        table->node_free(side);
    }
    accesskit_node* start = screen::make_node(grayed);
    OA_CHECK(start != nullptr);
    if (start != nullptr) {
        OA_CHECK(table->node_is_disabled(start));
        OA_CHECK(!table->node_supports_action(start, ACCESSKIT_ACTION_CLICK));
        table->node_free(start);
    }
    OA_CHECK(screen::make_node(covered) == nullptr);
    OA_CHECK(screen::make_node(decoration) == nullptr);

    // A label's words are its value too, because Windows names a label from
    // its value alone. Words are never greyed out.
    accesskit_node* words = screen::make_node(caption);
    OA_CHECK(words != nullptr);
    if (words != nullptr) {
        OA_CHECK(take_text(*table, table->node_value(words)) == "Commander");
        table->node_free(words);
    }
    accesskit_node* no_mod = screen::make_node(grayed_words);
    OA_CHECK(no_mod != nullptr);
    if (no_mod != nullptr) {
        OA_CHECK(!table->node_is_disabled(no_mod));
        OA_CHECK(take_text(*table, table->node_value(no_mod)) == "No Mod");
        table->node_free(no_mod);
    }

    const std::optional<std::string> none;
    const std::optional<double> no_number;
    const std::optional<screen::Event> clicked =
        screen::event_for(colour, screen::Action::click, none, no_number);
    OA_CHECK(clicked.has_value() && clicked->kind == screen::EventKind::press);
    OA_CHECK(clicked.has_value() && clicked->x == 25.0 && clicked->y == 40.0);
    // Focus never becomes an event: a press would change a radio, a tab, a
    // row or a slider.
    OA_CHECK(!screen::event_for(button, screen::Action::focus, none, no_number).has_value());
    OA_CHECK(!screen::event_for(field, screen::Action::focus, none, no_number).has_value());
    OA_CHECK(!screen::event_for(radio, screen::Action::focus, none, no_number).has_value());
    OA_CHECK(!screen::event_for(slider, screen::Action::focus, none, no_number).has_value());
    OA_CHECK(!screen::event_for(picture, screen::Action::focus, none, no_number).has_value());

    // A key or text reaches the game's focused control, so it is sent only
    // when that is the control the reader named. Increment of a button whose
    // press steps its value one way is that press, and does not need the focus.
    // Decrement is not that press. A drop-down is not a step.
    const std::optional<double> two_steps(5.0);
    const std::optional<std::string> typed("Ada");
    const std::optional<screen::Event> stepped =
        screen::event_for(colour, screen::Action::increment, none, no_number);
    OA_CHECK(stepped.has_value() && stepped->kind == screen::EventKind::press);
    OA_CHECK(stepped.has_value() && stepped->x == 25.0 && stepped->y == 40.0);
    accesskit_node* stepped_node = screen::make_node(colour);
    OA_CHECK(stepped_node != nullptr);
    if (stepped_node != nullptr) {
        OA_CHECK(table->node_supports_action(stepped_node, ACCESSKIT_ACTION_INCREMENT));
        OA_CHECK(!table->node_supports_action(stepped_node, ACCESSKIT_ACTION_DECREMENT));
        table->node_free(stepped_node);
    }
    OA_CHECK(!screen::event_for(colour, screen::Action::decrement, none, no_number).has_value());
    screen::Node choice = control_of(26, screen::Role::button, "Maximum zoom out");
    choice.value_text = "Automatic";
    choice.focused = true;
    OA_CHECK(!screen::event_for(choice, screen::Action::increment, none, no_number).has_value());
    OA_CHECK(!screen::event_for(choice, screen::Action::decrement, none, no_number).has_value());
    const std::optional<screen::Event> opened_choice =
        screen::event_for(choice, screen::Action::click, none, no_number);
    OA_CHECK(opened_choice.has_value() && opened_choice->kind == screen::EventKind::press);
    OA_CHECK(!screen::event_for(slider, screen::Action::decrement, none, no_number).has_value());
    OA_CHECK(!screen::event_for(slider, screen::Action::set_value, none, two_steps).has_value());
    OA_CHECK(!screen::event_for(contents_row, screen::Action::expand, none, no_number).has_value());
    OA_CHECK(!screen::event_for(field, screen::Action::set_value, typed, no_number).has_value());
    screen::Node focused_colour = colour;
    focused_colour.focused = true;
    screen::Node focused_slider = slider;
    focused_slider.focused = true;
    screen::Node focused_row = contents_row;
    focused_row.focused = true;
    screen::Node focused_field = field;
    focused_field.focused = true;

    const std::optional<screen::Event> next =
        screen::event_for(focused_colour, screen::Action::increment, none, no_number);
    OA_CHECK(next.has_value() && next->kind == screen::EventKind::press);
    OA_CHECK(next.has_value() && next->x == 25.0 && next->y == 40.0);
    OA_CHECK(
        !screen::event_for(focused_colour, screen::Action::decrement, none, no_number).has_value()
    );
    const std::optional<screen::Event> previous =
        screen::event_for(focused_slider, screen::Action::decrement, none, no_number);
    OA_CHECK(
        previous.has_value() && previous->kind == screen::EventKind::key &&
        previous->key == screen::Key::left
    );
    const std::optional<screen::Event> opened =
        screen::event_for(focused_row, screen::Action::expand, none, no_number);
    OA_CHECK(opened.has_value() && opened->key == screen::Key::right);
    const std::optional<screen::Event> closed =
        screen::event_for(focused_row, screen::Action::collapse, none, no_number);
    OA_CHECK(closed.has_value() && closed->key == screen::Key::left);
    const std::optional<screen::Event> set_text =
        screen::event_for(focused_field, screen::Action::set_value, typed, no_number);
    OA_CHECK(
        set_text.has_value() && set_text->kind == screen::EventKind::text &&
        set_text->text == "Ada" && set_text->clear == 3
    );
    screen::Node masked = secret;
    masked.focused = true;
    masked.value_text = "\xE2\x80\xA2\xE2\x80\xA2";
    const std::optional<screen::Event> replaced =
        screen::event_for(masked, screen::Action::set_value, typed, no_number);
    OA_CHECK(
        replaced.has_value() && replaced->kind == screen::EventKind::text && replaced->clear == 2
    );
    const std::optional<double> same_value(3.0);
    OA_CHECK(
        !screen::event_for(focused_slider, screen::Action::set_value, none, same_value).has_value()
    );
    const std::optional<screen::Event> set_slider =
        screen::event_for(focused_slider, screen::Action::set_value, none, two_steps);
    OA_CHECK(
        set_slider.has_value() && set_slider->kind == screen::EventKind::key &&
        set_slider->key == screen::Key::right && set_slider->repeats == 2
    );
    const std::optional<double> beyond(1000.0);
    const std::optional<screen::Event> capped =
        screen::event_for(focused_slider, screen::Action::set_value, none, beyond);
    OA_CHECK(capped.has_value() && capped->repeats == 64);
    OA_CHECK(!screen::event_for(grayed, screen::Action::click, none, no_number).has_value());
    OA_CHECK(!screen::event_for(picture, screen::Action::click, none, no_number).has_value());

    // A switch is pressed on the half it does not show: Off shows, so On.
    const std::optional<screen::Event> flipped =
        screen::event_for(toggle, screen::Action::click, none, no_number);
    OA_CHECK(flipped.has_value() && flipped->x == 10.0 + 30.0 * 0.75 && flipped->y == 40.0);
    screen::Node switched_on = toggle;
    switched_on.toggled = true;
    const std::optional<screen::Event> flipped_back =
        screen::event_for(switched_on, screen::Action::click, none, no_number);
    OA_CHECK(flipped_back.has_value() && flipped_back->x == 10.0 + 30.0 * 0.25);

    accesskit_tree_update* update = screen::build_update(nodes, button.id);
    OA_CHECK(update != nullptr);
    if (update != nullptr)
        table->tree_update_free(update);
}

bool* hook_ran = nullptr;

/// The test's stand-in for the adapter. It builds the staged tree and says so.
///
/// @return true, as a push that reached the factory
bool hook_updates() {
    if (hook_ran != nullptr)
        *hook_ran = true;
    accesskit_tree_update* update = screen::update_from_snapshot(nullptr);
    if (update != nullptr) {
        const screen::Calls* table = screen::calls();
        if (table != nullptr && table->tree_update_free != nullptr)
            table->tree_update_free(update);
    }
    return true;
}

/// An unchanged tree is not sent, so the factory does not run.
///
/// A live region left up by an earlier announcement would change the tree,
/// so the region is cleared first.
///
/// The reader is marked active and the push is a hook, so the test fails when
/// the guard is skipped and when the reader was never marked active.
void unchanged_tree_skips_the_factory() {
    screen::testing_reset_announcements();
    screen::Node window;
    window.id = screen::window_node;
    window.role = screen::Role::window;
    window.label = "Open Annihilation";
    const std::vector<screen::Node> nodes{window};
    bool ran = false;
    hook_ran = &ran;
    screen::testing_set_update_hook(hook_updates);
    screen::testing_set_reader_active(true);
    screen::forget_sent_tree();
    screen::clear_snapshot_factory();
    OA_CHECK(screen::stage_tree(nodes, screen::window_node));
    screen::mark_tree_sent(screen::staged_fingerprint());
    OA_CHECK(!screen::stage_tree(nodes, screen::window_node));
    ran = false;
    screen::clear_snapshot_factory();
    screen::submit_tree(nodes, screen::window_node);
    OA_CHECK(!ran);
    OA_CHECK(!screen::snapshot_factory_ran());

    screen::Node added = window;
    added.id = 10;
    added.role = screen::Role::button;
    added.label = "Single";
    const std::vector<screen::Node> changed{window, added};
    ran = false;
    screen::clear_snapshot_factory();
    screen::submit_tree(changed, screen::window_node);
    OA_CHECK(ran);
    OA_CHECK(screen::snapshot_factory_ran());

    screen::testing_set_update_hook(nullptr);
    screen::testing_set_reader_active(false);
    screen::forget_sent_tree();
    hook_ran = nullptr;
}

/// Two lines in one submit are two live nodes. A repeated line is a new node.
void two_lines_are_two_nodes() {
    screen::set_preference(screen::Preference::on);
    screen::testing_reset_announcements();
    screen::forget_sent_tree();
    bool ran = false;
    hook_ran = &ran;
    screen::testing_set_update_hook(hook_updates);
    screen::testing_set_reader_active(true);
    screen::Node window;
    window.id = screen::window_node;
    window.role = screen::Role::window;
    window.label = "Open Annihilation";
    screen::announce("First", screen::Politeness::polite);
    screen::announce("Second", screen::Politeness::assertive);
    screen::submit_tree(std::vector<screen::Node>{window}, screen::window_node);
    const std::vector<screen::Node> staged = screen::testing_staged_nodes();
    uint64_t first_id = 0;
    bool saw_second = false;
    int live = 0;
    bool parented = false;
    for (const screen::Node& node : staged) {
        if (node.id == screen::window_node) {
            parented = node.children.size() >= 2;
            continue;
        }
        if (node.role != screen::Role::label)
            continue;
        ++live;
        OA_CHECK(node.x == 8.0);
        OA_CHECK(node.y >= 8.0);
        OA_CHECK(node.width == 32.0);
        OA_CHECK(node.height == 16.0);
        if (node.label == "First") {
            first_id = node.id;
            OA_CHECK(node.live == screen::LiveSetting::polite);
        }
        if (node.label == "Second") {
            saw_second = true;
            OA_CHECK(node.live == screen::LiveSetting::assertive);
            OA_CHECK(node.id != first_id);
        }
    }
    OA_CHECK(live == 2);
    OA_CHECK(first_id != 0);
    OA_CHECK(saw_second);
    OA_CHECK(parented);

    screen::announce("First", screen::Politeness::polite);
    screen::submit_tree(std::vector<screen::Node>{window}, screen::window_node);
    const std::vector<screen::Node> again = screen::testing_staged_nodes();
    int repeated = 0;
    bool new_id = false;
    for (const screen::Node& node : again) {
        if (node.label != "First")
            continue;
        ++repeated;
        if (node.id != first_id)
            new_id = true;
    }
    OA_CHECK(repeated >= 2);
    OA_CHECK(new_id);

    screen::testing_set_update_hook(nullptr);
    screen::testing_set_reader_active(false);
    screen::forget_sent_tree();
    screen::testing_reset_announcements();
    screen::set_preference(screen::Preference::automatic);
    hook_ran = nullptr;
}
#endif

screen::SpokenOffer
line_of(const char* text, bool collapse, const char* room, const char* speaker) {
    screen::SpokenOffer offer;
    offer.text = text;
    offer.collapsible = collapse;
    offer.room = room;
    offer.speaker = speaker;
    return offer;
}

/// The queue holds polite chat, collapses a burst, and drops a muted player.
void check_queue() {
    OA_CHECK(screen::speak_chat_of("") == screen::SpeakChat::all);
    OA_CHECK(screen::speak_chat_of("later") == screen::SpeakChat::all);
    OA_CHECK(screen::speak_chat_of("off") == screen::SpeakChat::off);
    OA_CHECK(screen::speak_chat_of("pages") == screen::SpeakChat::pages);
    OA_CHECK(screen::speak_lobby_of(""));
    OA_CHECK(screen::speak_lobby_of("1"));
    OA_CHECK(screen::speak_lobby_of("yes"));
    OA_CHECK(!screen::speak_lobby_of("0"));
    OA_CHECK(!screen::speak_ticker_of(""));
    OA_CHECK(!screen::speak_ticker_of("0"));
    OA_CHECK(screen::speak_ticker_of("1"));

    const uint64_t hold = screen::line_hold_ms();

    screen::LineQueue alone;
    alone.set_now(0);
    alone.push(line_of("Ada: one", true, "Main", "Ada"));
    OA_CHECK(alone.flush().empty());
    alone.set_now(hold);
    const std::vector<screen::Announcement> one = alone.flush();
    OA_CHECK(one.size() == 1);
    OA_CHECK(!one[0].burst);
    OA_CHECK(one[0].text == "Ada: one");

    screen::LineQueue burst;
    burst.set_now(0);
    burst.push(line_of("Ada: one", true, "Main", "Ada"));
    burst.push(line_of("Bea: two", true, "Main", "Bea"));
    OA_CHECK(burst.flush().empty());
    burst.set_now(hold);
    const std::vector<screen::Announcement> collapsed = burst.flush();
    OA_CHECK(collapsed.size() == 1);
    OA_CHECK(collapsed[0].burst);
    OA_CHECK(collapsed[0].count == 2);
    OA_CHECK(collapsed[0].room == "Main");
    OA_CHECK(collapsed[0].text == "2 new lines in Main");
    OA_CHECK(collapsed[0].politeness == screen::Politeness::polite);

    screen::LineQueue rooms;
    rooms.set_now(0);
    rooms.push(line_of("Ada: one", true, "Main", "Ada"));
    rooms.push(line_of("Bea: two", true, "Other", "Bea"));
    rooms.set_now(hold);
    const std::vector<screen::Announcement> apart = rooms.flush();
    OA_CHECK(apart.size() == 2);
    OA_CHECK(!apart[0].burst && !apart[1].burst);

    screen::LineQueue split;
    split.set_now(0);
    split.push(line_of("Ada: one", true, "Main", "Ada"));
    split.push(line_of("Page from Cy", false, "", "Cy"));
    split.push(line_of("Bea: two", true, "Main", "Bea"));
    const std::vector<screen::Announcement> page = split.flush();
    OA_CHECK(page.size() == 1);
    OA_CHECK(page[0].text == "Page from Cy");
    split.set_now(hold);
    const std::vector<screen::Announcement> kept = split.flush();
    OA_CHECK(kept.size() == 1);
    OA_CHECK(kept[0].burst);
    OA_CHECK(kept[0].count == 2);
    OA_CHECK(kept[0].text == "2 new lines in Main");

    screen::LineQueue muted;
    muted.set_now(0);
    muted.set_muted("Ada", true);
    muted.set_muted("", true);
    muted.push(line_of("Ada: one", true, "Main", "Ada"));
    muted.push(line_of("Bea: two", true, "Main", "Bea"));
    muted.push(line_of("Ada: three", true, "Main", "Ada"));
    muted.push(line_of("a notice", false, "", ""));
    const std::vector<screen::Announcement> heard = muted.flush();
    OA_CHECK(heard.size() == 1);
    OA_CHECK(heard[0].text == "a notice");
    muted.set_now(hold);
    const std::vector<screen::Announcement> later = muted.flush();
    OA_CHECK(later.size() == 1);
    OA_CHECK(!later[0].burst);
    OA_CHECK(later[0].text == "Bea: two");

    screen::LineQueue again;
    again.set_now(0);
    again.set_muted("Ada", true);
    again.set_muted("Ada", false);
    again.push(line_of("Ada: back", true, "Main", "Ada"));
    again.set_now(hold);
    const std::vector<screen::Announcement> returned = again.flush();
    OA_CHECK(returned.size() == 1);
    OA_CHECK(returned[0].text == "Ada: back");

    // Five lines 300 ms apart are one burst. A page in between is said then.
    screen::LineQueue five;
    const char* texts[] = {"one", "two", "three", "four", "five"};
    for (int step = 0; step < 5; ++step) {
        five.set_now(static_cast<uint64_t>(step) * 300);
        five.push(line_of(texts[step], true, "Main", "Ada"));
        if (step == 2)
            five.push(line_of("Page from Cy", false, "", "Cy"));
        const std::vector<screen::Announcement> flushed = five.flush();
        if (step == 2) {
            OA_CHECK(flushed.size() == 1);
            OA_CHECK(flushed[0].text == "Page from Cy");
        } else {
            OA_CHECK(flushed.empty());
        }
    }
    five.set_now(hold);
    const std::vector<screen::Announcement> gathered = five.flush();
    OA_CHECK(gathered.size() == 1);
    OA_CHECK(gathered[0].burst);
    OA_CHECK(gathered[0].count == 5);
    OA_CHECK(gathered[0].room == "Main");
    OA_CHECK(gathered[0].text == "5 new lines in Main");
}

/// A line is recorded only while a reader is running or the preference is on.
void check_gate() {
    screen::set_preference(screen::Preference::automatic);
    screen::testing_reset_announcements();
    screen::announce("quiet", screen::Politeness::polite);
    if (!screen::reader_running()) {
        OA_CHECK(screen::take_announcements().empty());
        OA_CHECK(!screen::live_region().shown);
    } else {
        (void)screen::take_announcements();
    }
    screen::set_preference(screen::Preference::on);
    screen::testing_reset_announcements();
    screen::announce("Heard", screen::Politeness::assertive);
    const std::vector<screen::Announcement> taken = screen::take_announcements();
    OA_CHECK(taken.size() == 1);
    OA_CHECK(taken[0].text == "Heard");
    OA_CHECK(taken[0].politeness == screen::Politeness::assertive);
    OA_CHECK(screen::live_region().shown);
    OA_CHECK(screen::live_region().text == "Heard");
    OA_CHECK(screen::live_region().politeness == screen::Politeness::assertive);
    screen::announce("", screen::Politeness::polite);
    OA_CHECK(screen::take_announcements().empty());
    screen::set_preference(screen::Preference::off);
    screen::testing_reset_announcements();
    screen::announce("silent", screen::Politeness::polite);
    // Off records nothing, whether or not a reader is running.
    OA_CHECK(!screen::announcements_open());
    OA_CHECK(screen::take_announcements().empty());
    OA_CHECK(!screen::live_region().shown);
    screen::set_preference(screen::Preference::automatic);
    screen::testing_reset_announcements();
}

} // namespace

int main() {
    check_queue();
    check_gate();
    OA_CHECK(screen::preference_of("") == screen::Preference::automatic);
    OA_CHECK(screen::preference_of("auto") == screen::Preference::automatic);
    OA_CHECK(screen::preference_of("on") == screen::Preference::on);
    OA_CHECK(screen::preference_of("off") == screen::Preference::off);
    OA_CHECK(screen::preference_of("yes") == screen::Preference::automatic);

    const screen::LoadResult result = screen::load();
    if (result == screen::LoadResult::compiled_out) {
        OA_CHECK(!screen::compiled_in());
        return oa::test::check_exit_status();
    }
#if defined(_WIN32) && !defined(OA_SCREEN_READER_COMPILED_OUT)
    // The Visual Studio lane has the header and no DLL. An older Windows
    // does not load the DLL; speech speaks there instead.
    if (result == screen::LoadResult::missing_library)
        return 77;
    if (result == screen::LoadResult::old_windows) {
        OA_CHECK(screen::compiled_in());
        OA_CHECK(GetModuleHandleW(L"accesskit.dll") == nullptr);
        return oa::test::check_exit_status();
    }
#endif
    OA_CHECK(result == screen::LoadResult::ready);
#if !defined(OA_SCREEN_READER_COMPILED_OUT)
    if (result == screen::LoadResult::ready) {
        role_and_label_round_trip();
        roles_actions_and_silence();
        unchanged_tree_skips_the_factory();
        two_lines_are_two_nodes();
    }
#endif
    return oa::test::check_exit_status();
}
