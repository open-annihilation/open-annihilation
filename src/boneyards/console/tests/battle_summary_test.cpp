// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Main Frame's planet summary with the toolkit's fonts and texts: the
// labels battle_summary_create builds and the texts battle_summary_update and the frame's draw
// main_frame_draw put on them.
#include "console_state.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int failures{};

void check(bool result, const char* text) {
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", text);
        ++failures;
    }
}

struct Fixture {
    oa::AssetStore assets{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

bool read_asset(void* context, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Fixture*>(context)->assets.read(path).bytes;
        return true;
    } catch (...) {
        return false;
    }
}

void click(StatusWidget* widget, const Window* window) {
    Message message{};
    message.x = window->x + window->width / 2;
    message.y = window->y + window->height / 2;
    message.code = MessageCode::left_down;
    message.buttons = button_left;
    boneyards_status_widget_event(widget, message);
    message.code = MessageCode::left_up;
    message.buttons = 0;
    boneyards_status_widget_event(widget, message);
}

const char* text_of(const Window* label) {
    return text_get(label->states.texts[1]);
}

bool text_is(const Window* label, const char* text) {
    return std::strcmp(text_of(label), text) == 0;
}

screens::PlanetRecord planet(int32_t id, const char* name, int32_t x, char side) {
    screens::PlanetRecord r{};
    r.id = id;
    r.name = name;
    r.x = x;
    r.y = 100;
    r.sequence = "Planet";
    r.side = side;
    r.ta_map = "Coast to Coast";
    r.ta_mapsource = "TA";
    r.ta_maptype = "Green";
    return r;
}

void draw(StatusWidget* widget) {
    std::vector<uint8_t> pixels(640 * 480, 0);
    oa::Surface surface{};
    surface.width = 640;
    surface.height = 480;
    surface.pitch = 640;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(widget, &surface);
}

void test_summary(Fixture* f) {
    StatusWidgetConfig config{};
    config.files = {f, read_asset};
    config.palette = &f->palette;
    config.service.network_available = [](void*) { return true; };
    config.service.activate = [](void*) { return ActivationResult::ok; };
    StatusWidget* widget = boneyards_status_widget_create(config);
    check(widget != nullptr, "widget created");
    if (widget == nullptr)
        return;
    click(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    MainFrame* frame = widget->system.main_frame;
    BattleSummary* s = frame != nullptr ? frame->battle_summary : nullptr;
    check(s != nullptr, "the Main Frame built its planet summary");
    if (s == nullptr) {
        boneyards_status_widget_destroy(widget);
        return;
    }
    {
        ScopedServices scope(widget);
        const screens::HatFonts& fonts = widget->system.hat_fonts;
        check(
            fonts.small_green >= 0 && fonts.small_light >= 0 && fonts.small_dim >= 0,
            "hat fonts loaded"
        );
        check(
            s->name->font == fonts.small_green && s->owner->font == fonts.small_light &&
                s->battles_caption->font == fonts.small_dim &&
                s->battles->font == fonts.small_green &&
                s->forming_caption->font == fonts.small_dim &&
                s->forming->font == fonts.small_green && s->arm_wins->font == fonts.small_green &&
                s->core_wins->font == fonts.small_green,
            "name and counts in the green system font, owner in the light one, captions in the dim "
            "one"
        );
        check(
            text_is(s->battles_caption, "Battles:") && text_is(s->forming_caption, "Forming:") &&
                text_is(s->name, "") && text_is(s->owner, "") && text_is(s->arm_wins, ""),
            "the captions are set and the value labels start empty"
        );
    }

    screens::GalaxyView& galaxy = widget->navigation->galaxy;
    const screens::PlanetRecord planets[2] = {
        planet(1, "Core Prime", 10, 'C'), planet(2, "Empyrrean", 100, 'A')
    };
    const screens::ConnectorRecord link{1, 2};
    const screens::SpriteFrame frames[2] = {{16, 16, 8, 8}, {16, 16, 8, 8}};
    const std::size_t counts[2] = {1, 1};
    const screens::PlanetSprites sprites{frames, counts};
    check(
        screens::galaxy_load(&galaxy, 2400, 1600, planets, 2, &link, 1, &sprites), "war map loaded"
    );
    galaxy.locked = false;
    uint32_t area{};
    check(screens::galaxy_select_planet(&galaxy, 1, &area), "planet selected");
    screens::GalaxyPlanet& p = galaxy.planets[1];
    {
        ScopedServices scope(widget);
        battle_summary_update(s);
        check(
            text_is(s->name, "Empyrrean") && text_is(s->owner, "Arm") && text_is(s->battles, "0") &&
                text_is(s->forming, "0"),
            "the selected planet's name, owner and counts"
        );
        check(s->name->visible == 1 && s->arm_icon->visible == 0, "the first rows show");
        p.side = 'i';
        p.in_progress = 3;
        p.forming = -2;
        p.arm_wins = 5;
        p.core_wins = 7;
        battle_summary_update(s);
        check(
            text_is(s->owner, "Contested") && text_is(s->battles, "3") &&
                text_is(s->forming, "-2") && text_is(s->arm_wins, "5") &&
                text_is(s->core_wins, "7"),
            "a contested planet's counts and wins as signed decimals"
        );
        check(s->core_icon->visible == 1 && s->core_wins->visible == 1, "the win row shows");
        p.side = '?';
        p.arm_wins = 9;
        battle_summary_update(s);
        check(
            text_is(s->owner, "Contested") && text_is(s->arm_wins, "5") &&
                s->arm_wins->visible == 0,
            "an unknown side keeps the owner text and hides the wins without rewriting them"
        );
    }
    p.forming = 4;
    draw(widget);
    {
        ScopedServices scope(widget);
        check(text_is(s->forming, "4"), "each frame's draw refreshes the counts");
    }
    galaxy.territory.planet = screens::kNoPlanet;
    draw(widget);
    check(
        s->name->visible == 0 && s->forming_caption->visible == 0 && s->core_icon->visible == 0,
        "without a planet every summary window hides"
    );
    boneyards_status_widget_destroy(widget);
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the battle summary tests");
    Fixture fixture;
    fixture.assets.mount(root / "by.ccx");
    fixture.assets.mount(root / "totala1.hpi");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int32_t i = 0; i < 256; ++i)
        fixture.palette.entries[i] = {raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    test_summary(&fixture);
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("battle summary tests passed\n");
    return 0;
}
