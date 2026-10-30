// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pins the pictures of the settings dialog, its notices, its prompts and the
// OA button. Each scene is drawn on a fresh black surface. The program names
// a scene by the SHA-256 of its width, its height and its RGB bytes.
//
// Without --data the scenes are block/…, drawn with a synthetic font. With
// --data they are game/…, drawn with the installed game's fonts. Both are
// pictures of drawings without the modern fonts: the program stops when those
// fonts are beside it. --record writes this run's scenes to the table; without
// it, the program checks the table.

#include "oa/base/sha256.hpp"
#include "oa/data/languages.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/engine_settings/prompt.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace settings = oa::ui::engine_settings;
namespace renderer = oa::ui::frontend_renderer;
namespace sha256 = oa::base::sha256;

using settings::Dialog;
using settings::DialogFonts;
using settings::DialogKey;
using settings::Page;

std::vector<std::string> problems;

/// Records a scene that is not the one this program means to pin.
void problem(std::string text) {
    problems.push_back(std::move(text));
}

/// A 40 by 40 icon of one opaque colour, but for its clear top left 2 by 2
/// pixels, as the dialog's tests draw one.
struct Icon {
    static constexpr uint32_t side = 40;
    std::vector<uint8_t> pixels{};

    renderer::RgbaPicture picture() const { return {side, side, pixels}; }
};

Icon solid_icon() {
    Icon icon;
    constexpr renderer::Rgb color{0xd4, 0xa0, 0x30};
    icon.pixels.reserve(Icon::side * Icon::side * 4);
    for (uint32_t y = 0; y < Icon::side; ++y) {
        for (uint32_t x = 0; x < Icon::side; ++x) {
            icon.pixels.insert(icon.pixels.end(), color.begin(), color.end());
            icon.pixels.push_back(x < 2 && y < 2 ? 0 : 0xff);
        }
    }
    return icon;
}

/// A black surface of the picture's own size.
renderer::Surface black(int32_t width, int32_t height) {
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(width);
    surface.height = static_cast<uint32_t>(height);
    surface.rgb.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3, 0);
    return surface;
}

/// The synthetic font: each byte's glyph is 2 + byte % 4 columns by 8 rows,
/// set where (x + y + byte) % 3 is not 0. Regular and small are the same font.
DialogFonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// The digest of a picture: four little-endian bytes of width, four of
/// height, then the RGB bytes.
std::string digest_of(const renderer::Surface& surface) {
    std::array<uint8_t, 8> header{};
    const auto store = [&header](uint32_t value, std::size_t offset) {
        header[offset] = static_cast<uint8_t>(value);
        header[offset + 1] = static_cast<uint8_t>(value >> 8);
        header[offset + 2] = static_cast<uint8_t>(value >> 16);
        header[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    store(surface.width, 0);
    store(surface.height, 4);
    sha256::Hasher hasher;
    sha256::update(hasher, header);
    sha256::update(hasher, surface.rgb);
    const auto hex = sha256::to_hex(sha256::finish(hasher));
    return std::string(hex.begin(), hex.end());
}

std::string_view page_slug(Page page) {
    switch (page) {
    case Page::controls:
        return "controls";
    case Page::common_tweaks:
        return "engine-limits";
    case Page::language:
        return "language";
    case Page::graphics:
        return "graphics";
    case Page::user_interface:
        return "user-interface";
    case Page::touch:
        return "touch";
    case Page::controller:
        return "controller";
    case Page::developer:
        return "developer";
    case Page::game_files:
        return "game-files";
    case Page::downloads:
        return "downloads";
    case Page::system:
        return "system";
    case Page::game_content:
        return "game-content";
    case Page::updates:
        return "updates";
    case Page::accessibility:
        return "accessibility";
    case Page::mod_keys:
        return "mod-keys";
    case Page::mod_patrol:
        return "mod-patrol";
    case Page::mod_guard:
        return "mod-guard";
    case Page::mod_tools:
        return "mod-tools";
    case Page::mod_chat:
        return "mod-chat";
    case Page::registered:
        return "registered";
    }
    return "page";
}

void fill_game_files(Dialog& dialog) {
    dialog.game_files_summary = "3.1c · Core Contingency · Battle Tactics · music";
    dialog.game_files_sizes = "1.1 GB · 37 GB free on this tablet";
    dialog.game_files_location = "In the file manager: Open Annihilation \u203A Total Annihilation";
    dialog.game_files_device = "tablet";
}

/// The engine's dialog, listing Touch, Controller and Game files.
Dialog engine_dialog(Page page, const settings::Locks& locks = {}) {
    Dialog dialog;
    settings::open_dialog(
        dialog,
        {},
        {},
        locks,
        "v0.7.3",
        page,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        true,
        true,
        true
    );
    fill_game_files(dialog);
    return dialog;
}

/// The status Updates shows in a picture: up to date, or a release offered.
void fill_updates(Dialog& dialog, bool offered) {
    dialog.updates.version_line = "Open Annihilation 0.8.6";
    dialog.updates.can_check = true;
    dialog.updates.offered = offered;
    dialog.updates.mode = static_cast<int32_t>(settings::UpdateCheck::automatically);
    dialog.updates.status = offered ? "0.8.7 is out \u00b7 Stable \u00b7 checked today at 09:00"
                                    : "Up to date \u00b7 Stable \u00b7 checked today at 09:00";
}

/// The dialog drawn at its own size class, one point a pixel of that class.
renderer::Surface drawn_class_dialog(const Dialog& dialog, const DialogFonts& fonts) {
    const oa::ui::kit::Metrics& metrics = oa::ui::kit::metrics_of(dialog.size_class);
    renderer::Surface surface = black(metrics.dialog_width, metrics.dialog_height);
    settings::draw_dialog(surface, {0, 0, 1}, dialog, fonts, {});
    return surface;
}

void press_keys(Dialog& dialog, std::initializer_list<DialogKey> keys) {
    for (const DialogKey key : keys)
        static_cast<void>(settings::dialog_key(dialog, key));
}

std::optional<renderer::SourceRect>
control_rect(const Dialog& dialog, const DialogFonts& fonts, int32_t control) {
    for (const auto& part : settings::dialog_layout(dialog, &fonts)) {
        if (part.control == control && part.rect.width > 0 && part.rect.height > 0)
            return part.rect;
    }
    return std::nullopt;
}

renderer::Surface drawn_dialog(
    const Dialog& dialog, const DialogFonts& fonts, const renderer::RgbaPicture& icon, int32_t scale
) {
    renderer::Surface surface =
        black(settings::dialog_width * scale, settings::dialog_height * scale);
    settings::draw_dialog(surface, {0, 0, scale}, dialog, fonts, icon);
    return surface;
}

/// Tabs to the last row and back from the footer, which leaves the section
/// scrolled to its end.
void show_scroll_end(Dialog& dialog) {
    for (int step = 0; step < 64; ++step) {
        static_cast<void>(settings::dialog_key(dialog, DialogKey::tab));
        if (dialog.focused < settings::first_row_control) {
            static_cast<void>(settings::dialog_key(dialog, DialogKey::back_tab));
            return;
        }
    }
    problem("the section did not leave its rows");
}

settings::Notice short_notice() {
    settings::Notice notice;
    notice.title = "SAVED";
    notice.paragraphs.push_back({"The game was saved.", false});
    notice.open_caption = "Open folder";
    return notice;
}

settings::Notice long_notice() {
    settings::Notice notice;
    notice.title = "YOUR FILES";
    notice.open_caption = "Open folder";
    const std::string sentence =
        "Screenshots, films and mods now go in your own folder, and the game keeps them there.";
    notice.paragraphs.push_back({sentence + " " + sentence + " " + sentence, false});
    notice.paragraphs.push_back(
        {"Open the folder to see the saves, the screenshots and the mods.", false}
    );
    notice.paragraphs.push_back(
        {"The game makes the folder when it is missing, then shows it.", false}
    );
    notice.paragraphs.push_back(
        {"C:\\Users\\player\\Documents\\My Games\\Total Annihilation\\Saves", true}
    );
    notice.failure = "The folder could not be opened.";
    return notice;
}

settings::Prompt prompt_with(std::vector<settings::PromptButton> buttons, int32_t progress) {
    settings::Prompt prompt;
    prompt.title = "INSTALL MOD";
    prompt.paragraphs.push_back({"The mod is being readied for the next game.", false});
    prompt.buttons = std::move(buttons);
    prompt.cancel_button = 0;
    prompt.primary_button = static_cast<int32_t>(prompt.buttons.size()) - 1;
    prompt.progress = progress;
    return prompt;
}

renderer::Surface drawn_notice(const settings::Notice& notice, const DialogFonts& fonts) {
    const int32_t height = settings::notice_height(notice, &fonts);
    renderer::Surface surface = black(settings::notice_width, height);
    settings::draw_notice(surface, {0, 0, 1}, notice, fonts, {});
    return surface;
}

renderer::Surface drawn_prompt(const settings::Prompt& prompt, const DialogFonts& fonts) {
    const int32_t height = settings::prompt_height(prompt, &fonts);
    renderer::Surface surface = black(settings::notice_width, height);
    settings::draw_prompt(surface, {0, 0, 1}, prompt, fonts, {});
    return surface;
}

struct Picture {
    std::string name;
    renderer::Surface surface;
};

void add(std::vector<Picture>& pictures, std::string name, renderer::Surface surface) {
    pictures.push_back(Picture{std::move(name), std::move(surface)});
}

/// Installs Simplified Chinese for the pictures, then puts the compiled
/// available entry back. The open Language list keeps its 简体中文 row.
struct InstalledSimplifiedChinese {
    InstalledSimplifiedChinese() {
        oa::data::languages::LanguageEntry entry;
        entry.tag = "zh-Hans";
        entry.endonym = "\347\256\200\344\275\223\344\270\255\346\226\207";
        entry.english_name = "Chinese (Simplified)";
        entry.word = "Chinese";
        entry.locales = {"zh-Hans", "zh-CN", "zh-SG", "zh-MY", "zh"};
        entry.needs = oa::data::languages::TextNeeds::modern_fonts;
        const std::array<oa::data::languages::LanguageEntry, 1> installed{entry};
        oa::data::languages::set_pack_languages(installed, {});
    }

    ~InstalledSimplifiedChinese() { oa::data::languages::set_pack_languages({}, {}); }

    InstalledSimplifiedChinese(const InstalledSimplifiedChinese&) = delete;
    InstalledSimplifiedChinese& operator=(const InstalledSimplifiedChinese&) = delete;
};

std::vector<Picture> scenes(const DialogFonts& fonts, const Icon& icon) {
    std::vector<Picture> pictures;
    const renderer::RgbaPicture no_icon{};
    { // Simplified Chinese is installed for the scenes in this block.
        [[maybe_unused]] const InstalledSimplifiedChinese installed;
        const auto game = settings::settings_locks(settings::GameState{true, false, false, false});

        for (const Page page :
             settings::dialog_pages(settings::DialogKind::engine, true, true, true)) {
            // System's rows differ by whether this system can register file
            // types. Both layouts are pinned, so the table is the same
            // everywhere.
            if (page == Page::system)
                continue;
            Dialog dialog = engine_dialog(page);
            // The shared scene is up to date, with CHECK NOW. The offered
            // release and Regular are their own scenes below.
            if (page == Page::updates)
                fill_updates(dialog, false);
            add(pictures,
                std::string("dialog/") + std::string(page_slug(page)),
                drawn_dialog(dialog, fonts, no_icon, 1));
        }
        Dialog updates_offered = engine_dialog(Page::updates);
        fill_updates(updates_offered, true);
        add(pictures, "dialog/updates-offered", drawn_dialog(updates_offered, fonts, no_icon, 1));
        Dialog updates_regular = engine_dialog(Page::updates);
        updates_regular.size_class = oa::ui::kit::SizeClass::regular;
        fill_updates(updates_regular, false);
        add(pictures, "dialog/updates-regular", drawn_class_dialog(updates_regular, fonts));
        Dialog system_bundle = engine_dialog(Page::system);
        system_bundle.file_type_switches = false;
        add(pictures, "dialog/system-bundle", drawn_dialog(system_bundle, fonts, no_icon, 1));
        Dialog system_runtime = engine_dialog(Page::system);
        system_runtime.file_type_switches = true;
        add(pictures, "dialog/system-runtime", drawn_dialog(system_runtime, fonts, no_icon, 1));

        Dialog graphics_end = engine_dialog(Page::graphics);
        show_scroll_end(graphics_end);
        if (graphics_end.scroll[static_cast<std::size_t>(Page::graphics)] <= 0)
            problem("graphics is not at its scroll end");
        add(pictures, "dialog/graphics-end", drawn_dialog(graphics_end, fonts, no_icon, 1));

        Dialog accessibility_end = engine_dialog(Page::accessibility);
        show_scroll_end(accessibility_end);
        if (accessibility_end.scroll[static_cast<std::size_t>(Page::accessibility)] <= 0)
            problem("accessibility is not at its scroll end");
        add(pictures,
            "dialog/accessibility-end",
            drawn_dialog(accessibility_end, fonts, no_icon, 1));
        Dialog accessibility_regular = engine_dialog(Page::accessibility);
        accessibility_regular.size_class = oa::ui::kit::SizeClass::regular;
        add(pictures,
            "dialog/accessibility-regular",
            drawn_class_dialog(accessibility_regular, fonts));

        Dialog limits_end = engine_dialog(Page::common_tweaks);
        limits_end.scroll[static_cast<std::size_t>(Page::common_tweaks)] =
            settings::scroll_limit(limits_end);
        if (limits_end.scroll[static_cast<std::size_t>(Page::common_tweaks)] <= 0)
            problem("engine limits is not at its scroll end");
        add(pictures, "dialog/engine-limits-end", drawn_dialog(limits_end, fonts, no_icon, 1));

        Dialog language = engine_dialog(Page::language);
        press_keys(language, {DialogKey::tab, DialogKey::space});
        if (language.open_list == settings::no_control)
            problem("language drop-down is not open");
        add(pictures, "dialog/language-open", drawn_dialog(language, fonts, no_icon, 1));

        Dialog controls = engine_dialog(Page::controls);
        const auto cancel = control_rect(controls, fonts, settings::cancel_control);
        const auto ok = control_rect(controls, fonts, settings::ok_control);
        if (!cancel || !ok) {
            problem("controls has no OK or Cancel");
        } else {
            static_cast<void>(settings::dialog_pointer_down(
                controls, cancel->x + cancel->width / 2, cancel->y + cancel->height / 2
            ));
            static_cast<void>(settings::dialog_pointer_move(
                controls, ok->x + ok->width / 2, ok->y + ok->height / 2
            ));
            press_keys(controls, {DialogKey::tab, DialogKey::tab});
            if (controls.focused != settings::first_row_control + 1 ||
                controls.hovered != settings::ok_control ||
                controls.pressed != settings::cancel_control) {
                problem(
                    "controls focus is " + std::to_string(controls.focused) + " hover " +
                    std::to_string(controls.hovered) + " press " + std::to_string(controls.pressed)
                );
            }
        }
        add(pictures, "dialog/controls-focus", drawn_dialog(controls, fonts, no_icon, 1));

        Dialog controls_mod = engine_dialog(Page::controls);
        controls_mod.played_mod = "Ridge";
        controls_mod.played_mod_id = "ridge";
        controls_mod.key_layout_summary = "OA default";
        controls_mod.key_layouts.push_back({"plain-keys", "Plain keys", false});
        controls_mod.key_layouts.push_back({"ridge-keys", "Ridge keys", true});
        add(pictures, "dialog/controls-mod", drawn_dialog(controls_mod, fonts, no_icon, 1));

        add(pictures,
            "dialog/engine-limits-locked",
            drawn_dialog(engine_dialog(Page::common_tweaks, game), fonts, no_icon, 1));
        add(pictures,
            "dialog/graphics-locked",
            drawn_dialog(engine_dialog(Page::graphics, game), fonts, no_icon, 1));

        // Developer Mode on, then past Show performance statistics and UI
        // kit gallery to the list's first area, open.
        Dialog developer = engine_dialog(Page::developer);
        press_keys(
            developer,
            {DialogKey::tab,
             DialogKey::space,
             DialogKey::tab,
             DialogKey::tab,
             DialogKey::tab,
             DialogKey::space}
        );
        const bool area_open = std::any_of(
            developer.developer.areas_open.begin(),
            developer.developer.areas_open.end(),
            [](uint8_t open) { return open != 0; }
        );
        if (!developer.chosen.developer_mode || !area_open)
            problem("developer mode is not on with its first area open");
        add(pictures, "dialog/developer-open", drawn_dialog(developer, fonts, no_icon, 1));

        Dialog files = engine_dialog(Page::system);
        files.file_type_switches = false;
        files.user_folder = "/Users/player/My Games/Total Annihilation";
        static_cast<void>(settings::set_folder_notice(files, "The folder could not be opened."));
        add(pictures, "dialog/files-notice", drawn_dialog(files, fonts, no_icon, 1));

        Dialog mod_options;
        settings::open_mod_options_dialog(mod_options, {}, {}, {}, "v0.7.3", Page::mod_keys);
        add(pictures, "dialog/mod-options", drawn_dialog(mod_options, fonts, no_icon, 1));

        Dialog language_only;
        settings::open_language_text_dialog(language_only, {}, {}, {}, "v0.7.3");
        add(pictures, "dialog/language-only", drawn_dialog(language_only, fonts, no_icon, 1));

        add(pictures,
            "dialog/controls-scale-2",
            drawn_dialog(engine_dialog(Page::controls), fonts, no_icon, 2));
        add(pictures,
            "dialog/developer-scale-2",
            drawn_dialog(engine_dialog(Page::developer), fonts, no_icon, 2));
        add(pictures,
            "dialog/controls-scale-3",
            drawn_dialog(engine_dialog(Page::controls), fonts, no_icon, 3));
        add(pictures,
            "dialog/controls-icon",
            drawn_dialog(engine_dialog(Page::controls), fonts, icon.picture(), 1));

        add(pictures, "notice/short", drawn_notice(short_notice(), fonts));
        const settings::Notice long_one = long_notice();
        add(pictures, "notice/long", drawn_notice(long_one, fonts));
        settings::Notice marked = long_one;
        marked.marked = settings::notice_open_control;
        marked.hovered = settings::notice_ok_control;
        add(pictures, "notice/long-marked", drawn_notice(marked, fonts));

        const settings::PromptButton ok_button{"OK", true};
        const settings::PromptButton cancel_button{"Cancel", false};
        const settings::PromptButton later_button{"Later", false};
        add(pictures, "prompt/one", drawn_prompt(prompt_with({ok_button}, -1), fonts));
        add(pictures,
            "prompt/two",
            drawn_prompt(prompt_with({cancel_button, ok_button}, -1), fonts));
        add(pictures,
            "prompt/three",
            drawn_prompt(prompt_with({later_button, cancel_button, ok_button}, -1), fonts));
        add(pictures, "prompt/progress-0", drawn_prompt(prompt_with({ok_button}, 0), fonts));
        add(pictures, "prompt/progress-500", drawn_prompt(prompt_with({ok_button}, 500), fonts));
        add(pictures, "prompt/progress-1000", drawn_prompt(prompt_with({ok_button}, 1000), fonts));

        settings::Prompt cut;
        cut.title = "A LONG NOTE";
        const std::string line =
            "Screenshots, films and mods now go in your own folder, and the game keeps them there.";
        std::string body;
        // Enough repeats that the text is cut even in the synthetic font, whose
        // glyphs are only a few columns wide.
        for (int repeat = 0; repeat < 200; ++repeat) {
            if (!body.empty())
                body += ' ';
            body += line;
        }
        cut.paragraphs.push_back({body, false});
        cut.buttons.push_back(ok_button);
        cut.primary_button = 0;
        if (settings::prompt_fits(cut, &fonts))
            problem("prompt/cut fits, so its text is not cut");
        add(pictures, "prompt/cut", drawn_prompt(cut, fonts));

        for (const int32_t side : {settings::menu_button_side, settings::ingame_button_side}) {
            for (const auto look :
                 {settings::ButtonLook::idle,
                  settings::ButtonLook::hovered,
                  settings::ButtonLook::pressed}) {
                const char* look_name = "idle";
                if (look == settings::ButtonLook::hovered)
                    look_name = "hovered";
                else if (look == settings::ButtonLook::pressed)
                    look_name = "pressed";
                for (const bool with_icon : {false, true}) {
                    renderer::Surface surface = black(side, side);
                    settings::draw_oa_button(
                        surface, {0, 0, 1}, side, look, fonts, with_icon ? icon.picture() : no_icon
                    );
                    std::string name = "button/" + std::to_string(side) + "-" + look_name;
                    if (with_icon)
                        name += "-icon";
                    add(pictures, std::move(name), std::move(surface));
                }
            }
        }

        for (const int32_t side : {20, 32}) {
            renderer::Surface surface = black(side, side);
            settings::draw_oa_mark(surface, {0, 0, 1}, side, no_icon);
            add(pictures, "mark/" + std::to_string(side), std::move(surface));
        }
    }
    // Available packs, with Simplified Chinese not installed, so it is listed
    // only under the divider. The closed field then shows a download.
    const auto offer = [](Dialog& dialog) {
        settings::AvailableLanguage simplified;
        simplified.tag = "zh-Hans";
        simplified.endonym = "\347\256\200\344\275\223\344\270\255\346\226\207";
        simplified.english_name = "Chinese (Simplified)";
        simplified.aside = "4.0 MB \u21E9";
        settings::AvailableLanguage portuguese;
        portuguese.tag = "pt-BR";
        portuguese.endonym = "Portugu\u00EAs (Brasil)";
        portuguese.english_name = "Portuguese (Brazil)";
        portuguese.aside = "community \u00B7 92% \u00B7 1.1 MB";
        dialog.available_languages = {std::move(simplified), std::move(portuguese)};
    };
    Dialog available = engine_dialog(Page::language);
    offer(available);
    press_keys(available, {DialogKey::tab, DialogKey::space});
    if (available.open_list == settings::no_control)
        problem("language-available drop-down is not open");
    add(pictures, "language-available", drawn_dialog(available, fonts, no_icon, 1));

    Dialog downloading = engine_dialog(Page::language);
    offer(downloading);
    downloading.chosen.language = "zh-Hans";
    downloading.available_languages[0].state = settings::AvailableLanguage::State::downloading;
    downloading.available_languages[0].percent = 40;
    add(pictures, "language-downloading", drawn_dialog(downloading, fonts, no_icon, 1));
    return pictures;
}

std::map<std::string, std::string>
digests_of(const std::vector<Picture>& pictures, std::string_view group) {
    std::map<std::string, std::string> digests;
    for (const Picture& picture : pictures) {
        const std::string name = std::string(group) + "/" + picture.name;
        digests.emplace(name, digest_of(picture.surface));
    }
    return digests;
}

bool write_table(
    const std::filesystem::path& path, const std::map<std::string, std::string>& digests
) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "pixels: cannot write " << path.string() << '\n';
        return false;
    }
    for (const auto& [name, digest] : digests)
        out << name << ' ' << digest << '\n';
    return static_cast<bool>(out);
}

std::map<std::string, std::string> read_table(const std::filesystem::path& path, bool& ok) {
    std::ifstream in(path);
    std::map<std::string, std::string> table;
    if (!in) {
        std::cerr << "pixels: cannot read " << path.string() << '\n';
        ok = false;
        return table;
    }
    std::string line;
    std::size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.empty())
            continue;
        const auto space = line.find(' ');
        if (space == std::string::npos || line.find(' ', space + 1) != std::string::npos ||
            line.size() != space + 1 + sha256::hex_size) {
            std::cerr << "pixels: " << path.string() << ':' << number << " is not a scene line\n";
            ok = false;
            continue;
        }
        table.emplace(line.substr(0, space), line.substr(space + 1));
    }
    return table;
}

} // namespace

int main(int argc, char** argv) {
    bool record = false;
    bool data = false;
    std::string table;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--record")
            record = true;
        else if (argument == "--data")
            data = true;
        else if (table.empty())
            table = std::string(argument);
        else {
            std::cerr << "pixels: unexpected argument " << argument << '\n';
            return 2;
        }
    }
    if (table.empty()) {
        std::cerr << "pixels: give the table path\n";
        return 2;
    }

    const auto fonts_directory = oa::platform::text_font::bundled_font_directory();
    std::error_code error;
    if (!fonts_directory.empty() && std::filesystem::exists(fonts_directory, error)) {
        std::cerr << "pixels: modern fonts at " << fonts_directory.string()
                  << " would change these pictures, which are drawn without them\n";
        return 1;
    }

    DialogFonts fonts = block_fonts();
    if (data) {
        auto assets = oa::test::require_game_assets("the settings dialog's pinned pictures");
        fonts = settings::load_dialog_fonts(assets);
    }
    const Icon icon = solid_icon();
    const std::string group = data ? "game" : "block";
    const std::map<std::string, std::string> digests = digests_of(scenes(fonts, icon), group);
    if (!problems.empty()) {
        for (const std::string& text : problems)
            std::cerr << "pixels: " << text << '\n';
        return 1;
    }
    if (record)
        return write_table(table, digests) ? 0 : 1;

    bool ok = true;
    const std::map<std::string, std::string> expected = read_table(table, ok);
    for (const auto& [name, digest] : digests) {
        const auto found = expected.find(name);
        if (found == expected.end()) {
            std::cerr << "pixels: " << name << " is missing from the table\n";
            ok = false;
            continue;
        }
        if (found->second != digest) {
            std::cerr << "pixels: " << name << " differs\n";
            ok = false;
        }
    }
    const std::string prefix = group + "/";
    for (const auto& [name, digest] : expected) {
        if (name.compare(0, prefix.size(), prefix) != 0)
            continue;
        if (digests.find(name) == digests.end()) {
            std::cerr << "pixels: " << name << " was not drawn\n";
            ok = false;
        }
        static_cast<void>(digest);
    }
    return ok ? 0 : 1;
}
