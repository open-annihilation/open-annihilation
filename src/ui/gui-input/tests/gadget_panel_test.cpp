// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/base/game_math.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <cstdint>

namespace gui = oa::ui::gui_layout;
namespace input = oa::ui::gui_input;
namespace field = gui::field;
namespace key = input::key_code;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

// Deterministic host: 8-pixel glyphs, 10-pixel lines, a key queue and a log
// of redraw requests.
struct FakeHost {
    uint32_t tick = 100;
    std::deque<int32_t> keys;
    std::set<int32_t> held;
    std::string clipboard;
    std::vector<std::pair<input::GadgetDraw, int32_t>> draws;
    int32_t panel_draws = 0;
    int32_t sprite_frames = 3;
};

input::GadgetHost make_host(FakeHost& fake) {
    input::GadgetHost host;
    host.context = &fake;
    host.current_tick = [](void* context) { return static_cast<FakeHost*>(context)->tick; };
    host.pop_key = [](void* context) {
        auto& keys = static_cast<FakeHost*>(context)->keys;
        if (keys.empty())
            return 0;
        const auto value = keys.front();
        keys.pop_front();
        return value;
    };
    host.peek_key = [](void* context) {
        auto& keys = static_cast<FakeHost*>(context)->keys;
        return keys.empty() ? 0 : keys.front();
    };
    host.is_key_down = [](void* context, int32_t code) {
        return static_cast<FakeHost*>(context)->held.count(code) != 0;
    };
    host.read_clipboard = [](void* context, char* output, std::size_t capacity) {
        const auto& text = static_cast<FakeHost*>(context)->clipboard;
        const auto count = std::min(capacity, text.size() + 1);
        std::copy_n(text.c_str(), count, output);
        return text.size() + 1;
    };
    host.text_width = [](void*, const void*, const char* text) {
        return static_cast<int32_t>(std::strlen(text) * 8);
    };
    host.line_height = [](void*, const void*) { return 10; };
    host.sprite_frames = [](void* context, const void*) {
        return static_cast<FakeHost*>(context)->sprite_frames;
    };
    host.draw = [](void* context, input::GadgetPanel&, input::GadgetDraw what, int32_t index) {
        static_cast<FakeHost*>(context)->draws.emplace_back(what, index);
    };
    host.draw_panel = [](void* context, input::GadgetPanel&, uint32_t) {
        ++static_cast<FakeHost*>(context)->panel_draws;
        return 1;
    };
    return host;
}

gui::Layout parse_text(std::string_view text) {
    auto result = gui::parse(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
    require(result.ok(), "fixture parses");
    return std::move(*result.layout);
}

constexpr std::string_view screen = R"GUI(
[GADGET0] { [COMMON] { id=0; name=Screen; xpos=100; ypos=100; width=400; height=300; active=1; } totalgadgets=12; }
[GADGET1] { [COMMON] { id=1; name=OK; xpos=10; ypos=10; width=60; height=20; active=1; } text=Okay; quickkey=79; }
[GADGET2] { [COMMON] { id=1; name=Cancel; xpos=80; ypos=10; width=60; height=20; active=1; } text=Cancel; quickkey=67; }
[GADGET3] { [COMMON] { id=1; assoc=3; name=EASY; xpos=10; ypos=40; width=60; height=20; attribs=8; active=1; } text=Easy; }
[GADGET4] { [COMMON] { id=1; assoc=3; name=HARD; xpos=80; ypos=40; width=60; height=20; attribs=8; active=1; } text=Hard; }
[GADGET5] { [COMMON] { id=1; name=MUTE; xpos=150; ypos=40; width=60; height=20; attribs=64; active=1; } text=Mute; }
[GADGET6] { [COMMON] { id=2; assoc=9; name=MAPS; xpos=10; ypos=70; width=100; height=52; active=1; } itemheight=0; }
[GADGET7] { [COMMON] { id=4; assoc=9; name=MAPSBAR; xpos=112; ypos=70; width=12; height=52; active=1; }
  range=0; thick=0; knobpos=0; knobsize=10; }
[GADGET8] { [COMMON] { id=1; assoc=9; name=MAPSUP; xpos=126; ypos=70; width=12; height=12; attribs=4096; active=1; } text=; }
[GADGET9] { [COMMON] { id=3; name=NAME; xpos=10; ypos=140; width=100; height=14; active=1; } maxchars=8; text=; }
[GADGET10] { [COMMON] { id=5; name=HINT; xpos=10; ypos=170; width=100; height=10; active=1; } text=Okay hint; link=OK; }
[GADGET11] { [COMMON] { id=1; name=CYCLE; xpos=150; ypos=10; width=60; height=20; attribs=256; active=1; } text=Cycle; }
[GADGET12] { [COMMON] { id=1; name=HOLD; xpos=220; ypos=10; width=60; height=20; attribs=16; active=1; } text=Hold; }
)GUI";

const char kMapLines[] = "Alpha\0&GHeader\0Bravo\0Charlie\0Delta\0Echo\0Foxtrot\0";

struct Fixture {
    FakeHost fake;
    input::GadgetPanel panel;
    int32_t commands = 0;

    explicit Fixture(bool release_initial_focus = true) {
        panel.host = make_host(fake);
        input::init_gadget_panel(panel);
        input::load_panel(panel, parse_text(screen), "SCREEN", 0);
        require(panel.owner != nullptr, "panel loaded");
        if (release_initial_focus)
            panel.captured = -1;
    }

    gui::GadgetRecord& record(std::string_view name) {
        auto* found = gui::find_gadget_record(panel.owner->records(), name);
        require(found != nullptr, "record exists");
        return *found;
    }

    int32_t index(std::string_view name) { return gui::find_gadget(panel.owner->records(), name); }

    // Places the pointer at panel-relative (x, y) with a message and held buttons.
    void pointer(int32_t x, int32_t y, int32_t message, int32_t buttons) {
        panel.pointer.x = x + 100;
        panel.pointer.y = y + 100;
        panel.pointer.message = message;
        panel.buttons = buttons;
    }
};

int16_t status(Fixture& fixture, std::string_view name) {
    return gui::record_i16(fixture.record(name), field::button_status);
}

void panel_load_binds_defaults() {
    Fixture f(false);
    const auto& root = f.panel.owner->table.records[0];
    require(gui::record_string(root, field::name) == "SCREEN", "root renamed to the basename");
    require(gui::record_string(root, field::enter_default) == "OK", "OK bound to Enter");
    require(gui::record_string(root, field::escape_default) == "Cancel", "Cancel bound to Escape");
    // The search origin is the root's stored screen position (100,100), so the
    // first record below that row in reading order wins: the text box.
    require(
        f.panel.owner->focus == f.index("NAME"), "initial focus follows the root-origin search"
    );
    require(f.panel.captured == f.index("NAME"), "a focused text box captures input");
    require(f.fake.panel_draws == 1, "panel drawn once");
    require(input::top_panel_named(f.panel, "screen"), "case-insensitive panel name");
}

void push_button_states() {
    Fixture f;
    const auto ok = f.index("OK");
    f.pointer(20, 15, input::pointer_message::left_down, 1);
    require(!input::button_pointer(f.panel, ok, 0), "press does not activate");
    require(
        f.panel.captured == ok && status(f, "OK") == 1, "pressed button captured and shown down"
    );
    f.pointer(200, 200, 0, 1);
    require(!input::button_pointer(f.panel, ok, 0), "dragging off does not activate");
    require(status(f, "OK") == 0, "dragged-off button pops up");
    f.pointer(20, 15, 0, 1);
    input::button_pointer(f.panel, ok, 0);
    require(status(f, "OK") == 1, "dragging back presses again");
    f.pointer(20, 15, 0, 0);
    require(input::button_pointer(f.panel, ok, 0), "release inside activates");
    require(f.panel.captured == -1 && status(f, "OK") == 0, "released button idle");

    auto& ok_record = f.record("OK");
    ok_record.bytes[field::button_flags] |= 1;
    f.pointer(20, 15, input::pointer_message::left_down, 1);
    require(
        !input::button_pointer(f.panel, ok, 0) && f.panel.captured == -1,
        "grayed button ignores the pointer"
    );
    ok_record.bytes[field::button_flags] &= 0xFE;

    f.pointer(300, 300, 0, 0);
    require(input::button_pointer(f.panel, ok, 'o'), "quick key activates (case-insensitive)");
}

void toggle_checkbox_and_group() {
    Fixture f;
    const auto mute = f.index("MUTE");
    f.pointer(160, 45, input::pointer_message::left_down, 1);
    input::button_pointer(f.panel, mute, 0);
    require(status(f, "MUTE") == 1, "toggle shows pressed");
    f.pointer(160, 45, 0, 0);
    require(input::button_pointer(f.panel, mute, 0) && status(f, "MUTE") == 1, "toggle latches on");
    f.pointer(160, 45, input::pointer_message::left_down, 1);
    input::button_pointer(f.panel, mute, 0);
    f.pointer(160, 45, 0, 0);
    require(
        input::button_pointer(f.panel, mute, 0) && status(f, "MUTE") == 0, "toggle latches off"
    );

    const auto easy = f.index("EASY");
    const auto hard = f.index("HARD");
    f.pointer(20, 45, input::pointer_message::left_down, 1);
    input::button_pointer(f.panel, easy, 0);
    f.pointer(20, 45, 0, 0);
    require(input::button_pointer(f.panel, easy, 0) && status(f, "EASY") == 1, "checkbox set");
    f.pointer(90, 45, input::pointer_message::left_down, 1);
    input::button_pointer(f.panel, hard, 0);
    f.pointer(90, 45, 0, 0);
    require(input::button_pointer(f.panel, hard, 0), "second checkbox activates");
    require(
        status(f, "HARD") == 1 && status(f, "EASY") == 0, "radio group clears the other member"
    );

    input::set_status(f.panel, easy, 1);
    require(status(f, "EASY") == 1 && status(f, "HARD") == 0, "set_status clears the group");
    require(
        input::set_status_by_name(f.panel, "HARD", 1) && status(f, "EASY") == 0, "by-name status"
    );
}

void hold_and_cycle_buttons() {
    Fixture f;
    const auto hold = f.index("HOLD");
    f.pointer(230, 15, input::pointer_message::left_down, 1);
    require(input::button_pointer(f.panel, hold, 0), "hold button activates while held");
    require(status(f, "HOLD") == 1 && f.panel.captured == -1, "hold state");

    const auto cycle = f.index("CYCLE");
    auto& record = f.record("CYCLE");
    record.refs.sprite = &record;
    for (int expected : {1, 2, 0, 1}) {
        f.pointer(160, 15, input::pointer_message::left_down, 1);
        require(input::button_pointer(f.panel, cycle, 0), "cycle click activates");
        require(status(f, "CYCLE") == expected, "cycle wraps at the frame count");
    }
}

void list_selection_and_scrolling() {
    Fixture f;
    const auto maps = f.index("MAPS");
    const auto bar = f.index("MAPSBAR");
    input::init_text_list(f.panel, "MAPS", kMapLines, sizeof(kMapLines), 7, nullptr);
    auto& list = f.record("MAPS");
    require(gui::gadget_attributes(list) & gui::attribute::text_list, "text list flag");
    require(gui::record_i16(list, field::list_item_height) == 11, "row height from the font");
    require(gui::record_i16(list, field::list_last_top) == 3, "last scroll origin (4 rows of 7)");
    require(f.record("MAPSBAR").bytes[field::active] == 1, "overflow activates the scroll bar");
    require(
        gui::record_i16(f.record("MAPSBAR"), field::scroll_knob_size) == 28,
        "knob size trunc(4/7*49)"
    );
    require(
        gui::record_i16(f.record("MAPSBAR"), field::scroll_range) == 52 - 28 - 3, "knob travel"
    );

    // Click the third visible row (y = 72 + 2*11 + 1).
    f.pointer(20, 70 + 2 + 23, input::pointer_message::left_down, 1);
    input::list_pointer(f.panel, maps);
    require(gui::record_i16(list, field::list_selected) == 2, "clicked row selected");
    require(f.panel.owner->focus == maps, "clicked list focused");

    // The header row is refused when attributes skip headers.
    gui::set_record_u32(
        list, field::attributes, gui::gadget_attributes(list) | gui::attribute::skip_headers
    );
    f.pointer(20, 70 + 2 + 12, input::pointer_message::left_down, 1);
    input::list_pointer(f.panel, maps);
    require(gui::record_i16(list, field::list_selected) == 2, "header row refused");

    // Keyboard navigation through the dispatcher.
    f.panel.owner->focus = maps;
    input::dispatch_key(f.panel, key::down);
    require(gui::record_i16(list, field::list_selected) == 3, "down arrow");
    input::dispatch_key(f.panel, key::up);
    input::dispatch_key(f.panel, key::up);
    require(gui::record_i16(list, field::list_selected) == 2, "up arrow stops above the header");

    // Held below the list: auto-scroll one row per two ticks.
    f.panel.captured = maps;
    f.fake.tick = 1000;
    f.pointer(20, 200, 0, 1);
    input::list_pointer(f.panel, maps);
    require(gui::record_i16(list, field::list_top) == 1, "auto-scroll advanced the origin");
    require(
        gui::record_i16(f.record("MAPSBAR"), field::scroll_knob) == 7, "scroll bar follows (1*21/3)"
    );
    input::list_pointer(f.panel, maps);
    require(gui::record_i16(list, field::list_top) == 1, "repeat waits for the tick deadline");

    input::set_list_selection(f.panel, "MAPS", 6);
    require(gui::record_i16(list, field::list_top) == 3, "selection scrolls into view, clamped");
    require(
        gui::record_i16(f.record("MAPSBAR"), field::scroll_knob) == 21, "knob moved proportionally"
    );
    static_cast<void>(bar);
}

void scroll_bar_track_and_step() {
    Fixture f;
    const auto bar = f.index("MAPSBAR");
    input::init_text_list(f.panel, "MAPS", kMapLines, sizeof(kMapLines), 7, nullptr);
    auto& record = f.record("MAPSBAR");
    // Click below the knob: one step forward per event while held.
    f.pointer(116, 110, input::pointer_message::left_down, 1);
    input::scroll_bar_pointer(f.panel, bar);
    require(f.panel.captured == bar && f.panel.dragging == 0, "track click captured");
    f.pointer(116, 110, 0, 1);
    input::scroll_bar_pointer(f.panel, bar);
    require(gui::record_i16(record, field::scroll_knob) == 1, "track click steps the knob");
    require(gui::record_i16(f.record("MAPS"), field::list_top) == 0, "list follows trunc(3*1/20)");

    // Held and released below the knob: each event steps once more.
    input::scroll_bar_pointer(f.panel, bar);
    f.pointer(116, 110, 0, 0);
    input::scroll_bar_pointer(f.panel, bar);
    require(
        gui::record_i16(record, field::scroll_knob) == 3 && f.panel.captured == -1,
        "release steps and frees"
    );
    const auto knob_y = 70 + 2 + gui::record_i16(record, field::scroll_knob);
    f.pointer(116, knob_y + 1, input::pointer_message::left_down, 1);
    input::scroll_bar_pointer(f.panel, bar);
    require(f.panel.dragging == 1, "knob grab starts a drag");
    f.pointer(116, knob_y + 1 + 100, 0, 1);
    input::scroll_bar_pointer(f.panel, bar);
    require(gui::record_i16(record, field::scroll_knob) == 20, "drag clamps to range - 1");
    require(gui::record_i16(f.record("MAPS"), field::list_top) == 3, "list at the end");

    // Linked step button (attributes 0x1000 steps back) through the button handler.
    const auto up = f.index("MAPSUP");
    f.pointer(130, 75, input::pointer_message::left_down, 1);
    input::button_pointer(f.panel, up, 0);
    require(gui::record_i16(record, field::scroll_knob) == 19, "step button moved the knob back");

    input::step_scroll_forward(f.panel, bar);
    input::step_scroll_forward(f.panel, bar);
    require(gui::record_i16(record, field::scroll_knob) == 20, "keyboard step clamps");
    input::set_gadget_disabled(f.panel, bar, 1);
    require(
        (f.record("MAPSUP").bytes[field::button_flags] & 1) == 1,
        "locking the bar grays its step button"
    );
}

void text_box_editing() {
    Fixture f;
    const auto name = f.index("NAME");
    auto& box = f.record("NAME");
    f.pointer(20, 145, input::pointer_message::left_down, 1);
    f.panel.owner->keys_from_queue = 1;
    f.fake.keys = {'a', 'b', 'c'};
    input::text_box_pointer(f.panel, name, 'x');
    require(f.panel.captured == name, "click focuses the text box");
    require(gui::record_string(box, field::text) == "xabc", "typed characters inserted");
    require(f.panel.caret == 4, "caret after typing");
    input::edit_keystroke(f.panel, name, key::home);
    require(f.panel.caret == 0, "home moves the caret");
    input::edit_keystroke(f.panel, name, key::end);
    input::edit_keystroke(f.panel, name, key::left);
    input::edit_keystroke(f.panel, name, key::backspace);
    require(
        gui::record_string(box, field::text) == "xac" && f.panel.caret == 2,
        "backspace before the caret"
    );
    input::edit_keystroke(f.panel, name, key::delete_forward);
    require(gui::record_string(box, field::text) == "xa", "delete at the caret");
    for (const char c : std::string("123456789"))
        input::edit_keystroke(f.panel, name, c);
    require(gui::record_string(box, field::text) == "xa123456", "maxchars caps the length");
    input::edit_keystroke(f.panel, name, 0x7F);
    require(gui::record_string(box, field::text).size() == 8, "non-printable ignored");

    f.fake.clipboard = "pasted text that is long";
    input::edit_keystroke(f.panel, name, key::paste);
    require(gui::record_string(box, field::text) == "pasted ", "paste limited to maxchars - 1");

    f.pointer(300, 300, 0, 0);
    require(
        input::text_box_pointer(f.panel, name, key::enter) && f.panel.captured == -1,
        "enter ends editing"
    );
    input::focus_text_box(f.panel, name);
    require(input::text_box_pointer(f.panel, name, key::escape), "escape ends editing");
    require(gui::record_string(box, field::text).empty(), "escape clears the text");
}

void keyboard_focus_and_defaults() {
    Fixture f;
    f.panel.owner->focus = f.index("OK");
    input::focus_nearest(f.panel, input::FocusDirection::next);
    require(f.panel.owner->focus == f.index("Cancel"), "tab order follows y*5000+x");
    input::focus_nearest(f.panel, input::FocusDirection::previous);
    require(f.panel.owner->focus == f.index("OK"), "previous focus");
    f.panel.owner->focus = f.index("HOLD");
    input::focus_nearest(f.panel, input::FocusDirection::next);
    require(f.panel.owner->focus == f.index("EASY"), "next wraps to the following row");
    f.panel.owner->focus = f.index("OK");
    input::focus_nearest(f.panel, input::FocusDirection::down);
    require(f.panel.owner->focus == f.index("EASY"), "down keeps the column");

    require(input::dispatch_key(f.panel, key::enter) == 0, "enter consumed");
    require(f.panel.activated == f.index("OK"), "enter activates the default button");
    input::clear_activation(f.panel);
    require(input::dispatch_key(f.panel, key::escape) == 0, "escape consumed");
    require(input::activated_gadget_is(f.panel, "Cancel"), "escape activates Cancel");
    require(input::dispatch_key(f.panel, 'q') == 'q', "unhandled key returned");
}

void keyboard_tab_and_slider() {
    Fixture columns;
    columns.panel.owner->focus = columns.index("MAPS");
    require(input::dispatch_key(columns.panel, key::tab) == 0, "tab consumed");
    require(columns.panel.owner->focus == columns.index("NAME"), "tab from MAPS lands on NAME");
    Fixture spatial;
    spatial.panel.owner->focus = spatial.index("MAPS");
    input::focus_nearest(spatial.panel, input::FocusDirection::next);
    require(
        spatial.panel.owner->focus == spatial.index("MAPSUP"),
        "an arrow from MAPS still lands on MAPSUP"
    );

    Fixture bar;
    auto& slider = bar.record("MAPSBAR");
    gui::set_record_i16(slider, field::width, 80);
    gui::set_record_i16(slider, field::height, 12);
    gui::set_record_i16(slider, field::scroll_range, 21);
    gui::set_record_i16(slider, field::scroll_knob, 10);
    bar.panel.owner->focus = bar.index("MAPSBAR");
    require(input::dispatch_key(bar.panel, key::right) == 0, "right on a slider is consumed");
    require(gui::record_i16(slider, field::scroll_knob) == 11, "right steps the knob one position");
    require(input::dispatch_key(bar.panel, key::left) == 0, "left on a slider is consumed");
    require(gui::record_i16(slider, field::scroll_knob) == 10, "left steps the knob back");
    require(
        input::dispatch_key(bar.panel, key::page_down) == 0, "page down on a slider is consumed"
    );
    require(
        gui::record_i16(slider, field::scroll_knob) == 12, "page down steps a tenth of the travel"
    );
    require(input::dispatch_key(bar.panel, key::home) == 0, "home on a slider is consumed");
    require(gui::record_i16(slider, field::scroll_knob) == 0, "home puts the knob at the start");
    require(input::dispatch_key(bar.panel, key::end) == 0, "end on a slider is consumed");
    require(
        gui::record_i16(slider, field::scroll_knob) == 20, "end puts the knob at the last position"
    );

    bar.panel.owner->focus = bar.index("NAME");
    require(
        input::dispatch_key(bar.panel, key::home) == key::home,
        "home in a text box is left for the caret"
    );
}

void update_loop_activation() {
    Fixture f;
    static int32_t commands = 0;
    commands = 0;
    f.panel.owner->on_command = [](input::GadgetPanel& panel) {
        ++commands;
        if (input::activated_gadget_is(panel, "OK"))
            input::clear_activation(panel);
    };
    f.pointer(20, 15, input::pointer_message::left_down, 1);
    input::update_panel(f.panel);
    require(f.panel.hovered == f.index("OK"), "hover tracked");
    f.pointer(20, 15, 0, 0);
    input::update_panel(f.panel);
    require(commands == 1 && f.panel.owner != nullptr, "command callback kept the panel open");
    require(f.panel.owner->focus == f.index("OK"), "activation focuses the record");

    // Label linked to OK activates it too.
    f.pointer(20, 175, input::pointer_message::left_down, 1);
    input::update_panel(f.panel);
    f.pointer(20, 175, 0, 0);
    input::update_panel(f.panel);
    require(commands == 2, "label link activates its target");

    f.pointer(90, 15, input::pointer_message::left_down, 1);
    input::update_panel(f.panel);
    f.pointer(90, 15, 0, 0);
    input::update_panel(f.panel);
    require(commands == 4 && f.panel.owner == nullptr, "other activations close the panel");
}

void state_setters() {
    Fixture f;
    input::set_gadget_text_by_name(f.panel, "OK", "Accept", 0);
    require(gui::record_string(f.record("OK"), field::text) == "Accept", "caption replaced");
    require(
        f.record("OK").bytes[field::button_quick_key] == 'A',
        "quick key reassigned to a free letter"
    );
    input::set_gadget_text_by_name(f.panel, "Cancel", "Abort", 0);
    require(f.record("Cancel").bytes[field::button_quick_key] == 'b', "first unused letter");
    const int8_t taken[] = {'Y', 0, 'n'};
    require(input::free_quick_key("Yes", taken) == 'e', "a taken key is passed over");
    require(input::free_quick_key(" No", taken) == 'o', "keys compare without regard to case");
    require(input::free_quick_key(" y N", taken) == '\0', "no key when every letter is taken");
    // Hangul and Chinese characters take three bytes each, which no one key
    // types: they are passed over whole, never cut to their first byte.
    const std::string_view korean_ok = "\xED\x99\x95\xEC\x9D\xB8"; // 확인
    require(input::free_quick_key(korean_ok, {}) == '\0', "a Korean caption takes no key");
    require(
        input::free_quick_key("\xE7\xA1\xAE\xE5\xAE\x9A", {}) == '\0',
        "a Chinese caption takes no key"
    );
    require(
        input::free_quick_key("\xED\x99\x95\xEC\x9D\xB8 OK", taken) == 'O',
        "the first one-byte letter after them is the key"
    );
    // A byte of the game's one-byte code page is a letter of its own.
    const std::string code_page_caption = std::string(1, '\xDC') + "bersicht";
    require(input::free_quick_key(code_page_caption, {}) == '\xDC', "a code-page letter is a key");
    auto& ok_record = f.record("OK");
    input::set_gadget_text_by_name(f.panel, "OK", korean_ok, 0);
    require(
        ok_record.bytes[field::button_quick_key] == 0,
        "a button captioned in Korean holds no key, not the caption's first byte"
    );
    input::set_gadget_text_by_name(f.panel, "OK", "Accept", 0);
    using input::CaptionQuickKey;
    require(
        input::caption_quick_key(0, 0, "Yes") == CaptionQuickKey::assign,
        "a plain button takes a key from its caption"
    );
    require(
        input::caption_quick_key(0, 0, "") == CaptionQuickKey::keep,
        "an empty caption keeps the key"
    );
    require(
        input::caption_quick_key(gui::attribute::no_quick_key, 2, "Yes") == CaptionQuickKey::keep,
        "no_quick_key keeps the key"
    );
    require(
        input::caption_quick_key(0, 2, "One") == CaptionQuickKey::none,
        "a button with stages holds none"
    );
    auto& cycle = f.record("CYCLE");
    cycle.bytes[field::button_stages] = 2;
    input::set_gadget_text(f.panel, f.index("CYCLE"), "One|Two", 0);
    require(gui::record_string(cycle, field::text) == "One", "first stage caption");
    require(gui::record_string(cycle, field::text + 4) == "Two", "second stage caption");
    input::set_gadget_active_by_name(f.panel, "MAPS", 0);
    require(
        f.record("MAPSBAR").bytes[field::active] == 0, "deactivating a list deactivates its bar"
    );
    require(input::gadget_active_by_name(f.panel, "MAPS") == 0, "active byte read back");
    input::set_gadget_active_by_name(f.panel, "MAPSBAR", 1);
    require(f.record("MAPSUP").bytes[field::active] == 1, "activating a bar activates its buttons");
    input::rename_gadget(f.panel, "HINT", "HINT2");
    require(f.index("HINT2") > 0, "renamed");
    std::string copy;
    require(
        input::gadget_text_by_name(f.panel, "HINT2", &copy) != nullptr && copy == "Okay hint",
        "text read"
    );
    require(input::button_stage_by_name(f.panel, "HINT2") == -1, "labels have no stage");
    f.panel.hovered = f.index("OK");
    gui::set_record_string(f.record("OK"), field::help, "Accept changes", 0x80);
    input::rename_gadget(f.panel, "HINT2", "HELPTEXT");
    input::update_help_text(f.panel);
    require(
        gui::record_string(f.record("HELPTEXT"), field::text) == "Accept changes", "help copied"
    );
    // The default font is stored on the context; a record whose font byte
    // names no font record then selects it.
    static const uint8_t comix[] = {14};
    const void* selected = nullptr;
    f.panel.host.context = &selected;
    f.panel.host.select_font = [](void* context, const void* font) {
        *static_cast<const void**>(context) = font;
    };
    input::set_default_font(f.panel, comix);
    require(f.panel.default_font == comix, "default font stored");
    require(
        input::apply_gadget_font(f.panel, f.index("OK")) == -1 && selected == comix,
        "default font selected"
    );
}

void wrapping_and_blink_words() {
    Fixture f;
    const auto wrapped = input::wrap_text(f.panel, "one two three four", 64, -1);
    require(
        wrapped == "one two\r\nthree four", "breaks only when a separator follows an overlong line"
    );

    input::alloc_blink_words(f.panel, 2);
    auto& word = f.panel.blink_words[0];
    std::copy_n("Ready", 6, word.text.begin());
    word.on_seconds = 1.0F;
    word.off_seconds = 2.0F;
    word.color_on = 7;
    word.color_off = 3;
    static std::vector<int32_t> colors;
    colors.clear();
    f.panel.host.ticks_per_second = [](void*) { return 30; };
    f.panel.host.draw_label =
        [](void*, input::GadgetPanel&, const char*, int32_t, int32_t, int32_t color) {
            colors.push_back(color);
        };
    f.fake.tick = 10;
    input::update_blink_words(f.panel);
    require(word.lit == 1 && word.next_tick == 40.0F && colors.back() == 7, "lit for on_seconds");
    f.fake.tick = 41;
    input::update_blink_words(f.panel);
    require(
        word.lit == 0 && word.next_tick == 101.0F && colors.back() == 3, "dark for off_seconds"
    );
    require(colors.size() == 2, "empty entries are not drawn");
    input::free_blink_words(f.panel);
    input::update_blink_words(f.panel);
    require(colors.size() == 2, "freed table is inactive");
}

// LOADGAME.GUI's 494x420 root authored at (81,27): the load dialog's flags
// (0x980) centre it, the save dialog's (0x880) keep it where it fits.
void root_placement() {
    const uint32_t load = 0x980 | input::panel_flag::first_draw;
    const uint32_t save = 0x880 | input::panel_flag::first_draw;
    int16_t x = 81;
    int16_t y = 27;
    input::place_root(x, y, 494, 420, load, 640, 480, input::hud_strip_width);
    require(x == 73 && y == 30, "load dialog centred on 640x480");
    x = 81;
    y = 27;
    input::place_root(x, y, 494, 420, load, 1920, 1080, input::hud_strip_width);
    require(x == 713 && y == 330, "load dialog centred on 1920x1080");
    x = 81;
    y = 27;
    input::place_root(x, y, 494, 420, save, 640, 480, input::hud_strip_width);
    require(x == 81 && y == 27, "save dialog keeps its authored root");
    x = 200;
    y = 100;
    input::place_root(x, y, 494, 420, save, 640, 480, input::hud_strip_width);
    require(x == 73 && y == 30, "an axis past the screen is centred");
    x = 81;
    y = 27;
    input::place_root(x, y, 494, 420, load & ~input::panel_flag::first_draw, 640, 480, 0x80);
    require(x == 81 && y == 27, "centring waits for the first draw");
    x = 0;
    y = 0;
    input::place_root(
        x,
        y,
        320,
        200,
        input::panel_flag::beside_hud | input::panel_flag::first_draw,
        640,
        480,
        input::hud_strip_width
    );
    require(x == 224 && y == 140, "beside-HUD panel centred right of the strip");
    x = input::root_centred;
    y = 5;
    input::place_root(x, y, 320, 200, 0, 800, 600, input::hud_strip_width);
    require(x == 240 && y == 200, "a centre marker left in the root resolves on any draw");
}

void truncation_edges() {
    require(
        oa::base::game_math::truncate_to_int64(2.9) == 2 &&
            oa::base::game_math::truncate_to_int64(-2.9) == -2,
        "truncation"
    );
    require(
        oa::base::game_math::truncate_to_int64(std::numeric_limits<double>::infinity()) ==
            std::numeric_limits<int64_t>::min(),
        "infinity gives INT64_MIN"
    );
}

// The pointer queue and cursor of the GUI context: a queued event is taken
// when it lies over the top panel (400x300 at 100,100) or holds no button;
// one with buttons off the panel stays queued. An empty queue gives the
// latest move.
struct PointerDevice {
    std::deque<input::PointerEvent> queued;
    input::PointerEvent latest{};
    const oa::formats::gaf::Frame* shown = nullptr;
};

void bind_pointer_device(input::GadgetPanel& panel, PointerDevice& device) {
    static PointerDevice* bound = nullptr;
    bound = &device;
    panel.host.peek_pointer = [](void*, input::PointerEvent& out) {
        if (bound->queued.empty()) {
            out = bound->latest;
            return false;
        }
        out = bound->queued.front();
        return true;
    };
    panel.host.pop_pointer = [](void*, input::PointerEvent& out) {
        if (bound->queued.empty()) {
            out = bound->latest;
            return false;
        }
        out = bound->queued.front();
        bound->queued.pop_front();
        return true;
    };
    panel.host.current_pointer = [](void*, input::PointerEvent& out) { out = bound->latest; };
    panel.host.set_cursor_image = [](void*, const oa::formats::gaf::Frame* image) {
        bound->shown = image;
    };
    panel.host.cursor_image = [](void*) { return bound->shown; };
}

void pointer_queue_and_cursor() {
    Fixture f;
    PointerDevice device;
    bind_pointer_device(f.panel, device);
    device.latest = {321, 222, 0, 0, 0x200, 0};
    input::tick_cursor_and_read_pointer(f.panel);
    require(
        f.panel.pointer.x == 321 && f.panel.pointer.y == 222, "empty queue gives the latest move"
    );

    device.queued.push_back({150, 120, 1, 0, input::pointer_message::left_down, 1});
    input::tick_cursor_and_read_pointer(f.panel);
    require(
        device.queued.empty() && f.panel.pointer.x == 150 && f.panel.buttons == 1,
        "a press over the top panel is taken with its buttons"
    );

    device.queued.push_back({20, 30, 2, 0, input::pointer_message::right_down, 1});
    input::tick_cursor_and_read_pointer(f.panel);
    require(
        device.queued.size() == 1 && f.panel.pointer.x == 150 && f.panel.buttons == 1,
        "a press off the top panel stays queued"
    );
    device.queued.front().buttons = 0;
    input::tick_cursor_and_read_pointer(f.panel);
    require(
        device.queued.empty() && f.panel.pointer.x == 20 && f.panel.buttons == 0,
        "an event without buttons is taken anywhere"
    );
    device.queued.push_back({499, 399, 1, 0, input::pointer_message::left_down, 1});
    input::tick_cursor_and_read_pointer(f.panel);
    require(device.queued.empty(), "the panel's last pixel is over it");

    oa::formats::gaf::Sequence attack;
    attack.repeat_flags = 1;
    attack.frames.resize(3);
    for (auto& frame : attack.frames)
        frame.duration = 4;
    oa::formats::gaf::Sequence normal;
    normal.frames.resize(1);
    input::reset_cursor(f.panel, &normal.frames[0]);
    require(
        device.shown == &normal.frames[0] && f.panel.cursor == nullptr &&
            f.panel.hover_cursor_still == &normal.frames[0] && f.panel.buttons == 0,
        "the reset shows its still everywhere and drops the sequences"
    );
    input::set_cursor_sequence(f.panel, &attack);
    require(
        device.shown == &attack.frames[0] &&
            (f.panel.cursor_flags & input::cursor_flag::animating) != 0,
        "a sequence starts at its first frame"
    );
    f.panel.tick_delta = 3;
    input::tick_cursor_and_read_pointer(f.panel);
    require(device.shown == &attack.frames[0], "3 of 4 ticks keep the frame");
    f.panel.tick_delta = 6;
    input::tick_cursor_and_read_pointer(f.panel);
    require(device.shown == &attack.frames[2], "the elapsed ticks carry over frames");

    input::follow_hover_cursor(f.panel, true);
    require(
        device.shown == &normal.frames[0] && f.panel.cursor_frame.sequence == nullptr &&
            (f.panel.cursor_flags & input::cursor_flag::animating) == 0,
        "over the panel without a hover sequence the still shows"
    );
    f.panel.tick_delta = 8;
    input::tick_cursor_and_read_pointer(f.panel);
    require(device.shown == &normal.frames[0], "a still does not step");
    input::follow_hover_cursor(f.panel, false);
    require(
        device.shown == &attack.frames[0] && f.panel.cursor_frame.sequence == &attack,
        "off the panel the sequence runs again from its first frame"
    );
    input::tick_cursor_and_read_pointer(f.panel);
    input::follow_hover_cursor(f.panel, false);
    require(device.shown == &attack.frames[2], "a running sequence is not restarted");
}

} // namespace

int main() {
    try {
        panel_load_binds_defaults();
        push_button_states();
        toggle_checkbox_and_group();
        hold_and_cycle_buttons();
        list_selection_and_scrolling();
        scroll_bar_track_and_step();
        text_box_editing();
        keyboard_focus_and_defaults();
        keyboard_tab_and_slider();
        update_loop_activation();
        state_setters();
        wrapping_and_blink_words();
        root_placement();
        pointer_queue_and_cursor();
        truncation_edges();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
