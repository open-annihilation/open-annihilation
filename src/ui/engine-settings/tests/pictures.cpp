// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Writes the manual's pictures of the settings dialog: each section alone,
// unlocked, at Compact and 2x (960 by 648), in the installed game's fonts,
// and Controls, Graphics, Downloads and System scrolled to their ends.
// Updates is drawn twice: up to date, then with a release offered.
//
// Usage: oa-ui-engine-settings-pictures DIRECTORY
//
// The data test ui-engine-settings-pictures runs this into a scratch folder.
// Without OA_GAME_DIR it skips, as the other game-data tests do.

#include "oa/test/game_assets.hpp"
#include "oa/ui/engine_settings/dialog.hpp"

#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace png = oa::formats::png;
namespace renderer = oa::ui::frontend_renderer;
namespace settings = oa::ui::engine_settings;

using settings::Dialog;
using settings::Page;

constexpr int32_t kScale = 2;
constexpr std::string_view kVersion = "v0.8.0";

/// The app icon, cropped to the pixels that show, as the dialog's header
/// draws it. The file is the plates the window icon is made from.
struct AppIcon {
    std::vector<uint8_t> pixels{};
    renderer::RgbaPicture picture{};
};

/// Walks up from a directory until a relative path is a file.
std::filesystem::path
climb_for(std::filesystem::path start, const std::filesystem::path& relative) {
    std::error_code ignored;
    for (int32_t step = 0; step < 12 && !start.empty(); ++step) {
        const std::filesystem::path candidate = start / relative;
        if (std::filesystem::is_regular_file(candidate, ignored))
            return candidate;
        const std::filesystem::path parent = start.parent_path();
        if (parent == start)
            break;
        start = parent;
    }
    return {};
}

/// The shipped app icon. Empty when the file cannot be read.
AppIcon load_app_icon() {
    const std::filesystem::path relative =
        std::filesystem::path("branding") / "open-annihilation-256.png";
    std::error_code ignored;
    std::filesystem::path file;
#ifdef OA_BRANDING_DIR
    const std::filesystem::path given =
        std::filesystem::path(OA_BRANDING_DIR) / "open-annihilation-256.png";
    if (std::filesystem::is_regular_file(given, ignored))
        file = given;
#endif
    if (file.empty())
        file = climb_for(std::filesystem::current_path(ignored), relative);
    if (file.empty())
        file = climb_for(oa::platform::text_font::bundled_font_directory(), relative);
    if (file.empty()) {
        std::cerr << "pictures: the app icon was not found\n";
        return {};
    }
    std::ifstream input(file, std::ios::binary);
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (!input || length <= 0) {
        std::cerr << "pictures: the app icon could not be read\n";
        return {};
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (!input || !png::has_signature(bytes)) {
        std::cerr << "pictures: the app icon could not be read\n";
        return {};
    }
    png::Messages messages;
    png::Info info;
    if (!png::read_info(bytes, messages, &info))
        return {};
    const png::Header& header = info.header;
    if (header.color_type != png::ColorType::rgb_alpha || header.bit_depth != 8 ||
        header.width == 0 || header.height == 0)
        return {};
    const std::size_t stride = png::row_bytes(header, {});
    if (stride == 0 || header.height > std::numeric_limits<std::size_t>::max() / stride)
        return {};
    std::vector<uint8_t> pixels(stride * header.height);
    const png::Progress progress = png::read_image(bytes, info, {}, messages, pixels);
    if (progress == png::Progress::none)
        return {};
    const std::size_t row_bytes = std::size_t{header.width} * 4U;
    if (pixels.size() / row_bytes < header.height)
        return {};
    uint32_t left = header.width;
    uint32_t top = header.height;
    uint32_t right = 0;
    uint32_t bottom = 0;
    for (uint32_t y = 0; y < header.height; ++y) {
        for (uint32_t x = 0; x < header.width; ++x) {
            if (pixels[y * row_bytes + std::size_t{x} * 4U + 3U] == 0)
                continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x + 1);
            bottom = std::max(bottom, y + 1);
        }
    }
    if (left >= right || top >= bottom)
        return {};
    AppIcon icon;
    icon.picture.width = right - left;
    icon.picture.height = bottom - top;
    const std::size_t part_row = std::size_t{icon.picture.width} * 4U;
    icon.pixels.reserve(part_row * icon.picture.height);
    for (uint32_t y = top; y < bottom; ++y) {
        const auto first =
            pixels.begin() + static_cast<std::ptrdiff_t>(y * row_bytes + std::size_t{left} * 4U);
        icon.pixels.insert(icon.pixels.end(), first, first + static_cast<std::ptrdiff_t>(part_row));
    }
    icon.picture.pixels = icon.pixels;
    return icon;
}

/// The file docs/images/settings uses for a section's top.
std::string picture_name(Page page) {
    const std::string word(settings::page_word(page));
    if (word == "controls" || word == "language" || word == "graphics" || word == "downloads" ||
        word == "system" || word == "updates" || word == "accessibility")
        return word + "-1.png";
    return word + ".png";
}

/// Fills the rows a host supplies, at the defaults the manual shows.
void dress(Dialog& dialog) {
    // The main menu lists More in the Library… and REGISTRIES IN THE LIBRARY….
    dialog.library_link = true;
    dialog.user_folder = "/Users/player/Documents/Open Annihilation";
    dialog.game_files_summary = "3.1c · Core Contingency · Battle Tactics · music";
    dialog.game_files_sizes = "1.1 GB · 37 GB free on this tablet";
    dialog.game_files_location = "In the file manager: Open Annihilation \u203A Total Annihilation";
    dialog.game_files_device = "tablet";

    settings::InstallIdRow install;
    install.registry_id = "core-prime";
    install.registry_name = "Core Prime";
    install.on = true;
    settings::DownloadsOffer downloads;
    downloads.install_ids.push_back(std::move(install));
    settings::set_downloads_offer(dialog, downloads);

    const auto bundle = [](std::string_view id, std::string_view name, std::string_view size) {
        settings::GameContentRow row;
        row.id = std::string(id);
        row.name = std::string(name);
        row.size = std::string(size);
        row.check = "All files match";
        return row;
    };
    dialog.game_content.push_back(bundle("totala", "Total Annihilation", "188 MB"));
    dialog.game_content.push_back(bundle("cc", "Core Contingency", "42 MB"));
    dialog.game_content.push_back(bundle("bt", "Battle Tactics", "28 MB"));
    dialog.game_content.push_back(bundle("ta-music", "Total Annihilation Music", "61 MB"));

    dialog.updates.version_line = "Open Annihilation 0.8.0";
    dialog.updates.status = "Up to date \u00b7 Stable \u00b7 checked today at 09:00";
    dialog.updates.can_check = true;
    dialog.updates.offered = false;
    dialog.updates.mode = static_cast<int32_t>(settings::UpdateCheck::automatically);
    // Two of the registry's latchable actions are on, so the row reads "2 of 7".
    dialog.latched_count = 2;
}

Dialog section_dialog(Page page) {
    settings::Inputs inputs;
    inputs.players_own_profile = true;
    inputs.macos = true;
    const settings::EngineSettings defaults = settings::default_settings(inputs);
    Dialog dialog;
    settings::open_dialog(
        dialog,
        defaults,
        defaults,
        {},
        kVersion,
        page,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        true,
        true,
        true
    );
    dress(dialog);
    return dialog;
}

bool write_png(const renderer::Surface& surface, const std::filesystem::path& path) {
    png::Header header;
    header.width = surface.width;
    header.height = surface.height;
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, surface.rgb}, &file))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    return static_cast<bool>(output);
}

bool draw_section(
    const Dialog& dialog,
    const settings::DialogFonts& fonts,
    const renderer::RgbaPicture& icon,
    const std::filesystem::path& path
) {
    const int32_t width = settings::dialog_width * kScale;
    const int32_t height = settings::dialog_height * kScale;
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    settings::draw_dialog(surface, {0, 0, kScale}, dialog, fonts, icon);
    if (surface.width != 960 || surface.height != 648) {
        std::cerr << "pictures: " << path.filename().string() << " is " << surface.width << " by "
                  << surface.height << ", not 960 by 648\n";
        return false;
    }
    if (!write_png(surface, path)) {
        std::cerr << "pictures: cannot write " << path.string() << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: oa-ui-engine-settings-pictures DIRECTORY\n";
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    oa::AssetStore assets = oa::test::require_game_assets("the settings pictures");
    const settings::DialogFonts fonts = settings::load_dialog_fonts(assets);
    const AppIcon icon = load_app_icon();
    if (icon.pixels.empty())
        return 1;

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "pictures: cannot make " << directory.string() << '\n';
        return 1;
    }

    bool ok = true;
    for (const Page page : settings::dialog_pages(settings::DialogKind::engine, true, true, true)) {
        Dialog dialog = section_dialog(page);
        if (!draw_section(dialog, fonts, icon.picture, directory / picture_name(page)))
            ok = false;
        // Updates again, with a release offered: UPDATE… in place of CHECK NOW.
        // That picture is not the section scrolled to its end.
        if (page == Page::updates) {
            dialog.updates.offered = true;
            dialog.updates.can_check = true;
            dialog.updates.status = "0.8.7 is out \u00b7 Stable \u00b7 checked today at 09:00";
            if (!draw_section(dialog, fonts, icon.picture, directory / "updates-2.png"))
                ok = false;
        }
        // Controls, Engine Limits, Graphics, Downloads, System and
        // Accessibility scrolled to their ends: Window edge and Edge
        // stickiness, Weapon types and Sides under Set by the mod, the
        // water sliders down to Naval reflections, the downloads' last
        // rows, the file-type switches down to Launch files, and Edge
        // scrolling.
        if (page == Page::controls || page == Page::common_tweaks || page == Page::graphics ||
            page == Page::downloads || page == Page::system || page == Page::accessibility) {
            dialog.scroll[static_cast<std::size_t>(page)] = settings::scroll_limit(dialog);
            const std::string end = std::string(settings::page_word(page)) + "-2.png";
            if (!draw_section(dialog, fonts, icon.picture, directory / end))
                ok = false;
        }
    }
    return ok ? 0 : 1;
}
