// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-registries: the Library's Registries tab and the Add registry
// window over a registry list the check writes itself before the content
// service starts. Settings › Downloads links to that tab. One added
// registry, signed with a key the check makes from a fixed seed, its
// catalogue at 127.0.0.1:1, where nothing answers. The check shows the key
// that catalogue is checked against, asks that registry to refresh (refused,
// since nothing answers), switches it off and on at once, and removes it at
// once, reading the player's registry list after each; CLOSE does not put a
// change back. It types an https address on 127.0.0.1:1, which the content
// service is asked about and which is refused; and opens an .oareg file of
// another registry, which is refused once its catalogue cannot be reached.
// It makes no connection beyond those to 127.0.0.1.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/content/service.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/player_registries.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/url.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings/add_registry.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace registry = oa::data::registry;
namespace signing = oa::base::signing;

/// The modifier the settings' shortcut takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
#endif

/// The registry the check's list holds.
constexpr std::string_view kAddedId = "check-maps";
/// The registry the check's .oareg file describes.
constexpr std::string_view kOfferedId = "check-lan";
/// Where both registries' catalogues are: a port nothing answers on.
constexpr std::string_view kCatalogue = "http://127.0.0.1:1/v1/catalogue.json";
/// The address the refused .oareg is named by.
constexpr std::string_view kUnreachable = "127.0.0.1:1";
/// The https address the window asks the content service about. Nothing
/// answers on this port, and the address is not a public host.
constexpr std::string_view kHttpsAddress = "https://127.0.0.1:1/oa/registry.yaml";
/// The sentence when this system read https as http and the host served none.
constexpr std::string_view kNeedsHttps = "This registry needs HTTPS, which this system cannot use";
/// The .oareg file's name.
constexpr std::string_view kOfferedFile = "check-lan.oareg";
/// The window the check drives the dialog at: Large, at 1×, so that every
/// row it presses shows.
constexpr int kWindowWidth = 1280;
/// The window's height.
constexpr int kWindowHeight = 720;
/// The frames the check waits for the content service's answer.
constexpr int kAnswerFrames = 2000;

/// Throws when a step of the check failed.
///
/// @param ok the step held
/// @param what the step
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("registries check: " + std::string(what));
}

/// Returns a descriptor signed with a key made from a fixed seed.
///
/// @param id the registry's id
/// @param name its name
/// @param seed_byte the byte the seed is made of
/// @return the descriptor
registry::Descriptor
signed_descriptor(std::string_view id, std::string_view name, uint8_t seed_byte) {
    std::array<uint8_t, 32> seed{};
    seed.fill(seed_byte);
    signing::SecretKey secret{};
    signing::PublicKey key{};
    signing::key_pair_from_seed(seed, secret, key);
    registry::Descriptor descriptor;
    descriptor.id = std::string(id);
    descriptor.name = std::string(name);
    const std::optional<oa::formats::url::Url> catalogue =
        oa::formats::url::parse_http_url(kCatalogue);
    require(catalogue.has_value(), "the catalogue's address did not read");
    descriptor.catalogue = *catalogue;
    descriptor.keys.push_back(registry::RegistryKey{std::string(id) + "-key", key});
    descriptor.mode = registry::DownloadMode::direct;
    return descriptor;
}

/// Writes a text file whole.
///
/// @param file the file
/// @param text its text
void write_text(const std::filesystem::path& file, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    require(static_cast<bool>(out), "could not write " + oa::platform::path_to_utf8(file));
}

/// Reads a file's bytes whole.
///
/// @param file the file
/// @return its bytes; empty when it cannot be read
std::string file_bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Reads the player's registry list as the content service keeps it.
///
/// @param file the list
/// @return what it holds
registry::PlayerRegistries read_list(const std::filesystem::path& file) {
    registry::PlayerRegistries list;
    std::string error;
    require(
        registry::load_player_registries(file, list, &error) == registry::LoadResult::read,
        "the registry list did not read: " + error
    );
    return list;
}

/// Returns the added registry of an id in a list.
///
/// @param list the list
/// @param id the id
/// @return the registry; null when the list does not hold it
const registry::AddedRegistry*
added_of(const registry::PlayerRegistries& list, std::string_view id) {
    for (const registry::AddedRegistry& added : list.registries)
        if (added.descriptor.id == id)
            return &added;
    return nullptr;
}

/// Returns the path of one step's snapshot: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's snapshot path
std::filesystem::path step_snapshot(const std::filesystem::path& snapshot, std::string_view step) {
    std::filesystem::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

/// Returns the text of a control of a display list.
///
/// @param list the list
/// @param name the control's name
/// @return its text; none when the list has no such control
std::optional<std::string> text_of(const kit::DisplayList& list, std::string_view name) {
    const kit::Control* control = kit::control_of(list, kit::control_named(list, name));
    if (control == nullptr)
        return std::nullopt;
    return control->text;
}

} // namespace

void Runtime::prepare_registries_check() {
    // Written before the content service reads the player's folder.
    const std::filesystem::path folder = user_folder();
    registry::PlayerRegistries list;
    registry::AddedRegistry added;
    added.descriptor = signed_descriptor(kAddedId, "Check Maps", 0x21);
    added.added = "2026-10-01";
    added.enabled = true;
    list.registries.push_back(added);
    write_text(
        folder / std::string(registry::player_registries_file),
        registry::player_registries_text(list)
    );
    write_text(
        folder / std::string(kOfferedFile),
        registry::descriptor_text(signed_descriptor(kOfferedId, "Check LAN", 0x42))
    );
    std::cout << "registries check: the registry list holds " << kAddedId << '\n';
}

void Runtime::check_registries() {
    require(sdl_.renderer != nullptr && sdl_.window != nullptr, "needs the SDL presenter");
    require(options_.preferences_file.has_value(), "needs --preferences-file");
    preference_values_.clear();
    oa::platform::preferences::save(preference_path_, preference_values_);
    fake_frontend_tick_ = 1000U;
    require(
        SDL_SetWindowSize(sdl_.window, kWindowWidth, kWindowHeight) && SDL_SyncWindow(sdl_.window),
        "the window did not take its size"
    );
    apply_output_mode();
    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");
    const std::filesystem::path list_file =
        user_folder() / std::string(registry::player_registries_file);

    bool running = true;
    const auto tap = [this, &running](SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.mod = modifiers;
            event.key.down = down;
            dispatch_event(event, running);
        }
        require(running, "a key ended the run");
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
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            dispatch_event(event, running);
            require(running, "a click ended the run");
        }
    };
    // With --snapshot, the frame as presented at a step.
    const auto keep_frame = [this](std::string_view step) {
        if (options_.snapshot.empty())
            return;
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        write_ppm(step_snapshot(options_.snapshot, step), presented);
    };
    const auto open_downloads = [&] {
        tap(SDLK_COMMA, kShortcutModifier);
        require(engine_settings_dialog() != nullptr, "the shortcut did not open Settings");
        click_named("oa.settings.nav.downloads");
        require(
            named("oa.settings.registries-in-library").has_value(),
            "Downloads has no link to the Library"
        );
    };
    const auto open_registries = [&] {
        open_downloads();
        keep_frame("menu-downloads");
        click_named("oa.settings.registries-in-library");
        require(
            oa_layer().top() != nullptr && oa_layer().top()->name() == "library",
            "the link did not open the Library"
        );
        require(
            named("oa.library.tab.registries").has_value(),
            "the Library did not show its Registries tab"
        );
    };
    const auto close_library_and_settings = [&] {
        if (oa_layer().find("library") != nullptr)
            click_named("oa.library.close");
        require(oa_layer().find("library") == nullptr, "CLOSE did not close the Library");
        if (engine_settings_dialog() != nullptr)
            click_named("oa.settings.cancel");
        require(engine_settings_dialog() == nullptr, "Cancel did not close Settings");
    };
    const auto window_list = [this]() -> kit::DisplayList {
        LayerScreen* top = oa_layer().top();
        require(
            top != nullptr && top->name() == "add-registry", "the Add registry window is not on top"
        );
        return top->display_list();
    };

    // The tab lists the added registry with REMOVE, and every built-in
    // registry this build has with its switch, its key icon and REFRESH,
    // and no REMOVE.
    open_registries();
    const std::string added_switch = "oa.library.registry." + std::string(kAddedId);
    require(named(added_switch).has_value(), "the added registry is not listed");
    require(named(added_switch + ".remove").has_value(), "the added registry has no REMOVE");
    std::size_t built_ins = 0;
    if (const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot()) {
        for (const content::RegistryView& listed : snapshot->registries) {
            if (listed.registry.origin != registry::Origin::built_in)
                continue;
            const std::string word = "oa.library.registry." + listed.registry.descriptor.id;
            require(named(word).has_value(), word + " is not listed");
            require(!named(word + ".remove").has_value(), word + " offers REMOVE");
            require(!named(word + ".add-mirror").has_value(), word + " offers ADD MIRROR…");
            require(named(word + ".key").has_value(), word + " has no key icon");
            require(named(word + ".refresh").has_value(), word + " has no REFRESH");
            ++built_ins;
        }
    }
    keep_frame("menu-registries");
    std::cout << "registries check: listed " << kAddedId << " with REMOVE and " << built_ins
              << " built-in registr" << (built_ins == 1 ? "y" : "ies") << " without\n";

    // The added row shows where its catalogue is, the key that catalogue is
    // checked against, and REFRESH. The key icon asks for nothing; resting
    // on it shows the fingerprint and the full public key.
    const std::string added_address = added_switch + ".address";
    const std::string added_key = added_switch + ".key";
    const std::string added_refresh = added_switch + ".refresh";
    const std::string added_refreshed = added_switch + ".refreshed";
    {
        const std::optional<AutomationControl> address = named(added_address);
        require(
            address.has_value() && address->text.find("127.0.0.1") != std::string::npos,
            "the row does not show the catalogue address"
        );
        require(named(added_refresh).has_value(), "the added registry has no REFRESH");
        require(named(added_refreshed).has_value(), "the added registry has no refresh time");
        std::array<uint8_t, 32> seed{};
        seed.fill(0x21);
        signing::SecretKey secret{};
        signing::PublicKey public_key{};
        signing::key_pair_from_seed(seed, secret, public_key);
        const signing::FingerprintText fingerprint = signing::fingerprint(public_key);
        const signing::KeyText key_text = signing::public_key_text(public_key);
        const std::optional<AutomationControl> icon = named(added_key);
        require(
            icon.has_value() && icon->text.find(fingerprint.view()) != std::string::npos,
            "the key icon does not name the signing key"
        );
        const uint64_t generation_before = content_service().generation();
        click_named(added_key);
        require(
            content_service().generation() == generation_before, "the key icon asked for a refresh"
        );
        run_frame(running);
        require(running, "a frame ended the run");
        LayerScreen* library_screen = oa_layer().top();
        require(
            library_screen != nullptr && library_screen->name() == "library",
            "the key icon left the Library"
        );
        const kit::DisplayList shown = library_screen->display_list();
        bool card_holds_key = false;
        for (const kit::Item& item : shown.items) {
            if (item.role != kit::Role::hover_card)
                continue;
            const auto* look = std::get_if<kit::HoverCardLook>(&item.look);
            if (look == nullptr)
                continue;
            std::string card = look->title;
            card += look->aside;
            for (const std::string& line : look->block)
                card += line;
            card_holds_key = look->aside == "added by you" &&
                             card.find(fingerprint.view()) != std::string::npos &&
                             card.find(key_text.view()) != std::string::npos;
            if (card_holds_key)
                break;
        }
        require(card_holds_key, "the key tooltip does not show the registry's signing key");
    }
    std::cout << "registries check: the key tooltip shows the signing key\n";

    // REFRESH asks that one catalogue. Nothing answers at 127.0.0.1:1, so the
    // row says it could not be reached and the button can be pressed again.
    {
        bool refresh_ready = false;
        for (int frame = 0; frame < kAnswerFrames && !refresh_ready; ++frame) {
            const std::optional<AutomationControl> button = named(added_refresh);
            require(button.has_value(), "REFRESH left the row");
            refresh_ready = button->enabled;
            if (!refresh_ready) {
                run_frame(running);
                require(running, "a frame ended the run");
            }
        }
        require(refresh_ready, "REFRESH stayed disabled");
        const uint64_t generation_before = content_service().generation();
        click_named(added_refresh);
        bool saw_working = false;
        bool refresh_done = false;
        int frames_run = 0;
        for (int frame = 0; frame < kAnswerFrames && !refresh_done; ++frame) {
            const std::optional<AutomationControl> button = named(added_refresh);
            const std::optional<AutomationControl> line = named(added_refreshed);
            require(button.has_value() && line.has_value(), "REFRESH or its time left the row");
            if (!button->enabled)
                saw_working = true;
            // The click asks at once. The row shows that on the next frame,
            // so a row still reading as it did before the frame does not count.
            const bool settled =
                button->enabled && line->text.find("couldn't reach") != std::string::npos;
            const bool asked = content_service().generation() != generation_before &&
                               (saw_working || frames_run > 0);
            if (asked && settled)
                refresh_done = true;
            else {
                run_frame(running);
                require(running, "a frame ended the run");
                ++frames_run;
            }
        }
        require(refresh_done, "REFRESH did not report that the catalogue could not be reached");
    }
    std::cout << "registries check: REFRESH was refused for " << kAddedId << '\n';

    // Off applies at once, without OK.
    click_named(added_switch + ".off");
    {
        const registry::PlayerRegistries list = read_list(list_file);
        const registry::AddedRegistry* added = added_of(list, kAddedId);
        require(added != nullptr && !added->enabled, "the switch did not store the registry off");
        require(
            file_bytes(list_file).find("enabled: false") != std::string::npos,
            "the list does not say enabled: false"
        );
    }
    std::cout << "registries check: the switch stored the registry turned off\n";

    // On applies at once. CLOSE leaves the list as the switch wrote it.
    click_named(added_switch + ".on");
    {
        // Name the list: added_of's pointer is into it, and the temporary a
        // call returns is gone at the end of the statement.
        const registry::PlayerRegistries list = read_list(list_file);
        const registry::AddedRegistry* added = added_of(list, kAddedId);
        require(added != nullptr && added->enabled, "the switch did not store the registry on");
    }
    const std::string before_close = file_bytes(list_file);
    close_library_and_settings();
    require(file_bytes(list_file) == before_close, "CLOSE changed the registry list");
    std::cout << "registries check: CLOSE left the registry list as the switch wrote it\n";

    // REMOVE applies at once, with no UNDO.
    open_registries();
    click_named(added_switch + ".remove");
    require(!named(added_switch + ".undo").has_value(), "REMOVE offered UNDO");
    require(
        added_of(read_list(list_file), kAddedId) == nullptr, "REMOVE did not remove the registry"
    );
    std::cout << "registries check: REMOVE removed the registry\n";

    // ADD REGISTRY… with an https address on a closed port. The content
    // service is asked. TLS fails closed, or the address is read as http
    // and the host's silence is the needs-HTTPS sentence.
    const uint64_t asked = registry_checks_asked();
    click_named("oa.library.add-registry");
    require(!window_list().controls.empty(), "ADD REGISTRY… did not open the window");
    require(SDL_TextInputActive(sdl_.window), "the address field did not start text input");
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.windowID = SDL_GetWindowID(sdl_.window);
    const std::string https(kHttpsAddress);
    typed.text.text = https.c_str();
    dispatch_event(typed, running);
    require(running, "typing ended the run");
    require(
        text_of(window_list(), "add-registry.url") == https, "the field did not take the address"
    );
    tap(SDLK_RETURN);
    std::optional<std::string> https_refusal;
    for (int frame = 0; frame < kAnswerFrames && !https_refusal; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
        https_refusal = text_of(window_list(), "add-registry.refusal");
    }
    require(https_refusal.has_value(), "the https address's check never ended");
    const bool needs = *https_refusal == std::string(kNeedsHttps);
    const bool reached = https_refusal->find("127.0.0.1") != std::string::npos &&
                         (https_refusal->find("cannot be reached") != std::string::npos ||
                          https_refusal->find("certificate") != std::string::npos);
    require(needs || reached, "the https address was not refused: " + *https_refusal);
    require(
        https_refusal->find("Use the registry's http:// address") == std::string::npos,
        "the old https refusal is still shown"
    );
    require(
        registry_checks_asked() > asked, "the content service was not asked about an https address"
    );
    keep_frame("menu-add-registry-refused");
    click_named("oa.add-registry.cancel");
    require(oa_layer().find("add-registry") == nullptr, "CANCEL did not close the window");
    require(oa_layer().find("library") != nullptr, "CANCEL closed the Library too");
    close_library_and_settings();
    std::cout << "registries check: an https address was refused: " << *https_refusal << '\n';

    // An .oareg file of another registry, posted as --open posts it: the
    // window opens filled in, and ends refused, naming the address its
    // catalogue could not be reached at.
    oa::app::package_install::post_opened_file(user_folder() / std::string(kOfferedFile));
    LayerScreen* offered = nullptr;
    for (int frame = 0; frame < 60 && offered == nullptr; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
        offered = oa_layer().find("add-registry");
    }
    require(offered != nullptr, "the .oareg file did not open the window");
    std::optional<std::string> refusal;
    for (int frame = 0; frame < kAnswerFrames && !refusal; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
        refusal = text_of(window_list(), "add-registry.refusal");
        require(
            !text_of(window_list(), "add-registry.add").has_value(),
            "the window offered ADD for a registry whose catalogue cannot be reached"
        );
    }
    require(refusal.has_value(), "the .oareg file's check never ended");
    require(
        refusal->find(kUnreachable) != std::string::npos,
        "the refusal does not name " + std::string(kUnreachable) + ": " + *refusal
    );
    std::cout << "registries check: the .oareg file was refused: " << *refusal << '\n';
    keep_frame("menu-add-registry-file");
    click_named("oa.add-registry.cancel");
    require(oa_layer().find("add-registry") == nullptr, "CANCEL did not close the window");
    require(
        added_of(read_list(list_file), kOfferedId) == nullptr, "the refused registry was added"
    );
    std::cout << "registries check: passed\n";
}

} // namespace oa::app
