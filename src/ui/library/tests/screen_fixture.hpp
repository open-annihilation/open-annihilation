// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the Library screens' tests share: a Library of fifteen made-up
// packages across the three kinds from two registries, one built in and
// one added, the frames each size class is laid out in, and the checks a
// layout's parts are held to whatever fonts draw them.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/screen.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace screen_fixture {

namespace kit = oa::ui::kit;
namespace lib = oa::ui::library;

/// A mebibyte.
inline constexpr uint64_t mib = uint64_t{1} << 20;
/// The built-in registry's id; its name is "Fixture Core".
inline constexpr std::string_view core = "fixture-core";
/// The added registry's id; its name is "Example Mods".
inline constexpr std::string_view added = "example-mods";

/// Returns a digest whose first four bytes are given and whose others are zero.
///
/// @param b0 the first byte
/// @param b1 the second byte
/// @param b2 the third byte
/// @param b3 the fourth byte
/// @return the digest
inline oa::base::sha256::Digest digest(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    oa::base::sha256::Digest value{};
    value[0] = b0;
    value[1] = b1;
    value[2] = b2;
    value[3] = b3;
    return value;
}

/// Returns a listing.
///
/// @param registry the registry's id
/// @param kind the kind
/// @param key the key
/// @param name the name
/// @param version the version
/// @param size the size, in bytes
/// @param summary the summary
/// @param tags the tags
/// @return the listing, by the registry's own team
inline lib::Listing listing(
    std::string_view registry,
    lib::Kind kind,
    std::string_view key,
    std::string_view name,
    std::string_view version,
    uint64_t size,
    std::string_view summary,
    std::vector<std::string> tags
) {
    lib::Listing made;
    made.registry = std::string(registry);
    made.kind = kind;
    made.key = std::string(key);
    made.name = std::string(name);
    made.version = std::string(version);
    made.revision = 1;
    made.release = 1;
    made.size = size;
    made.author = "the " + std::string(name) + " team";
    made.summary = std::string(summary);
    made.tags = std::move(tags);
    made.requires_engine = kind == lib::Kind::mod ? ">= 0.8.0" : "";
    made.base = kind == lib::Kind::mod ? "ta-3.1c" : "";
    return made;
}

/// Returns an installed package matching a listing.
///
/// @param from the listing it was installed from
/// @param version its version
/// @param revision its revision
/// @return the package
inline lib::Installed
installed(const lib::Listing& from, std::string_view version, int64_t revision) {
    lib::Installed made;
    made.kind = from.kind;
    made.key = from.key;
    made.folder = from.key;
    made.name = from.name;
    made.version = std::string(version);
    made.revision = revision;
    made.registry = from.registry;
    made.release = 1;
    return made;
}

/// Returns a queue item for a listing.
///
/// @param from the listing
/// @param phase where it is
/// @return the item
inline lib::QueueItem queued(const lib::Listing& from, lib::QueueItem::Phase phase) {
    lib::QueueItem made;
    made.registry = from.registry;
    made.kind = from.kind;
    made.key = from.key;
    made.name = from.name;
    made.version = from.version;
    made.phase = phase;
    made.total_bytes = from.size;
    return made;
}

/// Returns a badge picture: a 16 by 16 gradient, the same on every run.
///
/// @return the badge
inline lib::Badge gradient_badge() {
    constexpr uint32_t side = 16;
    lib::Badge badge;
    badge.width = side;
    badge.height = side;
    for (uint32_t y = 0; y < side; ++y)
        for (uint32_t x = 0; x < side; ++x) {
            badge.rgba.push_back(static_cast<uint8_t>(0x40 + x * 8));
            badge.rgba.push_back(static_cast<uint8_t>(0x80 + y * 4));
            badge.rgba.push_back(static_cast<uint8_t>(0xc0 - x * 4));
            badge.rgba.push_back(0xff);
        }
    return badge;
}

/// The Library's inputs: fifteen listings across the three kinds from the
/// built-in registry and an added one, five of them dated for New; an
/// update of the built-in Ridge that changes the rules, a blocked update of
/// Ironclad and an update of a map pack; Highland being played; a GET of
/// Hollow blocked by three missing hacks; four queue items: TA Zero
/// downloading, Frontier waiting, Cinder failed, and Lattice's installed
/// update that changed the rules; and for the Active tab, a language pack in
/// use and two installed map packs.
///
/// @return the inputs
inline lib::Inputs fixture_inputs() {
    using lib::Kind;
    using Phase = lib::QueueItem::Phase;
    lib::Inputs inputs;
    lib::Registry built_in;
    built_in.id = std::string(core);
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    lib::Registry other;
    other.id = std::string(added);
    other.name = "Example Mods";
    inputs.registries = {built_in, other};
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    inputs.list_age_seconds = 240;

    lib::Listing ridge = listing(
        core,
        Kind::mod,
        "ridge",
        "Ridge",
        "4.8",
        43011223,
        "Balance overhaul for 3.1c. Revision 4 corrects two unit scripts and a build time, and "
        "evens out the early economy so that both sides open alike.",
        {"balance"}
    );
    ridge.revision = 4;
    ridge.release = 2;
    ridge.game_hacks = 8;
    ridge.view_hacks = 4;
    ridge.homepage = "https://example.org/ridge";
    ridge.sim_hash = digest(0x51, 0xd2, 0x07, 0xaa);
    lib::Listing bastion = listing(
        core,
        Kind::mod,
        "bastion",
        "Bastion",
        "10.2.0",
        197132288,
        "Large unit expansion for Arm and Core",
        {"expansion", "units"}
    );
    bastion.badge = gradient_badge();
    lib::Listing highland = listing(
        core,
        Kind::mod,
        "highland",
        "Highland",
        "0.4.0",
        13002342,
        "Rules for ranked play",
        {"rules"}
    );
    highland.badge = gradient_badge();
    lib::Listing ta_zero = listing(
        core,
        Kind::mod,
        "ta-zero",
        "TA Zero",
        "3.1",
        64 * mib,
        "Balance mod in the spirit of 3.1c",
        {"balance", "classic"}
    );
    lib::Listing frontier = listing(
        added,
        Kind::mod,
        "frontier",
        "Frontier",
        "Pack 5",
        210 * mib,
        "Total conversion",
        {"conversion"}
    );
    lib::Listing ironclad = listing(
        core,
        Kind::mod,
        "ironclad",
        "Ironclad",
        "2.0",
        30 * mib,
        "Heavier armour for both sides",
        {"units"}
    );
    lib::Listing hollow = listing(
        added,
        Kind::mod,
        "hollow",
        "Hollow",
        "0.3",
        5 * mib,
        "Experiments with new weapons",
        {"experimental"}
    );
    hollow.hack_ids = {"fixture.missing-one", "fixture.missing-two", "fixture.missing-three"};
    hollow.game_hacks = 3;
    lib::Listing cinder = listing(
        core,
        Kind::mod,
        "cinder",
        "Cinder",
        "1.2",
        9 * mib,
        "Faster fires and longer burns",
        {"effects"}
    );
    lib::Listing lattice = listing(
        core, Kind::mod, "lattice", "Lattice", "3.0", 22 * mib, "Unit veterancy for 3.1c", {"units"}
    );
    lattice.revision = 3;
    lib::Listing quarry = listing(
        core,
        Kind::mod,
        "quarry",
        "Quarry",
        "1.0",
        2 * mib,
        "Metal and energy rebalanced",
        {"economy"}
    );
    lib::Listing dunes = listing(
        added, Kind::mod, "dunes", "Dunes", "0.9", 7 * mib, "Desert units and weather", {"balance"}
    );
    lib::Listing core_maps = listing(
        core,
        Kind::map_pack,
        "core-maps",
        "Core Map Pack",
        "1.1",
        48 * mib,
        "Sixteen maps for two to eight players",
        {"maps"}
    );
    core_maps.maps = {{"Ridgeline", 4, "16x16"}, {"Twin Rivers", 2, "12x12"}};
    core_maps.badge = gradient_badge();
    lib::Listing desert = listing(
        added,
        Kind::map_pack,
        "desert-pack",
        "Desert Pack",
        "1.0",
        31 * mib,
        "Two desert maps",
        {"desert"}
    );
    desert.maps = {{"Dry Lake", 2, "8x8"}, {"Mesa", 4, "16x16"}};
    lib::Listing portuguese = listing(
        core, Kind::language, "pt-BR", "Portugues (Brasil)", "", 3 * mib, "Brazilian Portuguese", {}
    );
    portuguese.coverage = 0.92;
    lib::Listing polish = listing(added, Kind::language, "pl", "Polski", "", 2 * mib, "Polish", {});
    polish.coverage = 0.92;
    ridge.published = "2026-10-01";
    bastion.published = "2026-09-12";
    dunes.published = "2026-10-11";
    core_maps.published = "2026-10-08";
    polish.published = "2026-08-20";

    inputs.listings = {
        ridge,
        bastion,
        highland,
        ta_zero,
        frontier,
        ironclad,
        hollow,
        cinder,
        lattice,
        quarry,
        dunes,
        core_maps,
        desert,
        portuguese,
        polish,
    };

    lib::Installed ridge_installed = installed(ridge, "4.8", 3);
    ridge_installed.rules = digest(0x9c, 0x1f, 0x4e, 0x0b);
    ridge_installed.kept = lib::Kept{"4.8", 2};
    lib::Installed highland_installed = installed(highland, "0.4.0", 1);
    highland_installed.playing = true;
    lib::Installed lattice_installed = installed(lattice, "3.0", 3);
    lattice_installed.kept = lib::Kept{"3.0", 2};
    lib::Installed polish_installed = installed(polish, "1.0", 2);
    polish_installed.in_use = true;
    polish_installed.badge = gradient_badge();
    inputs.installed = {
        ridge_installed,
        installed(bastion, "10.2.0", 1),
        highland_installed,
        installed(ironclad, "2.0", 1),
        lattice_installed,
        installed(core_maps, "1.0", 1),
        polish_installed,
    };

    lib::UpdateNote ridge_update;
    ridge_update.registry = std::string(core);
    ridge_update.kind = Kind::mod;
    ridge_update.key = "ridge";
    ridge_update.to_release = 2;
    ridge_update.to_version = "4.8";
    ridge_update.to_revision = 4;
    ridge_update.size = 3355443;
    ridge_update.from_rules = digest(0x9c, 0x1f, 0x4e, 0x0b);
    ridge_update.to_rules = digest(0x51, 0xd2, 0x07, 0xaa);
    ridge_update.rules = lib::UpdateNote::Rules::changes;
    lib::UpdateNote ironclad_update;
    ironclad_update.registry = std::string(core);
    ironclad_update.kind = Kind::mod;
    ironclad_update.key = "ironclad";
    ironclad_update.to_release = 2;
    ironclad_update.to_version = "2.1";
    ironclad_update.size = 4 * mib;
    ironclad_update.blocked = lib::UpdateNote::Blocked::engine;
    ironclad_update.blocked_detail = "0.9.0 or later";
    lib::UpdateNote maps_update;
    maps_update.registry = std::string(core);
    maps_update.kind = Kind::map_pack;
    maps_update.key = "core-maps";
    maps_update.to_release = 2;
    maps_update.to_version = "1.1";
    maps_update.size = 6 * mib;
    inputs.updates = {ridge_update, ironclad_update, maps_update};

    lib::QueueItem downloading = queued(ta_zero, Phase::downloading);
    downloading.done_bytes = 19084083;
    lib::QueueItem failed = queued(cinder, Phase::failed);
    failed.problem = "Couldn't reach the registry";
    failed.retryable = true;
    lib::QueueItem updated = queued(lattice, Phase::installed);
    updated.update_rules = lib::UpdateNote::Rules::changes;
    updated.update_from = "revision 2";
    updated.update_to = "revision 3";
    inputs.queue = {downloading, queued(frontier, Phase::waiting), failed, updated};

    inputs.language.name = "Polski";
    inputs.language.pack = "Polski";
    inputs.language.version = "1.0";
    inputs.language.english = "Polish";
    inputs.language.revision = 2;
    inputs.map_packs = {
        {"Core Map Pack", {{"Ridgeline", 4, "16x16"}, {"Twin Rivers", 2, "12x12"}}},
        {"Island Hopping", {{"Atoll", 6, "20x20"}}},
    };
    return inputs;
}

/// Returns the fixture's Library on its Mods tab, nothing selected but the first row.
///
/// @return the Library
inline lib::Library fixture_library() {
    lib::Library library;
    lib::refresh(library, fixture_inputs());
    lib::set_tab(library, lib::Tab::mods);
    return library;
}

/// The id of Ridge, the built-in registry's update that changes the rules.
///
/// @return the id
inline lib::EntryId ridge_id() {
    return {std::string(core), lib::Kind::mod, "ridge"};
}

/// A frame the tests lay the Library out in, and its name.
struct NamedFrame {
    std::string name{}; ///< such as "compact" or "1920x1080"
    kit::Frame frame{}; ///< the frame
};

/// Returns the frames: each class's window at scale 1, and the windows of
/// 640 by 480 at 1×, 1920 by 1080 at 2× and 2560 by 1440 at 2×.
///
/// @return the frames
inline std::vector<NamedFrame> frames() {
    const auto of = [](int32_t width, int32_t height, int32_t percent) {
        kit::Viewport viewport;
        viewport.width = width;
        viewport.height = height;
        viewport.scale_percent = percent;
        return kit::frame_of(viewport);
    };
    return {
        {"compact", {{0, 0, 480, 324}, kit::SizeClass::compact, 100}},
        {"regular", {{0, 0, 720, 486}, kit::SizeClass::regular, 100}},
        {"large", {{0, 0, 960, 600}, kit::SizeClass::large, 100}},
        {"640x480", of(640, 480, 100)},
        {"1920x1080", of(1920, 1080, 200)},
        {"2560x1440", of(2560, 1440, 200)},
    };
}

/// Returns the part of an item that is drawn: its rectangle, within its
/// clip when it has one, and a focus ring's outline round its control.
///
/// @param item the item
/// @return the rectangle
inline kit::Rect drawn_area(const kit::Item& item) {
    kit::Rect area = item.rect;
    if (item.role == kit::Role::focus_ring) {
        const auto* look = std::get_if<kit::FocusRingLook>(&item.look);
        if (look != nullptr && look->around)
            area = kit::grown(area, kit::compact_metrics.focus_inset);
    }
    if (item.clip.width > 0 && item.clip.height > 0)
        area = kit::intersect(area, item.clip);
    return area;
}

/// Returns the part of a control a press reaches.
///
/// @param control the control
/// @return its rectangle, within its clip when it has one
inline kit::Rect press_area(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
inline bool has_room(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// Lists the parts of a layout that lie outside its window: every item's
/// drawn part and every control's press area must lie inside it.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each part outside; none when all lie inside
inline std::vector<std::string> outside_window(const kit::DisplayList& list, kit::Point window) {
    const kit::Rect whole{0, 0, window.x, window.y};
    std::vector<std::string> problems;
    for (const kit::Item& item : list.items) {
        const kit::Rect area = drawn_area(item);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back(
                "item " + std::to_string(static_cast<int>(item.role)) + " \"" + item.text +
                "\" at " + std::to_string(area.x) + "," + std::to_string(area.y) + " " +
                std::to_string(area.width) + "x" + std::to_string(area.height)
            );
    }
    for (const kit::Control& control : list.controls) {
        const kit::Rect area = press_area(control);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back("control " + control.name);
    }
    return problems;
}

/// Lists the texts of a layout that do not fit their rectangles by the
/// kit's measure. A list row cuts its own texts with "...", as the kit's
/// row is declared to; every other text must fit as it is.
///
/// @param list the display list
/// @param fonts the fonts the layout measured in
/// @return a line for each text that does not fit; none when all fit
inline std::vector<std::string>
texts_not_fitting(const kit::DisplayList& list, const kit::Fonts& fonts) {
    const kit::Metrics& metrics = kit::compact_metrics;
    std::vector<std::string> problems;
    const auto check = [&](bool fits, const std::string& what) {
        if (!fits)
            problems.push_back(what);
    };
    const auto width = [&fonts](kit::FontRole role, std::string_view text) {
        return kit::text_width(fonts, role, text);
    };
    for (const kit::Item& item : list.items) {
        switch (item.role) {
        case kit::Role::text:
            check(width(item.font, item.text) <= item.rect.width, "text \"" + item.text + "\"");
            break;
        case kit::Role::button:
            if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
                check(
                    width(look->font, look->caption) <= item.rect.width,
                    "button \"" + look->caption + "\""
                );
            break;
        case kit::Role::chip:
            if (const auto* look = std::get_if<kit::ChipLook>(&item.look))
                check(
                    kit::chip_width(fonts, *look) <= item.rect.width, "chip \"" + look->text + "\""
                );
            break;
        case kit::Role::pill:
            if (const auto* look = std::get_if<kit::PillLook>(&item.look)) {
                // The Library keeps Settings' label gap either side of a
                // pill's words, and each icon as far in from the pill's
                // round end as from its top.
                kit::PillLook plain;
                plain.text = look->text;
                const int32_t icons = (look->tag ? 1 : 0) + (look->icon ? 1 : 0);
                const int32_t icon_inset = (item.rect.height - (kit::pill_height() - 2)) / 2;
                const int32_t wanted = width(kit::FontRole::small, look->text) +
                                       2 * metrics.label_gap + kit::pill_width(fonts, *look) -
                                       kit::pill_width(fonts, plain) + icons * icon_inset;
                check(wanted <= item.rect.width, "pill \"" + look->text + "\"");
            }
            break;
        case kit::Role::link:
            if (const auto* look = std::get_if<kit::LinkLook>(&item.look))
                check(
                    kit::link_width(fonts, *look) <= item.rect.width,
                    "link \"" + look->caption + "\""
                );
            break;
        case kit::Role::radio:
            if (const auto* look = std::get_if<kit::RadioLook>(&item.look))
                check(
                    width(kit::FontRole::regular, look->caption) <=
                        kit::radio_parts(item.rect, !look->caption.empty()).caption.width,
                    "radio \"" + look->caption + "\""
                );
            break;
        case kit::Role::choice:
            if (const auto* look = std::get_if<kit::ChoiceLook>(&item.look))
                check(
                    width(kit::FontRole::regular, look->text) <=
                        item.rect.width - metrics.choice_text_inset - metrics.choice_arrow_room,
                    "drop-down \"" + look->text + "\""
                );
            break;
        case kit::Role::choice_menu:
            if (const auto* look = std::get_if<kit::ChoiceMenuLook>(&item.look))
                for (std::size_t place = 0; place < look->shown.size(); ++place) {
                    const kit::Rect box = kit::choice_item(item.rect, static_cast<int32_t>(place));
                    check(
                        width(kit::FontRole::regular, look->shown[place]) <=
                            box.width - metrics.choice_item_text_inset,
                        "menu item \"" + look->shown[place] + "\""
                    );
                }
            break;
        case kit::Role::header:
            if (const auto* look = std::get_if<kit::HeaderLook>(&item.look)) {
                check(
                    kit::tracked_width(
                        fonts, kit::FontRole::regular, look->title, look->tracking
                    ) <= look->title_width,
                    "header title \"" + look->title + "\""
                );
                check(
                    kit::tracked_width(
                        fonts, kit::FontRole::regular, look->second, look->tracking
                    ) <= look->second_width,
                    "header word \"" + look->second + "\""
                );
                check(
                    width(kit::FontRole::small, look->version) <= look->version_width,
                    "header version \"" + look->version + "\""
                );
                const int32_t title_left = metrics.padding + metrics.mark_side + metrics.header_gap;
                const int32_t words_right =
                    title_left + look->title_width +
                    (look->second.empty() ? 0 : metrics.header_gap + look->second_width);
                const int32_t version_left =
                    look->width - metrics.edge - metrics.padding - look->version_width;
                check(words_right <= version_left, "header words reach the version");
            }
            break;
        case kit::Role::tabs:
            if (const auto* look = std::get_if<kit::TabsLook>(&item.look)) {
                const kit::TabStrip placed = kit::place_tabs(fonts, item.rect, *look);
                if (!placed.overflows) {
                    check(
                        placed.tabs.empty() || kit::wholly_in(placed.tabs.back(), item.rect), "tabs"
                    );
                    check(
                        placed.left_arrow.width == 0 && placed.right_arrow.width == 0, "tabs arrows"
                    );
                } else if (placed.selected < placed.tabs.size()) {
                    check(
                        kit::wholly_in(placed.tabs[placed.selected], placed.view), "tabs selected"
                    );
                }
            }
            break;
        case kit::Role::nav:
            if (const auto* look = std::get_if<kit::NavLook>(&item.look))
                for (const kit::NavEntry& entry : look->entries)
                    check(
                        width(kit::FontRole::regular, entry.caption) <=
                            entry.rect.width - metrics.padding - metrics.list_row_inset,
                        "nav entry \"" + entry.caption + "\""
                    );
            break;
        case kit::Role::heading:
            if (const auto* look = std::get_if<kit::HeadingLook>(&item.look))
                check(
                    kit::tracked_width(
                        fonts, kit::FontRole::small, look->text, metrics.heading_tracking
                    ) <= item.rect.width,
                    "heading \"" + look->text + "\""
                );
            break;
        case kit::Role::field:
            if (const auto* look = std::get_if<kit::SearchLook>(&item.look);
                look != nullptr && look->field.text.empty())
                check(
                    width(kit::FontRole::regular, look->placeholder) <=
                        kit::field_text_rect(item.rect, *look).width,
                    "placeholder \"" + look->placeholder + "\""
                );
            break;
        default:
            break;
        }
    }
    return problems;
}

/// Returns a list's control of a name.
///
/// @param list the display list
/// @param name the name
/// @return the control, or null
inline const kit::Control* named(const kit::DisplayList& list, std::string_view name) {
    return kit::control_of(list, kit::control_named(list, name));
}

} // namespace screen_fixture
