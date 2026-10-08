// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Language settings over the Game files screen, before the game's
// fonts are installed: the settings dialog limited to that section, drawn in
// the bundled fonts, with the screen's own text hooks while it is up
// (game_files_screen.hpp). The interface's language is chosen here too for
// the screen, from the preferences file, before the Runtime starts.
#include "game_files_screen.hpp"
#include "language_packs.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/locale.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"

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
/// The margin kept round the dialog, in points.
constexpr float dialog_margin_points = 16.0F;
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

/// Reads the catalogue files beside the game once, and the engine's
/// language packs' interface.tdf after them.
void read_screen_catalogue() {
    static bool read = false;
    if (read)
        return;
    read = true;
    const std::string base = oa::platform::program_directory();
    if (base.empty())
        return;
    const fs::path folder = path_from_utf8(base.c_str()) / path_from_utf8(catalogue_folder);
    std::error_code error;
    if (!fs::is_directory(folder, error))
        return;
    std::vector<fs::path> files;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end;
         entry.increment(error))
        if (entry->is_regular_file(error) && catalogue_file(path_to_utf8(entry->path().filename())))
            files.push_back(entry->path());
    // A later file's translation of a text replaces an earlier one's.
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::ifstream input(file, std::ios::binary);
        std::string text;
        if (input)
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        std::string failure;
        if (!input || !screen_catalogue().add(text, &failure))
            std::cerr << "open-annihilation: the interface catalogue " << path_to_utf8(file)
                      << " was not read" << (failure.empty() ? "" : ": ") << failure << '\n';
    }
    // The engine's language packs' own words, read after the files beside
    // them; the screen shows before any game data is found, so its words
    // come from the packs alone, and their other tables are let go.
    std::vector<std::unique_ptr<LoadedLanguagePack>> packs;
    read_language_packs(folder, packs, &screen_catalogue());
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

/// Puts a language choice in effect for the interface's words.
///
/// @param choice the setting's value: system_choice or a language's tag
void use_language(std::string_view choice) {
    read_screen_catalogue();
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

/// Where the dialog lies on the canvas, and how large.
struct DialogPlace {
    int32_t x{};      ///< the canvas column of the dialog's left edge
    int32_t y{};      ///< the canvas row of its top edge
    int32_t scale{1}; ///< canvas pixels across each of the dialog's pixels
};

/// Places the dialog centred on a viewport at the largest whole scale that fits with a margin.
///
/// @param viewport the viewport
/// @return the place
[[nodiscard]] DialogPlace place_dialog(const oa::ui::game_files::Viewport& viewport) noexcept {
    const float margin = dialog_margin_points * viewport.px_per_point * 2.0F;
    const int32_t across = static_cast<int32_t>(std::floor(
        (static_cast<float>(viewport.width) - margin) / static_cast<float>(settings::dialog_width)
    ));
    const int32_t down = static_cast<int32_t>(std::floor(
        (static_cast<float>(viewport.height) - margin) / static_cast<float>(settings::dialog_height)
    ));
    DialogPlace place;
    place.scale = std::max(1, std::min(across, down));
    place.x = (viewport.width - settings::dialog_width * place.scale) / 2;
    place.y = (viewport.height - settings::dialog_height * place.scale) / 2;
    return place;
}

/// Draws the screen's frame darkened with the dialog over it.
///
/// @param under the screen's frame; another size than the viewport's leaves the background
/// @param viewport the viewport
/// @param place where the dialog lies
/// @param dialog the dialog
/// @return the frame
[[nodiscard]] touch_paint::Canvas compose_frame(
    const touch_paint::Canvas* under,
    const oa::ui::game_files::Viewport& viewport,
    const DialogPlace& place,
    const settings::Dialog& dialog
) {
    touch_paint::Canvas canvas = touch_paint::make_canvas(viewport.width, viewport.height);
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
    surface.width = static_cast<uint32_t>(settings::dialog_width * place.scale);
    surface.height = static_cast<uint32_t>(settings::dialog_height * place.scale);
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

/// Tells whether the dialog takes a touch device's fingers: a touch screen's, or the check's.
///
/// @param device the device
/// @return true for a direct touch device or the check's
[[nodiscard]] bool accepted_touch(SDL_TouchID device) noexcept {
    if (device == SDL_MOUSE_TOUCHID || device == SDL_PEN_TOUCHID)
        return false;
    if (device == game_files_check_touch_id)
        return true;
    return SDL_GetTouchDeviceType(device) == SDL_TOUCH_DEVICE_DIRECT;
}

} // namespace

void install_game_files_language(const std::optional<fs::path>& preferences_file) {
    const auto values =
        load_preferences(preferences_file.value_or(oa::platform::preferences::default_file()));
    use_language(settings::stored_language(values, !preferences_file.has_value()));
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
    DialogPlace place = place_dialog(viewport);
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
            use_language(dialog.chosen.language);
        } else {
            use_language(dialog.opened.language);
        }
        return accepted;
    };
    while (true) {
        if (hooks.viewport != nullptr) {
            const auto next = hooks.viewport(hooks.context);
            if (next.width != viewport.width || next.height != viewport.height ||
                next.px_per_point != viewport.px_per_point) {
                viewport = next;
                place = place_dialog(viewport);
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
                            cancelled ? -settings::dialog_width : source_x(x),
                            cancelled ? -settings::dialog_height : source_y(y)
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
                use_language(dialog.chosen.language);
                dirty = true;
                break;
            case settings::DialogAction::accepted:
                return finish(true);
            case settings::DialogAction::cancelled:
                return finish(false);
            case settings::DialogAction::manage_game_files:
            case settings::DialogAction::switch_mod:
            case settings::DialogAction::open_folder:
            case settings::DialogAction::roll_back_mod:
                // The Language dialog lists none of the controls that ask
                // for these.
                dirty = true;
                break;
            }
        } while (SDL_PollEvent(&event));
    }
}

} // namespace oa::app
