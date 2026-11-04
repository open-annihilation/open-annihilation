// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's layout under the Boneyards overlay, on the BY1117a install
// OA_GAME_DIR names: by.ccx carries its own MAINMENU.GUI, which shadows
// totala1.hpi's, beside the overlay's art. Its four buttons sit outside the
// pipe frames, under the overlay, and it shares the base layout's other
// gadgets.
#include "oa/boneyards_test/installs.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>

namespace {

namespace renderer = oa::ui::frontend_renderer;

int failures = 0;

/// Records a failed case.
///
/// @param condition what must hold
/// @param what the case, printed as "FAIL: <what>" when it does not hold
void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A gadget's rectangle on the 640x480 canvas.
struct Rect {
    int16_t x{};
    int16_t y{};
    int16_t width{};
    int16_t height{};
};

/// Tests whether a layout's first gadget of a name has a rectangle.
///
/// @param menu the loaded main menu
/// @param name the gadget's name
/// @param expected its rectangle
/// @return true when the gadget exists and has exactly that rectangle
bool has_rect(const renderer::MainMenuResources& menu, std::string_view name, Rect expected) {
    for (const auto& gadget : menu.layout.gadgets) {
        if (gadget.common.name != name)
            continue;
        const auto& common = gadget.common;
        return common.x == expected.x && common.y == expected.y && common.width == expected.width &&
               common.height == expected.height;
    }
    return false;
}

/// Compares two file names ignoring ASCII case.
///
/// @param a a name
/// @param b another
/// @return true when they differ only in case
bool same_name_ignoring_case(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        const auto upper = [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        };
        return upper(x) == upper(y);
    });
}

/// Tests whether the store mounted the overlay's archive.
///
/// @param assets the installed game's store
/// @return true when one mount is by.ccx
bool mounts_overlay_archive(const oa::AssetStore& assets) {
    return std::any_of(
        assets.mount_paths().begin(), assets.mount_paths().end(), [](const auto& path) {
            return same_name_ignoring_case(
                path.filename().string(), oa::boneyards_test::kBoneyardsArchive
            );
        }
    );
}

/// Checks the base game's layout, totala1.hpi's, whose buttons sit in the pipe frames.
///
/// @param assets the installed game's store
void check_base_layout(oa::AssetStore& assets) {
    const auto base = renderer::load_main_menu(assets, renderer::MainMenuLayout::base_game);
    check(has_rect(base, "SINGLE", {139, 393, 96, 20}), "base SINGLE");
    check(has_rect(base, "MULTI", {139, 430, 96, 20}), "base MULTI");
    check(has_rect(base, "INTRO", {409, 393, 96, 20}), "base INTRO");
    check(has_rect(base, "EXIT", {409, 430, 96, 20}), "base EXIT");
}

/// Checks by.ccx's layout under its overlay.
///
/// It shares Credits and DebugString with totala1.hpi's.
///
/// @param assets the installed game's store
void check_overlay_layout(oa::AssetStore& assets) {
    const auto by_archive = assets.providing_archive("guis/mainmenu.gui");
    check(
        by_archive && same_name_ignoring_case(
                          by_archive->filename().string(), oa::boneyards_test::kBoneyardsArchive
                      ),
        "by.ccx's MAINMENU.GUI shadows totala1.hpi's"
    );
    check(
        assets.providing_archive("anims/main_console.gaf") == by_archive,
        "the by.ccx overlay art comes from the same archive"
    );

    const auto overlaid = renderer::load_main_menu(assets, renderer::MainMenuLayout::with_overlay);
    check(has_rect(overlaid, "SINGLE", {82, 393, 96, 20}), "overlay SINGLE");
    check(has_rect(overlaid, "MULTI", {82, 430, 96, 20}), "overlay MULTI");
    check(has_rect(overlaid, "INTRO", {464, 393, 96, 20}), "overlay INTRO");
    check(has_rect(overlaid, "EXIT", {464, 430, 96, 20}), "overlay EXIT");

    const auto base = renderer::load_main_menu(assets, renderer::MainMenuLayout::base_game);
    check(base.layout.gadgets.size() == overlaid.layout.gadgets.size(), "same gadgets");
    for (std::size_t index = 0; index < base.layout.gadgets.size(); ++index) {
        const auto& ours = base.layout.gadgets[index].common;
        const auto& theirs = overlaid.layout.gadgets[index].common;
        if (ours.name == "Credits" || ours.name == "DebugString")
            check(
                ours.x == theirs.x && ours.y == theirs.y && ours.width == theirs.width &&
                    ours.height == theirs.height,
                "Credits and DebugString are shared by both layouts"
            );
    }
}

} // namespace

int main() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the main menu layout under the Boneyards overlay");
    oa::AssetStore assets = oa::test::open_game_assets(root);
    if (!mounts_overlay_archive(assets)) {
        std::fprintf(stderr, "FAIL: the install's by.ccx is not mounted\n");
        return 1;
    }
    try {
        check_base_layout(assets);
        check_overlay_layout(assets);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0)
        return 1;
    std::puts("main menu layouts: totala1.hpi 139/409, by.ccx 82/464 under the overlay");
    return 0;
}
