// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every widget's place, caption and action, from a GUI file in the test
// data, and the registration-unavailable box from coreprime.ccx against the
// message box it copies.
#include "oa/boneyards/console/gui_screen.hpp"

#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/rover_session.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

// The registration message, with the two-character line breaks the file holds.
constexpr char k_message[] =
    "This server does not take\\nregistrations from the game yet.\\nPlease use the web page.";

struct FileSource {
    oa::HpiArchive* archive = nullptr;
    std::filesystem::path fixtures;
};

bool read_disk(const std::filesystem::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0) {
        return false;
    }
    in.seekg(0);
    out->resize(static_cast<std::size_t>(length));
    if (length > 0) {
        in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(length));
    }
    return static_cast<bool>(in);
}

// A fixture on disk is served from the test data. Anything else, including
// the registration screen, is read from the built coreprime.ccx.
bool read_source(void* context, const char* path, std::vector<uint8_t>* out) {
    if (context == nullptr || path == nullptr || out == nullptr) {
        return false;
    }
    auto* source = static_cast<FileSource*>(context);
    const std::filesystem::path on_disk = source->fixtures / path;
    std::error_code error;
    if (std::filesystem::is_regular_file(on_disk, error)) {
        return read_disk(on_disk, out);
    }
    oa::base::bytes::Decoded<std::vector<uint8_t>> read = source->archive->read(path);
    if (!read.ok() || !read.value.has_value()) {
        return false;
    }
    *out = std::move(*read.value);
    return true;
}

bool archive_has(const oa::HpiArchive* archive, const char* path) {
    const oa::base::bytes::Decoded<std::vector<uint8_t>> read = archive->read(path);
    return read.ok() && read.value.has_value();
}

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

const GuiControl* require_control(const GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    CHECK(control != nullptr);
    if (control == nullptr) {
        std::fprintf(stderr, "missing control %s\n", name);
    }
    return control;
}

void expect_rect(const GuiControl* control, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (control == nullptr) {
        return;
    }
    const bool match =
        control->x == x && control->y == y && control->width == width && control->height == height;
    if (!match) {
        std::fprintf(
            stderr,
            "%s at %d,%d %dx%d, expected %d,%d %dx%d\n",
            control->name,
            control->x,
            control->y,
            control->width,
            control->height,
            x,
            y,
            width,
            height
        );
    }
    CHECK(match);
}

void click(Desktop* desktop, const Window* window) {
    Message down{};
    down.code = MessageCode::left_down;
    down.x = window->x + window->width / 2;
    down.y = window->y + window->height / 2;
    down.buttons = button_left;
    Message up = down;
    up.code = MessageCode::left_up;
    up.buttons = 0;
    desktop_route(desktop, &down);
    desktop_route(desktop, &up);
}

void count_fired(void* user, Window*) {
    *static_cast<int*>(user) += 1;
}

void press_key(Window* window, int32_t key_code) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = key_code;
    window_dispatch(window, &msg);
}

int32_t image_edge(const Window* window, int32_t* height) {
    *height = 0;
    if (window == nullptr || window->states.images == nullptr || window->states.current < 0 ||
        window->states.current >= window->states.count) {
        return 0;
    }
    int32_t width = 0;
    image_size(window->states.images[window->states.current], &width, height);
    return width;
}

void check_sample(Desktop* desktop) {
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load("gui/sample.gui", &error);
    if (screen == nullptr) {
        std::fprintf(stderr, "sample: %s\n", error.message);
    }
    CHECK(screen != nullptr);
    if (screen == nullptr) {
        return;
    }
    CHECK(gui_screen_count(screen) == 15);
    CHECK(std::strcmp(gui_screen_fired_name(screen), "") == 0);

    const GuiControl* root = gui_screen_at(screen, 0);
    CHECK(root != nullptr && root->kind == gui_kind::panel);
    expect_rect(root, 0, 0, 640, 480);
    CHECK(gui_screen_root(screen)->color == k_no_handle);

    const GuiControl* green = require_control(screen, "Green");
    CHECK(green != nullptr && green->kind == gui_kind::green && green->window != nullptr);
    expect_rect(green, 8, 8, 200, 120);
    CHECK(green != nullptr && green->window != nullptr && green->window->color == k_no_handle);

    const GuiControl* go = require_control(screen, "Go");
    CHECK(go != nullptr && go->kind == gui_kind::button);
    expect_rect(go, 8, 140, 96, 20);
    CHECK(go != nullptr && std::strcmp(go->caption, "Go") == 0);
    CHECK(go != nullptr && std::strcmp(go->action, "goto:sample_next") == 0);
    CHECK(go != nullptr && go->window != nullptr && go->window->color >= 0);

    const GuiControl* title = require_control(screen, "Title");
    CHECK(title != nullptr && title->kind == gui_kind::label);
    expect_rect(title, 120, 140, 80, 16);
    CHECK(title != nullptr && std::strcmp(title->caption, "Title") == 0);
    CHECK(title != nullptr && std::strcmp(title->action, "goto:sample_label") == 0);

    const GuiControl* rows = require_control(screen, "Rows");
    CHECK(rows != nullptr && rows->kind == gui_kind::list_box);
    expect_rect(rows, 8, 180, 160, 68);

    const GuiControl* alpha = require_control(screen, "Alpha");
    CHECK(alpha != nullptr && alpha->kind == gui_kind::list_row);
    expect_rect(alpha, 8, 180, 143, 17);
    CHECK(alpha != nullptr && std::strcmp(alpha->caption, "Alpha") == 0);
    CHECK(alpha != nullptr && std::strcmp(alpha->action, "page:alpha") == 0);

    const GuiControl* name = require_control(screen, "Name");
    CHECK(name != nullptr && name->kind == gui_kind::text_box);
    expect_rect(name, 180, 180, 100, 21);
    CHECK(name != nullptr && std::strcmp(name->caption, "Vex") == 0);
    if (name != nullptr && name->window != nullptr) {
        CHECK(std::strcmp(reinterpret_cast<const EditBox*>(name->window)->text, "Vex") == 0);
    }

    const GuiControl* meter = require_control(screen, "Meter");
    CHECK(meter != nullptr && meter->kind == gui_kind::scroll_bar);
    expect_rect(meter, 300, 180, 16, 68);
    CHECK(meter != nullptr && std::strcmp(meter->caption, "track") == 0);
    if (meter != nullptr && meter->window != nullptr) {
        const auto* slider = reinterpret_cast<const Slider*>(meter->window);
        CHECK(slider_range(slider) == 10);
        CHECK(slider_index(slider) == 0);
    }

    const GuiControl* mark = require_control(screen, "Mark");
    CHECK(mark != nullptr && mark->kind == gui_kind::icon);
    expect_rect(mark, 330, 180, 11, 11);
    int32_t height = 0;
    CHECK(mark != nullptr && image_edge(mark->window, &height) == 11 && height == 11);

    const GuiControl* code = require_control(screen, "Code");
    CHECK(code != nullptr && code->kind == gui_kind::qr);
    expect_rect(code, 360, 180, 113, 113);
    CHECK(code != nullptr && image_edge(code->window, &height) == 113 && height == 113);

    const GuiControl* badge = require_control(screen, "Badge");
    CHECK(badge != nullptr && badge->kind == gui_kind::image);
    expect_rect(badge, 490, 180, 11, 11);
    CHECK(badge != nullptr && image_edge(badge->window, &height) == 11 && height == 11);

    const GuiControl* spot = require_control(screen, "Spot");
    CHECK(spot != nullptr && spot->kind == gui_kind::hot_surface);
    expect_rect(spot, 520, 180, 40, 20);
    CHECK(spot != nullptr && std::strcmp(spot->action, "goto:sample_hot") == 0);

    const GuiControl* font = require_control(screen, "Font");
    CHECK(font != nullptr && font->kind == gui_kind::font_resource && font->window == nullptr);
    CHECK(font != nullptr && std::strcmp(font->caption, "anims\\Hattfont12.gaf") == 0);

    const GuiControl* file = require_control(screen, "File");
    CHECK(file != nullptr && file->kind == gui_kind::file_resource && file->window == nullptr);
    CHECK(file != nullptr && std::strcmp(file->caption, "BY_New.gaf") == 0);

    const GuiControl* spare = require_control(screen, "Spare");
    CHECK(spare != nullptr && spare->kind == 9 && spare->window == nullptr);

    if (go != nullptr && go->window != nullptr) {
        click(desktop, go->window);
        CHECK(std::strcmp(gui_screen_fired_name(screen), "Go") == 0);
        CHECK(std::strcmp(gui_screen_fired_action(screen), "goto:sample_next") == 0);
    }
    if (title != nullptr && title->window != nullptr) {
        click(desktop, title->window);
        CHECK(std::strcmp(gui_screen_fired_name(screen), "Title") == 0);
        CHECK(std::strcmp(gui_screen_fired_action(screen), "goto:sample_label") == 0);
    }
    if (alpha != nullptr && alpha->window != nullptr) {
        click(desktop, alpha->window);
        CHECK(std::strcmp(gui_screen_fired_name(screen), "Alpha") == 0);
        CHECK(std::strcmp(gui_screen_fired_action(screen), "page:alpha") == 0);
    }
    if (spot != nullptr && spot->window != nullptr) {
        click(desktop, spot->window);
        CHECK(std::strcmp(gui_screen_fired_name(screen), "Spot") == 0);
        CHECK(std::strcmp(gui_screen_fired_action(screen), "goto:sample_hot") == 0);
    }
    press_key(gui_screen_root(screen), key::enter);
    CHECK(std::strcmp(gui_screen_fired_name(screen), "Go") == 0);
    CHECK(std::strcmp(gui_screen_fired_action(screen), "goto:sample_next") == 0);
    int fires = 0;
    gui_screen_set_user(screen, &fires, count_fired, nullptr);
    press_key(gui_screen_root(screen), key::enter);
    CHECK(fires == 1);
    if (go != nullptr && go->window != nullptr)
        window_set_state(go->window, button_state::disabled);
    press_key(gui_screen_root(screen), key::enter);
    CHECK(fires == 1);
    press_key(gui_screen_root(screen), key::escape);
    CHECK(fires == 1);
    CHECK(std::strcmp(gui_screen_fired_name(screen), "Spot") == 0);
    CHECK(std::strcmp(gui_screen_fired_action(screen), "goto:sample_hot") == 0);

    gui_screen_destroy(screen);
}

void clear_overlay(Surface* surface) {
    for (int32_t y = 0; y < surface->height; ++y) {
        std::memset(
            surface->pixels + static_cast<std::size_t>(y * surface->pitch),
            k_overlay_transparent,
            static_cast<std::size_t>(surface->width)
        );
    }
}

std::vector<uint8_t> copy_overlay(const Surface* surface) {
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(surface->width) * static_cast<std::size_t>(surface->height)
    );
    for (int32_t y = 0; y < surface->height; ++y) {
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(y * surface->width),
            surface->pixels + static_cast<std::size_t>(y * surface->pitch),
            static_cast<std::size_t>(surface->width)
        );
    }
    return pixels;
}

void check_register(Desktop* desktop, Surface* overlay) {
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load("gui/register_unavailable.gui", &error);
    if (screen == nullptr) {
        std::fprintf(stderr, "register: %s\n", error.message);
    }
    CHECK(screen != nullptr);
    if (screen == nullptr) {
        return;
    }

    const GuiControl* root = gui_screen_find(screen, "RegBox");
    expect_rect(root, 142, 172, 216, 136);
    CHECK(root != nullptr && root->kind == gui_kind::panel);
    CHECK(gui_screen_root(screen)->color == k_no_handle);
    CHECK(
        root != nullptr && root->x == message_box_layout::x && root->y == message_box_layout::y &&
        root->width == message_box_layout::width && root->height == message_box_layout::height
    );

    const GuiControl* face = require_control(screen, "Face");
    expect_rect(face, 142, 172, 216, 136);
    CHECK(face != nullptr && face->kind == gui_kind::image && face->window != nullptr);
    int32_t face_height = 0;
    CHECK(face != nullptr && image_edge(face->window, &face_height) > 0);

    char shown[128];
    std::snprintf(shown, sizeof shown, "%s", k_message);
    convert_escaped_newlines(shown);
    const GuiControl* text = require_control(screen, "Text");
    expect_rect(text, 155, 178, 190, 96);
    CHECK(text != nullptr && text->kind == gui_kind::label);
    CHECK(text != nullptr && std::strcmp(text->caption, shown) == 0);
    CHECK(
        text != nullptr && text->x == message_box_layout::x + message_box_layout::text_dx &&
        text->y == message_box_layout::y + message_box_layout::text_dy &&
        text->width == message_box_layout::text_width &&
        text->height == message_box_layout::text_height
    );

    const GuiControl* ok = require_control(screen, "Ok");
    expect_rect(ok, 222, 278, 57, 21);
    CHECK(ok != nullptr && ok->kind == gui_kind::button);
    CHECK(ok != nullptr && std::strcmp(ok->caption, "Ok") == 0);
    CHECK(ok != nullptr && std::strcmp(ok->action, "goto:new_register_beta") == 0);
    CHECK(
        ok != nullptr && ok->x == message_box_layout::x + message_box_layout::single_button_dx &&
        ok->y == message_box_layout::y + message_box_layout::button_dy &&
        ok->width == message_box_layout::button_width &&
        ok->height == message_box_layout::button_height
    );
    if (ok != nullptr && ok->window != nullptr) {
        CHECK(ok->window->states.current == button_state::unpressed);
        CHECK(ok->window->ops == &k_blank_button_ops);
        CHECK(ok->window->focused == 0);
    }

    desktop_set_focus(desktop, nullptr);
    clear_overlay(overlay);
    desktop_draw(desktop);
    const std::vector<uint8_t> drawn = copy_overlay(overlay);
    int32_t inside = 0;
    int32_t outside = 0;
    for (int32_t y = 0; y < overlay->height; ++y) {
        for (int32_t x = 0; x < overlay->width; ++x) {
            const bool opaque =
                drawn[static_cast<std::size_t>(y * overlay->width + x)] != k_overlay_transparent;
            const bool in_box = x >= 142 && x < 142 + 216 && y >= 172 && y < 172 + 136;
            (in_box ? inside : outside) += opaque ? 1 : 0;
        }
    }
    CHECK(inside > 216 * 136 / 2);
    CHECK(outside < 216 * 4);

    gui_screen_destroy(screen);

    MessageBox* box =
        message_box_create(k_message, message_box_buttons::ok, nullptr, nullptr, nullptr);
    desktop_set_focus(desktop, nullptr);
    if (box->base.focused != 0) {
        desktop_set_focus(desktop, nullptr);
    }
    Window* box_ok = window_find_named(&box->base, message_box_name::ok, nullptr);
    if (box_ok != nullptr && box_ok->focused != 0) {
        desktop_set_focus(desktop, nullptr);
    }
    clear_overlay(overlay);
    desktop_draw(desktop);
    const std::vector<uint8_t> reference = copy_overlay(overlay);
    int32_t mismatches = 0;
    for (std::size_t i = 0; i < drawn.size() && i < reference.size(); ++i) {
        if (drawn[i] == reference[i]) {
            continue;
        }
        if (mismatches < 8) {
            const int32_t x = static_cast<int32_t>(i % 640);
            const int32_t y = static_cast<int32_t>(i / 640);
            std::fprintf(
                stderr, "pixel %d,%d screen %u reference %u\n", x, y, drawn[i], reference[i]
            );
        }
        ++mismatches;
    }
    if (mismatches != 0) {
        std::fprintf(stderr, "%d pixel mismatches\n", mismatches);
    }
    CHECK(mismatches == 0);
    window_destroy(&box->base);

    screen = gui_screen_load("gui/register_unavailable.gui", &error);
    CHECK(screen != nullptr);
    if (screen == nullptr) {
        return;
    }
    ok = gui_screen_find(screen, "Ok");
    if (ok != nullptr && ok->window != nullptr) {
        click(desktop, ok->window);
        CHECK(std::strcmp(gui_screen_fired_name(screen), "Ok") == 0);
        CHECK(std::strcmp(gui_screen_fired_action(screen), "goto:new_register_beta") == 0);
    }
    press_key(gui_screen_root(screen), key::enter);
    CHECK(std::strcmp(gui_screen_fired_name(screen), "Ok") == 0);
    CHECK(std::strcmp(gui_screen_fired_action(screen), "goto:new_register_beta") == 0);
    gui_screen_destroy(screen);
}

void check_rejected() {
    GuiLoadError error{};
    CHECK(gui_screen_load("gui/missing.gui", &error) == nullptr);
    CHECK(error.message[0] != '\0');
    CHECK(gui_screen_load("gui/broken.gui", &error) == nullptr);
    CHECK(std::strcmp(error.message, "first GUI record is not the required panel gadget") == 0);
    CHECK(gui_screen_load(nullptr, &error) == nullptr);
    CHECK(gui_screen_count(nullptr) == 0);
    CHECK(gui_screen_at(nullptr, 0) == nullptr);
    CHECK(gui_screen_find(nullptr, "Ok") == nullptr);
    CHECK(std::strcmp(gui_screen_fired_name(nullptr), "") == 0);
    gui_screen_destroy(nullptr);
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the GUI screen loaded from the install");
    const std::filesystem::path coreprime = oa::boneyards_test::coreprime_archive(argc, argv);
    if (coreprime.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    oa::base::bytes::Decoded<oa::HpiArchive> opened = oa::open_hpi_file(coreprime);
    if (!opened.ok() || !opened.value.has_value()) {
        std::fprintf(stderr, "coreprime.ccx could not be opened\n");
        return 1;
    }
    FileSource source;
    source.archive = &*opened.value;
    source.fixtures = OA_GUI_FIXTURES;
    CHECK(!archive_has(source.archive, "gui/sample.gui"));
    CHECK(!archive_has(source.archive, "gui/broken.gui"));
    CHECK(archive_has(source.archive, "gui/register_unavailable.gui"));
    CHECK(std::filesystem::is_regular_file(source.fixtures / "gui/sample.gui"));
    CHECK(std::filesystem::is_regular_file(source.fixtures / "gui/broken.gui"));
    rover_session_set_virtual_files(read_source, &source);

    oa::AssetStore assets(root);
    assets.mount(root / "by.ccx");
    assets.mount(root / "totala1.hpi");
    assets.mount(coreprime);
    InstalledAssets installed;
    installed.assets = &assets;
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i) {
        installed.palette.entries[i] = oa::PaletteEntry{
            raw[static_cast<std::size_t>(i) * 4],
            raw[static_cast<std::size_t>(i) * 4 + 1],
            raw[static_cast<std::size_t>(i) * 4 + 2],
            0
        };
    }
    ToolkitConfig config{};
    config.width = 640;
    config.height = 480;
    config.palette = &installed.palette;
    config.files = ToolkitFiles{&installed, read_asset};
    Toolkit* toolkit = toolkit_create(&config);
    CHECK(toolkit != nullptr);
    if (toolkit == nullptr) {
        rover_session_set_virtual_files(nullptr, nullptr);
        return 1;
    }
    const Services previous = services_install(toolkit_services(toolkit));
    Desktop* desktop = toolkit_desktop(toolkit);
    check_sample(desktop);
    check_register(desktop, toolkit_surface(toolkit));
    check_rejected();
    services_install(previous);
    toolkit_destroy(toolkit);
    rover_session_set_virtual_files(nullptr, nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gui screen tests passed\n");
    return 0;
}
