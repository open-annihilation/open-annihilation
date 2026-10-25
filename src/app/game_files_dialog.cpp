// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Language settings over the Game files screen, before the game's
// fonts are installed: the settings dialog limited to that section, drawn in
// the bundled fonts, with the screen's own text hooks while it is up
// (game_files_screen.hpp). The interface's language is chosen here too for
// the screen, from the preferences file, before the Runtime starts.
#include "game_files_screen.hpp"
#include "language_packs.hpp"
#include "window_pixels.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/locale.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace oa::app {

namespace {

namespace paint = oa::ui::paint;
namespace languages = oa::data::languages;
namespace settings = oa::ui::engine_settings;
namespace renderer = oa::ui::frontend_renderer;
namespace text_font = oa::platform::text_font;

/// The folder beside the game's other files that holds interface catalogue files.
constexpr std::string_view catalogue_folder = engine_languages_folder;
/// A catalogue file's extension, matched without regard to case.
constexpr std::string_view catalogue_extension = ".tdf";
/// The most drawn lines the dialog's text hooks keep before they start again.
constexpr std::size_t kept_lines = 512;
/// How long the dialog's loop waits for an event, in milliseconds: briefly while the check
/// drives it, else long enough to idle.
constexpr int32_t check_wait_ms = 16;
constexpr int32_t idle_wait_ms = 250;
/// The size the dialog's text is drawn at, as the Language setting's sizes go: the
/// modern fonts at the game fonts' own size stand wider than the game's fonts the dialog is
/// laid out for, so they are drawn at the setting's default size.
constexpr int32_t dialog_text_size = oa::present::default_text_size;
/// How far one notch of the wheel turns, as the dialog takes it.
constexpr float wheel_notch = 1.0F;

/// The pixel size and weight each face of the game's fonts is drawn in at a scale of 1 and
/// the game fonts' size, as the game's own text in the modern fonts is.
struct FaceSize {
    int32_t pixel_size{};       ///< pixels per em
    text_font::Weight weight{}; ///< the weight
};

constexpr FaceSize message_face{14, text_font::Weight::bold};
constexpr FaceSize status_face{11, text_font::Weight::bold};
constexpr FaceSize label_face{11, text_font::Weight::regular};

/// The interface catalogue the screen reads, kept for as long as the game runs so that the
/// texts it gives stay valid.
languages::InterfaceText& screen_catalogue() {
    static languages::InterfaceText catalogue;
    return catalogue;
}

/// Tells whether a file name ends in the catalogue's extension, in any case.
///
/// @param name the file name
/// @return true for a catalogue file
[[nodiscard]] bool catalogue_file(std::string_view name) {
    if (name.size() <= catalogue_extension.size())
        return false;
    const auto tail = name.substr(name.size() - catalogue_extension.size());
    return std::equal(tail.begin(), tail.end(), catalogue_extension.begin(), [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
    });
}

/// Loads the preferences file, empty when it is missing or cannot be read.
///
/// @param file the file
/// @return its values
[[nodiscard]] oa::platform::preferences::Values load_preferences(const fs::path& file) {
    try {
        return oa::platform::preferences::load(file);
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the preferences " << path_to_utf8(file)
                  << " were not read: " << error.what() << '\n';
        return {};
    }
}

/// Tells whether two words are the same, ignoring the case of ASCII letters.
///
/// @param left a word
/// @param right a word
/// @return true when they match
[[nodiscard]] bool same_word(std::string_view left, std::string_view right) {
    const languages::UnitTexts::NoCaseLess less{};
    return !less(left, right) && !less(right, left);
}

/// The engine's and the player's packs the Game files screen and the folder
/// chooser draw with. They are read once. published is set when they are
/// registered as installed languages; the management screen does not
/// register them, because the runtime already has.
struct HeldLanguagePacks {
    bool read{};
    bool published{};
    std::vector<std::unique_ptr<LoadedLanguagePack>> engine;
    std::vector<std::unique_ptr<LoadedLanguagePack>> player;
};

/// The packs, kept for as long as the game runs.
///
/// @return the packs
HeldLanguagePacks& held_language_packs() {
    static HeldLanguagePacks held;
    return held;
}

/// Reads the catalogue files beside the game once, then the engine's
/// language packs and the player's, without registering them.
///
/// The player's folder is read even when the program's own folder is not
/// there. The first call is the one that decides the folder.
///
/// @param user_folder_option --user-folder; empty leaves the folder to the preferences
/// @param preferences_file --preferences-file; empty: the player's own file
void read_held_language_packs(
    const std::optional<fs::path>& user_folder_option,
    const std::optional<fs::path>& preferences_file
) {
    HeldLanguagePacks& held = held_language_packs();
    if (held.read)
        return;
    std::vector<std::unique_ptr<LoadedLanguagePack>>& engine_packs = held.engine;
    const std::string base = oa::platform::program_directory();
    if (!base.empty()) {
        const fs::path folder = path_from_utf8(base.c_str()) / path_from_utf8(catalogue_folder);
        std::error_code error;
        if (fs::is_directory(folder, error)) {
            std::vector<fs::path> files;
            for (fs::directory_iterator entry(folder, error), end; !error && entry != end;
                 entry.increment(error))
                if (entry->is_regular_file(error) &&
                    catalogue_file(path_to_utf8(entry->path().filename())))
                    files.push_back(entry->path());
            // A later file's translation of a text replaces an earlier one's.
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                std::ifstream input(file, std::ios::binary);
                std::string text;
                if (input)
                    text.assign(
                        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
                    );
                std::string failure;
                if (!input || !screen_catalogue().add(text, &failure))
                    std::cerr << "open-annihilation: the interface catalogue " << path_to_utf8(file)
                              << " was not read" << (failure.empty() ? "" : ": ") << failure
                              << '\n';
            }
            // The engine's language packs' own words, read after the files
            // beside them. The screen shows before any game data is found,
            // so its words come from the packs, and their other tables are
            // let go.
            read_language_packs(folder, engine_packs, &screen_catalogue());
        }
    }
    const fs::path preferences = preference_file(preferences_file);
    const auto values = load_preferences(preferences);
    std::string note;
    const fs::path player_root =
        own_user_folder(user_folder_option, preferences_file, preferences, values, note);
    if (!note.empty())
        std::cerr << "open-annihilation: no Documents folder (" << note
                  << "); the player's language packs are read from " << path_to_utf8(player_root)
                  << '\n';
    if (!player_root.empty())
        read_language_packs(
            player_root / path_from_utf8(player_languages_folder), held.player, &screen_catalogue()
        );
    held.read = true;
}

/// Reads the catalogue and the packs once, then registers the installed
/// languages. The player's pack wins over the engine's when both hold one
/// tag. No mod's packs: this screen adds none of a mod's languages.
///
/// @param user_folder_option --user-folder; empty leaves the folder to the preferences
/// @param preferences_file --preferences-file; empty: the player's own file
void read_screen_catalogue(
    const std::optional<fs::path>& user_folder_option,
    const std::optional<fs::path>& preferences_file
) {
    read_held_language_packs(user_folder_option, preferences_file);
    HeldLanguagePacks& held = held_language_packs();
    if (held.published)
        return;
    languages::set_pack_languages(installed_entries({&held.player, &held.engine}), {});
    held.published = true;
}

/// Puts a language choice in effect for the interface's words.
///
/// The installed packs are registered before the choice is read, so a pack
/// can be the language the operating system chooses.
///
/// @param choice the setting's value: system_choice or a language's tag
/// @param preferences_file --preferences-file; empty: the player's own file
/// @param user_folder_option --user-folder; empty leaves the player's folder
void use_language(
    std::string_view choice,
    const std::optional<fs::path>& preferences_file,
    const std::optional<fs::path>& user_folder_option
) {
    read_screen_catalogue(user_folder_option, preferences_file);
    const languages::Language& system =
        languages::preferred_language(oa::platform::locale::preferred_locales());
    languages::set_interface_language(
        &screen_catalogue(), languages::chosen_language(choice, system)
    );
}

/// The text hooks the dialog draws with while it is up: the bundled fonts, and the lines they
/// drew, kept for the frames that draw them again.
struct DialogText {
    text_font::FontStack* fonts{}; ///< the fonts
    std::unordered_map<std::string, std::shared_ptr<const oa::present::TextMask>>
        lines{}; ///< drawn lines
};

/// Draws a line in the stack as a game text mask; null when it cannot.
///
/// @param stack the fonts
/// @param text UTF-8 text
/// @param style the style
/// @return the mask
std::shared_ptr<const oa::present::TextMask>
draw_mask(text_font::FontStack& stack, std::string_view text, const text_font::Style& style) {
    const auto drawn = stack.draw(text, style);
    const auto placements = stack.layout(text, style);
    const auto characters = text_font::decode_utf8(text);
    if (!drawn || !placements || !characters)
        return nullptr;
    auto mask = std::make_shared<oa::present::TextMask>();
    mask->width = drawn->width;
    mask->height = drawn->height;
    mask->baseline = drawn->baseline;
    mask->origin = drawn->origin;
    mask->advance = drawn->advance;
    mask->alpha = drawn->alpha;
    // The placements are the visible characters'; the others leave the pen.
    std::size_t placed = 0;
    int32_t pen = 0;
    for (const char32_t character : *characters) {
        if (!text_font::is_invisible(character) && placed < placements->size()) {
            const auto& placement = (*placements)[placed++];
            pen = placement.pen + placement.advance;
        }
        mask->character_ends.push_back(pen);
    }
    return mask;
}

/// Installs the dialog's text hooks for as long as it lives, then puts back the ones in place
/// before.
class TextHooksInstall {
  public:

    /// Installs the hooks.
    ///
    /// @param text the fonts and the kept lines
    explicit TextHooksInstall(DialogText& text) : previous_(oa::present::game_text_hooks()) {
        oa::present::GameTextHooks hooks{};
        hooks.context = &text;
        // Modern fonts on, plain letters: the dialog's panel needs no outline or shadow.
        hooks.settings = [](void*) {
            oa::present::TextSettings settings;
            settings.style.modern_fonts = true;
            settings.style.outline = false;
            settings.style.shadow = false;
            settings.style.background = false;
            settings.style.size = oa::present::game_font_text_size;
            settings.utf8 = true;
            return settings;
        };
        hooks.draw = [](void* context,
                        std::string_view line,
                        oa::present::TextFace face,
                        int32_t scale,
                        int32_t size) -> std::shared_ptr<const oa::present::TextMask> {
            auto& owner = *static_cast<DialogText*>(context);
            if (owner.fonts == nullptr || line.empty() || line.size() > text_font::max_text_bytes)
                return nullptr;
            const FaceSize face_size = face == oa::present::TextFace::message  ? message_face
                                       : face == oa::present::TextFace::status ? status_face
                                                                               : label_face;
            text_font::Style style;
            style.pixel_size = std::clamp(
                oa::present::face_pixel_size(
                    face_size.pixel_size,
                    scale,
                    size * dialog_text_size / oa::present::game_font_text_size
                ),
                1,
                text_font::max_pixel_size
            );
            style.weight = face_size.weight;
            style.rendering = text_font::Rendering::mono;
            std::string key;
            key.push_back(static_cast<char>(face));
            key += std::to_string(style.pixel_size);
            key.push_back('\0');
            key.append(line);
            if (const auto found = owner.lines.find(key); found != owner.lines.end())
                return found->second;
            auto mask = draw_mask(*owner.fonts, line, style);
            if (owner.lines.size() >= kept_lines)
                owner.lines.clear();
            owner.lines.emplace(std::move(key), mask);
            return mask;
        };
        // The dialog draws on a surface of red, green and blue: no palette.
        hooks.palette = [](void*) { return std::span<const uint8_t>{}; };
        oa::present::set_game_text_hooks(hooks);
    }

    /// Puts the hooks in place before back.
    ~TextHooksInstall() { oa::present::set_game_text_hooks(previous_); }

    TextHooksInstall(const TextHooksInstall&) = delete;
    TextHooksInstall& operator=(const TextHooksInstall&) = delete;

  private:

    oa::present::GameTextHooks previous_{}; ///< the hooks in place before
};

/// Where the dialog lies on the canvas, how large, and at which size class.
struct DialogPlace {
    int32_t x{};      ///< the canvas column of the dialog's left edge
    int32_t y{};      ///< the canvas row of its top edge
    int32_t scale{1}; ///< canvas pixels across each of the dialog's points
    /// The size class the dialog is laid out at, from the points the scale leaves.
    oa::ui::kit::SizeClass size_class{oa::ui::kit::SizeClass::compact};
    int32_t width{settings::dialog_width};   ///< the dialog's width, in points
    int32_t height{settings::dialog_height}; ///< the dialog's height, in points
};

/// Returns a quotient rounded down, for a numerator below 0 too.
///
/// @param numerator the numerator
/// @param denominator the denominator, above 0
/// @return floor(numerator / denominator)
[[nodiscard]] int32_t floor_quotient(int32_t numerator, int32_t denominator) noexcept {
    const int32_t quotient = numerator / denominator;
    return numerator % denominator < 0 ? quotient - 1 : quotient;
}

/// Places the dialog on a viewport as the OA layer places its screens: at
/// the scale the Interface size chooses (oa::ui::kit::layer_viewport: the
/// chosen step, or the Auto scale, with 150% among Auto's steps while
/// offer_fractional, no larger than lets Compact's dialog fit the canvas
/// less its safe insets and at least 1), laid out at the size class of the
/// points left over, and centred in the canvas less its insets, rounded
/// down. The bitmap scale is the whole part of that percent, as a chosen
/// 150% already is.
///
/// @param viewport the viewport
/// @param size the Interface size the settings in effect choose
/// @param offer_fractional 150% is among Auto's steps: the modern fonts are
///     on and opened
/// @return the place
[[nodiscard]] DialogPlace place_dialog(
    const oa::ui::game_files::Viewport& viewport,
    settings::InterfaceSize size,
    bool offer_fractional
) noexcept {
    namespace kit = oa::ui::kit;
    kit::Viewport canvas;
    canvas.width = viewport.width;
    canvas.height = viewport.height;
    canvas.density = viewport.px_per_point;
    canvas.safe = {
        viewport.safe.left, viewport.safe.top, viewport.safe.right, viewport.safe.bottom
    };
    const int32_t room_width = canvas.width - canvas.safe.left - canvas.safe.right;
    const int32_t room_height = canvas.height - canvas.safe.top - canvas.safe.bottom;
    int32_t chosen_percent = settings::interface_size_percent(size);
    if (chosen_percent <= 0 && offer_fractional)
        chosen_percent = kit::auto_with_fraction_percent;
    canvas = kit::layer_viewport(canvas, chosen_percent);
    DialogPlace place;
    place.scale = canvas.scale_percent / 100;
    place.size_class = kit::frame_of(canvas).size_class;
    place.width = kit::metrics_of(place.size_class).dialog_width;
    place.height = kit::metrics_of(place.size_class).dialog_height;
    place.x = canvas.safe.left + floor_quotient(room_width - place.width * place.scale, 2);
    place.y = canvas.safe.top + floor_quotient(room_height - place.height * place.scale, 2);
    return place;
}

/// Draws the screen's frame darkened with the dialog over it.
///
/// @param under the screen's frame; another size than the viewport's leaves the background
/// @param viewport the viewport
/// @param place where the dialog lies
/// @param dialog the dialog
/// @return the frame
[[nodiscard]] paint::Canvas compose_frame(
    const paint::Canvas* under,
    const oa::ui::game_files::Viewport& viewport,
    const DialogPlace& place,
    const settings::Dialog& dialog
) {
    paint::Canvas canvas = paint::make_canvas(viewport.width, viewport.height);
    const auto back = oa::ui::game_files::background_colour;
    const bool same = under != nullptr && under->width == canvas.width &&
                      under->height == canvas.height && under->rgba.size() == canvas.rgba.size();
    const uint32_t opacity = settings::menu_backdrop_opacity;
    for (std::size_t pixel = 0, count = canvas.rgba.size() / 4U; pixel < count; ++pixel) {
        uint8_t* out = &canvas.rgba[pixel * 4U];
        const std::array<uint8_t, 3> colour =
            same
                ? std::array<
                      uint8_t,
                      3>{under->rgba[pixel * 4U], under->rgba[pixel * 4U + 1U], under->rgba[pixel * 4U + 2U]}
                : std::array<uint8_t, 3>{back.r, back.g, back.b};
        for (std::size_t channel = 0; channel < 3; ++channel)
            out[channel] = static_cast<uint8_t>(
                (uint32_t{colour[channel]} * (256U - opacity) +
                 uint32_t{settings::backdrop_color[channel]} * opacity) /
                256U
            );
        out[3] = 255;
    }
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(place.width * place.scale);
    surface.height = static_cast<uint32_t>(place.height * place.scale);
    surface.rgb.assign(static_cast<std::size_t>(surface.width) * surface.height * 3U, 0);
    for (std::size_t pixel = 0, count = surface.rgb.size() / 3U; pixel < count; ++pixel) {
        surface.rgb[pixel * 3U] = back.r;
        surface.rgb[pixel * 3U + 1U] = back.g;
        surface.rgb[pixel * 3U + 2U] = back.b;
    }
    renderer::Placement placement;
    placement.scale = place.scale;
    static const settings::DialogFonts no_fonts{};
    settings::draw_dialog(surface, placement, dialog, no_fonts, renderer::RgbaPicture{});
    for (uint32_t row = 0; row < surface.height; ++row) {
        const int32_t y = place.y + static_cast<int32_t>(row);
        if (y < 0 || y >= canvas.height)
            continue;
        for (uint32_t column = 0; column < surface.width; ++column) {
            const int32_t x = place.x + static_cast<int32_t>(column);
            if (x < 0 || x >= canvas.width)
                continue;
            const uint8_t* source =
                &surface.rgb[(static_cast<std::size_t>(row) * surface.width + column) * 3U];
            uint8_t* out =
                &canvas.rgba
                     [(static_cast<std::size_t>(y) * static_cast<std::size_t>(canvas.width) +
                       static_cast<std::size_t>(x)) *
                      4U];
            out[0] = source[0];
            out[1] = source[1];
            out[2] = source[2];
            out[3] = 255;
        }
    }
    return canvas;
}

/// Gives a key its meaning in the dialog.
///
/// @param key the key
/// @return its meaning; nothing for a key the dialog does not take
[[nodiscard]] std::optional<settings::DialogKey>
dialog_key_of(const SDL_KeyboardEvent& key) noexcept {
    const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
    switch (key.key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return settings::DialogKey::enter;
    case SDLK_ESCAPE:
        return settings::DialogKey::escape;
    case SDLK_PERIOD:
        if ((key.mod & SDL_KMOD_GUI) != 0)
            return settings::DialogKey::escape;
        return std::nullopt;
    case SDLK_UP:
        return settings::DialogKey::up;
    case SDLK_DOWN:
        return settings::DialogKey::down;
    case SDLK_LEFT:
        return settings::DialogKey::left;
    case SDLK_RIGHT:
        return settings::DialogKey::right;
    case SDLK_SPACE:
        return settings::DialogKey::space;
    case SDLK_TAB:
        return shift ? settings::DialogKey::back_tab : settings::DialogKey::tab;
    case SDLK_PAGEUP:
        return settings::DialogKey::page_up;
    case SDLK_PAGEDOWN:
        return settings::DialogKey::page_down;
    case SDLK_HOME:
        return settings::DialogKey::home;
    case SDLK_END:
        return settings::DialogKey::end;
    default:
        return std::nullopt;
    }
}

/// Tells whether the dialog takes a touch device's fingers: a touch screen's (touch_screen), or
/// the check's.
///
/// @param device the device
/// @return true for a direct touch device or the check's
[[nodiscard]] bool accepted_touch(SDL_TouchID device) noexcept {
    return device == game_files_check_touch_id || touch_screen(device);
}

} // namespace

void install_game_files_language(
    const std::optional<fs::path>& preferences_file,
    const std::optional<fs::path>& user_folder_option
) {
    const auto values = load_preferences(preference_file(preferences_file));
    use_language(
        settings::stored_language(values, !preferences_file.has_value()),
        preferences_file,
        user_folder_option
    );
}

void read_game_files_language_packs(
    const std::optional<fs::path>& user_folder_option,
    const std::optional<fs::path>& preferences_file
) {
    read_held_language_packs(user_folder_option, preferences_file);
}

void use_game_files_language_fonts(text_font::FontStack& stack) {
    stack.remove_pack_faces();
    const languages::Language& chosen = languages::interface_language();
    const HeldLanguagePacks& held = held_language_packs();
    std::vector<PackFontFile> fonts;
    const auto add_group = [&](const std::vector<std::unique_ptr<LoadedLanguagePack>>& packs) {
        for (const auto& loaded : packs) {
            if (loaded == nullptr)
                continue;
            const languages::PackManifest& manifest = loaded->pack.manifest();
            if (manifest.tag != chosen.tag && !same_word(manifest.word, chosen.game_name))
                continue;
            for (const PackFontFile& font : pack_fonts(*loaded)) {
                const bool held_already =
                    std::any_of(fonts.begin(), fonts.end(), [&font](const PackFontFile& have) {
                        return have.file == font.file;
                    });
                if (held_already)
                    continue;
                // Ones past the stack's room are not tried, so they are not
                // reported as faces that did not open.
                if (fonts.size() >= text_font::most_pack_faces)
                    continue;
                fonts.push_back(font);
            }
        }
    };
    // The player's pack wins over the engine's when both name one file.
    add_group(held.player);
    add_group(held.engine);
    for (const PackFontFile& font : fonts) {
        if (!stack.add_face(font.file, font.role))
            std::cerr << "open-annihilation: the language font " << path_to_utf8(font.file)
                      << " did not open\n";
    }
}

bool run_game_files_language_dialog(const GameFilesLanguageRequest& request) {
    const fs::path file =
        request.preferences_file.value_or(oa::platform::preferences::default_file());
    auto values = load_preferences(file);
    settings::Inputs inputs{};
    inputs.players_own_profile = !request.preferences_file.has_value();
#if defined(SDL_PLATFORM_MACOS)
    inputs.macos = true;
#endif
    const settings::EngineSettings defaults = settings::default_settings(inputs);
    const settings::EngineSettings current = settings::read_settings(values, inputs, false);
    const languages::Language& system =
        languages::preferred_language(oa::platform::locale::preferred_locales());
    settings::Dialog dialog;
    // Language alone: its Restore defaults restores only that section's settings.
    settings::open_language_text_dialog(
        dialog, current, defaults, settings::Locks{}, request.version, &system
    );
    DialogText text{request.fonts, {}};
    const TextHooksInstall install(text);
    const GameFilesDialogHooks& hooks = request.hooks;
    bool dirty = true;
    std::optional<SDL_FingerID> finger;
    SDL_TouchID finger_device{};
    float pointer_x = 0.0F;
    float pointer_y = 0.0F;
    oa::ui::game_files::Viewport viewport =
        hooks.viewport != nullptr ? hooks.viewport(hooks.context) : oa::ui::game_files::Viewport{};
    // 150% is among Auto's steps while the modern fonts are on and this
    // screen's stack of them opened.
    const bool offer_fractional = current.modern_fonts && request.fonts != nullptr;
    DialogPlace place = place_dialog(viewport, current.interface_size, offer_fractional);
    dialog.size_class = place.size_class;
    // Canvas pixels to the dialog's own.
    const auto source_x = [&](float x) {
        return static_cast<int32_t>(
            std::floor((x - static_cast<float>(place.x)) / static_cast<float>(place.scale))
        );
    };
    const auto source_y = [&](float y) {
        return static_cast<int32_t>(
            std::floor((y - static_cast<float>(place.y)) / static_cast<float>(place.scale))
        );
    };
    const auto finish = [&](bool accepted) {
        if (accepted) {
            settings::write_settings(
                values, dialog.opened, dialog.chosen, defaults, dialog.restored
            );
            try {
                oa::platform::preferences::save(file, values);
            } catch (const std::exception& error) {
                std::cerr << "open-annihilation: the preferences " << path_to_utf8(file)
                          << " were not written: " << error.what() << '\n';
            }
            use_language(dialog.chosen.language, request.preferences_file, request.user_folder);
        } else {
            use_language(dialog.opened.language, request.preferences_file, request.user_folder);
        }
        if (request.fonts != nullptr) {
            use_game_files_language_fonts(*request.fonts);
            text.lines.clear();
        }
        return accepted;
    };
    while (true) {
        if (hooks.viewport != nullptr) {
            const auto next = hooks.viewport(hooks.context);
            if (next.width != viewport.width || next.height != viewport.height ||
                next.px_per_point != viewport.px_per_point) {
                viewport = next;
                place = place_dialog(viewport, current.interface_size, offer_fractional);
                dialog.size_class = place.size_class;
                dirty = true;
            }
        }
        if (dirty && viewport.width > 0 && viewport.height > 0 && hooks.present != nullptr) {
            hooks.present(hooks.context, compose_frame(request.under, viewport, place, dialog));
            dirty = false;
        }
        if (hooks.pass != nullptr)
            hooks.pass(hooks.context);
        SDL_Event event{};
        if (!SDL_WaitEventTimeout(&event, hooks.pass != nullptr ? check_wait_ms : idle_wait_ms))
            continue;
        do {
            settings::DialogAction action = settings::DialogAction::none;
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                // The screen ends too: the event is passed on.
                SDL_PushEvent(&event);
                return finish(false);
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_SHOWN:
            // SDL queues no lifecycle event; coming back restores the window.
            case SDL_EVENT_WINDOW_RESTORED:
                dirty = true;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.which != SDL_TOUCH_MOUSEID) {
                    pointer_x = event.motion.x * viewport.px_per_point;
                    pointer_y = event.motion.y * viewport.px_per_point;
                    action = settings::dialog_pointer_move(
                        dialog, source_x(pointer_x), source_y(pointer_y)
                    );
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.which != SDL_TOUCH_MOUSEID &&
                    event.button.button == SDL_BUTTON_LEFT) {
                    pointer_x = event.button.x * viewport.px_per_point;
                    pointer_y = event.button.y * viewport.px_per_point;
                    action = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                                 ? settings::dialog_pointer_down(
                                       dialog, source_x(pointer_x), source_y(pointer_y)
                                   )
                                 : settings::dialog_pointer_up(
                                       dialog, source_x(pointer_x), source_y(pointer_y)
                                   );
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: {
                if (event.wheel.which == SDL_TOUCH_MOUSEID)
                    break;
                float notches = event.wheel.y * wheel_notch;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                    notches = -notches;
                action = settings::dialog_wheel(
                    dialog, source_x(pointer_x), source_y(pointer_y), notches
                );
                break;
            }
            case SDL_EVENT_FINGER_DOWN:
                if (!finger && accepted_touch(event.tfinger.touchID)) {
                    finger = event.tfinger.fingerID;
                    finger_device = event.tfinger.touchID;
                    const float x = event.tfinger.x * static_cast<float>(viewport.width);
                    const float y = event.tfinger.y * static_cast<float>(viewport.height);
                    const int32_t reach = std::max(
                        1,
                        static_cast<int32_t>(std::lround(
                            oa::ui::game_files::pick_reach_points * viewport.px_per_point /
                            static_cast<float>(place.scale)
                        ))
                    );
                    action = settings::dialog_finger_down(dialog, source_x(x), source_y(y), reach);
                }
                break;
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
                if (finger && event.tfinger.fingerID == *finger &&
                    event.tfinger.touchID == finger_device) {
                    const float x = event.tfinger.x * static_cast<float>(viewport.width);
                    const float y = event.tfinger.y * static_cast<float>(viewport.height);
                    if (event.type == SDL_EVENT_FINGER_MOTION) {
                        action = settings::dialog_pointer_move(dialog, source_x(x), source_y(y));
                    } else {
                        // A cancelled touch lets go away from every control.
                        const bool cancelled = event.type == SDL_EVENT_FINGER_CANCELED;
                        action = settings::dialog_pointer_up(
                            dialog,
                            cancelled ? -place.width : source_x(x),
                            cancelled ? -place.height : source_y(y)
                        );
                        finger.reset();
                    }
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (const auto key = dialog_key_of(event.key))
                    action = settings::dialog_key(dialog, *key);
                break;
            default:
                break;
            }
            switch (action) {
            case settings::DialogAction::none:
                break;
            case settings::DialogAction::redraw:
                dirty = true;
                break;
            case settings::DialogAction::changed:
                // The language chosen shows at once, in the dialog's own words too.
                use_language(dialog.chosen.language, request.preferences_file, request.user_folder);
                if (request.fonts != nullptr) {
                    use_game_files_language_fonts(*request.fonts);
                    text.lines.clear();
                }
                dirty = true;
                break;
            case settings::DialogAction::accepted:
                return finish(true);
            case settings::DialogAction::cancelled:
                return finish(false);
            case settings::DialogAction::manage_game_files:
            case settings::DialogAction::open_folder:
            case settings::DialogAction::reset_install_id:
            case settings::DialogAction::empty_downloads:
            case settings::DialogAction::download_language:
            case settings::DialogAction::add_registry:
            case settings::DialogAction::add_mirror:
            case settings::DialogAction::open_library:
            case settings::DialogAction::show_gallery:
            case settings::DialogAction::check_content:
            case settings::DialogAction::copy_content_again:
            case settings::DialogAction::open_content_folder:
            case settings::DialogAction::cancel_content_copy:
            case settings::DialogAction::check_updates:
            case settings::DialogAction::open_release:
                // The Language dialog lists none of the controls that ask
                // for these, and no language that is only available to download.
                dirty = true;
                break;
            }
        } while (SDL_PollEvent(&event));
    }
}

} // namespace oa::app
