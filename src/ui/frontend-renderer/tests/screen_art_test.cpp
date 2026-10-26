// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Screens laid out over their art, as the match's side column lays out each
// of its pages (load_screen_layout over load_screen_art): for the installed
// game's Arm pages and in-game menu, the same layout, art and picture as
// load_screen gives for the same names, with the page's GUI file the only
// file read.
#include "oa/ui/frontend_renderer.hpp"
#include "oa/test/game_assets.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace renderer = oa::ui::frontend_renderer;

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

/// The names the match's side column loads an Arm page with
/// (Runtime::match_hud_art).
renderer::ScreenAssetNames arm_column(std::string layout) {
    return {
        std::move(layout),
        "bitmaps/armguisidetile.pcx",
        "palettes/guipal.pal",
        "anims/commongui.gaf",
        "anims/ARMINT.GAF"
    };
}

bool same_frames(
    const std::vector<oa::formats::gaf::Frame>& a, const std::vector<oa::formats::gaf::Frame>& b
) {
    if (a.size() != b.size())
        return false;
    for (std::size_t index = 0; index < a.size(); ++index)
        if (a[index].width != b[index].width || a[index].height != b[index].height ||
            a[index].pixels != b[index].pixels || a[index].coverage != b[index].coverage ||
            !same_frames(a[index].layers, b[index].layers))
            return false;
    return true;
}

bool same_sprites(const oa::formats::gaf::Archive& a, const oa::formats::gaf::Archive& b) {
    if (a.sequences.size() != b.sequences.size())
        return false;
    for (std::size_t index = 0; index < a.sequences.size(); ++index)
        if (a.sequences[index].name != b.sequences[index].name ||
            !same_frames(a.sequences[index].frames, b.sequences[index].frames))
            return false;
    return true;
}

void check_page(
    oa::AssetStore& assets, const renderer::ScreenResources& art, const std::string& page
) {
    const auto loaded = renderer::load_screen(assets, arm_column(page));
    std::vector<std::string> read;
    assets.observe_lookups({&read, [](void* context, std::string_view name) {
                                static_cast<std::vector<std::string>*>(context)->emplace_back(name);
                            }});
    const auto laid = renderer::load_screen_layout(assets, art, page);
    assets.observe_lookups({});
    check(
        read == std::vector<std::string>{page},
        page + ": laid out over its art, it read more than its GUI file"
    );
    check(
        !laid.layout.gadgets.empty() && laid.layout == loaded.layout, page + ": its layout differs"
    );
    check(
        laid.background.width == loaded.background.width &&
            laid.background.height == loaded.background.height &&
            laid.background.rgb == loaded.background.rgb &&
            laid.gui_palette == loaded.gui_palette && laid.light_table == loaded.light_table &&
            laid.shade_table == loaded.shade_table && laid.game_palette == loaded.game_palette,
        page + ": its background, palettes or tables differ"
    );
    check(
        same_sprites(laid.sprites, loaded.sprites) &&
            same_sprites(laid.shared_sprites, loaded.shared_sprites) &&
            same_sprites(laid.global_sprites, loaded.global_sprites),
        page + ": its sprites differ"
    );
    const auto drawn = renderer::render_screen(laid);
    const auto expected = renderer::render_screen(loaded);
    check(
        drawn.width == expected.width && drawn.height == expected.height &&
            drawn.rgb == expected.rgb,
        page + ": it is drawn otherwise"
    );
}

} // namespace

int main() {
    oa::AssetStore assets = oa::test::require_game_assets("the side column's pages over their art");
    try {
        const auto art = renderer::load_screen_art(assets, arm_column({}));
        check(art.layout.gadgets.empty(), "the art came with a layout");
        for (const char* page :
             {"guis/ARMMAIN2.GUI", "guis/ARMGEN.GUI", "guis/ARMCOM1.GUI", "guis/ARMOPT.GUI"})
            check_page(assets, art, page);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0)
        return 1;
    std::puts(
        "screen art: ARMMAIN2, ARMGEN, ARMCOM1 and ARMOPT laid out over the Arm column's art as "
        "loaded whole, each reading only its GUI file"
    );
    return 0;
}
