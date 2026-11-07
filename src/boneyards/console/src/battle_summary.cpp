// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Main Frame's planet summary beside the Recon button: the selected
// planet's picture painted into the frame's background, its name and owner,
// the Battles and Forming counts and, for a contested planet, each side's
// wins beside its logo.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "console_state.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
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
// "Núcleo 117" in the install's small green face is 43 pixels. The side
// words start so that line's last pixel is the picture's right edge.
constexpr int32_t side_line_pixels = 43;
constexpr int32_t side_words_x = picture.x2 - (side_line_pixels - 1);
// The battles and forming counts move left by one pixel while those words
// show, so their box ends on the pixel before the words.
constexpr int32_t count_x_beside_words = side_words_x - count_width;
static_assert(side_words_x + side_line_pixels - 1 == picture.x2);
static_assert(count_x_beside_words == count_x - 1);
static_assert(count_x_beside_words > label_x + caption_width);
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
// Colour-vision mode last applied to the side icons. Off means the loaded icons are unchanged.
// 0xFF is not a mode: the next summary applies the mode to the icons it loads.
uint8_t g_logo_mode = 0;

void drop_side_icon_colours() {
    g_logo_mode = 0xFF;
}

void recolour_index(Handle image, uint8_t from, uint8_t to) {
    if (image < 0 || from == to)
        return;
    Surface surface{};
    if (!image_surface(image, &surface) || surface.pixels == nullptr || surface.width <= 0 ||
        surface.height <= 0)
        return;
    for (int32_t row = 0; row < surface.height; ++row) {
        uint8_t* pixels = surface.pixels + static_cast<std::ptrdiff_t>(row) * surface.pitch;
        for (int32_t column = 0; column < surface.width; ++column) {
            if (pixels[column] == from)
                pixels[column] = to;
        }
    }
}

void replace_logo(Handle& slot, Window* icon, int32_t frame) {
    if (slot >= 0)
        image_release(slot);
    slot = image_load(screens::kWidgetsGaf, k_logos_sequence, frame, screens::kWidgetKey);
    if (icon != nullptr && slot >= 0)
        window_set_state_image(icon, k_state_normal, slot);
}

// Reloads the side icons when the colour-vision mode changes, and recolours only each logo's body.
// Off reloads the stored pictures and leaves them alone.
void refresh_side_icons(BattleSummary* summary) {
    const auto now = static_cast<uint8_t>(present::colour_vision::active_mode());
    if (summary == nullptr || now == g_logo_mode)
        return;
    replace_logo(summary->arm_logo, summary->arm_icon, k_logo_arm);
    replace_logo(summary->core_logo, summary->core_icon, k_logo_core);
    if (now != static_cast<uint8_t>(present::colour_vision::Mode::off)) {
        const screens::SideIconColours bodies =
            screens::side_icon_colours(screens::boneyards_palette());
        recolour_index(
            summary->arm_logo,
            static_cast<uint8_t>(screens::side_icon_body::arm),
            static_cast<uint8_t>(bodies.arm)
        );
        recolour_index(
            summary->core_logo,
            static_cast<uint8_t>(screens::side_icon_body::core),
            static_cast<uint8_t>(bodies.core)
        );
    }
    g_logo_mode = now;
}

void follow_role_font(Window* label, Handle font) {
    if (label != nullptr && font >= 0 && label->font != font)
        window_set_font_handle(label, font);
}

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
    refresh_side_icons(summary);
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
    // The next summary loads its own icons. A mode still on must recolour those, not skip them.
    drop_side_icon_colours();
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

void set_label_text(Window* label, const char* line) {
    if (label == nullptr || line == nullptr)
        return;
    const char* current = "";
    if (label->states.count > 1 && label->states.texts != nullptr && label->states.texts[1] >= 0)
        current = text_get(label->states.texts[1]);
    if (current != nullptr && std::strcmp(current, line) == 0)
        return;
    screens::lobby_label_set_text(label, line);
}

// The wins line starts at the count. While shape cues are in effect it starts
// where a 43-pixel line still ends on the panel's inner edge. The logo hides.
void place_wins_label(Window* label, bool take_logo) {
    if (label == nullptr || label->parent == nullptr)
        return;
    const int32_t x = label->parent->x + (take_logo ? layout::side_words_x : layout::wins_x);
    if (label->x != x)
        label->x = x;
}

// The battles and forming counts keep their width. They step one pixel left
// while the side words need the pixel their box used to end on.
void place_count_label(Window* label, bool beside_words) {
    if (label == nullptr || label->parent == nullptr)
        return;
    const int32_t x =
        label->parent->x + (beside_words ? layout::count_x_beside_words : layout::count_x);
    if (label->x != x)
        label->x = x;
}

// The wins label is fifteen pixels until shape cues put the side's name in front.
// It grows to the measured line. Its last pixel stays on the picture's right edge,
// the panel's inner edge, or to the left of it.
void fit_wins_label(const BattleSummary* summary, Window* label, const char* line, bool widen) {
    if (label == nullptr)
        return;
    int32_t width = layout::count_width;
    if (widen && line != nullptr && line[0] != '\0') {
        int32_t measured = 0;
        Toolkit* toolkit = summary->frame != nullptr && summary->frame->owner != nullptr
                               ? summary->frame->owner->toolkit
                               : nullptr;
        if (toolkit != nullptr) {
            int32_t height = 0;
            toolkit_measure_text(toolkit, label->font, line, &measured, &height);
        } else {
            measured = 40;
        }
        if (measured > width)
            width = measured;
        const int32_t room = layout::picture.x2 - label->x + 1;
        if (width > room)
            width = room;
    }
    if (label->width != width)
        label->width = width;
}

// The side logos show with a contested planet, and hide while shape cues put the side's name
// where the logo was.
void settle_side_logos(BattleSummary* summary) {
    if (summary == nullptr)
        return;
    const screens::GalaxyView* galaxy = galaxy_of(summary);
    const bool contested = galaxy != nullptr && galaxy->territory.contested_shown;
    const bool logos = contested && !present::colour_vision::shape_cues_active();
    if (summary->arm_icon != nullptr)
        summary->arm_icon->visible = logos ? 1 : 0;
    if (summary->core_icon != nullptr)
        summary->core_icon->visible = logos ? 1 : 0;
}

// Writes each side's win count, with the side's name while shape cues are in effect.
// A planet that is not contested keeps the text it already shows.
void show_side_counts(BattleSummary* summary) {
    if (summary == nullptr)
        return;
    const screens::GalaxyView* galaxy = galaxy_of(summary);
    const bool contested = galaxy != nullptr && galaxy->territory.contested_shown;
    const bool widen = contested && present::colour_vision::shape_cues_active();
    place_count_label(summary->battles, widen);
    place_count_label(summary->forming, widen);
    place_wins_label(summary->arm_wins, widen);
    place_wins_label(summary->core_wins, widen);
    if (!contested) {
        fit_wins_label(summary, summary->arm_wins, nullptr, false);
        fit_wins_label(summary, summary->core_wins, nullptr, false);
        return;
    }
    char arm_line[48];
    char core_line[48];
    summary_side_count(arm_line, sizeof arm_line, true, galaxy->territory.arm_wins.text);
    summary_side_count(core_line, sizeof core_line, false, galaxy->territory.core_wins.text);
    set_label_text(summary->arm_wins, arm_line);
    set_label_text(summary->core_wins, core_line);
    fit_wins_label(summary, summary->arm_wins, arm_line, widen);
    fit_wins_label(summary, summary->core_wins, core_line, widen);
}

} // namespace

const char* summary_win_icon_label(bool arm) {
    return arm ? k_arm_wins_label : k_core_wins_label;
}

void summary_win_icon_labels(const int32_t* tops, std::size_t count, const char** labels) {
    if (tops == nullptr || labels == nullptr)
        return;
    int32_t top = 0;
    int32_t bottom = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (index == 0) {
            top = tops[index];
            bottom = tops[index];
        } else {
            if (tops[index] < top)
                top = tops[index];
            if (tops[index] > bottom)
                bottom = tops[index];
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        const bool lower = count > 1 && tops[index] == bottom && bottom != top;
        labels[index] = summary_win_icon_label(!lower);
    }
}

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
    show_side_counts(summary);
    Window* const contested[] = {
        summary->arm_icon, summary->arm_wins, summary->core_icon, summary->core_wins
    };
    show(contested, sizeof contested / sizeof contested[0], t.contested_shown);
    settle_side_logos(summary);
    stack_labels(summary);
    refresh_side_icons(summary);
    if (summary->frame != nullptr && summary->frame->owner != nullptr) {
        const Handle role = summary->frame->owner->system.hat_fonts.small_green;
        follow_role_font(summary->name, role);
        follow_role_font(summary->battles, role);
        follow_role_font(summary->forming, role);
        follow_role_font(summary->arm_wins, role);
        follow_role_font(summary->core_wins, role);
    }
}

void summary_side_count(char* out, std::size_t capacity, bool arm, const char* count) {
    if (out == nullptr || capacity == 0)
        return;
    const char* digits = count != nullptr ? count : "";
    if (!present::colour_vision::shape_cues_active()) {
        std::snprintf(out, capacity, "%s", digits);
        return;
    }
    const char* name = shown_text(arm ? "Arm" : "Core");
    if (name == nullptr || name[0] == '\0')
        name = arm ? "Arm" : "Core";
    if (digits[0] == '\0')
        std::snprintf(out, capacity, "%s", name);
    else
        std::snprintf(out, capacity, "%s %s", name, digits);
}

void battle_summary_draw(BattleSummary* summary) {
    show_side_counts(summary);
    settle_side_logos(summary);
    refresh_side_icons(summary);
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
