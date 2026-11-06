// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Main Frame's planet summary beside the Recon button: the selected
// planet's picture painted into the frame's background, its name and owner,
// the Battles and Forming counts and, for a contested planet, each side's
// wins beside its logo.
#include <cstdlib>

#include "console_state.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"

namespace oa::boneyards::console {

namespace {

// Label and icon positions and sizes of the summary, and its picture area.
namespace layout {
constexpr int32_t label_x = 0x211;
constexpr int32_t count_x = 0x235;
constexpr int32_t icon_x = 0x248;
constexpr int32_t wins_x = 0x256;
constexpr int32_t name_y = 0x80;
constexpr int32_t owner_y = 0x8d;
constexpr int32_t battles_y = 0x9e;
constexpr int32_t forming_y = 0xab;
constexpr int32_t arm_icon_y = 0x9d;
constexpr int32_t core_icon_y = 0xaa;
constexpr int32_t name_width = 0x3c;
constexpr int32_t caption_width = 0x22;
constexpr int32_t count_width = 0xf;
constexpr int32_t label_height = 9;
constexpr int32_t icon_size = 0xb;
constexpr Rect32 picture{0x23a, 0x7e, 0x26d, 0x9c};
constexpr int32_t picture_x = 0x254;
constexpr int32_t picture_y = 0x8d;
} // namespace layout

constexpr uint8_t k_picture_fill = 0xaf;
// Index into a planet's Planets.gaf sizes 25, 50, 75 and 100.
constexpr std::size_t k_picture_size = 1;
constexpr int32_t k_logo_arm = 0;
constexpr int32_t k_logo_core = 1;
constexpr const char* k_logos_sequence = "ArmCoreLogos";
constexpr const char* k_icon_name = "SmallArmIcon"; // both icons carry it
constexpr const char* k_battles_caption = "Battles:";
constexpr const char* k_forming_caption = "Forming:";
constexpr int32_t k_state_normal = 1;

Window*
add_label(MainFrame* frame, int32_t x, int32_t y, const char* text, Handle font, int32_t width) {
    return screens::lobby_label_create(
        &frame->base, x, y, text, font, 0, 0, width, layout::label_height
    );
}

Window* add_icon(MainFrame* frame, int32_t y, Handle logo) {
    Window* icon = static_create();
    window_create(
        icon,
        k_icon_name,
        layout::icon_x,
        y,
        layout::icon_size,
        layout::icon_size,
        &frame->base,
        true
    );
    window_set_state_image(icon, k_state_normal, logo);
    return icon;
}

void show(Window* const* windows, std::size_t count, bool shown) {
    for (std::size_t i = 0; i < count; ++i)
        windows[i]->visible = shown ? 1 : 0;
}

screens::GalaxyView* galaxy_of(const BattleSummary* summary) {
    screens::BoneyardsScreens* nav = summary->frame->owner->navigation;
    return nav != nullptr ? &nav->galaxy : nullptr;
}

} // namespace

BattleSummary* battle_summary_create(MainFrame* frame) {
    auto* summary = static_cast<BattleSummary*>(std::calloc(1, sizeof(BattleSummary)));
    if (summary == nullptr)
        return nullptr;
    summary->frame = frame;
    summary->arm_logo = k_no_handle;
    summary->core_logo = k_no_handle;
    const screens::HatFonts& f = frame->owner->system.hat_fonts;
    summary->name =
        add_label(frame, layout::label_x, layout::name_y, "", f.small_green, layout::name_width);
    summary->owner =
        add_label(frame, layout::label_x, layout::owner_y, "", f.small_light, layout::name_width);
    summary->battles_caption = add_label(
        frame,
        layout::label_x,
        layout::battles_y,
        k_battles_caption,
        f.small_dim,
        layout::caption_width
    );
    summary->battles = add_label(
        frame, layout::count_x, layout::battles_y, "", f.small_green, layout::count_width
    );
    summary->forming_caption = add_label(
        frame,
        layout::label_x,
        layout::forming_y,
        k_forming_caption,
        f.small_dim,
        layout::caption_width
    );
    summary->forming = add_label(
        frame, layout::count_x, layout::forming_y, "", f.small_green, layout::count_width
    );
    summary->arm_logo =
        image_load(screens::kWidgetsGaf, k_logos_sequence, k_logo_arm, screens::kWidgetKey);
    summary->arm_icon = add_icon(frame, layout::arm_icon_y, summary->arm_logo);
    summary->arm_wins =
        add_label(frame, layout::wins_x, layout::battles_y, "", f.small_green, layout::count_width);
    summary->core_logo =
        image_load(screens::kWidgetsGaf, k_logos_sequence, k_logo_core, screens::kWidgetKey);
    summary->core_icon = add_icon(frame, layout::core_icon_y, summary->core_logo);
    summary->core_wins =
        add_label(frame, layout::wins_x, layout::forming_y, "", f.small_green, layout::count_width);
    Window* const all[] = {
        summary->name,
        summary->owner,
        summary->battles_caption,
        summary->battles,
        summary->forming_caption,
        summary->forming,
        summary->arm_icon,
        summary->arm_wins,
        summary->core_icon,
        summary->core_wins,
    };
    show(all, sizeof all / sizeof all[0], false);
    (void)image_surface(frame->base.states.images[k_state_normal], &summary->background);
    if (screens::GalaxyView* galaxy = galaxy_of(summary)) {
        galaxy->territory.planet = screens::kNoPlanet;
        galaxy->territory.shown = false;
        galaxy->territory.contested_shown = false;
    }
    return summary;
}

void battle_summary_release(BattleSummary* summary) {
    image_release(summary->arm_logo);
    image_release(summary->core_logo);
    summary->arm_logo = k_no_handle;
    summary->core_logo = k_no_handle;
}

namespace {

int32_t label_box(Toolkit* toolkit, Window* label) {
    if (label == nullptr || toolkit == nullptr) {
        return layout::label_height;
    }
    const char* text = "";
    if (label->states.count > 1 && label->states.texts != nullptr && label->states.texts[1] >= 0) {
        text = text_get(label->states.texts[1]);
    }
    TextFit fit{};
    toolkit_text_fit(toolkit, label->font, text, &fit);
    int32_t height = fit.line_height + fit.above + fit.below;
    if (height < layout::label_height) {
        height = layout::label_height;
    }
    return height;
}

void place_label(Window* label, int32_t y, int32_t height) {
    if (label == nullptr) {
        return;
    }
    label->y = y;
    label->height = height;
}

/// Stacks the summary from the frame's top. A label grows when its glyphs
/// pass the font's line; the rows below keep the gaps they were built with.
/// Captions and counts stay on their own ink. The picture is not moved.
void stack_labels(BattleSummary* summary) {
    Toolkit* toolkit = summary->frame->owner != nullptr ? summary->frame->owner->toolkit : nullptr;
    const int32_t name_h = label_box(toolkit, summary->name);
    const int32_t owner_h = label_box(toolkit, summary->owner);
    const int32_t battles_caption_h = label_box(toolkit, summary->battles_caption);
    const int32_t battles_h = label_box(toolkit, summary->battles);
    const int32_t arm_wins_h = label_box(toolkit, summary->arm_wins);
    const int32_t forming_caption_h = label_box(toolkit, summary->forming_caption);
    const int32_t forming_h = label_box(toolkit, summary->forming);
    const int32_t core_wins_h = label_box(toolkit, summary->core_wins);
    int32_t battles_row = battles_caption_h;
    if (battles_h > battles_row) {
        battles_row = battles_h;
    }
    if (arm_wins_h > battles_row) {
        battles_row = arm_wins_h;
    }
    int32_t forming_row = forming_caption_h;
    if (forming_h > forming_row) {
        forming_row = forming_h;
    }
    if (core_wins_h > forming_row) {
        forming_row = core_wins_h;
    }
    constexpr int32_t after_name = layout::owner_y - (layout::name_y + layout::label_height);
    constexpr int32_t after_owner = layout::battles_y - (layout::owner_y + layout::label_height);
    constexpr int32_t after_battles =
        layout::forming_y - (layout::battles_y + layout::label_height);
    int32_t y = summary->frame->base.y + layout::name_y;
    place_label(summary->name, y, name_h);
    y += name_h + after_name;
    place_label(summary->owner, y, owner_h);
    y += owner_h + after_owner;
    place_label(summary->battles_caption, y, battles_caption_h);
    place_label(summary->battles, y, battles_h);
    place_label(summary->arm_wins, y, arm_wins_h);
    if (summary->arm_icon != nullptr) {
        summary->arm_icon->y = y + (layout::arm_icon_y - layout::battles_y);
    }
    y += battles_row + after_battles;
    place_label(summary->forming_caption, y, forming_caption_h);
    place_label(summary->forming, y, forming_h);
    place_label(summary->core_wins, y, core_wins_h);
    if (summary->core_icon != nullptr) {
        summary->core_icon->y = y + (layout::core_icon_y - layout::forming_y);
    }
}

} // namespace

void battle_summary_update(BattleSummary* summary) {
    screens::GalaxyView* galaxy = galaxy_of(summary);
    if (galaxy == nullptr)
        return;
    screens::territory_panel_update(&galaxy->territory, galaxy);
    const screens::TerritoryPanel& t = galaxy->territory;
    if (t.shown) {
        screens::lobby_label_set_text(summary->name, t.name);
        if (t.owner != nullptr)
            screens::lobby_label_set_text(summary->owner, t.owner);
        screens::lobby_label_set_text(summary->battles, t.in_progress.text);
        screens::lobby_label_set_text(summary->forming, t.forming.text);
    }
    Window* const first[] = {
        summary->name,
        summary->owner,
        summary->battles_caption,
        summary->battles,
        summary->forming_caption,
        summary->forming,
    };
    show(first, sizeof first / sizeof first[0], t.shown);
    if (t.contested_shown) {
        screens::lobby_label_set_text(summary->arm_wins, t.arm_wins.text);
        screens::lobby_label_set_text(summary->core_wins, t.core_wins.text);
    }
    Window* const contested[] = {
        summary->arm_icon, summary->arm_wins, summary->core_icon, summary->core_wins
    };
    show(contested, sizeof contested / sizeof contested[0], t.contested_shown);
    stack_labels(summary);
}

void battle_summary_draw(BattleSummary* summary) {
    Surface* background = &summary->background;
    if (background->pixels == nullptr)
        return;
    (void)present::fill_clipped_rect(background, layout::picture, k_picture_fill);
    const screens::GalaxyView* galaxy = galaxy_of(summary);
    GalaxyGraphics* graphics = summary->frame->owner->galaxy_graphics.get();
    if (galaxy == nullptr || graphics == nullptr)
        return;
    const int32_t planet = galaxy->territory.planet;
    if (planet == screens::kNoPlanet || planet < 0 ||
        planet >= static_cast<int32_t>(galaxy->planet_count))
        return;
    const auto index = static_cast<std::size_t>(planet);
    const formats::gaf::Frame* frame = formats::gaf::frame_at(
        graphics->planet_sequences[index][k_picture_size], galaxy->planets[index].frame
    );
    const Rect32 clip = present::surface_clip(*background);
    present::set_surface_clip(*background, layout::picture);
    galaxy_draw_frame(*graphics, background, frame, layout::picture_x, layout::picture_y);
    present::set_surface_clip(*background, clip);
}

} // namespace oa::boneyards::console
