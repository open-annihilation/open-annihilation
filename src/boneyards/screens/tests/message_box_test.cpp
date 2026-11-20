// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards message box: layout, button sets, click and key results, and an
// installed-data draw of the dialog.
#include "oa/boneyards/message_box.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "check.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/platform/system.hpp"

using namespace oa::boneyards;

namespace {

struct InstalledAssets {
    oa::AssetStore* assets = nullptr;
    oa::Palette palette{};
};

uint8_t* read_asset(void* user, const char* path, size_t* size) {
    auto* installed = static_cast<InstalledAssets*>(user);
    std::vector<uint8_t> bytes;
    try {
        bytes = installed->assets->read(path).bytes;
    } catch (...) {
        return nullptr;
    }
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

Message key_message(int32_t k) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = k;
    return msg;
}

Message click(MessageCode code, const Window* w) {
    Message msg{};
    msg.code = code;
    msg.x = w->x + w->width / 2;
    msg.y = w->y + w->height / 2;
    msg.buttons = code == MessageCode::left_down ? button_left : 0;
    return msg;
}

void press_button(Desktop* desktop, Window* button) {
    const Message down = click(MessageCode::left_down, button);
    const Message up = click(MessageCode::left_up, button);
    desktop_route(desktop, &down);
    desktop_route(desktop, &up);
}

int g_closed = 0;

void on_closed(void*, MessageBox*) {
    ++g_closed;
}

void test_behaviour(Desktop* desktop) {
    char text[] = "Line one\\nLine two";
    convert_escaped_newlines(text);
    CHECK(std::strcmp(text, "Line one\r\nLine two") == 0);

    bool accepted = true;
    bool declined = true;
    MessageBox* box =
        message_box_create("Hello", message_box_buttons::ok, &accepted, &declined, nullptr);
    CHECK(!accepted && !declined);
    CHECK(box->base.x == message_box_layout::x && box->base.y == message_box_layout::y);
    CHECK(box->base.width == 0xD8 && box->base.height == 0x88);
    CHECK(window_is_named(&box->base, message_box_name::box));
    CHECK(desktop->windows.count == 1 && desktop->windows.items[0] == &box->base);
    Window* ok = window_find_named(&box->base, message_box_name::ok, class_name::button);
    CHECK(ok != nullptr && ok->x == box->base.x + 0x50 && ok->y == box->base.y + 0x6A);
    CHECK(ok != nullptr && ok->states.count == 4 && ok->states.current == 2);
    CHECK(window_find_named(&box->base, message_box_name::cancel, nullptr) == nullptr);
    const Window* label = window_find_named(&box->base, message_box_name::text, nullptr);
    CHECK(label != nullptr && label->width == 0xBE && label->x == box->base.x + 0xD);
    CHECK(message_box_find_open(desktop) == box);

    Message other = key_message('x');
    CHECK(window_dispatch(&box->base, &other));
    CHECK(box->base.owned == 0 && !accepted);
    Message enter = key_message(key::enter);
    window_dispatch(&box->base, &enter);
    CHECK(accepted && box->base.owned == 1);
    CHECK(message_box_find_open(desktop) == nullptr);
    desktop_destroy_owned(desktop);
    CHECK(desktop->windows.count == 0);

    Window* owner = gadget_create();
    window_create(owner, "owner", 0, 0, 10, 10, nullptr, true);
    box = message_box_create("Sure?", message_box_buttons::yes_no, &accepted, &declined, owner);
    box->on_closed = on_closed;
    Message space = key_message(' ');
    window_dispatch(&box->base, &space);
    CHECK(box->base.owned == 0);
    Window* no = window_find_named(&box->base, message_box_name::no, class_name::button);
    CHECK(no != nullptr && no->x == box->base.x + 0x82);
    press_button(desktop, no);
    CHECK(declined && !accepted && g_closed == 1);
    CHECK(desktop->focus == owner && box->base.owned == 1);
    desktop_destroy_owned(desktop);

    // Cancel only reports when an accepted flag was also given.
    box = message_box_create("Go?", message_box_buttons::ok_cancel, nullptr, &declined, nullptr);
    press_button(desktop, window_find_named(&box->base, message_box_name::cancel, nullptr));
    CHECK(!declined && box->base.owned == 1);
    desktop_destroy_owned(desktop);
    box = message_box_create("Go?", message_box_buttons::ok_cancel, &accepted, &declined, nullptr);
    Message c = key_message('c');
    window_dispatch(&box->base, &c);
    CHECK(declined && !accepted && box->base.owned == 1);
    desktop_destroy_owned(desktop);

    // The box keeps the keyboard. The first arrow lands on No, and a later
    // arrow moves between the answers. Y, N and Escape still answer the box.
    desktop_set_focus(desktop, owner);
    box = message_box_create("Exit?", message_box_buttons::yes_no, &accepted, &declined, nullptr);
    Window* no_answer = window_find_named(&box->base, message_box_name::no, class_name::button);
    Window* yes_answer = window_find_named(&box->base, message_box_name::yes, class_name::button);
    CHECK(desktop->focus == &box->base);
    Message right = key_message(key::right);
    CHECK(desktop_route(desktop, &right));
    CHECK(desktop->focus == no_answer);
    Message left = key_message(key::left);
    CHECK(desktop_route(desktop, &left));
    CHECK(desktop->focus == yes_answer);
    Message right_again = key_message(key::right);
    CHECK(desktop_route(desktop, &right_again));
    CHECK(desktop->focus == no_answer);
    Message tab_key = key_message(key::tab);
    CHECK(desktop_route(desktop, &tab_key));
    CHECK(desktop->focus == yes_answer || desktop->focus == no_answer);
    CHECK(desktop->focus != owner);
    Message lower_n = key_message('n');
    CHECK(desktop_route(desktop, &lower_n));
    CHECK(declined && !accepted && box->base.owned == 1 && desktop->focus == owner);
    desktop_destroy_owned(desktop);
    box = message_box_create("Exit?", message_box_buttons::yes_no, &accepted, &declined, nullptr);
    Message lower_y = key_message('y');
    CHECK(desktop_route(desktop, &lower_y));
    CHECK(accepted && !declined && box->base.owned == 1 && desktop->focus == owner);
    desktop_destroy_owned(desktop);
    box = message_box_create("Exit?", message_box_buttons::yes_no, &accepted, &declined, nullptr);
    Message enter_yes = key_message(key::enter);
    CHECK(desktop_route(desktop, &enter_yes));
    CHECK(accepted && !declined && box->base.owned == 1);
    desktop_destroy_owned(desktop);
    box = message_box_create("Exit?", message_box_buttons::yes_no, &accepted, &declined, nullptr);
    Message right_no = key_message(key::right);
    CHECK(desktop_route(desktop, &right_no));
    CHECK(
        desktop->focus == window_find_named(&box->base, message_box_name::no, class_name::button)
    );
    Message enter_no = key_message(key::enter);
    CHECK(desktop_route(desktop, &enter_no));
    CHECK(declined && !accepted && box->base.owned == 1);
    desktop_destroy_owned(desktop);
    box = message_box_create("Exit?", message_box_buttons::yes_no, &accepted, &declined, nullptr);
    Message escape_no = key_message(key::escape);
    CHECK(desktop_route(desktop, &escape_no));
    CHECK(declined && !accepted && box->base.owned == 1 && desktop->focus == owner);
    desktop_destroy_owned(desktop);
    window_destroy(owner);
}

// The window a box would hand focus back to can close while the box is up
// (the Main Frame's EXIT with a console message open); Ok then leaves focus
// on the box and still closes it.
void test_focus_return_closed(Desktop* desktop) {
    Window* owner = gadget_create();
    window_create(owner, "owner", 0, 0, 10, 10, nullptr, true);
    bool accepted = false;
    MessageBox* box =
        message_box_create("Gone", message_box_buttons::ok, &accepted, nullptr, owner);
    desktop_set_focus(desktop, &box->base);
    desktop_remove(desktop, owner);
    press_button(desktop, window_find_named(&box->base, message_box_name::ok, class_name::button));
    CHECK(accepted && box->base.owned == 1);
    CHECK(desktop->focus != owner);
    desktop_destroy_owned(desktop);
    CHECK(desktop->windows.count == 0 && desktop->focus == nullptr);
    window_destroy(owner);
}

/// Checks that the destructor clears the draw object's flag when the box's
/// draw_flag_on_close is zero and sets it otherwise.
void test_destroy_draw_flag() {
    DrawService draw{};
    Services s = services();
    s.draw = &draw;
    const Services previous = services_install(s);
    CHECK(draw.box_close_flag == 1);
    MessageBox* box = message_box_create("Bye", message_box_buttons::ok, nullptr, nullptr, nullptr);
    CHECK(box->draw_flag_on_close == 0);
    window_destroy(&box->base);
    CHECK(draw.box_close_flag == 0);
    box = message_box_create("Bye", message_box_buttons::ok, nullptr, nullptr, nullptr);
    box->draw_flag_on_close = 1;
    window_destroy(&box->base);
    CHECK(draw.box_close_flag == 1);
    services_install(previous);
}

/// Draws a message box from the Boneyards install's art and fonts and checks that it fills its
/// box and only its frame art overhangs it.
void test_installed_draw() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the message box drawn from the install");
    oa::AssetStore assets(root);
    assets.mount(root / "by.ccx");
    assets.mount(root / "totala1.hpi");
    InstalledAssets installed;
    installed.assets = &assets;
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        installed.palette.entries[i] =
            oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};

    ToolkitConfig config{};
    config.width = 640;
    config.height = 480;
    config.palette = &installed.palette;
    config.files = ToolkitFiles{&installed, read_asset};
    Toolkit* tk = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(tk));
    MessageBox* box = message_box_create(
        "Please connect to the Internet before entering the Boneyards",
        message_box_buttons::ok,
        nullptr,
        nullptr,
        nullptr
    );
    const Window* label = window_find_named(&box->base, message_box_name::text, nullptr);
    CHECK(box->base.states.images[1] >= 0);
    CHECK(label != nullptr && label->font >= 0);
    desktop_draw(toolkit_desktop(tk));
    const Surface* overlay = toolkit_surface(tk);
    int32_t inside = 0;
    int32_t outside = 0;
    for (int32_t y = 0; y < overlay->height; ++y) {
        for (int32_t x = 0; x < overlay->width; ++x) {
            const bool opaque = overlay->pixels[y * overlay->pitch + x] != k_overlay_transparent;
            const bool in_box = x >= box->base.x && x < box->base.x + box->base.width &&
                                y >= box->base.y && y < box->base.y + box->base.height;
            (in_box ? inside : outside) += opaque ? 1 : 0;
        }
    }
    CHECK(inside > 0xD8 * 0x88 / 2);
    CHECK(outside < 0xD8 * 4); // only the frame art overhangs the box
    if (const auto snapshot = oa::platform::environment_value("OA_BY_SNAPSHOT")) {
        std::ofstream out(*snapshot, std::ios::binary);
        out << "P6\n640 480\n255\n";
        for (int32_t i = 0; i < 640 * 480; ++i) {
            const oa::PaletteEntry& e = installed.palette.entries[overlay->pixels[i]];
            out.put(static_cast<char>(e.r)).put(static_cast<char>(e.g)).put(static_cast<char>(e.b));
        }
    }
    window_destroy(&box->base);
    services_install(previous);
    toolkit_destroy(tk);
}

} // namespace

int main(int argc, char** argv) {
    // --data draws the box from the BY1117a install instead.
    if (oa::test::game_data_requested(argc, argv)) {
        test_installed_draw();
        return by_test::finish("message box drawn from the install");
    }
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Services s{};
    s.desktop = &desktop;
    const Services previous = services_install(s);
    test_behaviour(&desktop);
    test_focus_return_closed(&desktop);
    test_destroy_draw_flag();
    desktop_release(&desktop);
    services_install(previous);
    return by_test::finish("message box");
}
