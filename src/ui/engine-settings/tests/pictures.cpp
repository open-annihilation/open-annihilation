// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Writes the manual's pictures of the settings dialog: each section alone,
// unlocked, at Compact and 2x (960 by 648), in the installed game's fonts,
// and Controls, Graphics, Downloads and System scrolled to their ends.
// Updates is the update centre's card: up to date (updates-1), a release
// offered (updates-2), and the section's end with Previous version
// (updates-3). An empty card heading would leave the older status row.
//
// Usage: oa-ui-engine-settings-pictures DIRECTORY [--classes] [--modern FONTS]
//
// With --classes it writes, in place of the manual's pictures, every section
// at each size class, Compact, Regular and Large, at 2x, named
// <section>-<class>.png, and <section>-<class>-end.png for one scrolled to
// its end: the key layout card naming four changes of the player's own, and
// the practice pad with Sticky modifiers and Hold to confirm on and Shift
// tapped.
//
// With --modern it draws in the modern look, Barlow from the fonts folder
// FONTS (the build's staged fonts, or local/deps/text-fonts), and shows Look
// at Modern; without it, in the classic look, the game's fonts.
//
// The data test ui-engine-settings-pictures runs this into a scratch folder.
// Without OA_GAME_DIR it skips, as the other game-data tests do.

#include "oa/test/game_assets.hpp"
#include "oa/ui/engine_settings/dialog.hpp"

#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace png = oa::formats::png;
namespace renderer = oa::ui::frontend_renderer;
namespace settings = oa::ui::engine_settings;

using settings::Dialog;
using settings::Page;

constexpr int32_t kScale = 2;
constexpr std::string_view kVersion = "v0.8.7";

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

/// Adds one of the update card's actions, already as the manual shows it.
///
/// @param dialog the dialog
/// @param id the button's word to automation
/// @param caption the button's words
void add_update_action(Dialog& dialog, std::string id, std::string caption) {
    settings::UpdateCardAction action;
    action.id = std::move(id);
    action.caption = std::move(caption);
    dialog.updates.actions.push_back(std::move(action));
}

/// Sets Look from the fonts: Modern while they hold Barlow, otherwise Classic.
///
/// @param dialog the dialog
/// @param fonts the fonts the picture is drawn in
void apply_look(Dialog& dialog, const settings::DialogFonts& fonts) {
    dialog.chosen.look =
        oa::ui::kit::modern_look(fonts) ? settings::Look::modern : settings::Look::classic;
}

/// Fills the rows a host supplies, at the defaults the manual shows.
/// Updates is the card: Installed 0.8.7, Up to date, Check now.
void dress(Dialog& dialog) {
    // The main menu lists More in the Library… and Registries in the Library….
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

    dialog.updates.version_line = "Open Annihilation 0.8.7";
    dialog.updates.status = "Up to date";
    dialog.updates.can_check = true;
    dialog.updates.offered = false;
    dialog.updates.mode = static_cast<int32_t>(settings::UpdateCheck::automatically);
    dialog.updates.card_installed = "Installed 0.8.7";
    dialog.updates.card_status = "Up to date";
    add_update_action(dialog, "check", "Check now");
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
    settings::DialogFonts drawn = fonts;
    drawn.size_class = dialog.size_class;
    const oa::ui::kit::Metrics& metrics = oa::ui::kit::metrics_of(dialog.size_class);
    const int32_t width = metrics.dialog_width * kScale;
    const int32_t height = metrics.dialog_height * kScale;
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    settings::draw_dialog(surface, {0, 0, kScale}, dialog, drawn, icon);
    if (dialog.size_class == oa::ui::kit::SizeClass::compact &&
        (surface.width != 960 || surface.height != 648)) {
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

/// Writes every section at each size class, at its top and, where it
/// scrolls, at its end.
///
/// @param fonts the dialog's fonts
/// @param icon the app icon
/// @param directory where the pictures go
/// @return true when every picture was written
bool draw_classes(
    const settings::DialogFonts& fonts,
    const renderer::RgbaPicture& icon,
    const std::filesystem::path& directory
) {
    constexpr std::array<std::pair<oa::ui::kit::SizeClass, std::string_view>, 3> classes{{
        {oa::ui::kit::SizeClass::compact, "compact"},
        {oa::ui::kit::SizeClass::regular, "regular"},
        {oa::ui::kit::SizeClass::large, "large"},
    }};
    bool ok = true;
    for (const auto& [size_class, word] : classes) {
        for (const Page page :
             settings::dialog_pages(settings::DialogKind::engine, true, true, true)) {
            Dialog dialog = section_dialog(page);
            settings::set_dialog_size_class(dialog, size_class);
            dialog.chosen.look =
                oa::ui::kit::modern_look(fonts) ? settings::Look::modern : settings::Look::classic;
            dialog.key_layout_name = "OA default";
            dialog.key_layout_changes = 4;
            dialog.chosen.sticky_modifiers = true;
            dialog.chosen.hold_to_confirm = true;
            if (page == Page::accessibility) {
                settings::PracticeKey shift;
                shift.modifier = settings::PracticeModifier::shift;
                shift.label = "Shift";
                std::ignore = settings::dialog_practice_key(dialog, shift, true, 1000);
                std::ignore = settings::dialog_practice_key(dialog, shift, false, 1100);
            }
            const std::string name =
                std::string(settings::page_word(page)) + "-" + std::string(word);
            if (!draw_section(dialog, fonts, icon, directory / (name + ".png")))
                ok = false;
            if (settings::scroll_limit(dialog) > 0) {
                dialog.scroll[static_cast<std::size_t>(page)] = settings::scroll_limit(dialog);
                if (!draw_section(dialog, fonts, icon, directory / (name + "-end.png")))
                    ok = false;
            }
        }
    }
    return ok;
}

int main(int argc, char** argv) {
    bool classes = false;
    std::filesystem::path modern_fonts;
    bool usable = argc >= 2;
    for (int index = 2; usable && index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--classes")
            classes = true;
        else if (option == "--modern" && index + 1 < argc)
            modern_fonts = argv[++index];
        else
            usable = false;
    }
    if (!usable) {
        std::cerr
            << "usage: oa-ui-engine-settings-pictures DIRECTORY [--classes] [--modern FONTS]\n";
        return 2;
    }
    const std::filesystem::path directory(argv[1]);
    oa::AssetStore assets = oa::test::require_game_assets("the settings pictures");
    settings::DialogFonts fonts = settings::load_dialog_fonts(assets);
    // The modern look draws in Barlow from the folder named.
    std::unique_ptr<oa::platform::text_font::FontStack> modern;
    if (!modern_fonts.empty()) {
        modern = oa::platform::text_font::FontStack::open(modern_fonts);
        if (!modern) {
            std::cerr << "pictures: cannot open the fonts in " << modern_fonts.string() << '\n';
            return 1;
        }
        fonts.modern = modern.get();
    }
    const AppIcon icon = load_app_icon();
    if (icon.pixels.empty())
        return 1;

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "pictures: cannot make " << directory.string() << '\n';
        return 1;
    }

    if (classes)
        return draw_classes(fonts, icon.picture, directory) ? 0 : 1;
    bool ok = true;
    for (const Page page : settings::dialog_pages(settings::DialogKind::engine, true, true, true)) {
        Dialog dialog = section_dialog(page);
        apply_look(dialog, fonts);
        if (!draw_section(dialog, fonts, icon.picture, directory / picture_name(page)))
            ok = false;
        // Updates again: the card while 0.8.8 is out. That picture is not
        // the section scrolled to its end.
        if (page == Page::updates) {
            dialog.updates.offered = true;
            dialog.updates.can_check = true;
            dialog.updates.card_status = "0.8.8 is out";
            dialog.updates.card_detail = "You have 0.8.7. The update is 24.5 MB.";
            dialog.updates.card_note = "Your saved games, settings and mods stay as they are.";
            dialog.updates.actions.clear();
            add_update_action(dialog, "download", "Download update");
            add_update_action(dialog, "skip", "Skip this version");
            if (!draw_section(dialog, fonts, icon.picture, directory / "updates-2.png"))
                ok = false;
            // The end: Check for updates, Install updates, and Previous
            // version · 0.8.6 with Roll back….
            Dialog end = section_dialog(page);
            apply_look(end, fonts);
            end.updates.show_download = true;
            end.updates.kept_version = "0.8.6";
            end.scroll[static_cast<std::size_t>(page)] = settings::scroll_limit(end);
            if (!draw_section(end, fonts, icon.picture, directory / "updates-3.png"))
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
