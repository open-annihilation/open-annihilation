// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Declared rows: a page of every kind laid out, drawn, focused and changed
// from a table, and rows shaped as the settings dialog's Controls, Graphics
// and Developer sections placed and scrolled as the dialog places and
// scrolls them.
//
// The column is the settings dialog's: from column 158 to 467, the first
// row's rule at row 54, the first row's control 13, and the view under the
// heading {158, 54, 309, 236}.
//
// The synthetic font's glyph for byte b is 2 + b % 4 columns by 8 rows, set
// where (x + y + b) % 3 is not 0, as in the other kit tests.

#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/rows.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;

// ---- The test model ----

/// How the gyro moves the pointer: an enum behind a drop-down's stepper.
enum class Gyro : uint8_t { off, always, touched };
/// How much is drawn: an enum behind a strip's stepper.
enum class Detail : uint8_t { low, medium, high };

/// A page's model: what its rows are bound to.
struct Sample {
    bool vertical_sync{};               ///< a switch's field
    bool escape_opens_menu{};           ///< a switch read and set through functions
    Gyro gyro{};                        ///< a drop-down's choice, behind a stepper
    Detail detail{};                    ///< a strip's level, behind a stepper
    int32_t detail_offered{2};          ///< the levels a player may choose from the left
    int32_t zoom{};                     ///< a drop-down's item, with captions
    int32_t volume{};                   ///< a slider's stop, 0 to 10
    int32_t anti_aliasing{};            ///< a strip of five levels
    std::string player_name{};          ///< a text field's text
    std::string install_id{"7f3a90d2"}; ///< a value with a button
    std::string folder{"Documents/Open Annihilation"}; ///< a text row's value
    bool in_game{};                                    ///< a game is running: some rows lock
};

constexpr std::array<std::string_view, 3> kGyroCaptions{"Off", "Always", "While touched"};
constexpr std::array<std::string_view, 3> kDetailCaptions{"Low", "Medium", "High"};
constexpr std::array<std::string_view, 3> kZoomCaptions{"Near", "Far", "Farthest"};

/// Returns three: the gyro's choices and the strip's levels.
int32_t three(const Sample&) {
    return 3;
}

/// Returns the gyro's choice, from 0.
int32_t gyro_of(const Sample& sample) {
    return static_cast<int32_t>(sample.gyro);
}

/// Sets the gyro's choice, from 0.
void set_gyro(Sample& sample, int32_t chosen) {
    sample.gyro = static_cast<Gyro>(chosen);
}

/// Returns a gyro choice's words, in English.
std::string gyro_caption(const Sample&, int32_t at) {
    return at >= 0 && at < 3 ? std::string(kGyroCaptions[static_cast<std::size_t>(at)])
                             : std::string();
}

/// Returns the detail's level, from 0.
int32_t detail_of(const Sample& sample) {
    return static_cast<int32_t>(sample.detail);
}

/// Sets the detail's level, from 0.
void set_detail(Sample& sample, int32_t chosen) {
    sample.detail = static_cast<Detail>(chosen);
}

/// Returns a detail level's words, in English.
std::string detail_caption(const Sample&, int32_t at) {
    return at >= 0 && at < 3 ? std::string(kDetailCaptions[static_cast<std::size_t>(at)])
                             : std::string();
}

/// Returns the detail levels a player may choose, from the left.
int32_t detail_offered(const Sample& sample) {
    return sample.detail_offered;
}

/// Returns eleven: the volume's stops, 0 to 10.
int32_t eleven(const Sample&) {
    return 11;
}

/// Returns the volume's stop.
int32_t volume_of(const Sample& sample) {
    return sample.volume;
}

/// Sets the volume's stop.
void set_volume(Sample& sample, int32_t stop) {
    sample.volume = stop;
}

/// Returns a volume stop's value text: ten percent a stop.
std::string volume_caption(const Sample&, int32_t stop) {
    return std::to_string(stop * 10) + "%";
}

/// Returns five: the anti-aliasing strip's levels.
int32_t five(const Sample&) {
    return 5;
}

/// Returns the anti-aliasing level, from 0.
int32_t anti_aliasing_of(const Sample& sample) {
    return sample.anti_aliasing;
}

/// Sets the anti-aliasing level, from 0.
void set_anti_aliasing(Sample& sample, int32_t level) {
    sample.anti_aliasing = level;
}

/// Returns an anti-aliasing level's words: Off, then 2x, 4x and on.
std::string anti_aliasing_caption(const Sample&, int32_t level) {
    return level == 0 ? std::string("Off") : std::to_string(1 << level) + "x";
}

/// Tells whether Escape opens the menu.
bool escape_opens_menu(const Sample& sample) {
    return sample.escape_opens_menu;
}

/// Sets whether Escape opens the menu.
void set_escape_opens_menu(Sample& sample, bool on) {
    sample.escape_opens_menu = on;
}

/// Returns the install ID, the value its row shows.
std::string install_id_text(const Sample& sample) {
    return sample.install_id;
}

/// Tells whether there is an install ID to reset.
bool has_install_id(const Sample& sample) {
    return !sample.install_id.empty();
}

/// Returns the folder a text row shows.
std::string folder_text(const Sample& sample) {
    return sample.folder;
}

/// Returns the lock's text while a game runs, and nothing otherwise.
std::string_view in_game_lock(const Sample& sample) {
    return sample.in_game ? "Not during a game" : "";
}

/// Tells that a control is never enabled.
bool never(const Sample&) {
    return false;
}

constexpr kit::Stepper<Sample> kGyro{three, gyro_of, set_gyro, gyro_caption, nullptr};
constexpr kit::Stepper<Sample> kDetail{
    three, detail_of, set_detail, detail_caption, detail_offered
};
constexpr kit::Stepper<Sample> kVolume{eleven, volume_of, set_volume, volume_caption, nullptr};
constexpr kit::Stepper<Sample> kAntiAliasing{
    five, anti_aliasing_of, set_anti_aliasing, anti_aliasing_caption, nullptr
};

constexpr kit::ActionId kReset = 7;
constexpr kit::ActionId kOpenSaves = 1;
constexpr kit::ActionId kOpenScreenshots = 2;
constexpr kit::ActionId kOpenMods = 3;
constexpr kit::ActionId kMoreMaps = 9;

/// Returns a spec locked while a game runs.
constexpr kit::RowSpec<Sample> locked_in_game(kit::RowSpec<Sample> spec) {
    spec.lock = in_game_lock;
    return spec;
}

/// Returns a spec whose hint lines are its status.
constexpr kit::RowSpec<Sample> status(kit::RowSpec<Sample> spec) {
    spec.hint_is_status = true;
    return spec;
}

/// Returns a drop-down spec with the wide field.
constexpr kit::RowSpec<Sample> wide(kit::RowSpec<Sample> spec) {
    spec.control_width = kit::compact_metrics.wide_choice_width;
    return spec;
}

/// Returns a spec enabled only while the model has an install ID.
constexpr kit::RowSpec<Sample> while_an_id(kit::RowSpec<Sample> spec) {
    spec.enabled = has_install_id;
    return spec;
}

/// The test page: one row of each kind, in RowKind's order.
constexpr std::array<kit::RowSpec<Sample>, 9> kEveryKind{
    locked_in_game(
        kit::toggle(
            "vertical-sync", "Vertical sync", &Sample::vertical_sync, {"Waits for the display."}
        )
    ),
    wide(
        kit::choice(
            "gyro",
            "Gyro pointer",
            kGyro,
            {"Moves the pointer as the pad turns.", "Off leaves the pointer to the sticks."}
        )
    ),
    kit::slider("volume", "Volume", kVolume, {"How loud the game plays."}),
    kit::levels(
        "detail", "Detail", kDetail, 34, {"How much is drawn.", "High needs a faster machine."}
    ),
    while_an_id(
        kit::value_and_button(
            "install-id",
            "",
            install_id_text,
            "RESET",
            "reset",
            kReset,
            {"A random number for downloads.", "RESET makes a new one."}
        )
    ),
    kit::buttons<Sample>(
        "your-files",
        "Your files",
        {"SAVES", "SCREENSHOTS", "MODS"},
        {"saves", "screenshots", "mods"},
        {kOpenSaves, kOpenScreenshots, kOpenMods},
        {"Your saved games, screenshots and mods.", "Each button opens its folder."}
    ),
    kit::text_field(
        "player-name", "Player name", &Sample::player_name, {"Shown to other players."}
    ),
    kit::link<Sample>("more-maps", "MORE MAPS...", kMoreMaps),
    kit::text("location", "Where they are", folder_text),
};

static_assert(
    std::all_of(
        kEveryKind.begin(),
        kEveryKind.end(),
        [](const kit::RowSpec<Sample>& spec) { return kit::row_word(spec.id); }
    ),
    "every row of the table has an id of one word"
);
static_assert(!kit::row_word("") && !kit::row_word("Vertical sync") && !kit::row_word("a.b"));

// ---- Helpers ----

/// The settings dialog's column, from the first row's rule.
kit::RowPlacement dialog_column(bool tall, int32_t row_padding = kit::compact_metrics.row_padding) {
    return {158, 467, 54, row_padding, tall, 13};
}

/// The view a section's rows scroll in, under its heading.
constexpr kit::Rect kView{158, 54, 309, 236};

/// Returns the views of a table's rows over a model.
template <class Model, std::size_t Count>
std::vector<kit::RowView> views_of(
    const std::array<kit::RowSpec<Model>, Count>& specs,
    const Model& model,
    const kit::ShownText& shown = {}
) {
    std::vector<kit::RowView> views;
    for (const kit::RowSpec<Model>& spec : specs)
        views.push_back(kit::view_of(spec, model, shown));
    return views;
}

/// Places the page of every kind in the dialog's column.
kit::PlacedRows place_every_kind(const Sample& sample, bool tall = false) {
    return kit::place_rows(views_of(kEveryKind, sample), dialog_column(tall));
}

/// Tells whether two rectangles are the same.
bool same(const kit::Rect& a, const kit::Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// Checks a placed part's rectangle, and says which when it differs.
void check_rect(
    const char* page, std::size_t row, const char* part, const kit::Rect& got, const kit::Rect& want
) {
    if (same(got, want))
        return;
    std::fprintf(
        stderr,
        "%s, row %zu, %s: {%d, %d, %d, %d}, expected {%d, %d, %d, %d}\n",
        page,
        row,
        part,
        got.x,
        got.y,
        got.width,
        got.height,
        want.x,
        want.y,
        want.width,
        want.height
    );
    OA_CHECK(false);
}

/// One row as it must be placed. Every hint line is {158, row, 309, 12}.
struct Expected {
    int32_t top{};
    int32_t height{};
    kit::Rect label{};
    kit::Rect lock{};
    std::vector<int32_t> hint_rows{};
    kit::Rect control{};
    kit::Rect value{};
};

/// Checks a column of placed rows against the rows it must be.
void placed_as(
    const char* page, const kit::PlacedRows& placed, std::span<const Expected> rows, int32_t bottom
) {
    OA_CHECK(placed.rows.size() == rows.size());
    if (placed.bottom != bottom) {
        std::fprintf(stderr, "%s: bottom %d, expected %d\n", page, placed.bottom, bottom);
        OA_CHECK(false);
    }
    for (std::size_t at = 0; at < std::min(placed.rows.size(), rows.size()); ++at) {
        const kit::PlacedRow& got = placed.rows[at];
        const Expected& want = rows[at];
        if (got.top != want.top || got.height != want.height) {
            std::fprintf(
                stderr,
                "%s, row %zu: top %d height %d, expected top %d height %d\n",
                page,
                at,
                got.top,
                got.height,
                want.top,
                want.height
            );
            OA_CHECK(false);
        }
        OA_CHECK(got.control == 13 + static_cast<int32_t>(at));
        check_rect(page, at, "label", got.label, want.label);
        check_rect(page, at, "lock", got.lock_area, want.lock);
        check_rect(page, at, "control", got.control_area, want.control);
        check_rect(page, at, "value", got.value, want.value);
        OA_CHECK(got.hints.size() == want.hint_rows.size());
        for (std::size_t line = 0; line < std::min(got.hints.size(), want.hint_rows.size()); ++line)
            check_rect(page, at, "hint", got.hints[line], {158, want.hint_rows[line], 309, 12});
    }
}

// ---- Placement ----

/// The rows of Controls: a switch, two drop-downs, a strip of three levels
/// 34 wide and two switches, with Controls' hint lines. The lockable rows
/// lock while a game runs, and Escape opens the menu's hint is its status.
std::array<kit::RowSpec<Sample>, 6> controls_rows() {
    return {
        locked_in_game(
            kit::toggle(
                "wheel-zoom",
                "Mouse wheel zoom",
                &Sample::vertical_sync,
                {"The wheel zooms the view."}
            )
        ),
        locked_in_game(
            kit::choice(
                "max-zoom-out",
                "Maximum zoom out",
                &Sample::zoom,
                kZoomCaptions,
                {"How far the view zooms out.", "Farther shows more of the map."}
            )
        ),
        kit::choice(
            "max-zoom-in",
            "Maximum zoom in",
            kGyro,
            {"How far the view zooms in.", "Nearer shows units larger."}
        ),
        locked_in_game(
            kit::levels(
                "view-past-map-edge",
                "View past the map's edge",
                kDetail,
                34,
                {"How far the view goes past the edges.", "Off keeps the map's edges."}
            )
        ),
        status(locked_in_game(
            kit::toggle(
                "escape-opens-menu",
                "Escape opens the menu",
                escape_opens_menu,
                set_escape_opens_menu,
                {"Escape opens the game's menu.", "Off leaves Escape to the game."}
            )
        )),
        kit::toggle("switch-alt", "Switch Alt", &Sample::vertical_sync, {"Swaps the Alt keys."}),
    };
}

/// The first three rows of Graphics: a slider, a strip of five levels 23
/// wide and a slider, all locked while a game runs; the strip's and the
/// last slider's hints are their status.
std::array<kit::RowSpec<Sample>, 3> graphics_rows() {
    return {
        locked_in_game(
            kit::slider(
                "max-frame-rate", "Maximum frame rate", kVolume, {"The most frames drawn a second."}
            )
        ),
        status(locked_in_game(
            kit::levels(
                "anti-aliasing",
                "Enhanced anti-aliasing",
                kAntiAliasing,
                23,
                {"Smooths the edges of units.", "Higher levels cost more."}
            )
        )),
        status(locked_in_game(
            kit::slider(
                "screen-size",
                "Screen size",
                kVolume,
                {"The game's picture size.", "The window keeps its own."}
            )
        )),
    };
}

// The expected rows below were computed once with the settings dialog's own
// placement, geometry::place_rows in src/ui/engine-settings (dialog.cpp), at
// the commit this test was written on (4747c9b3), for Controls' rows and for
// Graphics' first three, as the dialog has them and through a check's own
// section (SectionHooks) that locks them and makes their hints their status:
// with the game's fonts and with the modern fonts' taller hints, locked and
// not. The kit must place the rows its specs declare in exactly these places.

/// Controls, unlocked, with the game's fonts.
const std::vector<Expected> kControlsPlain{
    {54, 47, {158, 63, 259, 16}, {}, {81}, {425, 63, 42, 16}, {}},
    {101, 79, {158, 110, 309, 16}, {}, {128, 140}, {158, 156, 200, 16}, {}},
    {180, 79, {158, 189, 309, 16}, {}, {207, 219}, {158, 235, 200, 16}, {}},
    {259, 59, {158, 268, 197, 16}, {}, {286, 298}, {363, 268, 104, 16}, {}},
    {318, 59, {158, 327, 259, 16}, {}, {345, 357}, {425, 327, 42, 16}, {}},
    {377, 47, {158, 386, 259, 16}, {}, {404}, {425, 386, 42, 16}, {}},
};

/// Controls, unlocked, with the modern fonts' taller hints.
const std::vector<Expected> kControlsTall{
    {54, 47, {158, 63, 259, 16}, {}, {81}, {425, 63, 42, 16}, {}},
    {101, 82, {158, 110, 309, 16}, {}, {128, 143}, {158, 159, 200, 16}, {}},
    {183, 82, {158, 192, 309, 16}, {}, {210, 225}, {158, 241, 200, 16}, {}},
    {265, 62, {158, 274, 197, 16}, {}, {292, 307}, {363, 274, 104, 16}, {}},
    {327, 62, {158, 336, 259, 16}, {}, {354, 369}, {425, 336, 42, 16}, {}},
    {389, 47, {158, 398, 259, 16}, {}, {416}, {425, 398, 42, 16}, {}},
};

/// Controls, its first, second, fourth and fifth rows locked, the fifth's
/// hint its status, with the game's fonts. A switch or a strip whose label,
/// lock and control do not fit keeps the control and puts the lock at the
/// start of the line under the label. A drop-down's lock stays on the label
/// line, at its right.
const std::vector<Expected> kControlsLocked{
    {54, 65, {158, 63, 259, 16}, {158, 81, 148, 16}, {99}, {425, 63, 42, 16}, {}},
    {119, 79, {158, 128, 153, 16}, {319, 128, 148, 16}, {146, 158}, {158, 174, 200, 16}, {}},
    {198, 79, {158, 207, 309, 16}, {}, {225, 237}, {158, 253, 200, 16}, {}},
    {277, 77, {158, 286, 197, 16}, {158, 304, 148, 16}, {322, 334}, {363, 286, 104, 16}, {}},
    {354, 77, {158, 363, 259, 16}, {158, 381, 148, 16}, {399, 411}, {425, 363, 42, 16}, {}},
    {431, 47, {158, 440, 259, 16}, {}, {458}, {425, 440, 42, 16}, {}},
};

/// Controls, locked as above, with the modern fonts' taller hints.
const std::vector<Expected> kControlsLockedTall{
    {54, 65, {158, 63, 259, 16}, {158, 81, 148, 16}, {99}, {425, 63, 42, 16}, {}},
    {119, 82, {158, 128, 153, 16}, {319, 128, 148, 16}, {146, 161}, {158, 177, 200, 16}, {}},
    {201, 82, {158, 210, 309, 16}, {}, {228, 243}, {158, 259, 200, 16}, {}},
    {283, 80, {158, 292, 197, 16}, {158, 310, 148, 16}, {328, 343}, {363, 292, 104, 16}, {}},
    {363, 80, {158, 372, 259, 16}, {158, 390, 148, 16}, {408, 423}, {425, 372, 42, 16}, {}},
    {443, 47, {158, 452, 259, 16}, {}, {470}, {425, 452, 42, 16}, {}},
};

/// Graphics' first three rows, unlocked, with the game's fonts. A status
/// hint changes nothing while its row is unlocked.
const std::vector<Expected> kGraphicsPlain{
    {54, 65, {158, 63, 309, 16}, {}, {81}, {158, 97, 189, 14}, {357, 97, 110, 14}},
    {119, 59, {158, 128, 184, 16}, {}, {146, 158}, {350, 128, 117, 16}, {}},
    {178, 77, {158, 187, 309, 16}, {}, {205, 217}, {158, 233, 189, 14}, {357, 233, 110, 14}},
};

/// Graphics' first three rows, unlocked, with the modern fonts' taller hints.
const std::vector<Expected> kGraphicsTall{
    {54, 65, {158, 63, 309, 16}, {}, {81}, {158, 97, 189, 14}, {357, 97, 110, 14}},
    {119, 62, {158, 128, 184, 16}, {}, {146, 161}, {350, 128, 117, 16}, {}},
    {181, 80, {158, 190, 309, 16}, {}, {208, 223}, {158, 239, 189, 14}, {357, 239, 110, 14}},
};

/// Graphics' first three rows, locked, with the game's fonts: the strip,
/// whose hint is its status, stays, faded, and its lock drops under the
/// label because the three do not fit; the sliders' labels fit beside the
/// lock, so it stays at the label line's right.
const std::vector<Expected> kGraphicsLocked{
    {54, 65, {158, 63, 153, 16}, {319, 63, 148, 16}, {81}, {158, 97, 189, 14}, {357, 97, 110, 14}},
    {119, 77, {158, 128, 184, 16}, {158, 146, 148, 16}, {164, 176}, {350, 128, 117, 16}, {}},
    {196,
     77,
     {158, 205, 153, 16},
     {319, 205, 148, 16},
     {223, 235},
     {158, 251, 189, 14},
     {357, 251, 110, 14}},
};

/// Graphics' first three rows, locked, with the modern fonts' taller hints.
const std::vector<Expected> kGraphicsLockedTall{
    {54, 65, {158, 63, 153, 16}, {319, 63, 148, 16}, {81}, {158, 97, 189, 14}, {357, 97, 110, 14}},
    {119, 80, {158, 128, 184, 16}, {158, 146, 148, 16}, {164, 179}, {350, 128, 117, 16}, {}},
    {199,
     80,
     {158, 208, 153, 16},
     {319, 208, 148, 16},
     {226, 241},
     {158, 257, 189, 14},
     {357, 257, 110, 14}},
};

/// Developer's own two switches, half a row's padding apart.
const std::vector<Expected> kDeveloper{
    {54, 39, {158, 59, 259, 16}, {}, {77}, {425, 59, 42, 16}, {}},
    {93, 39, {158, 98, 259, 16}, {}, {116}, {425, 98, 42, 16}, {}},
};

/// Checks rows shaped as Controls' against the dialog's placement.
void rows_shaped_as_controls_lie_where_the_dialog_places_them() {
    const auto specs = controls_rows();
    Sample sample;
    placed_as(
        "Controls",
        kit::place_rows(views_of(specs, sample), dialog_column(false)),
        kControlsPlain,
        424
    );
    placed_as(
        "Controls, tall",
        kit::place_rows(views_of(specs, sample), dialog_column(true)),
        kControlsTall,
        436
    );
    sample.in_game = true;
    placed_as(
        "Controls, locked",
        kit::place_rows(views_of(specs, sample), dialog_column(false)),
        kControlsLocked,
        478
    );
    placed_as(
        "Controls, locked, tall",
        kit::place_rows(views_of(specs, sample), dialog_column(true)),
        kControlsLockedTall,
        490
    );
}

/// Checks rows shaped as Graphics' first three against the dialog's placement.
void rows_shaped_as_graphics_lie_where_the_dialog_places_them() {
    const auto specs = graphics_rows();
    Sample sample;
    placed_as(
        "Graphics",
        kit::place_rows(views_of(specs, sample), dialog_column(false)),
        kGraphicsPlain,
        255
    );
    placed_as(
        "Graphics, tall",
        kit::place_rows(views_of(specs, sample), dialog_column(true)),
        kGraphicsTall,
        261
    );
    sample.in_game = true;
    placed_as(
        "Graphics, locked",
        kit::place_rows(views_of(specs, sample), dialog_column(false)),
        kGraphicsLocked,
        273
    );
    placed_as(
        "Graphics, locked, tall",
        kit::place_rows(views_of(specs, sample), dialog_column(true)),
        kGraphicsLockedTall,
        279
    );
}

/// A locked slider, drop-down or text row whose label does not fit beside
/// the lock puts the lock at the start of the line under the label, before
/// the hint, and keeps the label the whole line. A label that fits keeps
/// the lock on the label line, at its right.
void a_lock_drops_under_a_label_it_does_not_fit_beside() {
    Sample sample;
    sample.in_game = true;
    const std::array<kit::RowSpec<Sample>, 2> sliders{
        locked_in_game(
            kit::slider(
                "mex-snap",
                "Metal extractor snap radius",
                kVolume,
                {"Cells a metal extractor snaps to."}
            )
        ),
        locked_in_game(
            kit::slider("screen-size", "Screen size", kVolume, {"The game's picture size."})
        ),
    };
    // Twenty-seven characters at 7 columns do not fit beside a lock of 148.
    // "Screen size" does, so its lock stays on the label line.
    const std::vector<Expected> slider_rows{
        {54,
         83,
         {158, 63, 309, 16},
         {158, 81, 148, 16},
         {99},
         {158, 115, 189, 14},
         {357, 115, 110, 14}},
        {137,
         65,
         {158, 146, 153, 16},
         {319, 146, 148, 16},
         {164},
         {158, 180, 189, 14},
         {357, 180, 110, 14}},
    };
    const kit::PlacedRows placed = kit::place_rows(views_of(sliders, sample), dialog_column(false));
    placed_as("Slider locks", placed, slider_rows, 202);
    OA_CHECK(placed.rows[0].label.width > kit::estimated_width(placed.rows[0].view.label));
    OA_CHECK(placed.rows[1].lock_area.y == placed.rows[1].label.y);
    kit::DisplayList list;
    kit::add_rows(list, placed, {});
    bool saw_dropped = false;
    for (const kit::Item& item : list.items) {
        if (item.role != kit::Role::lock || !same(item.rect, placed.rows[0].lock_area))
            continue;
        saw_dropped = true;
        OA_CHECK(item.text == "Not during a game");
    }
    OA_CHECK(saw_dropped);

    const std::array<kit::RowSpec<Sample>, 1> choice{
        locked_in_game(
            kit::choice(
                "gyro-pointer",
                "Metal extractor snap radius",
                &Sample::zoom,
                kZoomCaptions,
                {"One line."}
            )
        ),
    };
    const std::vector<Expected> choice_rows{
        {54, 85, {158, 63, 309, 16}, {158, 81, 148, 16}, {99}, {158, 115, 200, 16}, {}},
    };
    placed_as(
        "Drop-down lock",
        kit::place_rows(views_of(choice, sample), dialog_column(false)),
        choice_rows,
        139
    );

    const std::array<kit::RowSpec<Sample>, 2> texts{
        locked_in_game(kit::text("location", "Metal extractor snap radius", folder_text)),
        locked_in_game(kit::text("where", "Where", folder_text)),
    };
    const std::vector<Expected> text_rows{
        {54, 53, {158, 63, 309, 16}, {158, 81, 148, 16}, {}, {}, {158, 63, 309, 16}},
        {107, 35, {158, 116, 153, 16}, {319, 116, 148, 16}, {}, {}, {158, 116, 153, 16}},
    };
    placed_as(
        "Text locks", kit::place_rows(views_of(texts, sample), dialog_column(false)), text_rows, 142
    );
}

/// Checks Developer's own two switches, half a row's padding apart.
void developer_rows_lie_closer() {
    const std::array<kit::RowSpec<Sample>, 2> specs{
        kit::toggle("developer-mode", "Enable Developer Mode", &Sample::vertical_sync, {"Hacks."}),
        kit::toggle(
            "frame-stats", "Show performance statistics", &Sample::vertical_sync, {"Stats."}
        ),
    };
    placed_as(
        "Developer",
        kit::place_rows(
            views_of(specs, Sample{}), dialog_column(false, kit::compact_metrics.row_padding / 2)
        ),
        kDeveloper,
        132
    );
}

/// The page of every kind, placed by the same rules. No row of today's
/// dialog is a value and a button with its value, a text field, a link or a
/// text row with a value, so these places follow the rules alone: the
/// button 76 wide at the label line's right, its value in the label's box;
/// the text field on a line of its own slider_gap under the hint, 16 high
/// and 200 wide; the link alone on its label line; the text row's value in
/// its label's box.
void every_kind_lies_by_the_rows_rules() {
    const std::vector<Expected> rows{
        {54, 47, {158, 63, 259, 16}, {}, {81}, {425, 63, 42, 16}, {}},
        {101, 79, {158, 110, 309, 16}, {}, {128, 140}, {158, 156, 248, 16}, {}},
        {180, 65, {158, 189, 309, 16}, {}, {207}, {158, 223, 189, 14}, {357, 223, 110, 14}},
        {245, 59, {158, 254, 197, 16}, {}, {272, 284}, {363, 254, 104, 16}, {}},
        {304, 59, {158, 313, 225, 16}, {}, {331, 343}, {391, 313, 76, 16}, {158, 313, 225, 16}},
        {363, 59, {158, 372, 121, 16}, {}, {390, 402}, {287, 372, 180, 16}, {}},
        {422, 81, {158, 431, 309, 16}, {}, {449}, {158, 465, 200, 30}, {}},
        {503, 35, {}, {}, {}, {158, 512, 309, 16}, {}},
        {538, 35, {158, 547, 309, 16}, {}, {}, {}, {158, 547, 309, 16}},
    };
    placed_as("Every kind", place_every_kind(Sample{}), rows, 573);

    // Locked, a link keeps to the room left of its lock, and a text row's
    // label and value end short of it.
    std::array<kit::RowSpec<Sample>, 2> locked{
        locked_in_game(kit::link<Sample>("more-maps", "MORE MAPS...", kMoreMaps)),
        locked_in_game(kit::text("location", "Where they are", folder_text)),
    };
    Sample sample;
    sample.in_game = true;
    const std::vector<Expected> locked_rows{
        {54, 35, {}, {319, 63, 148, 16}, {}, {158, 63, 153, 16}, {}},
        {89, 35, {158, 98, 153, 16}, {319, 98, 148, 16}, {}, {}, {158, 98, 153, 16}},
    };
    placed_as(
        "Locked link and text",
        kit::place_rows(views_of(locked, sample), dialog_column(false)),
        locked_rows,
        124
    );

    // A link narrower than its line, given its width.
    locked[0].control_width = 60;
    sample.in_game = false;
    const kit::PlacedRows narrow = kit::place_rows(views_of(locked, sample), dialog_column(false));
    check_rect("Narrow link", 0, "control", narrow.rows[0].control_area, {158, 63, 60, 16});

    // A locked value row whose control stands below the label: the lock
    // stays at the right of the label line, and the value stands on the
    // button's line, before the button. A status hint does not hide it.
    std::array<kit::RowSpec<Sample>, 2> values{
        locked_in_game(
            kit::value_and_button(
                "downloaded-files",
                "Downloaded files",
                install_id_text,
                "EMPTY",
                "empty",
                kReset,
                {"Kept for resuming and repairs."}
            )
        ),
        locked_in_game(
            kit::value_and_button(
                "install-value",
                "Install ID",
                install_id_text,
                "RESET",
                "reset",
                kReset,
                {"A random number for downloads."}
            )
        ),
    };
    sample.in_game = true;
    sample.install_id = "877 KB kept for resuming and repairs";
    std::vector<kit::RowView> value_views = views_of(values, sample);
    value_views[0].control_below = true;
    value_views[0].hint_is_status = true;
    value_views[1].control_below = true;
    const std::vector<Expected> value_rows{
        {54,
         65,
         {158, 63, 153, 16},
         {319, 63, 148, 16},
         {99},
         {391, 81, 76, 16},
         {158, 81, 225, 16}},
        {119,
         65,
         {158, 128, 153, 16},
         {319, 128, 148, 16},
         {164},
         {391, 146, 76, 16},
         {158, 146, 225, 16}},
    };
    const kit::PlacedRows locked_values = kit::place_rows(value_views, dialog_column(false));
    placed_as("Locked values", locked_values, value_rows, 184);
    kit::DisplayList locked_list;
    kit::add_rows(locked_list, locked_values, {});
    bool saw_button = false;
    for (const kit::Item& item : locked_list.items) {
        const auto* look = std::get_if<kit::ButtonLook>(&item.look);
        if (look == nullptr)
            continue;
        saw_button = true;
        OA_CHECK(look->style == kit::ButtonStyle::plain);
    }
    OA_CHECK(saw_button);
}

// ---- Scroll ----

/// The rows scroll as the dialog's section does. The content's heights and
/// limits were computed once with the dialog's geometry::open_rows at the
/// same commit: Controls, and a section of a drop-down with a two-line hint
/// over four one-line switches, whose limit the modern fonts stretch so
/// that the view's top cuts no hint line.
void the_rows_scroll_as_the_dialog_scrolls_them() {
    const kit::Rect well{470, 54, 7, 236};
    const kit::Rect hit{467, 54, 12, 236};
    const auto controls = controls_rows();
    for (const bool tall : {false, true}) {
        const kit::PlacedRows placed =
            kit::place_rows(views_of(controls, Sample{}), dialog_column(tall));
        const kit::ScrollArea area = kit::rows_scroll(placed, kView, well, hit, 200, 8, tall);
        OA_CHECK(same(area.view, kView) && same(area.well, well) && same(area.hit, hit));
        OA_CHECK(area.page_step == 200 && area.offset == 0);
        OA_CHECK(area.content_height == (tall ? 391 : 379));
        OA_CHECK(area.limit == (tall ? 155 : 143));
    }
    const std::array<kit::RowSpec<Sample>, 5> stretched{
        kEveryKind[1],
        kit::toggle("one", "One", &Sample::vertical_sync, {"A line."}),
        kit::toggle("two", "Two", &Sample::vertical_sync, {"A line."}),
        kit::toggle("three", "Three", &Sample::vertical_sync, {"A line."}),
        kit::toggle("four", "Four", &Sample::vertical_sync, {"A line."}),
    };
    for (const bool tall : {false, true}) {
        kit::PlacedRows placed =
            kit::place_rows(views_of(stretched, Sample{}), dialog_column(tall));
        OA_CHECK(placed.bottom == (tall ? 324 : 321));
        const kit::ScrollArea area = kit::rows_scroll(placed, kView, well, hit, 200, 8, tall);
        OA_CHECK(area.content_height == (tall ? 290 : 276));
        OA_CHECK(area.limit == (tall ? 54 : 40));
        // Scrolled by 30, as the dialog's scroll_rows moves them.
        kit::scroll(placed, 30);
        const kit::PlacedRow& first = placed.rows[0];
        OA_CHECK(first.top == 24 && first.label.y == 33 && first.hints[0].y == 51);
        OA_CHECK(first.control_area.y == (tall ? 82 : 79));
        OA_CHECK(first.lock_area.y == 0 && first.value.y == 0);
        OA_CHECK(placed.bottom == (tall ? 294 : 291));
    }
}

// ---- Views ----

/// Every word a row shows comes through the caller's function; the kit
/// looks none up, and a text field's text is the player's own.
void a_view_shows_every_word_through_the_callers_function() {
    const kit::ShownText shown = [](std::string_view english) -> std::string_view {
        if (english == "Vertical sync")
            return "VSYNC";
        if (english == "Waits for the display.")
            return "WAITS";
        if (english == "OFF")
            return "AUS";
        if (english == "ON")
            return "EIN";
        if (english == "Not during a game")
            return "NICHT JETZT";
        if (english == "Always")
            return "IMMER";
        if (english == "RESET")
            return "NEU";
        if (english == "SCREENSHOTS")
            return "BILDER";
        if (english == "Ridge")
            return "WRONG";
        return english;
    };
    Sample sample;
    sample.in_game = true;
    sample.player_name = "Ridge";
    const std::vector<kit::RowView> views = views_of(kEveryKind, sample, shown);
    OA_CHECK(views[0].label == "VSYNC" && views[0].hints == std::vector<std::string>{"WAITS"});
    OA_CHECK(views[0].captions == (std::vector<std::string>{"AUS", "EIN"}));
    OA_CHECK(views[0].lock == "NICHT JETZT");
    OA_CHECK(views[1].captions[1] == "IMMER" && views[1].value == "Off");
    OA_CHECK(views[4].buttons == std::vector<std::string>{"NEU"});
    OA_CHECK(views[5].buttons[1] == "BILDER" && views[5].button_ids[1] == "screenshots");
    OA_CHECK(views[6].text == "Ridge");
    // Without a function, every word shows as written.
    const std::vector<kit::RowView> written = views_of(kEveryKind, sample);
    OA_CHECK(written[0].label == "Vertical sync" && written[0].captions[0] == "OFF");
    OA_CHECK(written[0].lock == "Not during a game");
}

/// What each kind's view reads from the model.
void a_view_reads_each_kind_from_the_model() {
    Sample sample;
    sample.vertical_sync = true;
    sample.gyro = Gyro::touched;
    sample.detail = Detail::medium;
    sample.volume = 4;
    sample.player_name = "Ridge";
    const std::vector<kit::RowView> views = views_of(kEveryKind, sample);
    for (std::size_t at = 0; at < views.size(); ++at)
        OA_CHECK(views[at].kind == kEveryKind[at].kind && views[at].id == kEveryKind[at].id);
    OA_CHECK(views[0].on && views[0].hints.size() == 1 && views[0].notices.size() == 1);
    OA_CHECK(views[1].count == 3 && views[1].index == 2 && views[1].value == "While touched");
    OA_CHECK(views[1].control_width == 248 && views[1].hints.size() == 2);
    OA_CHECK(views[2].count == 11 && views[2].index == 4 && views[2].value == "40%");
    OA_CHECK(views[3].count == 3 && views[3].index == 1 && views[3].offered == 2);
    OA_CHECK(views[3].captions == (std::vector<std::string>{"Low", "Medium", "High"}));
    OA_CHECK(views[4].value == "7f3a90d2" && views[4].label.empty() && views[4].enabled);
    OA_CHECK(views[4].button_ids == std::vector<std::string>{"reset"});
    OA_CHECK(views[5].buttons.size() == 3 && views[5].label == "Your files");
    OA_CHECK(views[6].text == "Ridge");
    OA_CHECK(
        views[7].label.empty() && views[7].buttons == std::vector<std::string>{"MORE MAPS..."}
    );
    OA_CHECK(views[8].value == "Documents/Open Annihilation" && views[8].hints.empty());
    sample.install_id.clear();
    OA_CHECK(!kit::view_of(kEveryKind[4], sample, {}).enabled);
}

/// A row of another model, whose label follows it.
struct Registry {
    std::string name{};   ///< what the player calls it
    bool install_id_on{}; ///< it is sent an install ID
};

/// Returns a registry's install ID row's label, with its name.
std::string install_id_label(const Registry& registry) {
    return "Install ID \xC2\xB7 " + registry.name;
}

/// Returns the hint lines a registry's row shows: a second while its ID is off.
std::size_t registry_hint_lines(const Registry& registry) {
    return registry.install_id_on ? 1 : 2;
}

/// Returns a line of a registry's row's hint.
std::string registry_hint(const Registry&, std::size_t line) {
    return line == 0 ? "Sent with each download." : "Off, downloads are off.";
}

/// Tells whether a line of a registry's row's hint is a notice: the second.
bool registry_notice(const Registry&, std::size_t line) {
    return line == 1;
}

/// Returns the install ID row a registry's model is bound to.
constexpr kit::RowSpec<Registry> install_id_row() {
    kit::RowSpec<Registry> spec = kit::toggle("install-id", "Install ID", &Registry::install_id_on);
    spec.label_text = install_id_label;
    spec.hint_lines = registry_hint_lines;
    spec.hint_text = registry_hint;
    spec.hint_notice = registry_notice;
    return spec;
}

constexpr kit::RowSpec<Registry> kInstallIdRow = install_id_row();

/// A row's label and hint that change with its model, read into a view that
/// outlives the model.
void a_label_and_hint_follow_the_model() {
    kit::RowView view;
    {
        const Registry registry{"Core Prime", false};
        view = kit::view_of(kInstallIdRow, registry, {});
    }
    OA_CHECK(view.label == "Install ID \xC2\xB7 Core Prime");
    OA_CHECK(
        view.hints ==
        (std::vector<std::string>{"Sent with each download.", "Off, downloads are off."})
    );
    OA_CHECK(view.notices == (std::vector<bool>{false, true}));
    const Registry on{"Mirror", true};
    const kit::RowView shorter = kit::view_of(kInstallIdRow, on, {});
    OA_CHECK(shorter.label == "Install ID \xC2\xB7 Mirror" && shorter.hints.size() == 1);
}

/// Rows bound to different models, two of them of one type, lie in one
/// column and answer together; a repeated row's id takes its item's word.
void rows_of_several_models_lay_out_and_answer_together() {
    Sample sample;
    Registry core{"Core Prime", true};
    Registry mirror{"Mirror", false};
    std::vector<kit::RowView> views{
        kit::view_of(kEveryKind[0], sample, {}),
        kit::view_of(kInstallIdRow, core, {}),
        kit::view_of(kInstallIdRow, mirror, {}),
    };
    views[1].id += "-1";
    views[2].id += "-2";
    const kit::PlacedRows placed = kit::place_rows(views, dialog_column(false));
    OA_CHECK(placed.rows.size() == 3);
    OA_CHECK(placed.rows[1].top == 101 && placed.rows[1].hints.size() == 1);
    OA_CHECK(placed.rows[2].top == 148 && placed.rows[2].hints.size() == 2);
    kit::DisplayList list;
    kit::RowsState state;
    state.name_prefix = "settings.downloads";
    kit::add_rows(list, placed, state);
    OA_CHECK(kit::name_problem(list).empty());
    OA_CHECK(kit::control_named(list, "settings.downloads.vertical-sync") == 13);
    OA_CHECK(kit::control_named(list, "settings.downloads.install-id-1") == 14);
    OA_CHECK(kit::control_named(list, "settings.downloads.install-id-2") == 15);
    // A press on the second registry's On half turns its ID on, and the
    // first's stays as it was.
    const kit::Rect& area = placed.rows[2].control_area;
    const kit::RowResult pressed =
        kit::press(kInstallIdRow, mirror, placed.rows[2], {area.x + area.width - 2, area.y + 4});
    OA_CHECK(pressed.event == kit::RowEvent::changed && mirror.install_id_on && core.install_id_on);
    OA_CHECK(kit::step(kInstallIdRow, core, false).event == kit::RowEvent::changed);
    OA_CHECK(!core.install_id_on && mirror.install_id_on && !sample.vertical_sync);
}

// ---- The display list ----

/// The role of each item, in order.
std::vector<kit::Role> roles_of(const kit::DisplayList& list) {
    std::vector<kit::Role> roles;
    for (const kit::Item& item : list.items)
        roles.push_back(item.role);
    return roles;
}

/// The page's controls: one per row with a control, named under the prefix,
/// of its row's kind, in Tab order; the items in the dialog's drawing order.
void the_display_list_names_and_orders_every_control() {
    Sample sample;
    sample.vertical_sync = true;
    const kit::PlacedRows placed = place_every_kind(sample);
    kit::DisplayList list;
    kit::RowsState state;
    state.name_prefix = "settings";
    state.focused = 18;
    state.marked_button = 1;
    state.group = 2;
    state.clip = {156, 54, 313, 520};
    kit::add_rows(list, placed, state);
    OA_CHECK(kit::name_problem(list).empty());

    struct Want {
        kit::ControlId id;
        const char* name;
        kit::ControlKind kind;
        bool steps;
    };

    const std::array<Want, 8> wanted{{
        {13, "settings.vertical-sync", kit::ControlKind::toggle, true},
        {14, "settings.gyro", kit::ControlKind::choice, true},
        {15, "settings.volume", kit::ControlKind::slider, true},
        {16, "settings.detail", kit::ControlKind::levels, true},
        {17, "settings.install-id.reset", kit::ControlKind::button, false},
        {18, "settings.your-files", kit::ControlKind::buttons, true},
        {19, "settings.player-name", kit::ControlKind::text_field, true},
        {20, "settings.more-maps", kit::ControlKind::link, false},
    }};
    OA_CHECK(list.controls.size() == wanted.size());
    for (std::size_t at = 0; at < std::min(list.controls.size(), wanted.size()); ++at) {
        const kit::Control& control = list.controls[at];
        OA_CHECK(control.id == wanted[at].id && control.name == wanted[at].name);
        OA_CHECK(control.kind == wanted[at].kind && control.steps == wanted[at].steps);
        OA_CHECK(control.enabled && control.focusable && control.group == 2);
        OA_CHECK(same(control.clip, state.clip));
        OA_CHECK(same(control.rect, placed.rows[at].control_area));
    }
    OA_CHECK(list.controls[0].checked && list.controls[0].text == "ON");
    OA_CHECK(list.tab_order == (std::vector<kit::ControlId>{13, 14, 15, 16, 17, 18, 19, 20}));

    using kit::Role;
    const std::vector<Role> order{
        Role::row_frame, Role::toggle,               // vertical-sync
        Role::row_frame, Role::choice,               // gyro
        Role::row_frame, Role::slider, Role::text,   // volume and its value
        Role::row_frame, Role::levels,               // detail
        Role::row_frame, Role::text,   Role::button, // the install ID and RESET
        Role::row_frame, Role::button, Role::button, Role::button, Role::focus_ring, // your files
        Role::row_frame, Role::field,                                                // player name
        Role::row_frame, Role::link,                                                 // more maps
        Role::row_frame, Role::text,                                                 // location
        Role::rule, // the rule under the last row
    };
    OA_CHECK(roles_of(list) == order);
    for (const kit::Item& item : list.items)
        OA_CHECK(same(item.clip, state.clip));
    // The marked button wears the ring; the slider's value is right of its track.
    OA_CHECK(same(list.items[16].rect, {337, 372, 82, 16}));
    OA_CHECK(list.items[6].text == "0%" && same(list.items[6].rect, {357, 223, 110, 14}));
    OA_CHECK(list.items[6].align == kit::Align::right && list.items[6].colour == kit::colour::text);
    // The install ID has no label, so its value stands in the label's place.
    OA_CHECK(list.items[10].text == "7f3a90d2" && list.items[10].align == kit::Align::left);
    const auto* reset = std::get_if<kit::ButtonLook>(&list.items[11].look);
    OA_CHECK(reset != nullptr && reset->caption == "RESET" && reset->enabled);
    OA_CHECK(reset != nullptr && reset->style == kit::ButtonStyle::accent);
    const auto* field = std::get_if<kit::SearchLook>(&list.items[18].look);
    OA_CHECK(field != nullptr && !field->magnifier && !field->focused);
    const auto* frame = std::get_if<kit::RowFrame>(&list.items[0].look);
    OA_CHECK(frame != nullptr && frame->label_text == "Vertical sync" && frame->top == 54);
    OA_CHECK(frame != nullptr && frame->left == 158 && frame->width == 309);
    OA_CHECK(frame != nullptr && frame->hint_clips.size() == 1 && frame->hint_clips[0].width == 0);
    // The install ID's hint keeps to its own columns, as a host's text does.
    const auto* host = std::get_if<kit::RowFrame>(&list.items[9].look);
    OA_CHECK(host != nullptr && host->hint_clips.size() == 2);
    OA_CHECK(host != nullptr && same(host->hint_clips[0], {158, 54, 309, 520}));
    OA_CHECK(same(list.items.back().rect, {158, 573, 309, 1}));

    // Tab moves through the declared order; a press reaches each control.
    kit::Interaction interaction;
    interaction.focused = 16;
    interaction.focus_shown = true;
    OA_CHECK(kit::key(interaction, list, kit::Key::tab).result == kit::KeyResult::redraw);
    OA_CHECK(interaction.focused == 17);
    OA_CHECK(kit::hit(list, {430, 70}) == 13 && kit::hit(list, {340, 375}) == 18);
    OA_CHECK(kit::hit(list, {200, 540}) == kit::no_control);
}

/// Returns a detail level's id: its enumerator's name.
std::string detail_id(const Sample&, int32_t at) {
    constexpr std::array<std::string_view, 3> ids{"low", "medium", "high"};
    return at >= 0 && at < 3 ? std::string(ids[static_cast<std::size_t>(at)]) : std::string();
}

/// The zoom's items' ids, beside kZoomCaptions.
constexpr std::array<std::string_view, 3> kZoomIds{"near", "far", "farthest"};

/// Choices and levels carry their ids: an index field's beside its captions,
/// a stepper's from its function. A view keeps them, each row's control
/// gives them, or its buttons' words, as its parts' words, and automation
/// names the parts by them, or by their places where a spec gives none.
void choices_and_levels_name_their_parts_by_their_ids() {
    kit::Stepper<Sample> detail = kDetail;
    detail.id = detail_id;
    const std::array<kit::RowSpec<Sample>, 4> specs{
        kit::choice("zoom", "Zoom", &Sample::zoom, kZoomCaptions, kZoomIds),
        kit::levels("detail", "Detail", detail, 34),
        kit::levels("anti-aliasing", "Anti-aliasing", kAntiAliasing, 30),
        kit::buttons<Sample>(
            "your-files",
            "Your files",
            {"SAVES", "SCREENSHOTS", "MODS"},
            {"saves", "screenshots", "mods"},
            {kOpenSaves, kOpenScreenshots, kOpenMods}
        ),
    };
    Sample sample;
    sample.zoom = 1;
    // The ids are words, unique within their row.
    for (const kit::RowSpec<Sample>& spec : specs) {
        OA_CHECK(kit::row_word(spec.id));
        std::vector<std::string> seen;
        for (int32_t at = 0; at < kit::row_count(spec, sample); ++at) {
            const std::string id = kit::row_choice_id(spec, sample, at);
            if (id.empty())
                continue;
            OA_CHECK(kit::row_word(id));
            OA_CHECK(std::find(seen.begin(), seen.end(), id) == seen.end());
            seen.push_back(id);
        }
    }
    OA_CHECK(kit::row_choice_id(specs[0], sample, 2) == "farthest");
    OA_CHECK(kit::row_choice_id(specs[0], sample, 3).empty());
    OA_CHECK(kit::row_choice_id(specs[1], sample, 0) == "low");
    OA_CHECK(kit::row_choice_id(specs[2], sample, 0).empty());

    const std::vector<kit::RowView> views = views_of(specs, sample);
    OA_CHECK((views[0].choice_ids == std::vector<std::string>{"near", "far", "farthest"}));
    OA_CHECK((views[1].choice_ids == std::vector<std::string>{"low", "medium", "high"}));
    OA_CHECK(views[2].choice_ids.size() == 5 && views[2].choice_ids[0].empty());
    OA_CHECK(views[3].choice_ids.empty());

    kit::DisplayList list;
    kit::RowsState state;
    state.name_prefix = "settings";
    state.open_menu = 13;
    kit::add_rows(list, kit::place_rows(views, dialog_column(false)), state);
    OA_CHECK(list.controls.size() == 4);
    if (list.controls.size() != 4)
        return;
    OA_CHECK((list.controls[0].parts == std::vector<std::string>{"near", "far", "farthest"}));
    OA_CHECK((list.controls[1].parts == std::vector<std::string>{"low", "medium", "high"}));
    OA_CHECK((list.controls[3].parts == std::vector<std::string>{"saves", "screenshots", "mods"}));

    std::vector<std::string> names;
    for (const kit::AutomationEntry& entry : kit::automation_parts(list, {}))
        names.push_back(entry.name);
    // The drop-down shows no menu of its own here, so it lists no items.
    OA_CHECK(
        (names == std::vector<std::string>{
                      "settings.zoom",
                      "settings.detail",
                      "settings.detail.low",
                      "settings.detail.medium",
                      "settings.detail.high",
                      "settings.anti-aliasing",
                      "settings.anti-aliasing.1",
                      "settings.anti-aliasing.2",
                      "settings.anti-aliasing.3",
                      "settings.anti-aliasing.4",
                      "settings.anti-aliasing.5",
                      "settings.your-files",
                      "settings.your-files.saves",
                      "settings.your-files.screenshots",
                      "settings.your-files.mods",
                  })
    );
}

/// Locked rows, the text row and a disabled button take no focus: a locked
/// row lists no control and draws its fade and its lock; a disabled button
/// is listed, drawn disabled, and left out of Tab.
void locked_rows_and_disabled_buttons_take_no_focus() {
    Sample sample;
    sample.in_game = true;
    sample.install_id.clear();
    const kit::PlacedRows placed = place_every_kind(sample);
    kit::DisplayList list;
    kit::RowsState state;
    state.name_prefix = "settings";
    state.focused = 13;
    kit::add_rows(list, placed, state);
    OA_CHECK(kit::name_problem(list).empty());
    OA_CHECK(list.controls.size() == 7);
    OA_CHECK(kit::control_of(list, 13) == nullptr && kit::control_of(list, 21) == nullptr);
    const kit::Control* reset = kit::control_of(list, 17);
    OA_CHECK(reset != nullptr && !reset->enabled && reset->name == "settings.install-id.reset");
    OA_CHECK(list.tab_order == (std::vector<kit::ControlId>{14, 15, 16, 18, 19, 20}));
    OA_CHECK(kit::next_in_tab_order(list, 16, true) == 18);
    OA_CHECK(kit::hit(list, {420, 320}) == kit::no_control);

    using kit::Role;
    const std::vector<Role> first_rows{
        Role::row_frame,
        Role::toggle,
        Role::locked_fade,
        Role::lock, // vertical-sync, locked
        Role::row_frame,
        Role::choice, // gyro
    };
    const std::vector<Role> roles = roles_of(list);
    OA_CHECK(std::equal(first_rows.begin(), first_rows.end(), roles.begin()));
    // The locked switch is drawn locked, takes no control and wears no ring.
    OA_CHECK(list.items[1].control == kit::no_control && list.items[1].state.locked);
    OA_CHECK(same(list.items[2].rect, {158, 55, 309, 46}));
    OA_CHECK(
        same(list.items[3].rect, {269, 63, 148, 16}) && list.items[3].text == "Not during a game"
    );
    OA_CHECK(std::none_of(list.items.begin(), list.items.end(), [](const kit::Item& item) {
        return item.role == Role::focus_ring;
    }));
    // RESET shows the disabled look, and its row no value.
    const auto disabled =
        std::find_if(list.items.begin(), list.items.end(), [](const kit::Item& item) {
            return item.control == 17;
        });
    OA_CHECK(disabled != list.items.end() && disabled[-1].role == Role::row_frame);
    const auto* look =
        disabled != list.items.end() ? std::get_if<kit::ButtonLook>(&disabled->look) : nullptr;
    OA_CHECK(look != nullptr && !look->enabled && look->style == kit::ButtonStyle::plain);

    // A locked row whose hint is its status fades its label line and a lock
    // that dropped under it, and stops at the status.
    const auto specs = controls_rows();
    const kit::PlacedRows controls = kit::place_rows(views_of(specs, sample), dialog_column(false));
    kit::DisplayList faded;
    kit::add_rows(faded, controls, {});
    std::vector<kit::Rect> fades;
    for (const kit::Item& item : faded.items)
        if (item.role == Role::locked_fade)
            fades.push_back(item.rect);
    OA_CHECK(fades.size() == 4);
    if (fades.size() == 4) {
        OA_CHECK(same(fades[2], {158, 278, 309, 76}));
        OA_CHECK(same(fades[3], {158, 355, 309, 44}));
    }
    // With no prefix, a control is named by its row's id alone.
    OA_CHECK(kit::control_named(faded, "max-zoom-in") == 15);
}

// ---- Events ----

/// Returns one of the placed rows.
const kit::PlacedRow& placed_row(const kit::PlacedRows& placed, std::size_t at) {
    return placed.rows[at];
}

/// Tells whether an event's outcome is the one expected.
bool is(kit::RowResult result, kit::RowEvent event) {
    return result.event == event;
}

/// Tells whether an event asks for an action.
bool asks(kit::RowResult result, kit::ActionId action) {
    return result.event == kit::RowEvent::action && result.action == action;
}

/// Left and Right step a switch Off and On, Space flips it, and a press on
/// its left half sets Off and on its right half On; locked, it takes nothing.
void a_switch_steps_flips_and_takes_either_half() {
    using kit::RowEvent;
    Sample sample;
    const kit::RowSpec<Sample>& spec = kEveryKind[0];
    const kit::PlacedRows placed = place_every_kind(sample);
    const kit::PlacedRow& row = placed_row(placed, 0); // {425, 63, 42, 16}
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::changed) && sample.vertical_sync);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::none) && sample.vertical_sync);
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::changed) && !sample.vertical_sync);
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::none));
    OA_CHECK(is(kit::activate(spec, sample, 0), RowEvent::changed) && sample.vertical_sync);
    // The switch is {425, 63, 42, 16}: its Off half to column 445, its On
    // half from 446.
    OA_CHECK(
        is(kit::press(spec, sample, row, {445, 70}), RowEvent::changed) && !sample.vertical_sync
    );
    OA_CHECK(is(kit::press(spec, sample, row, {426, 70}), RowEvent::none));
    OA_CHECK(
        is(kit::press(spec, sample, row, {446, 70}), RowEvent::changed) && sample.vertical_sync
    );
    OA_CHECK(is(kit::drag(spec, sample, row, 426), RowEvent::none) && sample.vertical_sync);
    OA_CHECK(is(kit::choose(spec, sample, 0), RowEvent::none) && sample.vertical_sync);
    sample.in_game = true;
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::none));
    OA_CHECK(is(kit::activate(spec, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::press(spec, sample, row, {426, 70}), RowEvent::none) && sample.vertical_sync);
    // Read and set through functions, as Escape opens the menu is.
    const auto controls = controls_rows();
    sample.in_game = false;
    OA_CHECK(is(kit::activate(controls[4], sample, 0), RowEvent::changed));
    OA_CHECK(sample.escape_opens_menu);
}

/// A drop-down steps one item, stopping at its ends, opens its menu on
/// Space or a press, and takes the item chosen from it.
void a_drop_down_steps_opens_and_takes_a_choice() {
    using kit::RowEvent;
    Sample sample;
    const kit::RowSpec<Sample>& spec = kEveryKind[1];
    const kit::PlacedRows placed = place_every_kind(sample);
    const kit::PlacedRow& row = placed_row(placed, 1);
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::none) && sample.gyro == Gyro::off);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::changed) && sample.gyro == Gyro::always);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::changed) && sample.gyro == Gyro::touched);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::none) && sample.gyro == Gyro::touched);
    OA_CHECK(is(kit::activate(spec, sample, 0), RowEvent::open_menu));
    OA_CHECK(is(kit::press(spec, sample, row, {160, 160}), RowEvent::open_menu));
    OA_CHECK(is(kit::choose(spec, sample, 0), RowEvent::changed) && sample.gyro == Gyro::off);
    OA_CHECK(is(kit::choose(spec, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::choose(spec, sample, 3), RowEvent::none) && sample.gyro == Gyro::off);
    OA_CHECK(is(kit::choose(spec, sample, -1), RowEvent::none) && sample.gyro == Gyro::off);
    OA_CHECK(is(kit::drag(spec, sample, row, 300), RowEvent::none));
    // A drop-down whose field holds its item's index, as Maximum zoom out.
    const auto controls = controls_rows();
    OA_CHECK(is(kit::choose(controls[1], sample, 2), RowEvent::changed) && sample.zoom == 2);
    OA_CHECK(is(kit::step(controls[1], sample, true), RowEvent::none) && sample.zoom == 2);
    OA_CHECK(is(kit::step(controls[1], sample, false), RowEvent::changed) && sample.zoom == 1);
    sample.in_game = true;
    OA_CHECK(is(kit::choose(controls[1], sample, 0), RowEvent::none) && sample.zoom == 1);
    OA_CHECK(is(kit::activate(controls[1], sample, 0), RowEvent::none));
}

/// A slider steps one stop, stopping at its ends, and a press or a drag
/// moves its knob to the stop under the pointer; Space does nothing.
void a_slider_steps_and_follows_the_pointer() {
    using kit::RowEvent;
    Sample sample;
    const kit::RowSpec<Sample>& spec = kEveryKind[2];
    const kit::PlacedRows placed = place_every_kind(sample);
    const kit::PlacedRow& row =
        placed_row(placed, 2); // {158, 223, 189, 14}: stops 18.2 apart from 161
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::none) && sample.volume == 0);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::changed) && sample.volume == 1);
    OA_CHECK(is(kit::activate(spec, sample, 0), RowEvent::none) && sample.volume == 1);
    OA_CHECK(
        is(kit::press(spec, sample, row, {252, 230}), RowEvent::changed) && sample.volume == 5
    );
    OA_CHECK(is(kit::drag(spec, sample, row, 252), RowEvent::none) && sample.volume == 5);
    OA_CHECK(is(kit::drag(spec, sample, row, 400), RowEvent::changed) && sample.volume == 10);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::none) && sample.volume == 10);
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::changed) && sample.volume == 9);
    OA_CHECK(is(kit::drag(spec, sample, row, 100), RowEvent::changed) && sample.volume == 0);
    OA_CHECK(is(kit::choose(spec, sample, 3), RowEvent::none) && sample.volume == 0);
}

/// A strip steps one level within the offered ones, and a press chooses the
/// level under it when it is offered; Space does nothing.
void a_strip_steps_within_its_offered_levels() {
    using kit::RowEvent;
    Sample sample;
    const kit::RowSpec<Sample>& spec = kEveryKind[3];
    const kit::PlacedRows placed = place_every_kind(sample);
    const kit::PlacedRow& row = placed_row(placed, 3); // {363, 254, 104, 16}, levels 34 wide
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::none) && sample.detail == Detail::low);
    OA_CHECK(
        is(kit::step(spec, sample, true), RowEvent::changed) && sample.detail == Detail::medium
    );
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::none) && sample.detail == Detail::medium);
    OA_CHECK(is(kit::press(spec, sample, row, {440, 260}), RowEvent::none));
    OA_CHECK(
        is(kit::press(spec, sample, row, {365, 260}), RowEvent::changed) &&
        sample.detail == Detail::low
    );
    OA_CHECK(
        is(kit::press(spec, sample, row, {398, 260}), RowEvent::changed) &&
        sample.detail == Detail::medium
    );
    OA_CHECK(is(kit::activate(spec, sample, 0), RowEvent::none));
    sample.detail_offered = 3;
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::changed) && sample.detail == Detail::high);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::none) && sample.detail == Detail::high);
    OA_CHECK(
        is(kit::press(spec, sample, row, {397, 260}), RowEvent::changed) &&
        sample.detail == Detail::low
    );
    // A level the strip shows but no longer offers steps only down.
    sample.detail = Detail::high;
    sample.detail_offered = 1;
    OA_CHECK(is(kit::step(spec, sample, false), RowEvent::none) && sample.detail == Detail::high);
    OA_CHECK(
        is(kit::press(spec, sample, row, {365, 260}), RowEvent::changed) &&
        sample.detail == Detail::low
    );
}

/// A button asks for its action on Space and on a press, and nothing while
/// it is disabled; it takes no steps.
void a_button_asks_for_its_action_while_enabled() {
    using kit::RowEvent;
    Sample sample;
    const kit::RowSpec<Sample>& spec = kEveryKind[4];
    const kit::PlacedRows placed = place_every_kind(sample);
    const kit::PlacedRow& row = placed_row(placed, 4); // {391, 313, 76, 16}
    OA_CHECK(asks(kit::activate(spec, sample, 0), kReset));
    OA_CHECK(asks(kit::press(spec, sample, row, {400, 320}), kReset));
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::none));
    sample.install_id.clear();
    OA_CHECK(is(kit::activate(spec, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::press(spec, sample, row, {400, 320}), RowEvent::none));
}

/// A row of buttons asks for the marked button's action on Space and the
/// pressed button's on a press, none between two; Left and Right move its
/// mark and change nothing in the model.
void a_row_of_buttons_answers_with_the_button_meant() {
    using kit::RowEvent;
    Sample sample;
    const kit::RowSpec<Sample>& spec = kEveryKind[5];
    const kit::PlacedRows placed = place_every_kind(sample);
    const kit::PlacedRow& row = placed_row(placed, 5); // {287, 372, 180, 16}
    OA_CHECK(same(kit::row_button(row.control_area, 0), {287, 372, 46, 16}));
    OA_CHECK(same(kit::row_button(row.control_area, 1), {337, 372, 82, 16}));
    OA_CHECK(same(kit::row_button(row.control_area, 2), {423, 372, 44, 16}));
    OA_CHECK(asks(kit::press(spec, sample, row, {290, 375}), kOpenSaves));
    OA_CHECK(asks(kit::press(spec, sample, row, {340, 375}), kOpenScreenshots));
    OA_CHECK(asks(kit::press(spec, sample, row, {466, 375}), kOpenMods));
    OA_CHECK(is(kit::press(spec, sample, row, {334, 375}), RowEvent::none));
    OA_CHECK(asks(kit::activate(spec, sample, 1), kOpenScreenshots));
    OA_CHECK(is(kit::activate(spec, sample, 3), RowEvent::none));
    std::size_t marked = 0;
    OA_CHECK(is(kit::step(spec, sample, true, marked), RowEvent::none) && marked == 1);
    OA_CHECK(is(kit::step(spec, sample, true, marked), RowEvent::none) && marked == 2);
    OA_CHECK(is(kit::step(spec, sample, true, marked), RowEvent::none) && marked == 2);
    OA_CHECK(is(kit::step(spec, sample, false, marked), RowEvent::none) && marked == 1);
    OA_CHECK(is(kit::step(spec, sample, false, marked), RowEvent::none) && marked == 0);
    OA_CHECK(is(kit::step(spec, sample, false, marked), RowEvent::none) && marked == 0);
    kit::RowSpec<Sample> disabled = spec;
    disabled.enabled = never;
    OA_CHECK(is(kit::activate(disabled, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::press(disabled, sample, row, {290, 375}), RowEvent::none));
}

/// A text field takes typed text and the editing keys through the kit's
/// field editing: changed only when its text moves.
void a_text_field_takes_typing_and_editing() {
    using kit::RowEvent;
    Sample sample;
    const kit::RowSpec<Sample>& spec = kEveryKind[6];
    const kit::PlacedRows placed = place_every_kind(sample);
    std::size_t caret = 0;
    OA_CHECK(is(kit::type(spec, sample, caret, "Ridge"), RowEvent::changed));
    OA_CHECK(sample.player_name == "Ridge" && caret == 5);
    OA_CHECK(is(kit::type(spec, sample, caret, "\x07"), RowEvent::none) && caret == 5);
    OA_CHECK(is(kit::type(spec, sample, caret, "\xC3\xA9"), RowEvent::changed) && caret == 7);
    OA_CHECK(is(kit::edit(spec, sample, caret, kit::Key::left), RowEvent::none) && caret == 5);
    OA_CHECK(is(kit::edit(spec, sample, caret, kit::Key::backspace), RowEvent::changed));
    OA_CHECK(sample.player_name == "Ridg\xC3\xA9" && caret == 4);
    OA_CHECK(is(kit::edit(spec, sample, caret, kit::Key::end), RowEvent::none) && caret == 6);
    OA_CHECK(is(kit::edit(spec, sample, caret, kit::Key::delete_forward), RowEvent::none));
    OA_CHECK(is(kit::edit(spec, sample, caret, kit::Key::home), RowEvent::none) && caret == 0);
    OA_CHECK(is(kit::edit(spec, sample, caret, kit::Key::delete_forward), RowEvent::changed));
    OA_CHECK(sample.player_name == "idg\xC3\xA9" && caret == 0);
    OA_CHECK(is(kit::step(spec, sample, true), RowEvent::none));
    OA_CHECK(is(kit::activate(spec, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::press(spec, sample, placed_row(placed, 6), {170, 470}), RowEvent::none));
    // Typing reaches no other kind.
    OA_CHECK(is(kit::type(kEveryKind[8], sample, caret, "x"), RowEvent::none));
    // The focused field shows its text and its caret where the state puts it.
    kit::DisplayList list;
    kit::RowsState state;
    state.focused = 19;
    state.caret = 2;
    kit::add_rows(list, place_every_kind(sample), state);
    const auto field =
        std::find_if(list.items.begin(), list.items.end(), [](const kit::Item& item) {
            return item.role == kit::Role::field;
        });
    const auto* look =
        field != list.items.end() ? std::get_if<kit::SearchLook>(&field->look) : nullptr;
    OA_CHECK(look != nullptr && look->focused && look->field.caret == 2);
    OA_CHECK(look != nullptr && look->field.text == "idg\xC3\xA9");
}

/// A link asks for its action on Space and a press, and nothing disabled; a
/// text row takes nothing.
void a_link_acts_and_a_text_row_takes_nothing() {
    using kit::RowEvent;
    Sample sample;
    const kit::PlacedRows placed = place_every_kind(sample);
    const kit::RowSpec<Sample>& link = kEveryKind[7];
    OA_CHECK(asks(kit::activate(link, sample, 0), kMoreMaps));
    OA_CHECK(asks(kit::press(link, sample, placed_row(placed, 7), {160, 500}), kMoreMaps));
    OA_CHECK(is(kit::step(link, sample, true), RowEvent::none));
    kit::RowSpec<Sample> disabled = link;
    disabled.enabled = never;
    OA_CHECK(is(kit::activate(disabled, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::press(disabled, sample, placed_row(placed, 7), {160, 500}), RowEvent::none));
    const kit::RowSpec<Sample>& text = kEveryKind[8];
    std::size_t caret = 0;
    OA_CHECK(is(kit::step(text, sample, true), RowEvent::none));
    OA_CHECK(is(kit::activate(text, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::press(text, sample, placed_row(placed, 8), {160, 540}), RowEvent::none));
    OA_CHECK(is(kit::drag(text, sample, placed_row(placed, 8), 160), RowEvent::none));
    OA_CHECK(is(kit::choose(text, sample, 0), RowEvent::none));
    OA_CHECK(is(kit::edit(text, sample, caret, kit::Key::backspace), RowEvent::none));
}

// ---- Painting ----

/// Returns a black surface.
renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

/// The synthetic font of the settings pixel test, for both roles.
kit::Fonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y)
            for (uint16_t x = 0; x < width; ++x)
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// Returns the colour a surface holds at a pixel.
kit::Colour pixel(const renderer::Surface& surface, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    return {surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
}

/// Checks that a pixel holds a colour, and says which when it does not.
void expect(const renderer::Surface& surface, int32_t x, int32_t y, kit::Colour colour) {
    const kit::Colour got = pixel(surface, x, y);
    if (got != colour) {
        std::fprintf(
            stderr,
            "pixel %d,%d is #%02x%02x%02x, expected #%02x%02x%02x\n",
            x,
            y,
            got.r,
            got.g,
            got.b,
            colour.r,
            colour.g,
            colour.b
        );
        OA_CHECK(false);
    }
}

/// Paints the page of every kind as the model has it.
renderer::Surface painted_page(const Sample& sample) {
    static const kit::Fonts fonts = block_fonts();
    renderer::Surface surface = black(480, 600);
    kit::DisplayList list;
    kit::RowsState state;
    state.name_prefix = "settings";
    kit::add_rows(list, place_every_kind(sample), state);
    kit::paint({&surface, {0, 0, 1, {}}, &fonts, {}}, list);
    return surface;
}

/// The page painted from the table draws its switch On in the accent where
/// the model says so, and Off on its own face where it does not.
void a_page_painted_draws_its_switch_as_the_model_has_it() {
    Sample sample;
    sample.vertical_sync = true;
    const renderer::Surface on = painted_page(sample);
    // The switch is {425, 63, 42, 16}: a lime track with its knob at the
    // right while On, and the selected surface with its knob at the left
    // while Off.
    expect(on, 441, 64, kit::colour::accent);
    expect(on, 430, 70, kit::colour::accent);
    expect(on, 458, 70, kit::colour::on_accent);
    expect(on, 466, 78, {0x00, 0x00, 0x00});
    // The rows' rules, the first's and the one under the last.
    expect(on, 158, 54, {0x2b, 0x30, 0x27});
    expect(on, 466, 573, {0x2b, 0x30, 0x27});
    sample.vertical_sync = false;
    const renderer::Surface off = painted_page(sample);
    expect(off, 441, 64, kit::colour::off_selected);
    expect(off, 432, 70, kit::colour::text_secondary);
    // Locked, an On switch keeps its value without the accent.
    sample.vertical_sync = true;
    sample.in_game = true;
    const renderer::Surface locked = painted_page(sample);
    OA_CHECK(pixel(locked, 441, 64) != (kit::Colour{0x9c, 0xcc, 0x3c}));
}

} // namespace

/// Each preference row writes its key and reads it back. A missing or
/// malformed stored value reads as the default. A malformed key is refused.
void preference_rows_round_trip_and_refuse_bad_keys() {
    OA_CHECK(kit::preference_key_problem("check.toggle") == nullptr);
    OA_CHECK(kit::preference_key_problem("ridge.mode") == nullptr);
    OA_CHECK(kit::preference_key_problem("open-annihilation.language") != nullptr);
    OA_CHECK(kit::preference_key_problem("Section|Key") != nullptr);
    OA_CHECK(kit::preference_key_problem("toggle") != nullptr);
    OA_CHECK(kit::preference_key_problem("Check.Toggle") != nullptr);
    OA_CHECK(kit::preference_key_problem("") != nullptr);
    OA_CHECK(kit::preference_key_problem(std::string(65, 'a') + ".b") != nullptr);

    kit::PreferenceModel model;
    const kit::PreferenceRow toggle =
        kit::preference_toggle("toggle", "Toggle", "check.toggle", "1", {"A switch"});
    OA_CHECK(kit::row_on(toggle, model));
    model.values["check.toggle"] = "nope";
    OA_CHECK(kit::row_on(toggle, model));
    kit::set_row_on(toggle, model, false);
    OA_CHECK(model.values["check.toggle"] == "0" && !kit::row_on(toggle, model));
    kit::set_row_on(toggle, model, true);
    OA_CHECK(model.values["check.toggle"] == "1" && kit::row_on(toggle, model));

    constexpr std::array<std::string_view, 2> ids{"quiet", "loud"};
    constexpr std::array<std::string_view, 2> captions{"Quiet", "Loud"};
    const kit::PreferenceRow choice =
        kit::preference_choice("mode", "Mode", "check.mode", "quiet", ids, captions, {"A choice"});
    OA_CHECK(kit::row_index(choice, model) == 0);
    OA_CHECK(kit::row_caption(choice, model, 1) == "Loud");
    OA_CHECK(kit::row_choice_id(choice, model, 1) == "loud");
    model.values["check.mode"] = "sideways";
    OA_CHECK(kit::row_index(choice, model) == 0);
    kit::set_row_index(choice, model, 1);
    OA_CHECK(model.values["check.mode"] == "loud" && kit::row_index(choice, model) == 1);
    const kit::RowView choice_view = kit::view_of(choice, model, {});
    OA_CHECK(choice_view.value == "Loud" && choice_view.choice_ids[1] == "loud");

    const kit::PreferenceRow slider =
        kit::preference_slider("rate", "Rate", "check.rate", "0.5", 0.0, 1.0, 0.5, {"A slider"});
    OA_CHECK(kit::row_count(slider, model) == 3);
    OA_CHECK(kit::row_index(slider, model) == 1);
    OA_CHECK(kit::row_caption(slider, model, kit::row_index(slider, model)) == "0.5");
    model.values["check.rate"] = "0.25";
    OA_CHECK(kit::row_index(slider, model) == 1);
    kit::set_row_index(slider, model, 2);
    OA_CHECK(model.values["check.rate"] == "1" && kit::row_index(slider, model) == 2);

    const kit::PreferenceRow field =
        kit::preference_text_field("note", "Note", "check.note", "hello", {"A field"});
    OA_CHECK(kit::view_of(field, model, {}).text == "hello");
    model.values["check.note"] = std::string(kit::preference_text_most + 1, 'x');
    OA_CHECK(kit::view_of(field, model, {}).text == "hello");
    model.values["check.note"] = "";
    std::size_t caret = 0;
    OA_CHECK(kit::type(field, model, caret, "ab").event == kit::RowEvent::changed);
    OA_CHECK(model.values["check.note"] == "ab" && caret == 2);
    const std::string full(kit::preference_text_most, 'z');
    model.values["check.note"] = full;
    caret = full.size();
    OA_CHECK(kit::type(field, model, caret, "q").event == kit::RowEvent::none);
    OA_CHECK(model.values["check.note"] == full);
    OA_CHECK(kit::edit(field, model, caret, kit::Key::backspace).event == kit::RowEvent::changed);
    OA_CHECK(model.values["check.note"].size() == kit::preference_text_most - 1);
}

int main() {
    every_kind_lies_by_the_rows_rules();
    rows_shaped_as_controls_lie_where_the_dialog_places_them();
    rows_shaped_as_graphics_lie_where_the_dialog_places_them();
    a_lock_drops_under_a_label_it_does_not_fit_beside();
    developer_rows_lie_closer();
    the_rows_scroll_as_the_dialog_scrolls_them();
    a_view_shows_every_word_through_the_callers_function();
    a_view_reads_each_kind_from_the_model();
    a_label_and_hint_follow_the_model();
    rows_of_several_models_lay_out_and_answer_together();
    the_display_list_names_and_orders_every_control();
    choices_and_levels_name_their_parts_by_their_ids();
    locked_rows_and_disabled_buttons_take_no_focus();
    a_switch_steps_flips_and_takes_either_half();
    a_drop_down_steps_opens_and_takes_a_choice();
    a_slider_steps_and_follows_the_pointer();
    a_strip_steps_within_its_offered_levels();
    a_button_asks_for_its_action_while_enabled();
    a_row_of_buttons_answers_with_the_button_meant();
    a_text_field_takes_typing_and_editing();
    a_link_acts_and_a_text_row_takes_nothing();
    a_page_painted_draws_its_switch_as_the_model_has_it();
    preference_rows_round_trip_and_refuse_bad_keys();
    return oa::test::check_exit_status();
}
