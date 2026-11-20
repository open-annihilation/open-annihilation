// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen-reader tree of one control listing. A fixture holds each role
// a reader is shown, and the nodes are read back through AccessKit's C API.

#include "oa/platform/screen_reader.hpp"
#include "oa/test/check.hpp"

#if !defined(OA_SCREEN_READER_COMPILED_OUT)

#include "oa/app/automation/tree.hpp"
#include "oa/app/screen_reader_tree.hpp"
#include "oa/platform/screen_reader/api.hpp"
#include "oa/platform/screen_reader/tree.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::automation {

/// Where the game's window shows the canvas. The same struct as input.hpp.
/// This test defines the canvas mapping so the game's input file stays out
/// of the link. Every control below is in window pixels, so it is not called.
struct WindowPlacement {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_WindowID window_id{};
    float density{1.0F};
};

bool canvas_to_window(
    const WindowPlacement& placement, float x, float y, float& window_x, float& window_y
) {
    (void)placement;
    (void)x;
    (void)y;
    (void)window_x;
    (void)window_y;
    return false;
}

} // namespace oa::app::automation

namespace {

namespace screen = oa::platform::screen_reader;
namespace automation = oa::app::automation;
using oa::app::AutomationControl;
using oa::app::AutomationControlRole;

/// One shown control, in the window's own pixels.
AutomationControl shown(
    const char* name,
    AutomationControlRole role,
    const char* label,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    AutomationControl control;
    control.name = name;
    control.role = role;
    control.role_set = true;
    control.label = label == nullptr ? "" : label;
    control.window_pixels = true;
    control.window_x = x;
    control.window_y = y;
    control.window_width = width;
    control.window_height = height;
    control.visible = true;
    control.enabled = true;
    return control;
}

/// The fixture: each role, a dialog, a list's items, tabs before their tab
/// list, a hidden control and a masked field.
std::vector<AutomationControl> fixture_controls() {
    std::vector<AutomationControl> controls;
    AutomationControl covered =
        shown("SINGLE", AutomationControlRole::button, "SINGLE", 8, 8, 40, 16);
    covered.window = "MainMenuOverlay";
    covered.hidden = true;
    covered.focused = true;
    controls.push_back(covered);
    AutomationControl single =
        shown("SingleBtn", AutomationControlRole::button, "SINGLE", 8, 8, 80, 18);
    single.window = "MainMenuOverlay";
    controls.push_back(single);
    AutomationControl intro =
        shown("IntroBtn", AutomationControlRole::button, "INTRO", 8, 32, 80, 18);
    intro.window = "MainMenuOverlay";
    controls.push_back(intro);

    AutomationControl lamp = shown("Lamp", AutomationControlRole::button, "Lamp", 4, 4, 40, 16);
    lamp.window = "console";
    controls.push_back(lamp);

    AutomationControl play = shown("Play", AutomationControlRole::button, "Play", 0, 0, 60, 16);
    play.shortcut = "S";
    controls.push_back(play);
    controls.push_back(shown("Ready", AutomationControlRole::check_box, "Ready", 0, 16, 80, 16));
    controls.push_back(
        shown("Chat", AutomationControlRole::switch_control, "Chat", 0, 40, 100, 24)
    );
    controls.push_back(shown("Off", AutomationControlRole::check_box, "Off", 4, 44, 40, 16));
    controls.push_back(shown("On", AutomationControlRole::check_box, "On", 52, 44, 40, 16));
    AutomationControl core = shown("Core", AutomationControlRole::radio, "Core", 60, 80, 36, 16);
    core.place = 1;
    core.set_size = 2;
    controls.push_back(core);
    AutomationControl arm = shown("Arm", AutomationControlRole::radio, "Arm", 10, 80, 36, 16);
    arm.place = 2;
    arm.set_size = 2;
    controls.push_back(arm);
    controls.push_back(shown("Side", AutomationControlRole::radio_group, "Side", 0, 72, 120, 28));
    AutomationControl maps = shown("Maps", AutomationControlRole::tab, "Maps", 8, 120, 40, 16);
    maps.place = 2;
    maps.set_size = 2;
    controls.push_back(maps);
    AutomationControl mods = shown("Mods", AutomationControlRole::tab, "Mods", 56, 120, 40, 16);
    mods.place = 1;
    mods.set_size = 2;
    controls.push_back(mods);
    controls.push_back(
        shown("Sections", AutomationControlRole::tab_list, "Sections", 0, 112, 140, 32)
    );
    AutomationControl games = shown("Games", AutomationControlRole::list, "Games", 0, 160, 100, 40);
    games.items = {"Alpha", "Beta"};
    games.rows = 2;
    games.first_visible = 0;
    games.selected = 0;
    controls.push_back(games);
    controls.push_back(shown("Bar", AutomationControlRole::slider, "", 110, 160, 12, 40));
    controls.push_back(shown("Folder", AutomationControlRole::tree_item, "Folder", 4, 214, 80, 16));
    controls.push_back(shown("Contents", AutomationControlRole::tree, "Contents", 0, 210, 100, 24));
    AutomationControl gamma =
        shown("Gamma", AutomationControlRole::slider, "Gamma", 0, 250, 100, 16);
    gamma.value_text = "Gamma 1.0";
    controls.push_back(gamma);
    AutomationControl name =
        shown("Name", AutomationControlRole::text_field, "Name", 0, 280, 100, 16);
    name.value_text = "Ada";
    controls.push_back(name);
    AutomationControl secret =
        shown("Password", AutomationControlRole::password, "Password", 0, 310, 100, 16);
    secret.masked = true;
    secret.text = "nope";
    secret.text_length = 4;
    controls.push_back(secret);
    AutomationControl search =
        shown("Search", AutomationControlRole::search, "Search", 0, 340, 100, 16);
    search.value_text = "tank";
    controls.push_back(search);
    controls.push_back(shown("ChatLog", AutomationControlRole::log, "Chat log", 0, 370, 100, 16));
    controls.push_back(
        shown("Latency", AutomationControlRole::image, "Latency good", 0, 400, 16, 16)
    );
    controls.push_back(shown("Manual", AutomationControlRole::link, "Manual", 0, 430, 60, 16));
    controls.push_back(
        shown("Commander", AutomationControlRole::label, "Commander", 0, 460, 80, 14)
    );
    AutomationControl quiet =
        shown("NoMod", AutomationControlRole::label, "No Mod", 0, 480, 60, 14);
    quiet.enabled = false;
    controls.push_back(quiet);
    controls.push_back(shown("blank", AutomationControlRole::label, "", 0, 500, 40, 14));
    AutomationControl gone =
        shown("Covered", AutomationControlRole::button, "Covered", 0, 520, 40, 16);
    gone.hidden = true;
    controls.push_back(gone);

    AutomationControl refresh =
        shown("Refresh", AutomationControlRole::button, "Refresh", 20, 80, 70, 18);
    refresh.dialog = "shelf";
    controls.push_back(refresh);
    AutomationControl mark = shown("Mark", AutomationControlRole::image, "Mark", 20, 40, 40, 16);
    mark.dialog = "shelf";
    controls.push_back(mark);
    AutomationControl library =
        shown("LibraryLabel", AutomationControlRole::label, "Library", 20, 16, 90, 14);
    library.dialog = "shelf";
    library.title = "Library";
    controls.push_back(library);
    AutomationControl missing = shown(
        "Missing", AutomationControlRole::alert_dialog, "The disc is missing", 20, 16, 120, 20
    );
    missing.dialog = "disc";
    controls.push_back(missing);
    return controls;
}

/// The nodes of one listing on the main menu.
std::vector<screen::Node> built_from(const std::vector<AutomationControl>& controls) {
    uint64_t focus = 0;
    const automation::ControlListing listing =
        automation::control_listing(controls, "main_menu", "");
    const automation::WindowPlacement placement{};
    return oa::app::nodes_of(listing, placement, focus);
}

/// The nodes of the fixture on one screen package.
std::vector<screen::Node> nodes_for(const char* package, uint64_t& focus) {
    const automation::ControlListing listing =
        automation::control_listing(fixture_controls(), package, "");
    const automation::WindowPlacement placement{};
    return oa::app::nodes_of(listing, placement, focus);
}

/// The first node of a role and a label.
const screen::Node*
labeled(const std::vector<screen::Node>& nodes, screen::Role role, std::string_view label) {
    for (const screen::Node& node : nodes)
        if (node.role == role && node.label == label)
            return &node;
    return nullptr;
}

/// The node of an id.
const screen::Node* node_of(const std::vector<screen::Node>& nodes, uint64_t id) {
    for (const screen::Node& node : nodes)
        if (node.id == id)
            return &node;
    return nullptr;
}

/// A group's child labels, in the group's order.
std::vector<std::string>
child_labels(const std::vector<screen::Node>& nodes, const screen::Node& parent) {
    std::vector<std::string> labels;
    for (const uint64_t child : parent.children) {
        const screen::Node* node = node_of(nodes, child);
        if (node != nullptr)
            labels.push_back(node->label);
    }
    return labels;
}

/// Reads a string AccessKit returned, and frees it.
std::string take_text(const screen::Calls& table, char* text) {
    if (text == nullptr)
        return {};
    const std::string copy(text);
    table.string_free(text);
    return copy;
}

/// The tree's shape, before AccessKit.
void check_shape() {
    uint64_t focus = 0;
    const std::vector<screen::Node> menu = nodes_for("main_menu", focus);
    const screen::Role roles[] = {
        screen::Role::button,      screen::Role::check_box, screen::Role::switch_control,
        screen::Role::radio_group, screen::Role::radio,     screen::Role::tab_list,
        screen::Role::tab,         screen::Role::list,      screen::Role::list_item,
        screen::Role::tree,        screen::Role::tree_item, screen::Role::text_field,
        screen::Role::password,    screen::Role::search,    screen::Role::slider,
        screen::Role::log,         screen::Role::dialog,    screen::Role::alert_dialog,
        screen::Role::group,       screen::Role::image,     screen::Role::link,
        screen::Role::label,
    };
    for (const screen::Role role : roles) {
        bool found = false;
        for (const screen::Node& node : menu)
            found = found || node.role == role;
        OA_CHECK(found);
    }

    const std::string bullets = std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
    const screen::Node* secret = labeled(menu, screen::Role::password, "Password");
    OA_CHECK(secret != nullptr && secret->value_text == bullets);
    OA_CHECK(secret != nullptr && secret->value_text.find("nope") == std::string::npos);

    OA_CHECK(labeled(menu, screen::Role::button, "Covered") == nullptr);
    OA_CHECK(labeled(menu, screen::Role::check_box, "Off") == nullptr);
    OA_CHECK(labeled(menu, screen::Role::check_box, "On") == nullptr);
    OA_CHECK(labeled(menu, screen::Role::check_box, "Ready") != nullptr);
    for (const screen::Node& node : menu)
        OA_CHECK(!(node.role == screen::Role::label && node.label.empty()));

    const screen::Node* dialog = labeled(menu, screen::Role::dialog, "Library");
    OA_CHECK(dialog != nullptr);
    OA_CHECK(labeled(menu, screen::Role::dialog, "Refresh") == nullptr);
    if (dialog != nullptr) {
        const std::vector<std::string> labels = child_labels(menu, *dialog);
        OA_CHECK(labels.size() == 3);
        if (labels.size() == 3) {
            OA_CHECK(labels[0] == "Library");
            OA_CHECK(labels[1] == "Mark");
            OA_CHECK(labels[2] == "Refresh");
        }
        OA_CHECK(dialog->x == 20.0 && dialog->y == 16.0);
        OA_CHECK(dialog->width == 90.0 && dialog->height == 82.0);
    }
    OA_CHECK(labeled(menu, screen::Role::alert_dialog, "The disc is missing") != nullptr);

    const screen::Node* tabs = labeled(menu, screen::Role::tab_list, "Sections");
    OA_CHECK(tabs != nullptr);
    if (tabs != nullptr) {
        const std::vector<std::string> labels = child_labels(menu, *tabs);
        OA_CHECK(labels.size() == 2 && labels[0] == "Mods" && labels[1] == "Maps");
    }
    const screen::Node* games = labeled(menu, screen::Role::list, "Games");
    OA_CHECK(games != nullptr);
    if (games != nullptr) {
        const std::vector<std::string> labels = child_labels(menu, *games);
        OA_CHECK(labels.size() == 2 && labels[0] == "Alpha" && labels[1] == "Beta");
    }
    const screen::Node* side = labeled(menu, screen::Role::radio_group, "Side");
    OA_CHECK(side != nullptr);
    if (side != nullptr) {
        const std::vector<std::string> labels = child_labels(menu, *side);
        OA_CHECK(labels.size() == 2 && labels[0] == "Core" && labels[1] == "Arm");
    }
    const screen::Node* folder = labeled(menu, screen::Role::tree_item, "Folder");
    const screen::Node* contents = labeled(menu, screen::Role::tree, "Contents");
    OA_CHECK(folder != nullptr && contents != nullptr);
    if (contents != nullptr) {
        const std::vector<std::string> labels = child_labels(menu, *contents);
        OA_CHECK(labels.size() == 1 && labels[0] == "Folder");
    }

    OA_CHECK(labeled(menu, screen::Role::slider, "Gamma") != nullptr);
    OA_CHECK(labeled(menu, screen::Role::slider, "Games") != nullptr);
    for (const screen::Node& node : menu)
        OA_CHECK(!(node.role == screen::Role::slider && node.label.empty()));

    bool lamp_group = false;
    for (const screen::Node& node : menu) {
        if (node.role != screen::Role::group)
            continue;
        const std::vector<std::string> labels = child_labels(menu, node);
        for (const std::string& label : labels) {
            if (label == "Lamp") {
                lamp_group = true;
                OA_CHECK(node.label.empty());
            }
        }
    }
    OA_CHECK(lamp_group);

    const screen::Node* single = labeled(menu, screen::Role::button, "SINGLE");
    const screen::Node* intro = labeled(menu, screen::Role::button, "INTRO");
    OA_CHECK(single != nullptr && single->focused);
    OA_CHECK(intro != nullptr && !intro->focused);
    OA_CHECK(single != nullptr && focus == single->id);

    uint64_t match_focus = 0;
    const std::vector<screen::Node> match = nodes_for("match", match_focus);
    OA_CHECK(labeled(match, screen::Role::button, "SINGLE") == nullptr);
    OA_CHECK(labeled(match, screen::Role::button, "INTRO") == nullptr);
    OA_CHECK(labeled(match, screen::Role::button, "Lamp") != nullptr);
    OA_CHECK(match_focus == screen::window_node);
}

/// The same nodes, read back through AccessKit.
void check_api() {
    const screen::Calls* table = screen::calls();
    OA_CHECK(table != nullptr);
    if (table == nullptr)
        return;
    uint64_t focus = 0;
    const std::vector<screen::Node> menu = nodes_for("main_menu", focus);
    const screen::Node* secret = labeled(menu, screen::Role::password, "Password");
    const screen::Node* dialog = labeled(menu, screen::Role::dialog, "Library");
    const screen::Node* tabs = labeled(menu, screen::Role::tab_list, "Sections");
    const screen::Node* play = labeled(menu, screen::Role::button, "Play");
    const screen::Node* words = labeled(menu, screen::Role::label, "Commander");
    const screen::Node* quiet = labeled(menu, screen::Role::label, "No Mod");
    OA_CHECK(secret && dialog && tabs && play && words && quiet);
    if (secret == nullptr || dialog == nullptr || tabs == nullptr || play == nullptr ||
        words == nullptr || quiet == nullptr)
        return;

    accesskit_node* made = screen::make_node(*secret);
    OA_CHECK(made != nullptr);
    if (made != nullptr) {
        OA_CHECK(table->node_role(made) == ACCESSKIT_ROLE_PASSWORD_INPUT);
        OA_CHECK(take_text(*table, table->node_label(made)) == "Password");
        const std::string value = take_text(*table, table->node_value(made));
        OA_CHECK(value == secret->value_text);
        OA_CHECK(value.find("nope") == std::string::npos);
        table->node_free(made);
    }

    made = screen::make_node(*dialog);
    OA_CHECK(made != nullptr);
    if (made != nullptr) {
        OA_CHECK(table->node_role(made) == ACCESSKIT_ROLE_DIALOG);
        OA_CHECK(take_text(*table, table->node_label(made)) == "Library");
        table->node_free(made);
    }

    made = screen::make_node(*tabs);
    OA_CHECK(made != nullptr);
    if (made != nullptr) {
        OA_CHECK(table->node_role(made) == ACCESSKIT_ROLE_TAB_LIST);
        // AccessKit reads a tab's set size from its tab list.
        const accesskit_opt_index size = table->node_size_of_set(made);
        OA_CHECK(size.has_value && size.value == 2);
        const accesskit_node_ids children = table->node_children(made);
        OA_CHECK(children.length == tabs->children.size());
        if (children.values != nullptr) {
            for (size_t index = 0; index < children.length && index < tabs->children.size();
                 ++index)
                OA_CHECK(children.values[index] == tabs->children[index]);
        }
        table->node_free(made);
    }

    const screen::Node* mods = labeled(menu, screen::Role::tab, "Mods");
    OA_CHECK(mods != nullptr);
    made = mods != nullptr ? screen::make_node(*mods) : nullptr;
    if (made != nullptr) {
        // The first of two tabs is place 0 to AccessKit, read as "1 of 2".
        const accesskit_opt_index place = table->node_position_in_set(made);
        OA_CHECK(place.has_value && place.value == 0);
        table->node_free(made);
    }
    const screen::Node* games = labeled(menu, screen::Role::list, "Games");
    OA_CHECK(games != nullptr);
    made = games != nullptr ? screen::make_node(*games) : nullptr;
    if (made != nullptr) {
        // Rows built from a list's items take their count from the list.
        const accesskit_opt_index size = table->node_size_of_set(made);
        OA_CHECK(size.has_value && size.value == 2);
        table->node_free(made);
    }

    made = screen::make_node(*play);
    OA_CHECK(made != nullptr);
    if (made != nullptr) {
        OA_CHECK(table->node_role(made) == ACCESSKIT_ROLE_BUTTON);
        OA_CHECK(take_text(*table, table->node_label(made)) == "Play");
        OA_CHECK(take_text(*table, table->node_keyboard_shortcut(made)) == "S");
        table->node_free(made);
    }

    made = screen::make_node(*words);
    OA_CHECK(made != nullptr);
    if (made != nullptr) {
        OA_CHECK(take_text(*table, table->node_value(made)) == "Commander");
        OA_CHECK(!table->node_is_disabled(made));
        table->node_free(made);
    }
    made = screen::make_node(*quiet);
    OA_CHECK(made != nullptr);
    if (made != nullptr) {
        OA_CHECK(!table->node_is_disabled(made));
        OA_CHECK(take_text(*table, table->node_value(made)) == "No Mod");
        table->node_free(made);
    }
}

/// A dialog is named from its title, an empty list adopts the buttons inside
/// it, a focused empty list stays, loose tabs get a tab list each, and Tab
/// order beats position.
void check_fixes() {
    {
        std::vector<AutomationControl> controls;
        AutomationControl caption =
            shown("GameType", AutomationControlRole::label, "Game type:", 20, 16, 80, 14);
        caption.dialog = "load";
        controls.push_back(caption);
        AutomationControl ok = shown("OK", AutomationControlRole::button, "OK", 20, 80, 40, 16);
        ok.dialog = "load";
        controls.push_back(ok);
        AutomationControl picture =
            shown("Portrait", AutomationControlRole::image, "", 4, 4, 16, 16);
        controls.push_back(picture);
        AutomationControl kept =
            shown("Latency", AutomationControlRole::image, "Latency good", 24, 4, 16, 16);
        controls.push_back(kept);
        AutomationControl placeholder =
            shown("Hint", AutomationControlRole::label, "text", 0, 40, 40, 14);
        controls.push_back(placeholder);
        AutomationControl field = shown("", AutomationControlRole::text_field, "", 0, 60, 80, 16);
        field.value_text = "Name Of Game to Load";
        controls.push_back(field);
        const std::vector<screen::Node> nodes = built_from(controls);
        const screen::Node* dialog = nullptr;
        for (const screen::Node& node : nodes) {
            if (node.role == screen::Role::dialog && node.label.empty())
                dialog = &node;
        }
        OA_CHECK(dialog != nullptr);
        OA_CHECK(labeled(nodes, screen::Role::dialog, "Game type:") == nullptr);
        if (dialog != nullptr) {
            const std::vector<std::string> labels = child_labels(nodes, *dialog);
            OA_CHECK(labels.size() == 2);
            if (labels.size() == 2) {
                OA_CHECK(labels[0] == "Game type:");
                OA_CHECK(labels[1] == "OK");
            }
        }
        OA_CHECK(labeled(nodes, screen::Role::image, "Latency good") != nullptr);
        OA_CHECK(labeled(nodes, screen::Role::image, "") == nullptr);
        OA_CHECK(labeled(nodes, screen::Role::label, "text") == nullptr);
        bool unnamed_field = false;
        for (const screen::Node& node : nodes) {
            if (node.value_text == "Name Of Game to Load")
                unnamed_field = true;
        }
        OA_CHECK(!unnamed_field);
    }
    {
        std::vector<AutomationControl> controls;
        AutomationControl list =
            shown("Expansions", AutomationControlRole::list, "Expansions", 0, 0, 200, 40);
        controls.push_back(list);
        controls.push_back(
            shown("Campaign", AutomationControlRole::button, "Campaign", 4, 4, 60, 16)
        );
        controls.push_back(
            shown("Core", AutomationControlRole::button, "Core Contingency", 70, 4, 80, 16)
        );
        controls.push_back(shown("Music", AutomationControlRole::button, "Music", 0, 80, 40, 16));
        controls.push_back(shown("Empty", AutomationControlRole::list, "Empty", 0, 200, 40, 20));
        const std::vector<screen::Node> nodes = built_from(controls);
        const screen::Node* expansions = labeled(nodes, screen::Role::list, "Expansions");
        OA_CHECK(expansions != nullptr);
        OA_CHECK(labeled(nodes, screen::Role::list, "Empty") == nullptr);
        if (expansions != nullptr) {
            const std::vector<std::string> labels = child_labels(nodes, *expansions);
            OA_CHECK(labels.size() == 2);
            if (labels.size() == 2) {
                OA_CHECK(labels[0] == "Campaign");
                OA_CHECK(labels[1] == "Core Contingency");
            }
        }
        OA_CHECK(labeled(nodes, screen::Role::button, "Music") != nullptr);
    }
    {
        // Load Game with no saved games focuses its empty list. The list
        // stays, and the reader is told it holds the focus.
        std::vector<AutomationControl> controls;
        AutomationControl games =
            shown("GAMES", AutomationControlRole::list, "Saved games", 0, 0, 200, 160);
        games.focused = true;
        controls.push_back(games);
        controls.push_back(shown("LOAD", AutomationControlRole::button, "OK", 220, 140, 60, 20));
        uint64_t focus = 0;
        const automation::ControlListing listing =
            automation::control_listing(controls, "load_game", "");
        const automation::WindowPlacement placement{};
        const std::vector<screen::Node> nodes = oa::app::nodes_of(listing, placement, focus);
        const screen::Node* games_list = labeled(nodes, screen::Role::list, "Saved games");
        OA_CHECK(games_list != nullptr);
        OA_CHECK(games_list != nullptr && games_list->id == focus);
    }
    {
        std::vector<AutomationControl> controls;
        AutomationControl commands =
            shown("Commands", AutomationControlRole::tab, "Commands", 0, 300, 40, 16);
        commands.place = 1;
        commands.set_size = 3;
        controls.push_back(commands);
        AutomationControl keyboard =
            shown("Keyboard", AutomationControlRole::tab, "Keyboard", 50, 300, 40, 16);
        keyboard.place = 2;
        keyboard.set_size = 3;
        controls.push_back(keyboard);
        AutomationControl mouse =
            shown("Mouse", AutomationControlRole::tab, "Mouse", 100, 300, 40, 16);
        mouse.place = 3;
        mouse.set_size = 3;
        controls.push_back(mouse);
        controls.push_back(shown("Build", AutomationControlRole::tab, "Build", 400, 400, 80, 20));
        controls.push_back(shown("Orders", AutomationControlRole::tab, "Orders", 400, 430, 80, 20));
        const std::vector<screen::Node> nodes = built_from(controls);
        const screen::Node* views = nullptr;
        const screen::Node* categories = nullptr;
        int lists = 0;
        for (const screen::Node& node : nodes) {
            if (node.role != screen::Role::tab_list)
                continue;
            ++lists;
            const std::vector<std::string> labels = child_labels(nodes, node);
            if (labels.size() == 3)
                views = &node;
            if (labels.size() == 2)
                categories = &node;
        }
        OA_CHECK(lists == 2);
        OA_CHECK(views != nullptr && categories != nullptr);
        if (views != nullptr) {
            const std::vector<std::string> labels = child_labels(nodes, *views);
            OA_CHECK(labels.size() == 3);
            if (labels.size() == 3) {
                OA_CHECK(labels[0] == "Commands");
                OA_CHECK(labels[1] == "Keyboard");
                OA_CHECK(labels[2] == "Mouse");
            }
        }
        if (categories != nullptr) {
            const std::vector<std::string> labels = child_labels(nodes, *categories);
            OA_CHECK(labels.size() == 2);
            if (labels.size() == 2) {
                OA_CHECK(labels[0] == "Build");
                OA_CHECK(labels[1] == "Orders");
            }
            // Tabs the screen gave no place take their place in the list.
            OA_CHECK(categories->children.size() == 2);
            if (categories->children.size() == 2) {
                const screen::Node* build = node_of(nodes, categories->children[0]);
                const screen::Node* orders = node_of(nodes, categories->children[1]);
                OA_CHECK(build != nullptr && build->place == 1 && build->set_size == 2);
                OA_CHECK(orders != nullptr && orders->place == 2 && orders->set_size == 2);
            }
        }
    }
    {
        std::vector<AutomationControl> controls;
        AutomationControl right =
            shown("Right", AutomationControlRole::button, "Right", 80, 0, 40, 16);
        right.tab_index = 2;
        controls.push_back(right);
        AutomationControl left =
            shown("Left", AutomationControlRole::button, "Left", 0, 40, 40, 16);
        left.tab_index = 1;
        controls.push_back(left);
        const std::vector<screen::Node> nodes = built_from(controls);
        const screen::Node* window = labeled(nodes, screen::Role::window, "Open Annihilation");
        OA_CHECK(window != nullptr);
        if (window != nullptr) {
            const std::vector<std::string> labels = child_labels(nodes, *window);
            OA_CHECK(labels.size() == 2);
            if (labels.size() == 2) {
                OA_CHECK(labels[0] == "Left");
                OA_CHECK(labels[1] == "Right");
            }
        }
    }
    {
        std::vector<AutomationControl> controls;
        controls.push_back(shown("A", AutomationControlRole::button, "A", 0, 0, 40, 30));
        controls.push_back(shown("C", AutomationControlRole::button, "C", 0, 40, 40, 30));
        controls.push_back(shown("B", AutomationControlRole::button, "B", 80, 0, 40, 80));
        const std::vector<screen::Node> nodes = built_from(controls);
        const screen::Node* window = labeled(nodes, screen::Role::window, "Open Annihilation");
        OA_CHECK(window != nullptr);
        if (window != nullptr) {
            const std::vector<std::string> labels = child_labels(nodes, *window);
            OA_CHECK(labels.size() == 3);
            if (labels.size() == 3) {
                OA_CHECK(labels[0] == "A");
                OA_CHECK(labels[1] == "C");
                OA_CHECK(labels[2] == "B");
            }
        }
    }
    {
        // A tall control at the left, as the Library's sections are, beside
        // rows of labels and buttons. The left column is read first, then
        // the right side row by row: each label with the button on its row,
        // not every label that shares a left edge first.
        std::vector<AutomationControl> controls;
        controls.push_back(shown("Nav", AutomationControlRole::button, "Nav", 0, 0, 100, 300));
        controls.push_back(shown("Keys", AutomationControlRole::label, "Keys", 110, 100, 300, 30));
        controls.push_back(
            shown("ChangeKeys", AutomationControlRole::button, "Change keys", 420, 105, 60, 20)
        );
        controls.push_back(shown("Game", AutomationControlRole::label, "Game", 110, 10, 150, 30));
        controls.push_back(
            shown("ChangeGame", AutomationControlRole::button, "Change game", 270, 15, 60, 20)
        );
        controls.push_back(
            shown("Language", AutomationControlRole::label, "Language", 340, 10, 150, 30)
        );
        const std::vector<screen::Node> nodes = built_from(controls);
        const screen::Node* window = labeled(nodes, screen::Role::window, "Open Annihilation");
        OA_CHECK(window != nullptr);
        if (window != nullptr) {
            const std::vector<std::string> labels = child_labels(nodes, *window);
            const std::vector<std::string> expected{
                "Nav", "Game", "Change game", "Language", "Keys", "Change keys"
            };
            OA_CHECK(labels == expected);
        }
    }
}

} // namespace

#endif

int main() {
#if defined(OA_SCREEN_READER_COMPILED_OUT)
    return 0;
#else
    const oa::platform::screen_reader::LoadResult result = oa::platform::screen_reader::load();
#if defined(_WIN32)
    if (result == oa::platform::screen_reader::LoadResult::missing_library)
        return 77;
#endif
    check_shape();
    check_fixes();
    if (result == oa::platform::screen_reader::LoadResult::ready)
        check_api();
    else if (result != oa::platform::screen_reader::LoadResult::old_windows)
        OA_CHECK(result == oa::platform::screen_reader::LoadResult::ready);
    return oa::test::check_exit_status();
#endif
}
