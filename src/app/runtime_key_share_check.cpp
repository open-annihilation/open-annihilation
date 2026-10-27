// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-key-share: sharing a layout, pasting text, importing one key from
// an older settings file, and installing a dropped .oakeys. With --snapshot
// it writes key-share-<w>x<h>.ppm, key-paste-<w>x<h>.ppm and
// key-import-<w>x<h>.ppm at 640x480, 1280x720 and 1920x1080.

#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/formats/zip.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/key_editor/screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace editor = oa::ui::key_editor;
namespace oakeys = oa::formats::oakeys;
namespace library = oa::data::key_library;
namespace zip = oa::formats::zip;
namespace first_run = oa::ui::first_run;
namespace layout = oa::ui::display_layout;
namespace install = oa::app::package_install;

/// The modifier a paste chord takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kPasteModifier = SDL_KMOD_GUI;
constexpr SDL_Keycode kPasteKey = SDLK_LGUI;
#else
constexpr SDL_Keymod kPasteModifier = SDL_KMOD_CTRL;
constexpr SDL_Keycode kPasteKey = SDLK_LCTRL;
#endif

/// The text PASTE… is given.
constexpr std::string_view kPasteText = "order.move: [\"@KeyQ\"]\n"
                                        "select.all: [Ctrl+B]\n"
                                        "camera.orbit-left: [Q]\n";

/// One window the check opens the editor in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the pictures are taken. The steps run at the large one.
constexpr std::array<CheckWindow, 3> kWindows{{
    {640, 480},
    {1280, 720},
    {1920, 1080},
}};

/// The large window. The steps run here, not at Compact.
constexpr CheckWindow kLarge = kWindows[1];

/// How many times the install question is asked before the check stops.
constexpr int kMostFrames = 400;

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("key share check: " + std::string(what));
}

/// Names one step, so the log lists step 7.
///
/// @param line the step
void note(std::string_view line) {
    std::cout << "key share check: " << line << '\n';
}

/// Reads a whole file.
///
/// @param path the file
/// @return its bytes; empty when it cannot be opened
std::vector<uint8_t> read_all(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes;
    if (!in)
        return bytes;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0)
        return bytes;
    in.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!in)
        bytes.clear();
    return bytes;
}

/// Writes a text file, creating its folder.
///
/// @param path the file
/// @param text the text
void write_text(const fs::path& path, std::string_view text) {
    std::error_code made;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), made);
    require(!made, "a scratch folder was not made");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(out), "a scratch file was not written");
}

/// A layout manifest the packer accepts.
///
/// @param id the layout's id
/// @param name the name players see
/// @return the manifest
oakeys::Manifest manifest_of(std::string id, std::string name) {
    oakeys::Manifest manifest;
    manifest.format = oakeys::format_version;
    manifest.id = std::move(id);
    manifest.name = std::move(name);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ridge";
    manifest.tags = {"grid"};
    manifest.base = oakeys::Base::oa_default;
    return manifest;
}

/// Packs a manifest and writes it beside the player's folder.
///
/// @param path the package
/// @param manifest the layout
void write_layout_package(const fs::path& path, const oakeys::Manifest& manifest) {
    std::error_code made;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), made);
    require(!made, "a package folder was not made");
    const std::string yaml = oakeys::write_manifest(manifest);
    library::PackageFile held;
    held.name = std::string(oakeys::manifest_file);
    held.bytes.assign(yaml.begin(), yaml.end());
    const std::array<library::PackageFile, 1> files{std::move(held)};
    std::string problem;
    const std::optional<std::vector<uint8_t>> packed = library::package_bytes(files, &problem);
    require(packed.has_value(), problem.empty() ? "the package was not made" : problem);
    require(
        library::write_package(path, *packed, &problem),
        problem.empty() ? "the package was not written" : problem
    );
}

/// The prompt's words, joined.
///
/// @param question the prompt
/// @return the paragraphs
std::string prompt_body(const oa::ui::kit::Question& question) {
    std::string body;
    for (const auto& paragraph : question.paragraphs) {
        if (!body.empty())
            body.push_back('\n');
        body += paragraph.text;
    }
    return body;
}

/// True when a diagnostic names a line and a column.
///
/// @param body the prompt
/// @return true when one oakeys.yaml line does
bool lists_a_place(std::string_view body) {
    constexpr std::string_view marker = "oakeys.yaml:";
    for (std::size_t at = 0; (at = body.find(marker, at)) != std::string_view::npos;) {
        std::size_t index = at + marker.size();
        bool line = false;
        while (index < body.size() && body[index] >= '0' && body[index] <= '9') {
            line = true;
            ++index;
        }
        bool column = false;
        if (line && index < body.size() && body[index] == ':') {
            ++index;
            while (index < body.size() && body[index] >= '0' && body[index] <= '9') {
                column = true;
                ++index;
            }
        }
        if (line && column && body.substr(at, index - at).find(":0:0") == std::string_view::npos)
            return true;
        at = index;
    }
    return false;
}

} // namespace

void Runtime::check_key_share() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();

    const fs::path import_folder = user_folder_.parent_path() / "share-mod";
    write_text(import_folder / "Settings.ini", "[Settings]\nWhiteboardKey=114\n");
    key_import_folder_ = import_folder;
    key_share_save_path_ = user_folder_ / "grid-left-hand.oakeys";

    const fs::path packages = user_folder_.parent_path() / "share-files";
    oakeys::Manifest bad = manifest_of("ridge-bad", "Ridge bad");
    oakeys::BindingEntry broken;
    broken.action = "order.move";
    broken.chords.push_back(oakeys::ChordText{"Ctrl+", 0, 0});
    bad.bindings.push_back(std::move(broken));
    oakeys::BindingEntry unknown;
    unknown.action = "camera.orbit-left";
    unknown.chords.push_back(oakeys::ChordText{"Q", 0, 0});
    bad.bindings.push_back(std::move(unknown));
    write_layout_package(packages / "ridge-bad.oakeys", bad);
    write_layout_package(packages / "ridge-good.oakeys", manifest_of("ridge-good", "Ridge good"));

    require(stage_clipboard_text(kPasteText), "the clipboard was not set");

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto require_named = [&](std::string_view name, std::string_view why) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), why);
        return *control;
    };
    const auto pointer_at = [&](layout::Point pixel, SDL_EventType type, uint8_t button) {
        SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
    };
    const auto click_named = [&](std::string_view name) {
        const AutomationControl control = require_named(name, std::string(name) + " is not shown");
        const layout::Point pixel{
            control.window_x + control.window_width / 2,
            control.window_y + control.window_height / 2
        };
        pointer_at(pixel, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(1);
    };
    const auto key_event = [&](SDL_EventType type, SDL_Keycode key, SDL_Keymod mods) {
        SDL_Event event{};
        event.type = type;
        event.key.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
        event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
        event.key.key = key;
        event.key.mod = mods;
        event.key.down = type == SDL_EVENT_KEY_DOWN;
        event.key.repeat = false;
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    // Compact keeps IMPORT, SHARE and PASTE in MORE, and the menu shows eight
    // rows. End brings the last row into view when PASTE is past that.
    const auto press_named = [&](std::string_view name) {
        if (!named(name) && named("oa.key-editor.more")) {
            click_named("oa.key-editor.more");
            if (!named(name)) {
                key_event(SDL_EVENT_KEY_DOWN, SDLK_END, SDL_KMOD_NONE);
                key_event(SDL_EVENT_KEY_UP, SDLK_END, SDL_KMOD_NONE);
                frames(1);
            }
        }
        click_named(name);
    };
    const auto type_chars = [&](std::string_view words) {
        for (const char letter : words) {
            const char text[2] = {letter, '\0'};
            SDL_Event typed{};
            typed.type = SDL_EVENT_TEXT_INPUT;
            typed.text.windowID = SDL_GetWindowID(sdl_.window);
            typed.text.text = text;
            dispatch_event(typed, running);
            require(running, "typed text ended the run");
        }
        frames(1);
    };
    const auto show_row = [&](std::string_view name) {
        for (int32_t pass = 0; pass < 30 && !named(name); ++pass) {
            require(named("oa.key-editor.list").has_value(), "the list is not shown");
            if (pass == 0)
                click_named("oa.key-editor.list");
            PadAccess::send_key(*this, SDLK_PAGEDOWN);
            frames(1);
        }
        require(named(name).has_value(), std::string(name) + " is not shown");
    };
    const auto snapshot = [&](std::string_view stem, const CheckWindow& window) {
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        const fs::path path = options_.snapshot.parent_path() /
                              (std::string(stem) + '-' + std::to_string(window.width) + 'x' +
                               std::to_string(window.height) + ".ppm");
        write_ppm(path, presented);
        require(fs::exists(path), "a snapshot was not written");
    };
    const auto resize = [&](const CheckWindow& window) {
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take the size"
        );
        apply_output_mode();
    };
    const auto dismiss_welcome = [&] {
        if (LayerScreen* welcome = oa_layer().find("first-run")) {
            oa_layer().close(welcome);
            frames(1);
        }
    };
    const auto close_editor = [&] {
        if (LayerScreen* open = oa_layer().find(editor::screen_name)) {
            oa_layer().close(open);
            frames(1);
        }
    };
    const auto open_editor = [&] {
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open");
        frames(2);
        dismiss_welcome();
        close_editor();
        require(open_key_editor({}), "the editor did not open");
        frames(2);
        require(oa_layer().find(editor::screen_name) != nullptr, "the editor is not shown");
    };
    const auto open_share = [&](const CheckWindow& window, bool keep) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        note(std::string("SHARE picture ") + size);
        press_named("oa.key-editor.share");
        require(named("oa.key-editor.share.close").has_value(), "SHARE did not open");
        if (named("oa.key-editor.share.name").has_value())
            type_chars("Grid, left hand");
        snapshot("key-share", window);
        if (!keep) {
            click_named("oa.key-editor.share.close");
            require(!named("oa.key-editor.share.close").has_value(), "SHARE stayed open");
            return;
        }
        note("SHARE shows grid-left-hand");
        const AutomationControl id =
            require_named("oa.key-editor.share.id", "the share id is not shown");
        require(id.text == "grid-left-hand", "SHARE did not show grid-left-hand");
        note("COPY AS TEXT matches the text form");
        const std::string form =
            require_named("oa.key-editor.share.text", "the share text is not shown").text;
        click_named("oa.key-editor.share.copy");
        char* clipboard = SDL_GetClipboardText();
        const std::string copied = clipboard != nullptr ? clipboard : "";
        SDL_free(clipboard);
        require(copied == form, "COPY AS TEXT did not put the text form on the clipboard");
        note("SAVE FILE writes a package");
        require(
            require_named("oa.key-editor.share.save-file", "SAVE FILE is not shown").enabled,
            "SAVE FILE is disabled"
        );
        click_named("oa.key-editor.share.save-file");
        const std::vector<uint8_t> packed = read_all(key_share_save_path_);
        require(!packed.empty(), "SAVE FILE wrote no file");
        zip::CentralDirectory directory;
        zip::ZipError zip_fault;
        require(zip::read_directory(packed, directory, zip_fault), "the package did not read");
        const zip::Entry* manifest_entry = zip::find_entry(directory, oakeys::manifest_file);
        require(manifest_entry != nullptr, "the package holds no oakeys.yaml");
        std::vector<uint8_t> yaml;
        require(
            zip::read_entry(packed, *manifest_entry, yaml, zip_fault), "oakeys.yaml did not unpack"
        );
        oakeys::Manifest read;
        std::vector<oakeys::Problem> problems;
        require(
            oakeys::read_manifest(yaml, read, problems) && problems.empty(),
            "the shared layout has a problem"
        );
        require(read.id == "grid-left-hand", "the shared layout's id is not grid-left-hand");
        click_named("oa.key-editor.share.close");
    };
    const auto open_paste = [&](const CheckWindow& window, bool keep) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        note(std::string("PASTE picture ") + size);
        if (keep) {
            if (named("oa.key-editor.search").has_value())
                click_named("oa.key-editor.list");
            // COPY AS TEXT replaced the sample. Put it back for Primary+V.
            require(stage_clipboard_text(kPasteText), "the clipboard was not set");
            note("PASTE previews three rows, one clash and one unknown");
            key_event(SDL_EVENT_KEY_DOWN, kPasteKey, kPasteModifier);
            key_event(SDL_EVENT_KEY_DOWN, SDLK_V, kPasteModifier);
            key_event(SDL_EVENT_KEY_UP, SDLK_V, kPasteModifier);
            key_event(SDL_EVENT_KEY_UP, kPasteKey, SDL_KMOD_NONE);
            frames(1);
        } else {
            press_named("oa.key-editor.paste");
        }
        require(named("oa.key-editor.paste.cancel").has_value(), "PASTE did not open");
        snapshot("key-paste", window);
        if (!keep) {
            click_named("oa.key-editor.paste.cancel");
            require(!named("oa.key-editor.paste.cancel").has_value(), "PASTE stayed open");
            return;
        }
        const AutomationControl summary =
            require_named("oa.key-editor.paste.summary", "the paste summary is not shown");
        const AutomationControl rows =
            require_named("oa.key-editor.paste.list", "the paste list is not shown");
        require(summary.text.find('3') != std::string::npos, "the paste does not name 3 keys");
        require(rows.text.find("order.move") != std::string::npos, "order.move is not listed");
        require(rows.text.find("select.all clash") != std::string::npos, "the clash is not listed");
        require(
            rows.text.find("camera.orbit-left unknown") != std::string::npos,
            "the unknown action is not listed"
        );
        note("APPLY holds SAVE back until the card settles the clash");
        const AutomationControl apply =
            require_named("oa.key-editor.paste.apply", "APPLY is not shown");
        require(apply.enabled && apply.text.find('3') != std::string::npos, "APPLY 3 is not shown");
        click_named("oa.key-editor.paste.apply");
        require(!named("oa.key-editor.paste.apply").has_value(), "APPLY left the paste open");
        require(
            !require_named("oa.key-editor.save", "SAVE is not shown").enabled,
            "SAVE was not held back"
        );
        click_named("oa.key-editor.search");
        type_chars("select.all");
        show_row("oa.key-editor.row.select.all");
        click_named("oa.key-editor.row.select.all");
        for (int32_t pass = 0; pass < 6 && !named("oa.key-editor.card.keep-here"); ++pass) {
            SDL_Event wheel{};
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.windowID = SDL_GetWindowID(sdl_.window);
            wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            wheel.wheel.y = -1.0F;
            wheel.wheel.mouse_x = static_cast<float>(window.width) * 0.75F;
            wheel.wheel.mouse_y = static_cast<float>(window.height) * 0.5F;
            dispatch_event(wheel, running);
            require(running, "a wheel ended the run");
            frames(1);
        }
        click_named("oa.key-editor.card.keep-here");
        require(
            require_named("oa.key-editor.save", "SAVE is not shown after the card").enabled,
            "SAVE stayed held after the card"
        );
    };
    const auto open_import = [&](const CheckWindow& window, bool keep) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        note(std::string("IMPORT picture ") + size);
        press_named("oa.key-editor.import");
        click_named("oa.key-editor.import.from.tdraw");
        require(named("oa.key-editor.import.cancel").has_value(), "IMPORT did not open");
        snapshot("key-import", window);
        if (!keep) {
            click_named("oa.key-editor.import.cancel");
            require(!named("oa.key-editor.import.cancel").has_value(), "IMPORT stayed open");
            return;
        }
        const AutomationControl summary =
            require_named("oa.key-editor.import.summary", "the import summary is not shown");
        const AutomationControl rows =
            require_named("oa.key-editor.import.list", "the import list is not shown");
        require(summary.text.find("Found 1") != std::string::npos, "IMPORT did not find one key");
        require(rows.text.find("whiteboard.draw") != std::string::npos, "F3 is not listed");
        note("IMPORT one key, then SAVE writes whiteboard.draw and imported-from");
        const AutomationControl apply =
            require_named("oa.key-editor.import.apply", "IMPORT KEYS is not shown");
        require(
            apply.enabled && apply.text.find('1') != std::string::npos, "IMPORT 1 is not shown"
        );
        click_named("oa.key-editor.import.apply");
        require(!named("oa.key-editor.import.apply").has_value(), "IMPORT left the prompt open");
        click_named("oa.key-editor.save");
        require(oa_layer().find(editor::screen_name) == nullptr, "SAVE did not close the editor");
        const fs::path own = user_folder_ / std::string(library::keys_folder_name) /
                             library::own_folder_name("oa-default") /
                             std::string(oakeys::manifest_file);
        const std::vector<uint8_t> yaml = read_all(own);
        const std::string text(yaml.begin(), yaml.end());
        require(
            text.find("whiteboard.draw") != std::string::npos, "whiteboard.draw was not written"
        );
        require(text.find("F3") != std::string::npos, "F3 was not written");
        require(text.find("imported-from") != std::string::npos, "imported-from was not written");
        require(text.find("tdraw") != std::string::npos, "imported-from does not name tdraw");
        require(
            text.find("camera.orbit-left") != std::string::npos, "the unknown action was dropped"
        );
    };

    for (const CheckWindow& window : kWindows) {
        if (window.width == kLarge.width)
            continue;
        resize(window);
        open_editor();
        open_share(window, false);
        open_paste(window, false);
        open_import(window, false);
        close_editor();
    }

    note("the steps at 1280x720");
    resize(kLarge);
    open_editor();
    open_share(kLarge, true);
    open_paste(kLarge, true);
    open_import(kLarge, true);

    note("a bad layout lists both problems and refuses INSTALL");
    auto& state = mod_install_state();
    state.check_shows_prompts = true;
    fake_frontend_tick_ = 1000U;
    frames(1);
    const std::string layout_key = "open-annihilation.keys.layout";
    const auto before = preference_values_.find(layout_key);
    const std::string layout_before =
        before == preference_values_.end() ? std::string{} : before->second;
    require(layout_before != "ridge-good", "the chosen layout was already the dropped one");
    const auto until_title = [&](std::string_view title, std::string_view what) {
        for (int frame = 0; frame < kMostFrames; ++frame) {
            tell_mod_installs();
            if (state.prompt != nullptr && state.prompt->question().title == title)
                return;
        }
        require(false, std::string("no prompt showed: ") + std::string(what));
    };
    const auto drop_file = [&](const fs::path& path) {
        static std::string dropped;
        dropped = path_to_utf8(path);
        SDL_Event drop{};
        drop.type = SDL_EVENT_DROP_FILE;
        drop.drop.windowID = SDL_GetWindowID(sdl_.window);
        drop.drop.data = dropped.c_str();
        require(SDL_PushEvent(&drop), "the drop could not be sent");
    };
    const auto dismiss = [&] {
        key_event(SDL_EVENT_KEY_DOWN, SDLK_RETURN, SDL_KMOD_NONE);
        key_event(SDL_EVENT_KEY_UP, SDLK_RETURN, SDL_KMOD_NONE);
        frames(1);
        require(state.prompt == nullptr, "Enter did not close the prompt");
    };
    drop_file(packages / "ridge-bad.oakeys");
    until_title("KEY LAYOUT NOT INSTALLED", "the bad layout");
    const std::string refused = prompt_body(state.prompt->question());
    require(
        refused.find("This layout has problems:") != std::string::npos,
        "the refusal does not list the problems"
    );
    require(
        refused.find("bindings.order.move") != std::string::npos, "the bad chord is not listed"
    );
    require(
        refused.find("camera.orbit-left") != std::string::npos, "the unknown action is not listed"
    );
    require(lists_a_place(refused), "a problem is listed without its line and column");
    require(
        std::find(state.answers.begin(), state.answers.end(), install::Answer::alongside) ==
            state.answers.end(),
        "INSTALL was offered for a layout with an error"
    );
    require(!fs::exists(user_folder_ / "Keys" / "ridge-bad"), "the bad layout was installed");
    dismiss();

    note("a good layout installs and the chosen layout stays");
    drop_file(packages / "ridge-good.oakeys");
    until_title("INSTALL KEY LAYOUT", "the good layout");
    require(
        std::find(state.answers.begin(), state.answers.end(), install::Answer::play_now) ==
            state.answers.end(),
        "PLAY NOW was offered"
    );
    require(
        std::find(state.answers.begin(), state.answers.end(), install::Answer::alongside) !=
            state.answers.end(),
        "INSTALL is not offered"
    );
    // The install names each button by the answer it gives (alongside), not by
    // its place, so the control is prompt.alongside.
    frames(1);
    click_named("oa.prompt.alongside");
    until_title("KEY LAYOUT INSTALLED", "the good layout installed");
    dismiss();
    require(
        fs::exists(user_folder_ / "Keys" / "ridge-good" / "oakeys.yaml"),
        "the good layout is not in Keys"
    );
    const auto after = preference_values_.find(layout_key);
    const std::string layout_after =
        after == preference_values_.end() ? std::string{} : after->second;
    require(layout_after == layout_before, "installing switched the layout on");
    for (const auto& entry : preference_values_) {
        if (entry.first.starts_with(layout_key))
            require(entry.second != "ridge-good", "a layout preference names the installed layout");
    }
    note("done");
}

} // namespace oa::app
