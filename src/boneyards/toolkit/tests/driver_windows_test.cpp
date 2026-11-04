// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The controls a program driving the game lists from a desktop: shown
// windows, front to back, under the window that holds them.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "oa/boneyards/toolkit.hpp"

using namespace oa::boneyards;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Seen {
    std::string window;
    std::string name;
    DriverControlKind kind;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    bool enabled = false;
    bool focused = false;
    bool checked = false;
    std::string text;
    bool masked = false;
    int32_t text_length = 0;
    std::vector<std::string> items;
};

void remember(void* context, const char* window, const DriverControlView& control) {
    auto* seen = static_cast<std::vector<Seen>*>(context);
    Seen item;
    item.window = window != nullptr ? window : "";
    item.name = control.name != nullptr ? control.name : "";
    item.kind = control.kind;
    item.x = control.x;
    item.y = control.y;
    item.width = control.width;
    item.height = control.height;
    item.enabled = control.enabled;
    item.focused = control.focused;
    item.checked = control.checked;
    item.text = control.text != nullptr ? control.text : "";
    item.masked = control.masked;
    item.text_length = control.text_length;
    for (int32_t row = 0; control.items != nullptr && row < control.item_count; ++row)
        item.items.emplace_back(control.items[row] != nullptr ? control.items[row] : "");
    seen->push_back(std::move(item));
}

const Seen* find(const std::vector<Seen>& seen, const char* window, const char* name) {
    for (const Seen& item : seen) {
        if (item.window == window && item.name == name)
            return &item;
    }
    return nullptr;
}

void add_top(Desktop* desktop, Window* window, const char* name, bool visible) {
    window_create(window, name, 0, 0, 100, 80, nullptr, visible);
    desktop_add(desktop, window);
}

void test_listed_controls() {
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);

    Window* back = gadget_create();
    add_top(&desktop, back, "Back", true);
    Window* back_button = button_create();
    window_create(back_button, "BACK", 4, 4, 30, 12, back, true);

    Window* login = gadget_create();
    add_top(&desktop, login, "Login", true);
    Window* login_button = button_create();
    window_create(login_button, "LOGIN", 8, 8, 40, 14, login, true);
    window_set_state(login_button, button_state::disabled);
    Window* hidden = button_create();
    window_create(hidden, "HIDDEN", 8, 28, 40, 14, login, false);
    Window* unnamed = button_create();
    window_create(unnamed, "", 8, 48, 20, 10, login, true);

    SingleEdit* name = single_edit_create();
    window_create(&name->base, "Username", 10, 20, 60, 12, login, true);
    name->base.focused = 1;
    single_edit_set_text(name, "oa_check");

    Window* guest = radio_button_create();
    window_create(guest, "Guest Login", 10, 36, 50, 12, login, true);
    window_set_state(guest, button_state::pressed);
    Window* member = radio_button_create();
    window_create(member, "Registered User", 10, 50, 50, 12, login, true);
    window_set_state(member, button_state::unpressed);

    CheckBox* box = check_box_create();
    window_create(&box->base, "Remember", 10, 64, 50, 12, login, true);
    window_set_state(&box->base, check_state::checked_up);

    std::vector<Seen> seen;
    desktop_visit_driver_controls(&desktop, &seen, remember);

    check(!seen.empty() && seen.front().window == "Login", "the front window is listed first");
    const Seen* pressed = find(seen, "Login", "LOGIN");
    check(
        pressed != nullptr && pressed->kind == DriverControlKind::button && !pressed->enabled &&
            pressed->x == 8 && pressed->y == 8 && pressed->width == 40 && pressed->height == 14,
        "a disabled button keeps its place"
    );
    check(find(seen, "Login", "HIDDEN") == nullptr, "a hidden control is left out");
    check(find(seen, "Login", "") == nullptr, "a control with no name is left out");
    const Seen* field = find(seen, "Login", "Username");
    check(
        field != nullptr && field->kind == DriverControlKind::text_field && field->focused &&
            field->text == "oa_check",
        "a field reports the text it holds and that it is focused"
    );
    const Seen* guest_seen = find(seen, "Login", "Guest Login");
    check(
        guest_seen != nullptr && guest_seen->kind == DriverControlKind::check_box &&
            guest_seen->checked,
        "a selected radio is a checked box"
    );
    const Seen* member_seen = find(seen, "Login", "Registered User");
    check(member_seen != nullptr && !member_seen->checked, "an unselected radio is not checked");
    const Seen* remember_seen = find(seen, "Login", "Remember");
    check(
        remember_seen != nullptr && remember_seen->kind == DriverControlKind::check_box &&
            remember_seen->checked,
        "a checked box reports that it is checked"
    );
    check(find(seen, "Back", "BACK") != nullptr, "a window behind is still listed");
    const auto login_at = std::find_if(seen.begin(), seen.end(), [](const Seen& item) {
        return item.window == "Login";
    });
    const auto back_at = std::find_if(seen.begin(), seen.end(), [](const Seen& item) {
        return item.window == "Back";
    });
    check(
        login_at != seen.end() && back_at != seen.end() && login_at < back_at,
        "a window in front is listed ahead of the one behind it"
    );

    desktop_visit_driver_controls(nullptr, &seen, remember);
    desktop_visit_driver_controls(&desktop, &seen, nullptr);
    check(seen.size() > 4, "a null desktop or visit lists nothing more");

    desktop_remove(&desktop, login);
    desktop_remove(&desktop, back);
    window_destroy(login);
    window_destroy(back);
    desktop_release(&desktop);
}

// A masked field reports how many characters it holds and not the characters.
void test_masked_fields() {
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Window* login = gadget_create();
    add_top(&desktop, login, "Login", true);

    SingleEdit* password = single_edit_create();
    window_create(&password->base, "Password", 10, 20, 60, 12, login, true);
    single_edit_set_text(password, "ridge");
    password->mask = '*';

    SingleEdit* empty = single_edit_create();
    window_create(&empty->base, "Join password", 10, 36, 60, 12, login, true);
    empty->mask = '*';

    EditBox* battle = edit_box_create();
    window_create(&battle->base, "Battle password", 10, 52, 60, 12, login, true);
    std::memcpy(battle->text, "blaze", 6);
    battle->length = 5;
    battle->mask = '*';

    SingleEdit* frequency = single_edit_create();
    window_create(&frequency->base, "Frequency password", 10, 68, 60, 12, login, true);
    single_edit_set_text(frequency, "pw1");
    frequency->mask = '*';

    SingleEdit* name = single_edit_create();
    window_create(&name->base, "Username", 10, 4, 60, 12, login, true);
    single_edit_set_text(name, "oa_check");

    std::vector<Seen> seen;
    desktop_visit_driver_controls(&desktop, &seen, remember);

    const Seen* hidden = find(seen, "Login", "Password");
    check(
        hidden != nullptr && hidden->kind == DriverControlKind::text_field && hidden->masked &&
            hidden->text_length == 5 && hidden->text.empty(),
        "a masked field reports its length and not its text"
    );
    const Seen* none = find(seen, "Login", "Join password");
    check(
        none != nullptr && none->masked && none->text_length == 0 && none->text.empty(),
        "an empty masked field reports a length of 0"
    );
    const Seen* box = find(seen, "Login", "Battle password");
    check(
        box != nullptr && box->masked && box->text_length == 5 && box->text.empty(),
        "a masked edit box reports its length and not its text"
    );
    const Seen* room = find(seen, "Login", "Frequency password");
    check(
        room != nullptr && room->masked && room->text_length == 3 && room->text.empty(),
        "a frequency password reports its length and not its text"
    );
    const Seen* open = find(seen, "Login", "Username");
    check(
        open != nullptr && !open->masked && open->text == "oa_check",
        "an unmasked field still reports its text"
    );

    desktop_remove(&desktop, login);
    window_destroy(login);
    desktop_release(&desktop);
}

const char* main_caption(Window* /*self*/, int32_t /*state*/) {
    return "Main";
}

const char* player_caption(Window* /*self*/, int32_t /*state*/) {
    return "Ghost_Commander";
}

/// A static that shows fixed words, as a lobby label shows its text.
///
/// @param text the words it shows
/// @return the window operations
WindowOps captioned(const char* (*text)(Window*, int32_t)) {
    WindowOps ops = k_static_ops;
    ops.state_text = text;
    return ops;
}

const WindowOps k_main_row_ops = captioned(main_caption);
const WindowOps k_player_label_ops = captioned(player_caption);

// A list row that draws its words with a label of its own (a player or a chat
// frequency) reads as that label's words; a row that shows none reads as "".
void test_row_words() {
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Window* chat = gadget_create();
    add_top(&desktop, chat, "ChatConsole", true);
    ListBox* list = list_box_create();
    window_create(&list->base, "Chat players", 0, 0, 100, 40, chat, true);
    // A lobby list's first child is its slider; the rows follow it.
    Slider* bar = slider_create();
    window_create(&bar->base, "slider", 90, 0, 10, 40, &list->base, true);

    Window* plain = static_create();
    plain->ops = &k_main_row_ops;
    window_create(plain, "", 0, 0, 80, 10, nullptr, true);
    list_box_add(list, plain);

    Window* player = static_create();
    window_create(player, "", 0, 0, 80, 10, nullptr, true);
    Window* icon = static_create();
    window_create(icon, "RankIcon", 2, 2, 8, 8, player, true);
    Window* name = static_create();
    name->ops = &k_player_label_ops;
    window_create(name, "", 12, 0, 60, 10, player, true);
    list_box_add(list, player);

    Window* blank = static_create();
    window_create(blank, "", 0, 0, 80, 10, nullptr, true);
    list_box_add(list, blank);

    std::vector<Seen> seen;
    desktop_visit_driver_controls(&desktop, &seen, remember);
    const Seen* players = find(seen, "ChatConsole", "Chat players");
    check(players != nullptr && players->kind == DriverControlKind::list, "the list is listed");
    check(players != nullptr && players->items.size() == 3, "the list reports its three rows");
    if (players != nullptr && players->items.size() == 3) {
        check(players->items[0] == "Main", "a row with a caption reads as its caption");
        check(players->items[1] == "Ghost_Commander", "a row reads as the label it draws");
        check(players->items[2].empty(), "a row that shows no words reads as empty");
    }

    desktop_remove(&desktop, chat);
    window_destroy(chat);
    desktop_release(&desktop);
}

// A label that holds radios (a radio group) lists them under the window
// that holds the group, so each can be read and pressed.
void test_radio_group() {
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Window* host = gadget_create();
    add_top(&desktop, host, "HOST BATTLE", true);
    Window* group = static_create();
    window_create(group, "RadioGroup", 10, 10, 120, 30, host, true);
    Window* title = static_create();
    title->ops = &k_main_row_ops;
    window_create(title, "", 10, 10, 60, 10, group, true);
    Window* novice = radio_button_create();
    window_create(novice, "Novice", 10, 25, 40, 10, group, true);
    window_set_state(novice, button_state::pressed);
    Window* expert = radio_button_create();
    window_create(expert, "Expert", 60, 25, 40, 10, group, true);
    window_set_state(expert, button_state::unpressed);
    Window* hidden = radio_button_create();
    window_create(hidden, "Hidden", 100, 25, 20, 10, group, false);

    std::vector<Seen> seen;
    desktop_visit_driver_controls(&desktop, &seen, remember);
    const Seen* chosen = find(seen, "HOST BATTLE", "Novice");
    check(
        chosen != nullptr && chosen->kind == DriverControlKind::check_box && chosen->checked &&
            chosen->x == 10 && chosen->y == 25,
        "a radio in a group is listed under the group's window, with its state"
    );
    const Seen* other = find(seen, "HOST BATTLE", "Expert");
    check(other != nullptr && !other->checked, "the group's other radio is listed unchecked");
    check(find(seen, "HOST BATTLE", "Main") != nullptr, "the group's title is listed");
    check(find(seen, "HOST BATTLE", "Hidden") == nullptr, "a hidden radio is left out");

    desktop_remove(&desktop, host);
    window_destroy(host);
    desktop_release(&desktop);
}

} // namespace

int main() {
    test_listed_controls();
    test_masked_fields();
    test_row_words();
    test_radio_group();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", g_failures);
        return 1;
    }
    std::printf("driver windows: the shown controls were listed\n");
    return 0;
}
