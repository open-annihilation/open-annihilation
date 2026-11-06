// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed game's MAINMENU.GUI layout: totala1.hpi's, which fits
// FrontendX's pipe frames and is the main menu when no overlay is drawn, and
// the rule that picks it: with no overlay drawn, a layout in an archive
// without FrontendX is passed over unless a mod's folder holds it, checked
// over add-on archives, mod folders and loose files made from the installed
// ones. A mod folder's MAINMENU.GUI that moves a button has it drawn and hit
// where the layout says.
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/main_menu_art.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/test/game_assets.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace renderer = oa::ui::frontend_renderer;

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Rect {
    int16_t x{};
    int16_t y{};
    int16_t width{};
    int16_t height{};
};

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

const oa::formats::gaf::Sequence*
sequence_named(const oa::formats::gaf::Archive& archive, std::string_view name) {
    for (const auto& sequence : archive.sequences)
        if (sequence.name == name)
            return &sequence;
    return nullptr;
}

// The frame's covered texels in columns [0, columns) drawn at (x, y) through
// FrontendX's palette.
bool frame_drawn_at(
    const renderer::Surface& surface,
    const renderer::MainMenuResources& menu,
    const oa::formats::gaf::Frame& frame,
    int x,
    int y,
    int columns
) {
    const auto rendered = oa::formats::gaf::render_normal(frame);
    if (!rendered.ok() || !menu.background.palette)
        return false;
    const auto& image = *rendered.frame;
    const auto& palette = *menu.background.palette;
    for (int row = 0; row < image.height; ++row)
        for (int column = 0; column < columns; ++column) {
            const auto source =
                static_cast<std::size_t>(row) * image.width + static_cast<std::size_t>(column);
            if (image.coverage[source] == 0)
                continue;
            const auto color =
                static_cast<std::size_t>(image.pixels[source]) * oa::palette_entry_bytes;
            const auto target = (static_cast<std::size_t>(y + row) * surface.width +
                                 static_cast<std::size_t>(x + column)) *
                                3U;
            if (!std::equal(
                    palette.begin() + static_cast<std::ptrdiff_t>(color),
                    palette.begin() + static_cast<std::ptrdiff_t>(color + 3),
                    surface.rgb.begin() + static_cast<std::ptrdiff_t>(target)
                ))
                return false;
        }
    return true;
}

// Rows [top, bottom] of columns [left, right] show FrontendX unchanged.
bool background_kept(
    const renderer::Surface& surface,
    const renderer::MainMenuResources& menu,
    int left,
    int right,
    int top,
    int bottom
) {
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3U;
            if (!std::equal(
                    surface.rgb.begin() + static_cast<std::ptrdiff_t>(offset),
                    surface.rgb.begin() + static_cast<std::ptrdiff_t>(offset + 3),
                    menu.background.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
                ))
                return false;
        }
    return true;
}

/// Checks totala1.hpi's layout, drawn with COMMONGUI.GAF's buttons in the frames.
///
/// @param assets the installed game's store
void check_base_layout(oa::AssetStore& assets) {
    const auto base = renderer::load_main_menu(assets, renderer::MainMenuLayout::base_game);
    check(has_rect(base, "SINGLE", {139, 393, 96, 20}), "base SINGLE");
    check(has_rect(base, "MULTI", {139, 430, 96, 20}), "base MULTI");
    check(has_rect(base, "INTRO", {409, 393, 96, 20}), "base INTRO");
    check(has_rect(base, "EXIT", {409, 430, 96, 20}), "base EXIT");

    // MAINMENU.GAF has no sequence for the four buttons, so the first panel draw takes
    // COMMONGUI.GAF BUTTONS0's 96x20 group, which starts at frame 12.
    const auto* buttons = sequence_named(base.shared_sprites, "BUTTONS0");
    check(buttons != nullptr && buttons->frames.size() > 12, "COMMONGUI.GAF BUTTONS0");
    if (buttons == nullptr || buttons->frames.size() <= 12)
        return;
    const auto& normal = buttons->frames[12];
    check(normal.width == 96 && normal.height == 20, "BUTTONS0 frame 12 is 96x20");
    constexpr int caption_free_columns = 8;
    const auto drawn = renderer::render_main_menu(base);
    for (const auto& origin : {Rect{139, 393}, Rect{139, 430}, Rect{409, 393}, Rect{409, 430}})
        check(
            frame_drawn_at(drawn, base, normal, origin.x, origin.y, caption_free_columns),
            "each base button is BUTTONS0 frame 12 at its GUI rectangle"
        );
    // Nothing is drawn beside the frames' slots: FrontendX stays as it is
    // there.
    check(background_kept(drawn, base, 82, 130, 393, 449), "nothing drawn left of the frames");
    check(background_kept(drawn, base, 512, 559, 393, 449), "nothing drawn right of the frames");
}

// The resources the rule cases read, by their paths in the store.
constexpr std::string_view kLayout = "guis/mainmenu.gui";
constexpr std::string_view kBackground = "bitmaps/frontendx.pcx";

/// A folder that stands in for an installation's loose files and holds the
/// archives a case mounts; removed with its contents when it goes.
class ScratchFolder {
  public:

    /// Creates an empty folder under the system's temporary folder.
    ScratchFolder() {
        static int created = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("oa-main-menu-layout-" + std::to_string(stamp) + "-" + std::to_string(created++));
        std::filesystem::create_directories(path_);
    }

    ~ScratchFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    /// Writes a file below the folder, creating its folders.
    ///
    /// @param name '/'-separated path below the folder
    /// @param bytes the file's content
    /// @return the file's path
    std::filesystem::path write(std::string_view name, const std::vector<uint8_t>& bytes) const {
        const auto target = path_ / std::filesystem::path(name);
        std::filesystem::create_directories(target.parent_path());
        std::ofstream stream(target, std::ios::binary);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
        return target;
    }

    /// Returns the folder.
    ///
    /// @return its path
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:

    std::filesystem::path path_;
};

/// Returns a copy of a MAINMENU.GUI with its two button columns moved.
///
/// @param layout the installed layout, whose columns are at x 139 and 409
/// @param left the new x of SINGLE and MULTI
/// @param right the new x of INTRO and EXIT
/// @return the moved layout; empty when a column is not where it should be
std::vector<uint8_t> moved_layout(const std::vector<uint8_t>& layout, int left, int right) {
    std::string text(layout.begin(), layout.end());
    const auto move = [&text](std::string_view from, int to) {
        int moved = 0;
        const std::string replacement = "xpos=" + std::to_string(to) + ";";
        for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
            text.replace(at, from.size(), replacement);
            at += replacement.size();
            ++moved;
        }
        return moved == 2;
    };
    if (!move("xpos=139;", left) || !move("xpos=409;", right))
        return {};
    return {text.begin(), text.end()};
}

/// Returns a copy of a MAINMENU.GUI with one gadget moved.
///
/// @param layout the installed layout
/// @param name the gadget's name
/// @param x the gadget's new x
/// @param y the gadget's new y
/// @return the moved layout; empty when the gadget or its place is missing
std::vector<uint8_t>
moved_gadget(const std::vector<uint8_t>& layout, std::string_view name, int x, int y) {
    std::string text(layout.begin(), layout.end());
    const auto named = text.find("name=" + std::string(name) + ";");
    if (named == std::string::npos)
        return {};
    const auto place = [&text, named](std::string_view field, int value) {
        const auto at = text.find(field, named);
        const auto end = at == std::string::npos ? at : text.find(';', at);
        if (end == std::string::npos)
            return false;
        text.replace(at + field.size(), end - at - field.size(), std::to_string(value));
        return true;
    };
    if (!place("xpos=", x) || !place("ypos=", y))
        return {};
    return {text.begin(), text.end()};
}

/// Returns an archive that holds some of the main menu's files.
///
/// @param files each file's path in the archive and its content
/// @return the archive's bytes
std::vector<uint8_t>
archive_of(std::initializer_list<std::pair<std::string_view, const std::vector<uint8_t>*>> files) {
    std::vector<oa::HpiWriteFile> entries;
    for (const auto& [path, bytes] : files)
        entries.push_back({std::string(path), *bytes});
    return oa::write_hpi(entries);
}

/// Opens a store over a scratch folder's loose files, with archives mounted
/// above the installation's.
///
/// @param folder the scratch folder, whose loose files come first
/// @param archives archives in the folder, mounted in this order first
/// @param installed the installed game's store, whose archives follow
/// @return the store
oa::AssetStore store_over(
    const ScratchFolder& folder,
    std::initializer_list<std::filesystem::path> archives,
    const oa::AssetStore& installed
) {
    oa::AssetStore assets(folder.path());
    for (const auto& archive : archives)
        assets.mount(archive);
    for (const auto& archive : installed.mount_paths())
        assets.mount(archive);
    return assets;
}

/// Opens a store over a mod's folder layered over a game folder, with the
/// mod's archives mounted first, then the game folder's, then the
/// installation's.
///
/// @param mod the mod's scratch folder, whose loose files come first
/// @param mod_archives archives in the mod's folder
/// @param game the scratch game folder
/// @param game_archives archives in the game folder
/// @param installed the installed game's store, whose archives follow
/// @return the store
oa::AssetStore store_with_mod(
    const ScratchFolder& mod,
    std::initializer_list<std::filesystem::path> mod_archives,
    const ScratchFolder& game,
    std::initializer_list<std::filesystem::path> game_archives,
    const oa::AssetStore& installed
) {
    oa::AssetStore assets(std::vector<std::filesystem::path>{mod.path(), game.path()});
    for (const auto& archive : mod_archives)
        assets.mount(archive);
    for (const auto& archive : game_archives)
        assets.mount(archive);
    for (const auto& archive : installed.mount_paths())
        assets.mount(archive);
    return assets;
}

/// Returns the x of the main menu's SINGLE button.
///
/// @param menu the loaded main menu
/// @return its x, or -1 when the layout has no SINGLE
int single_x(const renderer::MainMenuResources& menu) {
    for (const auto& gadget : menu.layout.gadgets)
        if (gadget.common.name == "SINGLE")
            return gadget.common.x;
    return -1;
}

/// Checks which MAINMENU.GUI each layout takes in one case.
///
/// @param assets the case's store
/// @param without_overlay SINGLE's x expected with no overlay drawn
/// @param with_overlay SINGLE's x expected with an overlay drawn
/// @param what the case, for the failure line
/// @param mod_folder the played mod's folder; empty when no mod is played
void check_case(
    oa::AssetStore& assets,
    int without_overlay,
    int with_overlay,
    const char* what,
    const std::filesystem::path& mod_folder = {}
) {
    const auto base =
        renderer::load_main_menu(assets, renderer::MainMenuLayout::base_game, mod_folder);
    const auto overlaid = renderer::load_main_menu(assets, renderer::MainMenuLayout::with_overlay);
    if (single_x(base) != without_overlay || single_x(overlaid) != with_overlay) {
        std::fprintf(
            stderr,
            "FAIL: %s: SINGLE at x %d without an overlay (expected %d), %d with one (expected "
            "%d)\n",
            what,
            single_x(base),
            without_overlay,
            single_x(overlaid),
            with_overlay
        );
        ++failures;
    }
}

/// Checks the rule over add-on archives, mod folders and loose files made
/// from the installed layout and background: with no overlay a layout in an
/// archive that holds no FrontendX and lies outside a mod's folder is passed
/// over, and a loose layout comes first; with an overlay drawn the top copy
/// applies.
///
/// @param installed the installed game's store
void check_layout_rule(const oa::AssetStore& installed) {
    const auto installed_archive = installed.providing_archive(kBackground);
    check(installed_archive.has_value(), "the installation's FrontendX comes from an archive");
    if (!installed_archive)
        return;
    const auto layout =
        oa::open_hpi_file(*installed_archive).value.value().read(kLayout).value.value();
    const auto background = installed.read(kBackground).bytes;
    const auto overlay_layout = moved_layout(layout, 82, 464);
    const auto own_layout = moved_layout(layout, 100, 450);
    const auto loose_layout = moved_layout(layout, 120, 420);
    check(!overlay_layout.empty(), "the installed layout's columns are at x 139 and 409");
    if (overlay_layout.empty())
        return;

    {
        // An add-on archive with a layout and no background is passed over.
        ScratchFolder folder;
        const auto addon = folder.write("layout.hpi", archive_of({{kLayout, &overlay_layout}}));
        auto assets = store_over(folder, {addon}, installed);
        check_case(assets, 139, 82, "a layout without a background");
    }
    {
        // An add-on archive with its own background brings its own layout,
        // whatever archive above it carries another.
        ScratchFolder folder;
        const auto addon = folder.write("layout.hpi", archive_of({{kLayout, &overlay_layout}}));
        const auto art = folder.write(
            "art.hpi", archive_of({{kLayout, &own_layout}, {kBackground, &background}})
        );
        auto assets = store_over(folder, {addon, art}, installed);
        check_case(assets, 100, 82, "an add-on background with its own layout");
    }
    {
        // A loose layout comes first, as loose files do.
        ScratchFolder folder;
        const auto art = folder.write(
            "art.hpi", archive_of({{kLayout, &own_layout}, {kBackground, &background}})
        );
        (void)folder.write(kLayout, loose_layout);
        auto assets = store_over(folder, {art}, installed);
        check_case(assets, 120, 120, "a loose layout");
    }
    {
        // A background without a layout keeps the layout of the background
        // it stands over, not the one made for an overlay.
        ScratchFolder folder;
        const auto addon = folder.write("layout.hpi", archive_of({{kLayout, &overlay_layout}}));
        const auto art = folder.write("background.hpi", archive_of({{kBackground, &background}}));
        auto assets = store_over(folder, {addon, art}, installed);
        check_case(assets, 139, 82, "an add-on background without a layout");
    }
    {
        // The same from a mod's folder over a game folder that holds the
        // layout made for an overlay.
        ScratchFolder mod;
        ScratchFolder game;
        const auto art = mod.write("art.ccx", archive_of({{kBackground, &background}}));
        const auto addon = game.write("layout.ccx", archive_of({{kLayout, &overlay_layout}}));
        auto assets = store_with_mod(mod, {art}, game, {addon}, installed);
        check_case(assets, 139, 82, "a mod's background without a layout", mod.path());
    }
    {
        // An expansion's folder layered between a mod's and the game's is
        // the game's own: its layout made for an overlay is passed over, so a
        // mod with art of its own and no layout keeps the installed one.
        ScratchFolder mod;
        ScratchFolder expansion;
        ScratchFolder game;
        const auto art = mod.write("art.ccx", archive_of({{kBackground, &background}}));
        const auto addon = expansion.write("layout.ccx", archive_of({{kLayout, &overlay_layout}}));
        oa::AssetStore assets(
            std::vector<std::filesystem::path>{mod.path(), expansion.path(), game.path()}
        );
        assets.mount(art);
        assets.mount(addon);
        for (const auto& archive : installed.mount_paths())
            assets.mount(archive);
        check_case(assets, 139, 82, "a mod's background over an expansion's layout", mod.path());
        const auto menu =
            renderer::load_main_menu(assets, renderer::MainMenuLayout::base_game, mod.path());
        check(
            has_rect(menu, "SINGLE", {139, 393, 96, 20}),
            "a mod over an expansion's layout keeps SINGLE at 139,393"
        );
        check(
            renderer::from_mod_folder(assets, kBackground, mod.path()) &&
                !renderer::from_mod_folder(assets, kLayout, mod.path()),
            "the mod's background is the mod's and the expansion's layout is not"
        );
        // With no mod played the expansion's layout is passed over too.
        oa::AssetStore plain(std::vector<std::filesystem::path>{expansion.path(), game.path()});
        plain.mount(addon);
        for (const auto& archive : installed.mount_paths())
            plain.mount(archive);
        check_case(plain, 139, 82, "an expansion's layout with no mod");
        check(!renderer::from_mod_folder(plain, kBackground, {}), "with no mod nothing is a mod's");
        const auto base = renderer::base_game_main_menu_background(assets, mod.path());
        check(
            base.has_value() && base->source == *installed_archive,
            "the base game's FrontendX is the installation's, past the mod's"
        );
    }
}

/// Checks a mod folder whose MAINMENU.GUI, in an archive without FrontendX,
/// moves SINGLE: the main menu takes it with and without an overlay, draws
/// SINGLE at its new place and nothing at its old one, and a click there
/// lands on it.
///
/// @param installed the installed game's store
void check_mod_layout(const oa::AssetStore& installed) {
    const auto installed_archive = installed.providing_archive(kBackground);
    if (!installed_archive)
        return;
    const auto layout =
        oa::open_hpi_file(*installed_archive).value.value().read(kLayout).value.value();
    const auto overlay_layout = moved_layout(layout, 82, 464);
    // SINGLE's place in the installed layout, and the one the mod gives it.
    constexpr Rect installed_single{139, 393, 96, 20};
    constexpr Rect moved{272, 356, 96, 20};
    const auto mod_layout = moved_gadget(layout, "SINGLE", moved.x, moved.y);
    check(!mod_layout.empty(), "the installed layout names SINGLE with its place");
    if (overlay_layout.empty() || mod_layout.empty())
        return;
    ScratchFolder mod;
    ScratchFolder game;
    const auto own = mod.write("layout.ufo", archive_of({{kLayout, &mod_layout}}));
    const auto addon = game.write("layout.ccx", archive_of({{kLayout, &overlay_layout}}));
    auto assets = store_with_mod(mod, {own}, game, {addon}, installed);
    check_case(assets, moved.x, moved.x, "a mod's own layout", mod.path());

    const auto menu =
        renderer::load_main_menu(assets, renderer::MainMenuLayout::base_game, mod.path());
    check(has_rect(menu, "SINGLE", moved), "the mod's SINGLE keeps its layout's rectangle");
    check(has_rect(menu, "MULTI", {139, 430, 96, 20}), "the mod's MULTI stays in place");
    const auto* buttons = sequence_named(menu.shared_sprites, "BUTTONS0");
    if (buttons == nullptr || buttons->frames.size() <= 12)
        return;
    constexpr int caption_free_columns = 8;
    const auto drawn = renderer::render_main_menu(menu);
    check(
        frame_drawn_at(drawn, menu, buttons->frames[12], moved.x, moved.y, caption_free_columns),
        "the mod's SINGLE is drawn where its layout places it"
    );
    check(
        background_kept(
            drawn,
            menu,
            installed_single.x,
            installed_single.x + installed_single.width - 1,
            installed_single.y,
            installed_single.y + installed_single.height - 1
        ),
        "nothing is drawn at SINGLE's old place"
    );

    const oa::ui::gui_input::MenuObject object{menu.layout.gadgets, -1};
    const auto hit =
        oa::ui::gui_input::hit_test(object, moved.x + moved.width / 2, moved.y + moved.height / 2);
    check(
        hit && menu.layout.gadgets[*hit].common.name == "SINGLE",
        "a click on the mod's SINGLE lands on it"
    );
    check(
        !oa::ui::gui_input::hit_test(
            object,
            installed_single.x + installed_single.width / 2,
            installed_single.y + installed_single.height / 2
        ),
        "a click on SINGLE's old place lands on nothing"
    );
}

} // namespace

int main() {
    oa::AssetStore assets = oa::test::require_game_assets("the installed main menu layout");
    try {
        check_base_layout(assets);
        check_layout_rule(assets);
        check_mod_layout(assets);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0)
        return 1;
    std::puts(
        "main menu layout: totala1.hpi 139/409; with no overlay a layout without a background "
        "passed over in five cases of add-on archives, mod folders and loose files; a mod "
        "folder's moved SINGLE drawn and hit in its place"
    );
    return 0;
}
