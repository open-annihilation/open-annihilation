// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-text-fit: opens every Open Annihilation screen in English, German,
// French, Spanish, Italian and Simplified Chinese, or in the languages
// --check-language names, such as an installed pack's, at 640x480 and 1280x720.
// The kit records a text drawn wider than its box or across another text.
// The check fails naming the screen, the language and the text. The kit
// records only while a text is drawn, so the layer draws every screen again
// for the frame read, those whose drawing it keeps among them; a kept
// notice whose OK is too wide for its button must be recorded first. The Game
// files screen and the folder chooser paint through their own painter, which
// records the same way. A startup notice drawn by the game's own windows is
// opened too; that painter keeps no record. SKIRMISH is painted with
// Mutators reading "Mutators" and "Mutators (5)", with three computer rows'
// brain buttons, one of them a long name, and NEWGAME with Mutators and with
// the campaign brain button showing that long name, in every language and
// both sizes; Mutators' words must read whole.
// The battle room is painted with one computer row whose brain has a long
// name, in every language and both sizes.
// The Brains tab's details are painted too: one brain not installed and one
// installed, at both sizes and at 960x540, Compact on its details page.
// "Load with the built-in player?" is painted with two 64-byte names.
// The brain picker is painted on its fixture feed: the list, the inline
// details at Regular and Large, and Compact's details page.
// The key editor's list is painted at both sizes, and Compact's details page,
// then the capture, the clash prompt, the keyboard, the mouse and the draft card.
// The keys overlay is painted in a benchmark skirmish, in every language, at
// 640x480 only.
// Controls is painted with an own layout of OA default that holds twelve
// changes, in every language and both sizes, so Key layout's value is the
// layout's name and its count. The own layout is removed before the check ends.

#include "folder_chooser_screen.hpp"
#include "game_files_screen.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_import.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/main_menu.hpp"
#include "oa/ui/frontend_state/skirmish_ui.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/use_question.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/brain_picker/prompts.hpp"
#include "oa/ui/mutator_picker/prompts.hpp"
#include "oa/ui/mutator_picker/screen.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace library = oa::ui::library;
namespace first_run = oa::ui::first_run;
namespace dialogs = oa::ui::frontend_dialogs;
namespace picker = oa::ui::mutator_picker;
namespace brains = oa::ui::brain_picker;
namespace brain_prompts = oa::ui::brain_picker;
namespace brain = oa::data::brain_library;
namespace layout = oa::ui::display_layout;
namespace menu = oa::ui::frontend_state::main_menu;
namespace mp = oa::ui::frontend_multiplayer;
namespace oakeys = oa::formats::oakeys;
namespace key_library = oa::data::key_library;

/// Languages the check opens every screen in, in order, unless --check-language
/// names others.
constexpr std::string_view kLanguages[] = {"en", "de", "fr", "es", "it", "zh-Hans"};

/// Joins the languages for the report: "en, de and zh-Hans".
///
/// @param tags the languages
/// @return the list
std::string joined_languages(const std::vector<std::string>& tags) {
    std::string joined;
    for (std::size_t at = 0; at < tags.size(); ++at) {
        if (at != 0)
            joined += at + 1 == tags.size() ? " and " : ", ";
        joined += tags[at];
    }
    return joined;
}

/// Window sizes. 640x480 is the compact settings dialog; 1280x720 is larger.
constexpr int32_t kWidths[] = {640, 1280};
constexpr int32_t kHeights[] = {480, 720};

/// How far a settings page is scrolled, in source pixels. The dialog clamps
/// each one, so the last one draws the foot of a long page.
constexpr int32_t kScrolls[] = {0, 200, 2000};

/// How many failures the report names before it counts the rest.
constexpr std::size_t kMostFailures = 40;

/// A long catalogue sentence, so a notice and a question wrap a whole text.
constexpr std::string_view kLongText =
    "OA looks for new content and updates in the registries' lists. It never installs anything "
    "until you say so.";

/// A caption no notice's button holds, which the check must find on a
/// drawing the layer keeps.
constexpr std::string_view kWideCaption = "A CAPTION FAR WIDER THAN ITS BUTTON";

/// A mutator's name at the manifest's most, 64 bytes. A question's title is
/// one line, so this is the longest name "Start without {name}?" can hold.
constexpr std::string_view kMutatorName =
    "Mutator With A Fairly Long Name That Reaches Sixty-Four Bytes xx";
static_assert(kMutatorName.size() == 64);

/// A brain's name at the manifest's most, 64 bytes. The missing-brain
/// question wraps one paragraph for each.
constexpr std::string_view kBrainName =
    "Brain With A Fairly Long Name That Reaches Sixty-Four Bytes xxxx";
constexpr std::string_view kOtherBrainName =
    "Other Brain With A Long Name That Reaches Sixty-Four Bytes xxxxx";
static_assert(kBrainName.size() == 64);
static_assert(kOtherBrainName.size() == 64);

/// Looks one interface word up in the language the check is showing.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view fit_shown(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// A startup notice's sentence, as the main menu shows it.
constexpr std::string_view kStartupNotice =
    "This game includes no skirmish maps, so skirmish is not available.";

/// The window the folder chooser paints on. The check's own window.
struct FitWindow {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
};

/// Throws when a step of the check cannot run.
///
/// @param ok the step passed
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("text-fit check: " + std::string(what));
}

/// Returns a Library tab's name in the check's report.
///
/// @param tab the tab
/// @return the name
std::string_view tab_word(library::Tab tab) noexcept {
    switch (tab) {
    case library::Tab::active:
        return "active";
    case library::Tab::mods:
        return "mods";
    case library::Tab::maps:
        return "maps";
    case library::Tab::mutators:
        return "mutators";
    case library::Tab::brains:
        return "brains";
    case library::Tab::keys:
        return "keys";
    case library::Tab::languages:
        return "languages";
    case library::Tab::updates:
        return "updates";
    case library::Tab::registries:
        return "registries";
    }
    return "library";
}

/// Returns whether a row is a drop-down the check opens.
///
/// @param setting the row
/// @return true for a drop-down
bool choice_setting(settings::Setting setting) noexcept {
    switch (setting) {
    case settings::Setting::max_zoom_out:
    case settings::Setting::max_zoom_in:
    case settings::Setting::language:
    case settings::Setting::pad_gyro:
    case settings::Setting::pad_prompts:
    case settings::Setting::zoomed_out_after:
    case settings::Setting::interface_size:
    case settings::Setting::window_size:
        return true;
    default:
        return false;
    }
}

/// The chooser's window.
///
/// @param context a FitWindow
/// @return the window
SDL_Window* fit_window(void* context) {
    return static_cast<FitWindow*>(context)->window;
}

/// The chooser's renderer.
///
/// @param context a FitWindow
/// @return the renderer
SDL_Renderer* fit_renderer(void* context) {
    return static_cast<FitWindow*>(context)->renderer;
}

/// One long-named brain written under the player's Brains folder for the
/// seat buttons, and removed when the paint is done. The destructor does
/// not throw.
struct LongBrainPackage {
    fs::path folder{}; ///< the package folder

    LongBrainPackage() = default;

    ~LongBrainPackage() {
        std::error_code removed;
        fs::remove_all(folder, removed);
    }

    LongBrainPackage(const LongBrainPackage&) = delete;
    LongBrainPackage& operator=(const LongBrainPackage&) = delete;
};

/// Writes ridge-long-beacon, a brain whose name does not fit a seat button.
///
/// The name stays within the 64 bytes a manifest keeps. A longer name is
/// not installed, and the campaign button then stays hidden.
///
/// @param folder the brain's folder
void write_long_brain(const fs::path& folder) {
    oa::formats::oabrain::Manifest manifest;
    manifest.format = oa::formats::oabrain::format_version;
    manifest.id = "ridge-long-beacon";
    manifest.name = "Ridge Long Beacon Watching The Far Western Hills Past Nightfall";
    require(
        manifest.name.size() <= oa::formats::oabrain::most_name_bytes,
        "the long brain's name is longer than a manifest allows"
    );
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ridge Testers";
    manifest.description = "A made-up brain with a long name.";
    manifest.tags = {"teaching"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    oa::formats::oabrain::Level level;
    level.id = "far";
    level.name = "Far Watch";
    level.difficulty = oa::formats::oabrain::Difficulty::medium;
    manifest.levels.push_back(std::move(level));
    if (!folder.parent_path().empty())
        fs::create_directories(folder);
    const std::string text = oa::formats::oabrain::write_manifest(manifest);
    std::ofstream manifest_out(
        folder / std::string(oa::formats::oabrain::manifest_file),
        std::ios::binary | std::ios::trunc
    );
    manifest_out.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(manifest_out), "could not write the long brain");
    std::ofstream script_out(
        folder / std::string(oa::formats::oabrain::default_entry),
        std::ios::binary | std::ios::trunc
    );
    constexpr std::string_view script = "local brain = {}\nreturn brain\n";
    script_out.write(script.data(), static_cast<std::streamsize>(script.size()));
    require(static_cast<bool>(script_out), "could not write the long brain's script");
}

/// Ends the Game files screen after the pass that painted it.
///
/// @param screen the screen
void quit_after_paint(void* /*context*/, GameFilesScreen& screen) {
    screen.request_quit();
}

/// One failure kept for the report.
struct FitFailure {
    std::string line{}; ///< screen, language, size and text
};

/// Writes an own layout of OA default that names twelve actions, so Controls
/// shows a change count. The check removes the file after the key editor.
///
/// @param user_folder the player's folder
void write_own_layout_changes(const fs::path& user_folder) {
    oakeys::Manifest manifest;
    manifest.format = oakeys::format_version;
    manifest.id = "own-oa-default";
    manifest.name = "Your changes";
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "You";
    manifest.tags = {"own"};
    manifest.base = oakeys::Base::oa_default;
    constexpr std::string_view actions[] = {
        "order.attack",
        "order.move",
        "order.stop",
        "order.patrol",
        "order.guard",
        "order.reclaim",
        "order.repair",
        "order.dgun",
        "camera.zoom.in",
        "camera.zoom.out",
        "select.idle",
        "select.all",
    };
    for (const std::string_view action : actions) {
        oakeys::BindingEntry binding;
        binding.action = std::string(action);
        binding.chords.push_back(oakeys::ChordText{"A", 0, 0});
        manifest.bindings.push_back(std::move(binding));
    }
    const fs::path own_dir = user_folder / std::string(key_library::keys_folder_name) /
                             key_library::own_folder_name("oa-default");
    std::error_code made;
    fs::create_directories(own_dir, made);
    require(!made, "the own layout folder was not made");
    const fs::path own_file = own_dir / std::string(oakeys::manifest_file);
    std::ofstream out(own_file, std::ios::binary);
    require(static_cast<bool>(out), "the own layout was not opened");
    out << oakeys::write_manifest(manifest);
    require(static_cast<bool>(out), "the own layout was not written");
}

} // namespace

void Runtime::check_text_fit() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(static_cast<bool>(options_.preferences_file), "needs --preferences-file");
    set_extension_overlays_aside(true);

    struct PutOverlaysBack {
        Runtime& runtime;

        ~PutOverlaysBack() { runtime.set_extension_overlays_aside(false); }
    } put_overlays_back{*this};

    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;

    // The languages named with --check-language, else the check's own.
    std::vector<std::string> languages = options_.check_languages;
    const bool named_languages = !languages.empty();
    if (!named_languages)
        for (const std::string_view tag : kLanguages)
            languages.emplace_back(tag);

    std::vector<FitFailure> failures;
    std::size_t extra_failures = 0;
    std::string language = "en";
    int32_t width = kWidths[0];
    int32_t height = kHeights[0];

    const auto keep = [&] {
        for (const kit::TextFitRecord& noted : kit::text_fit_records()) {
            if (!noted.wider && !noted.across)
                continue;
            std::string line = noted.screen;
            line += " / ";
            line += language;
            if (shown_language().tag != language) {
                line += " (shown as ";
                line += shown_language().tag;
                line += ')';
            }
            line += " / ";
            line += std::to_string(width);
            line += 'x';
            line += std::to_string(height);
            line += " / ";
            line += noted.text;
            line += noted.wider ? " (wider than its box)" : " (across other text)";
            // Where it is drawn, so that a text cut down to "..." can be found.
            line += ": " + std::to_string(noted.text_width) + " wide in a box of " +
                    std::to_string(noted.box_width) + " at " + std::to_string(noted.box_x) + "," +
                    std::to_string(noted.box_y);
            bool seen = false;
            for (const FitFailure& earlier : failures) {
                if (earlier.line == line)
                    seen = true;
            }
            if (seen)
                continue;
            if (failures.size() >= kMostFailures) {
                ++extra_failures;
                continue;
            }
            failures.push_back(FitFailure{std::move(line)});
        }
    };

    /// Runs the frames whose records are read, and leaves the records.
    const auto paint_frames = [&](std::string_view screen) {
        kit::set_text_fit_screen(screen);
        kit::begin_text_fit_frame();
        bool going = true;
        run_frame(going);
        require(going, std::string(screen) + ": a frame ended the run");
        // The first frame settles the size class. The second is the one read.
        // The kit records a text only while it is drawn, and the layer would
        // show a screen whose revision covers what it draws from the drawing
        // it kept, so every screen is drawn again.
        kit::begin_text_fit_frame();
        oa_layer().forget_kept_drawings();
        going = true;
        run_frame(going);
        require(going, std::string(screen) + ": a frame ended the run");
    };

    const auto paint = [&](std::string_view screen) {
        paint_frames(screen);
        keep();
    };

    /// Mutators' words read whole on its button: never cut with "…".
    const auto require_whole_words = [&](std::string_view screen) {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        bool found = false;
        for (const AutomationControl& control : controls) {
            if (control.name != "oa.mutators" || !control.visible)
                continue;
            found = true;
            require(
                control.text.find("…") == std::string::npos,
                std::string(screen) + ": Mutators' words are cut (" + control.text + ")"
            );
        }
        require(found, std::string(screen) + ": oa.mutators is not shown");
    };

    /// Keeps a model the check built from being replaced by the feed.
    const auto hold_library = [&] {
        LibraryHost& host = library_host();
        host.refreshed = true;
        host.feed_generation =
            host.feed.generation != nullptr ? host.feed.generation(host.feed.context) : 0;
    };

    /// Paints one screen without letting the feed rebuild the Library.
    const auto paint_held = [&](std::string_view screen) {
        hold_library();
        paint(screen);
    };

    /// Made-up mutators on the Mutators tab. Fast Builders is not installed
    /// and Steady Hands is. Gatehouse Keep needs Gatehouse, and Outer Wall
    /// needs Gatehouse and Ridge Walls: two downloads, then three. None of
    /// those four is installed, and Fit Core offers each one. The host's
    /// check stays unset, so there is no Checked fact.
    const auto inject_mutators = [&] {
        LibraryHost& host = library_host();
        library::Inputs inputs = host.model.inputs;
        bool listed = false;
        for (const library::Registry& registry : inputs.registries)
            if (registry.id == "fit-core")
                listed = true;
        if (!listed) {
            library::Registry registry;
            registry.id = "fit-core";
            registry.name = "Fit Core";
            registry.built_in = true;
            inputs.registries.push_back(std::move(registry));
        }
        library::Listing missing;
        missing.registry = "fit-core";
        missing.kind = library::Kind::mutator;
        missing.key = "fast-builders";
        missing.name = "Fast Builders";
        missing.version = "2.0";
        missing.revision = 1;
        missing.release = 1;
        missing.author = "Ada";
        missing.summary = "Builds faster.";
        missing.homepage = "https://example.test/fast";
        missing.size = 1024;
        missing.requires_engine = ">= 0.8.0";
        library::MutatorFacts missing_facts;
        missing_facts.works_with.push_back({"*", {}, {}});
        missing_facts.layers.push_back("units");
        missing_facts.tags.push_back("economy");
        missing.mutator = missing_facts;
        inputs.listings.push_back(std::move(missing));

        library::Listing steady_listing;
        steady_listing.registry = "fit-core";
        steady_listing.kind = library::Kind::mutator;
        steady_listing.key = "steady";
        steady_listing.name = "Steady Hands";
        steady_listing.version = "1.0";
        steady_listing.release = 1;
        steady_listing.author = "Ada";
        steady_listing.summary = "Hands stay steady.";
        steady_listing.homepage = "https://example.test/steady";
        steady_listing.size = 1024;
        steady_listing.requires_engine = ">= 0.8.0";
        library::MutatorFacts steady_facts;
        steady_facts.works_with.push_back({"*", {}, {}});
        steady_facts.layers.push_back("units");
        library::OptionLine option;
        option.label = "Speed";
        option.values.push_back("slow");
        option.values.push_back("fast");
        steady_facts.options.push_back(std::move(option));
        steady_listing.mutator = steady_facts;
        library::Installed steady;
        steady.kind = library::Kind::mutator;
        steady.key = "steady";
        steady.registry = "fit-core";
        steady.release = 1;
        steady.folder = "steady";
        steady.name = "Steady Hands";
        steady.version = "1.0";
        steady.author = "Ada";
        steady.description = "Hands stay steady.";
        steady.mutator = std::move(steady_facts);
        inputs.listings.push_back(std::move(steady_listing));
        inputs.installed.push_back(std::move(steady));

        // GET WITH's question: one missing prerequisite, then two. The names
        // are the ones the question writes, so each language's body is fitted.
        const auto offered = [&](std::string_view key,
                                 std::string_view name,
                                 std::string_view version,
                                 std::vector<library::Prerequisite> needs) {
            library::Listing listing;
            listing.registry = "fit-core";
            listing.kind = library::Kind::mutator;
            listing.key = std::string(key);
            listing.name = std::string(name);
            listing.version = std::string(version);
            listing.release = 1;
            listing.author = "Ada";
            listing.summary = "A made-up mutator.";
            listing.size = 1024;
            listing.requires_engine = ">= 0.8.0";
            library::MutatorFacts facts;
            facts.works_with.push_back({"*", {}, {}});
            facts.layers.push_back("units");
            facts.needs = std::move(needs);
            listing.mutator = std::move(facts);
            inputs.listings.push_back(std::move(listing));
        };
        offered("gatehouse", "Gatehouse", "4.1", {});
        offered("ridge-walls", "Ridge Walls", "1.2", {});
        offered(
            "gatehouse-keep",
            "Gatehouse Keep",
            "2.0",
            {library::Prerequisite{"gatehouse", ">= 4.0", "Gatehouse"}}
        );
        offered(
            "outer-wall",
            "Outer Wall",
            "1.0",
            {library::Prerequisite{"gatehouse", {}, "Gatehouse"},
             library::Prerequisite{"ridge-walls", {}, "Ridge Walls"}}
        );

        library::refresh(host.model, std::move(inputs));
        // The refresh freed the badges the Library's last layout points into:
        // it lays out again before it draws, as after the feed's own rebuild.
        ++host.model_revision;
        library::set_tab(host.model, library::Tab::mutators);
        hold_library();
    };

    /// The injected mutator `key`, injected again when the feed has rebuilt
    /// the Library since. A holding frame keeps out what the feed changed
    /// before it, but a picture or a minimap that arrives during the frame
    /// moves the feed on, and a slow build sees that often.
    const auto injected = [&](std::string_view key) -> const library::Entry* {
        const library::EntryId id{"fit-core", library::Kind::mutator, std::string(key)};
        if (library::find_entry(library_host().model, id) == nullptr)
            inject_mutators();
        return library::find_entry(library_host().model, id);
    };

    /// The details of one injected mutator, scrolled, at the window's size.
    /// Compact shows the details page. Regular and Large show the pane.
    const auto show_mutator = [&](std::string_view key) {
        LibraryHost& host = library_host();
        host.state.details_page = width <= 640;
        host.state.menu = library::MenuKind::none;
        const std::string screen = std::string("library/mutators/") + std::string(key);
        for (const int32_t scroll : kScrolls) {
            require(injected(key) != nullptr, screen + " has no " + std::string(key));
            library::select(host.model, {"fit-core", library::Kind::mutator, std::string(key)});
            library::open_details(host.model);
            host.state.details_scroll = scroll;
            host.state.list_scroll = 0;
            ++host.model_revision;
            paint_held(screen);
        }
    };

    /// Made-up brains on the Brains tab. Ridge Raider is not installed.
    /// Steady Commander is, and it sees every unit and carries a network,
    /// so those long facts are fitted. The model is held: the feed's check
    /// would replace a verdict the check set by hand.
    const auto inject_brains = [&] {
        LibraryHost& host = library_host();
        // This check's own brains are put in again whole, so a model that
        // kept their listings but not their entries gets both back. A
        // catalogue's brain with the same key under another registry stays.
        library::Inputs inputs = host.model.inputs;
        std::erase_if(inputs.listings, [](const library::Listing& listed) {
            return listed.kind == library::Kind::brain && listed.registry == "fit-core";
        });
        std::erase_if(inputs.installed, [](const library::Installed& held) {
            return held.kind == library::Kind::brain && held.registry == "fit-core";
        });
        bool listed = false;
        for (const library::Registry& registry : inputs.registries)
            if (registry.id == "fit-core")
                listed = true;
        if (!listed) {
            library::Registry registry;
            registry.id = "fit-core";
            registry.name = "Fit Core";
            registry.built_in = true;
            inputs.registries.push_back(std::move(registry));
        }
        library::Listing missing;
        missing.registry = "fit-core";
        missing.kind = library::Kind::brain;
        missing.key = "ridge-raider";
        missing.name = "Ridge Raider";
        missing.version = "1.0";
        missing.revision = 1;
        missing.release = 1;
        missing.author = "Ada";
        missing.summary = "Raids the ridge.";
        missing.homepage = "https://example.test/ridge-raider";
        missing.size = 1024;
        library::BrainFacts ridge_facts;
        ridge_facts.works_with.push_back({"ridge", {}, "Ridge"});
        ridge_facts.tags.push_back("rush");
        missing.brain = std::move(ridge_facts);
        inputs.listings.push_back(std::move(missing));

        library::BrainFacts steady_facts;
        steady_facts.works_with.push_back({"*", {}, {}});
        steady_facts.tags.push_back("classic");
        steady_facts.full_awareness = true;
        steady_facts.network_bytes = uint64_t{2} << 20;
        steady_facts.levels.push_back({"steady", "Steady", library::BrainDifficulty::easy});
        library::Verdict verdict;
        verdict.state = library::Verdict::State::unchecked;
        steady_facts.verdict = std::move(verdict);
        library::Listing steady_listing;
        steady_listing.registry = "fit-core";
        steady_listing.kind = library::Kind::brain;
        steady_listing.key = "steady-commander";
        steady_listing.name = "Steady Commander";
        steady_listing.version = "1.0";
        steady_listing.release = 1;
        steady_listing.author = "Ada";
        steady_listing.summary = "Holds the line.";
        steady_listing.homepage = "https://example.test/steady-commander";
        steady_listing.size = 1024;
        steady_listing.brain = steady_facts;
        library::Installed steady;
        steady.kind = library::Kind::brain;
        steady.key = "steady-commander";
        steady.registry = "fit-core";
        steady.release = 1;
        steady.folder = "steady-commander";
        steady.name = "Steady Commander";
        steady.version = "1.0";
        steady.author = "Ada";
        steady.description = "Holds the line.";
        steady.brain = std::move(steady_facts);
        inputs.listings.push_back(std::move(steady_listing));
        inputs.installed.push_back(std::move(steady));

        library::refresh(host.model, std::move(inputs));
        ++host.model_revision;
        hold_library();
    };

    /// The details of one injected brain, scrolled, at the window's size.
    /// Compact shows the details page. Regular and Large show the pane.
    const auto show_brain = [&](std::string_view key) {
        LibraryHost& host = library_host();
        const library::EntryId id{"fit-core", library::Kind::brain, std::string(key)};
        host.state.details_page = width <= 640;
        host.state.menu = library::MenuKind::none;
        const std::string screen = std::string("library/brains/") + std::string(key);
        for (const int32_t scroll : kScrolls) {
            // A feed that finished during the last paint rebuilt the Library
            // from its own inputs, so the check's brains go in again.
            if (library::find_entry(host.model, id) == nullptr)
                inject_brains();
            require(
                library::find_entry(host.model, id) != nullptr,
                screen + " has no " + std::string(key)
            );
            library::set_tab(host.model, library::Tab::brains);
            library::select(host.model, id);
            library::open_details(host.model);
            host.state.details_scroll = scroll;
            host.state.list_scroll = 0;
            ++host.model_revision;
            paint_held(screen);
        }
    };

    /// One short installed layout on the Keys tab, and USE's question open
    /// over it. The question is drawn on the Library, not as its own screen.
    /// A feed that rebuilt the Library gets the layout again.
    const auto show_key_question = [&] {
        LibraryHost& host = library_host();
        const library::EntryId id{"fit-core", library::Kind::key_layout, "grid"};
        if (library::find_entry(host.model, id) == nullptr) {
            library::Inputs inputs = host.model.inputs;
            bool listed = false;
            for (const library::Registry& registry : inputs.registries)
                if (registry.id == "fit-core")
                    listed = true;
            if (!listed) {
                library::Registry registry;
                registry.id = "fit-core";
                registry.name = "Fit Core";
                registry.built_in = true;
                inputs.registries.push_back(std::move(registry));
            }
            library::KeyLayoutFacts facts;
            facts.helpers = {"sticky-modifiers"};
            facts.changes = 2;
            facts.tags = {"grid"};
            library::Listing listing;
            listing.registry = "fit-core";
            listing.kind = library::Kind::key_layout;
            listing.key = "grid";
            listing.name = "Grid";
            listing.version = "1.0";
            listing.release = 1;
            listing.author = "Ada";
            listing.summary = "A grid.";
            listing.size = 1024;
            listing.keys = facts;
            library::Installed held;
            held.kind = library::Kind::key_layout;
            held.key = "grid";
            held.registry = "fit-core";
            held.release = 1;
            held.folder = "grid";
            held.name = "Grid";
            held.version = "1.0";
            held.keys = facts;
            inputs.listings.push_back(std::move(listing));
            inputs.installed.push_back(std::move(held));
            inputs.keys_own_changes = 2;
            library::refresh(host.model, std::move(inputs));
        }
        library::set_tab(host.model, library::Tab::keys);
        library::select(host.model, id);
        const library::Entry* entry = library::find_entry(host.model, id);
        require(entry != nullptr, "library/keys/question has no grid");
        library::open_key_use(host.model, *entry);
        host.state.menu = library::MenuKind::none;
        host.state.details_page = width <= 640;
        ++host.model_revision;
        hold_library();
        paint_held("library/keys/question");
        library::close_key_use(host.model);
        ++host.model_revision;
    };

    const auto close_covering = [&] {
        dialogs::reset_dialogs();
        if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
            oa_layer().close(notice);
        if (LayerScreen* question = oa_layer().find(QuestionScreen::screen_name))
            oa_layer().close(question);
        if (LayerScreen* picker_open = oa_layer().find(picker::screen_name))
            oa_layer().close(picker_open);
        if (LayerScreen* brain_open = oa_layer().find(brains::screen_name))
            oa_layer().close(brain_open);
        if (LayerScreen* welcome = oa_layer().find("first-run"))
            oa_layer().close(welcome);
        if (LayerScreen* keys_open = oa_layer().find("key-editor"))
            oa_layer().close(keys_open);
        if (LayerScreen* keys_name = oa_layer().find("key-editor-name"))
            oa_layer().close(keys_name);
        if (LayerScreen* keys_prompt = oa_layer().find("key-editor-prompt"))
            oa_layer().close(keys_prompt);
        if (LayerScreen* keys_notice = oa_layer().find("key-editor-notice"))
            oa_layer().close(keys_notice);
        if (LayerScreen* keys_overlay = oa_layer().find("keys-overlay"))
            oa_layer().close(keys_overlay);
        close_library();
        restore_mutator_picker_feed();
        restore_brain_picker_feed();
        if (engine_settings_dialog() != nullptr)
            take_settings_screen_action(settings::DialogAction::cancelled);
        else if (LayerScreen* settings_screen = oa_layer().find("settings"))
            oa_layer().close(settings_screen);
    };

    const auto open_menu = [&](settings::Dialog& dialog, int32_t control, std::string_view screen) {
        const settings::EngineSettings chosen = dialog.chosen;
        const settings::DownloadsOffer offer = dialog.downloads;
        const std::vector<settings::InstallIdChoice> chosen_ids = dialog.install_id_chosen;
        const bool welcome_again = dialog.welcome_again;
        if (dialog.open_list != settings::no_control)
            std::ignore = settings::dialog_key(dialog, kit::Key::escape);
        dialog.focused = control;
        std::ignore = settings::dialog_key(dialog, kit::Key::space);
        if (dialog.open_list != settings::no_control) {
            paint(screen);
            if (dialog.open_list != settings::no_control)
                std::ignore = settings::dialog_key(dialog, kit::Key::escape);
        }
        dialog.chosen = chosen;
        dialog.downloads = offer;
        dialog.install_id_chosen = chosen_ids;
        dialog.welcome_again = welcome_again;
        dialog.open_list = settings::no_control;
    };

    const auto walk_settings = [&](settings::Dialog& dialog, std::string_view prefix) {
        const auto pages =
            settings::dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller);
        for (const settings::Page page : pages) {
            dialog.page = page;
            dialog.open_list = settings::no_control;
            dialog.focused = settings::no_control;
            const std::string page_name =
                std::string(prefix) + "/" + std::string(settings::page_word(page));
            for (const int32_t scroll : kScrolls) {
                dialog.scroll[static_cast<std::size_t>(page)] = scroll;
                paint(page_name);
            }
            const auto rows = settings::page_settings(page);
            for (std::size_t row_at = 0; row_at < rows.size(); ++row_at) {
                if (!choice_setting(rows[row_at]))
                    continue;
                open_menu(
                    dialog,
                    settings::first_row_control + static_cast<int32_t>(row_at),
                    page_name + "/list"
                );
            }
            if (page == settings::Page::downloads) {
                for (int32_t row_at = 0; row_at < 4; ++row_at)
                    open_menu(dialog, settings::first_row_control + row_at, page_name + "/list");
            }
        }
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    for (int32_t settle = 0; settle < 3; ++settle) {
        bool going = true;
        run_frame(going);
        require(going, "the main menu ended the run");
    }

#if !defined(NDEBUG)
    // The check reads the texts of a drawing the layer keeps: a notice's,
    // whose OK is too wide for its button. A release build records nothing.
    {
        kit::Notice wide;
        wide.title = "NOTICE";
        wide.open_caption = "OK";
        wide.ok_caption = std::string(kWideCaption);
        oa_layer().push(
            std::make_unique<NoticeScreen>(
                oa_layer(), Screen::main_menu, std::move(wide), NoticeScreen::Host{}
            )
        );
        paint_frames("kept-notice");
        bool recorded = false;
        for (const kit::TextFitRecord& noted : kit::text_fit_records())
            recorded = recorded || (noted.wider && noted.text == kWideCaption);
        if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
            oa_layer().close(notice);
        kit::begin_text_fit_frame();
        require(recorded, "a kept notice's caption, too wide for its button, was not recorded");
    }
#endif

    for (std::size_t size_at = 0; size_at < 2; ++size_at) {
        width = kWidths[size_at];
        height = kHeights[size_at];
        require(
            SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
            "the window did not take " + std::to_string(width) + "x" + std::to_string(height)
        );
        apply_output_mode();

        for (const std::string& choice : languages) {
            language = choice;
            close_covering();
            preference_values_[std::string(settings::key::language)] = language;
            preferences_dirty_ = true;
            oa::platform::preferences::save(preference_path_, preference_values_);
            set_language_choice(language);
            // A language named on the command line, such as an installed
            // pack's, must be the one shown, or the check would pass over
            // another.
            require(
                !named_languages || shown_language().tag == language,
                language + " is not a language the game shows (" +
                    std::string(shown_language().tag) + " is shown); is its pack installed?"
            );
            paint("main-menu");

            // Mutators on the skirmish setup, with none and then five chosen,
            // and on NEWGAME. The rest of this language still starts at the
            // main menu.
            load(Screen::skirmish);
            skirmish::setup(state_, skirmish_settings_, preferences_, skirmish_ui_, *this);
            paint("skirmish/mutators");
            require_whole_words("skirmish/mutators");
            {
                std::vector<picker::Chosen> long_set;
                for (const char* id :
                     {"north-ridge-gatehouse",
                      "outer-keep-wall",
                      "steady-builders",
                      "lantern-line",
                      "beacon-west"}) {
                    picker::Chosen one;
                    one.id = id;
                    long_set.push_back(std::move(one));
                }
                replace_setup_mutators(mutators_memory::Setup::skirmish, std::move(long_set));
            }
            paint("skirmish/mutators-five");
            require_whole_words("skirmish/mutators-five");
            replace_setup_mutators(mutators_memory::Setup::skirmish, {});
            // Three computer rows, one of them a brain whose name is longer
            // than the button. The package is removed before the next screen.
            {
                require(!user_folder_.empty(), "the run has no folder of the player's own");
                LongBrainPackage package;
                package.folder = user_folder_ /
                                 std::string(oa::data::brain_library::brains_folder_name) /
                                 "ridge-long-beacon";
                write_long_brain(package.folder);
                const auto saved_slots = skirmish_settings_.slots;
                const oa::data::brain_library::Seats saved_seats =
                    setup_brain_seats(oa::data::brain_library::Setup::skirmish);
                constexpr int32_t computer_rows[] = {1, 2, 3};
                for (const int32_t slot : computer_rows) {
                    if (static_cast<std::size_t>(slot) < skirmish_settings_.slots.size()) {
                        skirmish_settings_.slots[static_cast<std::size_t>(slot)].controller =
                            entry::controller::computer;
                    }
                }
                reload_installed_brains();
                oa::data::brain_library::Seat seat;
                seat.place = 1;
                seat.brain = "ridge-long-beacon";
                seat.level = "far";
                oa::data::brain_library::Seats chosen;
                chosen.seats.push_back(std::move(seat));
                setup_brain_seats(oa::data::brain_library::Setup::skirmish) = std::move(chosen);
                skirmish::setup(state_, skirmish_settings_, preferences_, skirmish_ui_, *this);
                paint("skirmish/brain-seats");
                skirmish_settings_.slots = saved_slots;
                setup_brain_seats(oa::data::brain_library::Setup::skirmish) = saved_seats;
                std::error_code removed;
                fs::remove_all(package.folder, removed);
                reload_installed_brains();
                skirmish::setup(state_, skirmish_settings_, preferences_, skirmish_ui_, *this);
            }
            load(Screen::single_player);
            load(Screen::new_campaign);
            paint("newgame/mutators");
            require_whole_words("newgame/mutators");
            replace_setup_mutators(mutators_memory::Setup::campaign, {});
            // The campaign seat's button, with a brain name longer than the
            // button. The package is removed before the next screen.
            {
                require(!user_folder_.empty(), "the run has no folder of the player's own");
                LongBrainPackage package;
                package.folder = user_folder_ /
                                 std::string(oa::data::brain_library::brains_folder_name) /
                                 "ridge-long-beacon";
                write_long_brain(package.folder);
                reload_installed_brains();
                require(
                    oa::data::brain_library::find(installed_brains(), "ridge-long-beacon") !=
                        nullptr,
                    "newgame/brain-seat: the long brain is not installed"
                );
                const oa::data::brain_library::Seats saved_seats =
                    setup_brain_seats(oa::data::brain_library::Setup::campaign);
                oa::data::brain_library::Seat seat;
                seat.place = 1;
                seat.brain = "ridge-long-beacon";
                seat.level = "far";
                oa::data::brain_library::Seats chosen;
                chosen.seats.push_back(std::move(seat));
                setup_brain_seats(oa::data::brain_library::Setup::campaign) = std::move(chosen);
                paint("newgame/brain-seat");
                bool shown = false;
                std::vector<AutomationControl> controls;
                oa_layer().automation_controls(controls);
                for (const AutomationControl& control : controls) {
                    if (control.name == "oa.brain-seat.campaign" && control.visible)
                        shown = true;
                }
                require(shown, "newgame/brain-seat: oa.brain-seat.campaign is not shown");
                setup_brain_seats(oa::data::brain_library::Setup::campaign) = saved_seats;
                std::error_code removed;
                fs::remove_all(package.folder, removed);
                reload_installed_brains();
            }
            load(Screen::main_menu);

            // One computer row in the battle room, labelled with a brain
            // whose name is longer than the button. The row is left, and
            // the remembered seat is cleared, before Settings opens.
            {
                close_covering();
                mp::DirectGame room_game;
                room_game.kind = mp::DirectGame::Kind::host;
                room_game.player_name = "Steve";
                room_game.game_name = "Ridge Fit";
                mp::multiplayer_bind_direct_game(room_game);

                // --host names TCP before the menus open, and the provider
                // list then takes that transport. This step is already on the
                // main menu, so the launch block is named here and put back
                // once the room is up: a transport left set would leave the
                // main menu again when Settings opens.
                mp::launch::LaunchBlock* const launch = mp::multiplayer_lobby().launch_link.block;
                require(launch != nullptr, "the battle room has no launch block");
                const int32_t saved_connection = launch->connection_type;
                launch->connection_type = mp::launch::connection_type::tcpip;

                bool found_multi = false;
                for (const auto& gadget : resources_.layout.gadgets) {
                    if (gadget.common.name != menu::resource_name(menu::Button::multiplayer))
                        continue;
                    found_multi = true;
                    break;
                }
                require(found_multi, "the main menu has no MULTI");
                exercise_click(menu::resource_name(menu::Button::multiplayer));
                // The click sets the multiplayer signal. The dispatcher runs
                // on a later frame only when a pass was asked for; --host asks
                // on every frame until a multiplayer screen shows, and this
                // step asks once.
                frontend_pass_requested_ = true;

                bool room_opened = false;
                std::string room_notice;
                for (int32_t frame = 0; frame < 300 && !room_opened; ++frame) {
                    bool going = true;
                    run_frame(going);
                    require(going, "the battle room ended the run");
                    if (frame % 15 == 14) {
                        dialogs::reset_dialogs();
                        if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
                            oa_layer().close(notice);
                    }
                    mp::BattleRoomMap battle;
                    room_opened = mp::multiplayer_battle_room_map(&battle) && !battle.joiner;
                    const mp::DirectGameProgress& progress = mp::multiplayer_direct_game_progress();
                    if (progress.reached == mp::DirectGameStep::failed)
                        room_notice = progress.notice;
                }
                require(
                    room_opened,
                    room_notice.empty() ? "the battle room did not open"
                                        : "the battle room did not open: " + room_notice
                );
                launch->connection_type = saved_connection;

                require(!user_folder_.empty(), "the run has no folder of the player's own");
                LongBrainPackage room_package;
                room_package.folder =
                    user_folder_ / std::string(brain::brains_folder_name) / "ridge-long-beacon";
                write_long_brain(room_package.folder);
                brain::Seat room_seat;
                room_seat.place = 0;
                room_seat.brain = "ridge-long-beacon";
                room_seat.level = "far";
                brain::Seats room_seats;
                room_seats.seats.push_back(std::move(room_seat));
                preference_values_[std::string(settings::key::brains_multiplayer)] =
                    brain::seats_text(room_seats);
                preferences_dirty_ = true;
                reload_installed_brains();

                mp::Lobby& room_lobby = mp::multiplayer_lobby();
                require(room_lobby.game != nullptr, "the battle room has no game");
                int32_t room_slot = -1;
                for (int32_t candidate = 0; candidate < mp::kSlotCount; ++candidate) {
                    if (mp::slot_player(room_lobby, candidate).status == mp::kSlotOpen) {
                        room_slot = candidate;
                        break;
                    }
                }
                require(room_slot >= 0, "the battle room has no open slot");
                mp::lobby_add_computer(room_lobby, room_slot);
                const oa::Player& added = mp::slot_player(room_lobby, room_slot);
                require(
                    added.status == mp::kSlotComputer && added.player_id != 0,
                    "the computer player was not added"
                );
                note_room_computers(room_lobby);
                paint("battle-room/brain");

                bool room_labelled = false;
                std::vector<AutomationControl> room_controls;
                oa_layer().automation_controls(room_controls);
                for (const AutomationControl& control : room_controls) {
                    constexpr std::string_view prefix = "oa.brain-seat.";
                    if (control.name.compare(0, prefix.size(), prefix) != 0 || !control.visible)
                        continue;
                    require(
                        control.text.find("Ridge Long Beacon") != std::string::npos,
                        "the battle room's seat does not name the long brain (" + control.text + ")"
                    );
                    room_labelled = true;
                }
                require(room_labelled, "the battle room shows no brain seat");

                auto room_context = screen_context();
                require(
                    mp::multiplayer_click(&room_context, "PREVMENU"),
                    "the battle room has no PREVMENU"
                );
                bool room_left = false;
                bool pressed_again = false;
                for (int32_t frame = 0; frame < 60 && !room_left; ++frame) {
                    mp::BattleRoomMap battle;
                    room_left = !mp::multiplayer_battle_room_open();
                    if (mp::multiplayer_battle_room_map(&battle))
                        room_left = false;
                    if (room_left)
                        break;
                    // A click while a notice is up only clears that notice.
                    if (frame == 1 && !pressed_again) {
                        pressed_again = true;
                        require(
                            mp::multiplayer_click(&room_context, "PREVMENU"),
                            "the battle room has no PREVMENU"
                        );
                    }
                    bool going = true;
                    run_frame(going);
                    require(going, "leaving the battle room ended the run");
                }
                require(room_left, "the battle room stayed open");
                mp::DirectGame room_done;
                mp::multiplayer_bind_direct_game(room_done);
                load(Screen::main_menu);
                preference_values_.erase(std::string(settings::key::brains_multiplayer));
                preferences_dirty_ = true;
                note_room_computers(mp::multiplayer_lobby());
            }
            reload_installed_brains();

            require(!user_folder_.empty(), "the run has no folder of the player's own");
            write_own_layout_changes(fs::path(user_folder_));
            open_engine_settings_from_menu();
            settings::Dialog* dialog = engine_settings_dialog();
            require(dialog != nullptr, "Settings did not open");
            std::ignore = settings::set_touch_controls(*dialog, true);
            std::ignore = settings::set_controller_section(*dialog, true, true);
            dialog->game_files = true;
            settings::DownloadsOffer offer;
            offer.https_over_http = true;
            settings::InstallIdRow ident;
            ident.registry_id = "ridge";
            ident.registry_name = "Ridge";
            ident.on = true;
            ident.id_text = "a1b2";
            ident.made = true;
            offer.install_ids.push_back(std::move(ident));
            settings::set_downloads_offer(*dialog, offer);
            walk_settings(*dialog, "settings");
            settings::Dialog& mod_dialog =
                open_engine_settings_dialog(settings::DialogKind::mod_options);
            walk_settings(mod_dialog, "mod-options");
            take_settings_screen_action(settings::DialogAction::cancelled);

            const library::Tab tabs[] = {
                library::Tab::active,
                library::Tab::mods,
                library::Tab::maps,
                library::Tab::mutators,
                library::Tab::brains,
                library::Tab::keys,
                library::Tab::languages,
                library::Tab::updates,
                library::Tab::registries
            };
            for (const library::Tab tab : tabs) {
                open_library(tab);
                require(
                    oa_layer().find(library_screen_name) != nullptr, "the Library did not open"
                );
                LibraryHost& host = library_host();
                const std::string tab_name = "library/" + std::string(tab_word(tab));
                for (const int32_t scroll : kScrolls) {
                    host.state.list_scroll = scroll;
                    host.state.menu = library::MenuKind::none;
                    host.state.details_page = false;
                    library::close_details(host.model);
                    ++host.model_revision;
                    paint(tab_name);
                }
                host.state.menu = library::MenuKind::filter;
                ++host.model_revision;
                paint(tab_name + "/filter");
                host.state.menu = library::MenuKind::tags;
                ++host.model_revision;
                paint(tab_name + "/tags");
                host.state.menu = library::MenuKind::none;
                if (!host.model.visible.empty() && host.model.selected) {
                    library::open_details(host.model);
                    host.state.details_page = true;
                    ++host.model_revision;
                    paint(tab_name + "/details");
                    library::close_details(host.model);
                    host.state.details_page = false;
                }
            }
            // Made-up mutators, installed and not, at this size. At 640 the
            // same pair is also drawn at 960x540, Regular, then the window
            // returns to 640x480 for the screens that follow.
            require(
                oa_layer().find(library_screen_name) != nullptr,
                "the Library did not stay open for the mutator details"
            );
            inject_mutators();
            show_mutator("fast-builders");
            show_mutator("steady");
            inject_brains();
            show_brain("ridge-raider");
            show_brain("steady-commander");
            show_key_question();
            if (size_at == 0) {
                const int32_t saved_width = width;
                const int32_t saved_height = height;
                width = 960;
                height = 540;
                require(
                    SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
                    "the window did not take 960x540"
                );
                apply_output_mode();
                show_mutator("fast-builders");
                show_mutator("steady");
                show_brain("ridge-raider");
                show_brain("steady-commander");
                show_key_question();
                width = saved_width;
                height = saved_height;
                require(
                    SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
                    "the window did not return to 640x480"
                );
                apply_output_mode();
            }
            // GET WITH, over the injected plans: two packages, then three.
            // The question is built while the injected model is still held.
            const auto paint_get_with = [&](std::string_view key, std::string_view screen) {
                LibraryHost& host = library_host();
                const library::Entry* entry = injected(key);
                require(entry != nullptr, std::string(screen) + " has no " + std::string(key));
                oa_layer().push(
                    std::make_unique<QuestionScreen>(
                        oa_layer(),
                        Screen::main_menu,
                        library::get_with_question(host.model, *entry),
                        QuestionScreen::Host{}
                    )
                );
                paint_held(screen);
                if (LayerScreen* question = oa_layer().find(QuestionScreen::screen_name))
                    oa_layer().close(question);
            };
            paint_get_with("gatehouse-keep", "library/get-with");
            paint_get_with("outer-wall", "library/get-all");
            library_host().refreshed = false;
            if (oa_layer().find(library_screen_name) != nullptr) {
                kit::Question asked;
                asked.title = "Remove";
                asked.paragraphs.push_back(kit::Paragraph{std::string(kLongText), false});
                asked.buttons.push_back(kit::QuestionButton{"CANCEL", false, "cancel"});
                asked.buttons.push_back(kit::QuestionButton{"OK", true, "ok"});
                asked.cancel_button = 0;
                asked.primary_button = 1;
                oa_layer().push(
                    std::make_unique<QuestionScreen>(
                        oa_layer(), Screen::main_menu, std::move(asked), QuestionScreen::Host{}
                    )
                );
                paint("library/question");
                if (LayerScreen* question = oa_layer().find(QuestionScreen::screen_name))
                    oa_layer().close(question);
            }
            close_library();

            // The mutator picker, on the check's feed: the list, the details,
            // the waiting line, the turn-off question and the read-only footer.
            {
                const auto named =
                    [this](std::string_view name) -> std::optional<AutomationControl> {
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
                    bool going = true;
                    for (const SDL_EventType type :
                         {SDL_EVENT_MOUSE_MOTION,
                          SDL_EVENT_MOUSE_BUTTON_DOWN,
                          SDL_EVENT_MOUSE_BUTTON_UP}) {
                        SDL_Event event = window_pointer_event(
                            sdl_.window,
                            sdl_.renderer,
                            type,
                            pixel,
                            type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
                        );
                        dispatch_event(event, going);
                        require(going, std::string(name) + " ended the run");
                    }
                    going = true;
                    run_frame(going);
                    require(going, std::string(name) + " ended the run");
                };
                const auto open_picker = [&](bool read_only) {
                    MutatorPickerRequest request;
                    request.setup = picker::Setup::skirmish;
                    request.read_only = read_only;
                    picker::Chosen keep;
                    keep.id = "gatehouse-keep";
                    picker::Chosen fast;
                    fast.id = "fast-builders";
                    fast.options.emplace_back("build_speed", "4");
                    request.set.push_back(std::move(keep));
                    request.set.push_back(std::move(fast));
                    require(
                        open_mutator_picker(std::move(request)), "the mutator picker did not open"
                    );
                };
                install_mutator_picker_check_feed();
                open_picker(false);
                paint("mutator-picker");
                click_named("oa.mutators.row.fast-builders");
                if (width <= 640)
                    click_named("oa.mutators.details");
                paint("mutator-picker/details");
                if (width <= 640)
                    click_named("oa.mutators.back");
                if (size_at == 0) {
                    const int32_t saved_width = width;
                    const int32_t saved_height = height;
                    width = 960;
                    height = 540;
                    require(
                        SDL_SetWindowSize(sdl_.window, width, height) &&
                            SDL_SyncWindow(sdl_.window),
                        "the window did not take 960x540"
                    );
                    apply_output_mode();
                    paint("mutator-picker/details");
                    width = saved_width;
                    height = saved_height;
                    require(
                        SDL_SetWindowSize(sdl_.window, width, height) &&
                            SDL_SyncWindow(sdl_.window),
                        "the window did not return to 640x480"
                    );
                    apply_output_mode();
                }
                click_named("oa.mutators.row.grounded.switch.on");
                paint("mutator-picker/waiting");
                click_named("oa.mutators.row.gatehouse.switch.off");
                paint("mutator-picker/turn-off");
                click_named("oa.prompt.cancel");
                if (LayerScreen* picker_open = oa_layer().find(picker::screen_name))
                    oa_layer().close(picker_open);
                open_picker(true);
                paint("mutator-picker/read-only");
                if (LayerScreen* picker_open = oa_layer().find(picker::screen_name))
                    oa_layer().close(picker_open);
                restore_mutator_picker_feed();
            }

            // The key editor's list, and Compact's details page.
            {
                close_covering();
                require(open_key_editor({}), "the key editor did not open");
                paint("key-editor");
                if (width <= 640) {
                    std::optional<AutomationControl> row;
                    std::vector<AutomationControl> controls;
                    oa_layer().automation_controls(controls);
                    for (const AutomationControl& control : controls) {
                        if (control.name.starts_with("oa.key-editor.row.") && control.visible &&
                            control.window_width > 0 && control.window_height > 0) {
                            row = control;
                            break;
                        }
                    }
                    require(row.has_value(), "the key editor shows no row");
                    const layout::Point pixel{
                        row->window_x + row->window_width / 2,
                        row->window_y + row->window_height / 2
                    };
                    bool going = true;
                    for (const SDL_EventType type :
                         {SDL_EVENT_MOUSE_MOTION,
                          SDL_EVENT_MOUSE_BUTTON_DOWN,
                          SDL_EVENT_MOUSE_BUTTON_UP}) {
                        SDL_Event event = window_pointer_event(
                            sdl_.window,
                            sdl_.renderer,
                            type,
                            pixel,
                            type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
                        );
                        dispatch_event(event, going);
                        require(going, "the key editor's row ended the run");
                    }
                    going = true;
                    run_frame(going);
                    require(going, "the key editor's row ended the run");
                    PadAccess::send_key(*this, SDLK_RETURN);
                    paint("key-editor/details");
                }
                const auto named =
                    [this](std::string_view name) -> std::optional<AutomationControl> {
                    std::vector<AutomationControl> controls;
                    oa_layer().automation_controls(controls);
                    for (const AutomationControl& control : controls) {
                        if (control.name == name && control.visible && control.window_width > 0 &&
                            control.window_height > 0)
                            return control;
                    }
                    return std::nullopt;
                };
                const auto click_named = [&](std::string_view name) {
                    const std::optional<AutomationControl> control = named(name);
                    require(control.has_value(), std::string(name) + " is not shown");
                    const layout::Point pixel{
                        control->window_x + control->window_width / 2,
                        control->window_y + control->window_height / 2
                    };
                    bool going = true;
                    for (const SDL_EventType type :
                         {SDL_EVENT_MOUSE_MOTION,
                          SDL_EVENT_MOUSE_BUTTON_DOWN,
                          SDL_EVENT_MOUSE_BUTTON_UP}) {
                        SDL_Event event = window_pointer_event(
                            sdl_.window,
                            sdl_.renderer,
                            type,
                            pixel,
                            type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
                        );
                        dispatch_event(event, going);
                        require(going, std::string(name) + " ended the run");
                    }
                    going = true;
                    run_frame(going);
                    require(going, std::string(name) + " ended the run");
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
                    bool going = true;
                    dispatch_event(event, going);
                    require(going, "a key in the key editor ended the run");
                };
                if (named("oa.key-editor.back").has_value())
                    click_named("oa.key-editor.back");
                click_named("oa.key-editor.row.order.attack");
                if (!named("oa.key-editor.detail.add").has_value()) {
                    PadAccess::send_key(*this, SDLK_RETURN);
                    bool going = true;
                    run_frame(going);
                    require(going, "opening the key editor's details ended the run");
                }
                click_named("oa.key-editor.detail.add");
                paint("key-editor/capture");
                key_event(SDL_EVENT_KEY_DOWN, SDLK_LCTRL, SDL_KMOD_CTRL);
                key_event(SDL_EVENT_KEY_DOWN, SDLK_B, SDL_KMOD_CTRL);
                key_event(SDL_EVENT_KEY_UP, SDLK_B, SDL_KMOD_CTRL);
                key_event(SDL_EVENT_KEY_UP, SDLK_LCTRL, SDL_KMOD_NONE);
                paint("key-editor/capture");
                click_named("oa.key-editor.capture.ok");
                paint("key-editor/clash");
                click_named("oa.key-editor.clash.cancel");
                if (named("oa.key-editor.back").has_value())
                    click_named("oa.key-editor.back");
                click_named("oa.key-editor.view.keyboard");
                paint("key-editor/keyboard");
                click_named("oa.key-editor.view.mouse");
                paint("key-editor/mouse");
                click_named("oa.key-editor.view.list");
                const auto press_editor = [&](std::string_view name) {
                    if (!named(name).has_value() && named("oa.key-editor.more").has_value()) {
                        click_named("oa.key-editor.more");
                        // MORE shows eight rows. End brings PASTE into view.
                        if (!named(name).has_value()) {
                            key_event(SDL_EVENT_KEY_DOWN, SDLK_END, SDL_KMOD_NONE);
                            key_event(SDL_EVENT_KEY_UP, SDLK_END, SDL_KMOD_NONE);
                            bool going = true;
                            run_frame(going);
                            require(going, "the key editor's menu did not move");
                        }
                    }
                    click_named(name);
                };
                press_editor("oa.key-editor.share");
                paint("key-editor/share");
                if (named("oa.key-editor.share.close").has_value())
                    click_named("oa.key-editor.share.close");
                press_editor("oa.key-editor.paste");
                paint("key-editor/paste");
                if (named("oa.key-editor.paste.cancel").has_value())
                    click_named("oa.key-editor.paste.cancel");
                press_editor("oa.key-editor.import");
                paint("key-editor/import");
                if (named("oa.key-editor.import.cancel").has_value())
                    click_named("oa.key-editor.import.cancel");

                if (LayerScreen* keys_open = oa_layer().find("key-editor"))
                    oa_layer().close(keys_open);

                require(!user_folder_.empty(), "the run has no folder of the player's own");
                oakeys::Manifest manifest;
                manifest.format = oakeys::format_version;
                manifest.id = "own-oa-default";
                manifest.name = "Your changes";
                manifest.version.major = 1;
                manifest.version.minor = 0;
                manifest.version.parts = 2;
                manifest.author.name = "You";
                manifest.tags = {"own"};
                manifest.base = oakeys::Base::oa_default;
                // Attack takes Select builders' Ctrl+B, the clash a player makes
                // before SAVE. A second binding of the same chord in this file
                // is dropped as the layout loads, so the card would not show.
                oakeys::BindingEntry attack_binding;
                attack_binding.action = "order.attack";
                attack_binding.chords.push_back(oakeys::ChordText{"Ctrl+B", 0, 0});
                manifest.bindings.push_back(std::move(attack_binding));
                const std::filesystem::path own_dir = std::filesystem::path(user_folder_) /
                                                      std::string(key_library::keys_folder_name) /
                                                      key_library::own_folder_name("oa-default");
                std::error_code made;
                std::filesystem::create_directories(own_dir, made);
                require(!made, "the own layout folder was not made");
                const std::filesystem::path own_file = own_dir / std::string(oakeys::manifest_file);
                {
                    std::ofstream out(own_file, std::ios::binary);
                    require(static_cast<bool>(out), "the own layout was not opened");
                    out << oakeys::write_manifest(manifest);
                    require(static_cast<bool>(out), "the own layout was not written");
                }
                require(open_key_editor({}), "the key editor did not open on the clash");
                if (!named("oa.key-editor.card.settle").has_value() && width <= 640) {
                    std::optional<AutomationControl> clash_row;
                    std::vector<AutomationControl> clash_controls;
                    oa_layer().automation_controls(clash_controls);
                    for (const AutomationControl& control : clash_controls) {
                        if (control.name.starts_with("oa.key-editor.row.") && control.visible &&
                            control.window_width > 0 && control.window_height > 0) {
                            clash_row = control;
                            break;
                        }
                    }
                    require(clash_row.has_value(), "the clashes list shows no row");
                    click_named(clash_row->name);
                    PadAccess::send_key(*this, SDLK_RETURN);
                    bool going = true;
                    run_frame(going);
                    require(going, "opening the clash card ended the run");
                }
                require(
                    named("oa.key-editor.card.settle").has_value(), "the clash card is not shown"
                );
                paint("key-editor/card");
                // Settle… opens the one clash question: its outcomes and their words.
                click_named("oa.key-editor.card.settle");
                paint("key-editor/settle");
                if (LayerScreen* keys_card = oa_layer().find("key-editor"))
                    oa_layer().close(keys_card);
                std::error_code removed;
                std::filesystem::remove(own_file, removed);
                require(!removed, "the own layout was not removed");
            }

            // The brain picker, on the check's feed: the list, and the
            // details. Compact's details are their own page. Regular and
            // Large show them beside the list, and 960x540 paints that
            // inline pane once.
            {
                const auto named =
                    [this](std::string_view name) -> std::optional<AutomationControl> {
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
                    bool going = true;
                    for (const SDL_EventType type :
                         {SDL_EVENT_MOUSE_MOTION,
                          SDL_EVENT_MOUSE_BUTTON_DOWN,
                          SDL_EVENT_MOUSE_BUTTON_UP}) {
                        SDL_Event event = window_pointer_event(
                            sdl_.window,
                            sdl_.renderer,
                            type,
                            pixel,
                            type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
                        );
                        dispatch_event(event, going);
                        require(going, std::string(name) + " ended the run");
                    }
                    going = true;
                    run_frame(going);
                    require(going, std::string(name) + " ended the run");
                };
                install_brain_picker_check_feed();
                BrainPickerRequest request;
                request.setup = oa::data::brain_library::Setup::skirmish;
                request.seat_name = "North";
                request.current.brain = "steady-commander";
                request.current.level = "firm";
                request.current.options.emplace_back("aggression", "high");
                request.setup_difficulty = oa::data::brain_library::Difficulty::medium;
                request.built_in_level_fixed = true;
                request.offer_every_seat = true;
                require(open_brain_picker(std::move(request)), "the brain picker did not open");
                paint("brain-picker");
                if (width <= 640)
                    click_named("oa.brains.details");
                paint("brain-picker/details");
                if (width <= 640)
                    click_named("oa.brains.back");
                if (size_at == 0) {
                    const int32_t saved_width = width;
                    const int32_t saved_height = height;
                    width = 960;
                    height = 540;
                    require(
                        SDL_SetWindowSize(sdl_.window, width, height) &&
                            SDL_SyncWindow(sdl_.window),
                        "the window did not take 960x540"
                    );
                    apply_output_mode();
                    paint("brain-picker/details");
                    width = saved_width;
                    height = saved_height;
                    require(
                        SDL_SetWindowSize(sdl_.window, width, height) &&
                            SDL_SyncWindow(sdl_.window),
                        "the window did not return to 640x480"
                    );
                    apply_output_mode();
                }
                if (LayerScreen* brain_open = oa_layer().find(brains::screen_name))
                    oa_layer().close(brain_open);
                restore_brain_picker_feed();
            }

            // The four mutator questions, in this language. The mutators' names
            // are 64 bytes, the manifest's most. Reasons and problems stay a
            // long sentence, which wraps.
            {
                library::TextHooks hooks;
                hooks.shown = fit_shown;
                const std::string mutator_name(kMutatorName);
                const std::string long_name(kLongText);
                const auto paint_prompt = [&](kit::Question question, std::string_view screen) {
                    oa_layer().push(
                        std::make_unique<QuestionScreen>(
                            oa_layer(),
                            Screen::main_menu,
                            std::move(question),
                            QuestionScreen::Host{}
                        )
                    );
                    paint(screen);
                    if (LayerScreen* asked = oa_layer().find(QuestionScreen::screen_name))
                        oa_layer().close(asked);
                };
                picker::Failed failed;
                failed.name = mutator_name;
                failed.reason = long_name;
                const picker::Failed one_failure[] = {failed};
                paint_prompt(
                    picker::start_without_question(one_failure, hooks), "mutator-prompts/start-one"
                );
                const picker::Failed many_failures[] = {failed, failed};
                paint_prompt(
                    picker::start_without_question(many_failures, hooks),
                    "mutator-prompts/start-many"
                );
                const std::string names[] = {mutator_name, mutator_name};
                paint_prompt(picker::forget_set_question(names, hooks), "mutator-prompts/forget");
                picker::GetProgress fetching;
                fetching.name = mutator_name;
                fetching.index = 1;
                fetching.total = 2;
                fetching.progress = 400;
                paint_prompt(picker::get_mutators_question(fetching, hooks), "mutator-prompts/get");
                fetching.problem = long_name;
                paint_prompt(
                    picker::get_mutators_question(fetching, hooks), "mutator-prompts/get-failed"
                );
                picker::Differs differs;
                differs.player = long_name;
                differs.name = mutator_name;
                differs.here = long_name;
                differs.there = long_name;
                differs.fetches = long_name;
                const picker::Differs one_differs[] = {differs};
                paint_prompt(
                    picker::did_not_start_question(one_differs, hooks), "mutator-prompts/differ"
                );
                differs.there.clear();
                differs.fetches.clear();
                picker::Differs other = differs;
                other.player = long_name;
                const picker::Differs many_differs[] = {differs, other};
                paint_prompt(
                    picker::did_not_start_question(many_differs, hooks),
                    "mutator-prompts/differ-many"
                );
                brain_prompts::MissingBrain first_missing;
                first_missing.name = std::string(kBrainName);
                first_missing.version = "1.2";
                first_missing.player = 0;
                brain_prompts::MissingBrain second_missing;
                second_missing.name = std::string(kOtherBrainName);
                second_missing.version = "1.3";
                second_missing.player = 1;
                const brain_prompts::MissingBrain missing_brains[] = {
                    first_missing, second_missing
                };
                paint_prompt(
                    brain_prompts::load_built_in_question(missing_brains, hooks),
                    "brain-prompts/load-built-in"
                );
            }

            kit::Notice told;
            told.title = "NOTICE";
            told.paragraphs.push_back(kit::Paragraph{std::string(kLongText), false});
            told.open_caption = "OK";
            told.ok_caption = "OK";
            oa_layer().push(
                std::make_unique<NoticeScreen>(
                    oa_layer(), Screen::main_menu, std::move(told), NoticeScreen::Host{}
                )
            );
            paint("notice");
            if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
                oa_layer().close(notice);

            const std::string_view steps[] = {"language", "online", "buttons"};
            for (const std::string_view step : steps) {
                require(open_staged_first_run(step), "the welcome did not open");
                paint(std::string("first-run/") + std::string(step));
            }
            if (LayerScreen* welcome = oa_layer().find("first-run"))
                oa_layer().close(welcome);

            auto context = screen_context();
            const std::string startup =
                std::string(oa::data::languages::interface_text(kStartupNotice));
            // The game's own notice, or the message box it uses when that
            // window cannot be drawn. Either one is the sentence the main
            // menu shows for a game with no skirmish maps.
            if (!dialogs::open_notice(&context, startup, {}, nullptr, nullptr))
                show_frontend_message(
                    startup, 0x140, entry::message_show_ok, entry::message_fit_width
                );
            const bool startup_open = dialogs::dialog_kind() == dialogs::DialogKind::notice ||
                                      dialogs::dialog_kind() == dialogs::DialogKind::message_box;
            if (!startup_open) {
                std::string line = "startup-notice / ";
                line += language;
                line += " / ";
                line += std::to_string(width);
                line += 'x';
                line += std::to_string(height);
                line += " / expected \"";
                line += startup;
                line += "\" but the notice window did not open (no dialog art, or its layout did ";
                line += "not load) and the message box did not open either";
                if (failures.size() >= kMostFailures)
                    ++extra_failures;
                else
                    failures.push_back(FitFailure{std::move(line)});
            } else {
                paint("startup-notice");
                dialogs::reset_dialogs();
            }

            std::error_code here_error;
            const fs::path here = fs::current_path(here_error);
            require(!here_error, "the working folder is not known");
            const fs::path data_folder = here / "text-fit-game-files";
            const fs::path game_folder = data_folder / "Total Annihilation";
            std::error_code make_folder;
            fs::create_directories(game_folder, make_folder);
            require(!make_folder, "could not make a game files folder");

            const auto run_files = [&](GameFilesEntry entry, std::string_view screen) {
                GameFilesScreenRequest request;
                request.window = sdl_.window;
                request.renderer = sdl_.renderer;
                request.entry = entry;
                request.paths = oa::app::game_files::import_paths(game_folder, data_folder);
                request.version = game_files_version_text();
                request.preferences_file = options_.preferences_file;
                request.user_folder = options_.user_folder;
                request.players_own_profile = false;
                request.check.pass = quit_after_paint;
                kit::set_text_fit_screen(screen);
                kit::begin_text_fit_frame();
                std::ignore = run_game_files_screen(request);
                keep();
                apply_output_mode();
            };
            run_files(GameFilesEntry::manage, "game-files/manage");
            run_files(GameFilesEntry::first_run, "game-files/first-run");

            FitWindow shown{sdl_.window, sdl_.renderer};
            FolderChooserDisplayHooks hooks;
            hooks.context = &shown;
            hooks.window = fit_window;
            hooks.renderer = fit_renderer;
            GameFilesNeeded needed;
            needed.needed = true;
            needed.chooser = true;
            std::optional<GameDirectory> chosen_folder;
            kit::begin_text_fit_frame();
            std::ignore = run_folder_chooser_until_resolved(options_, hooks, needed, chosen_folder);
            keep();
            apply_output_mode();
            require(!chosen_folder, "the folder chooser kept a folder");

            // The keys overlay, beside the side column of one skirmish, at 640 only.
            if (size_at == 0) {
                close_covering();
                load(Screen::main_menu);
                bool overlay_going = true;
                run_frame(overlay_going);
                require(overlay_going, "the main menu ended the run before the keys overlay");
                start_benchmark_skirmish();
                require(
                    screen_ == Screen::match && match_ != nullptr,
                    "the keys overlay's skirmish did not start"
                );
                toggle_keys_overlay_screen();
                require(
                    oa_layer().find("keys-overlay") != nullptr, "the keys overlay did not open"
                );
                paint("keys-overlay");
                if (LayerScreen* overlay = oa_layer().find("keys-overlay"))
                    oa_layer().close(overlay);
                leave_match();
                load(Screen::main_menu);
                overlay_going = true;
                run_frame(overlay_going);
                require(overlay_going, "the main menu ended the run after the keys overlay");
            }
        }
    }

    close_covering();
    if (failures.empty()) {
        std::cout << "text-fit check: every screen fits in " << joined_languages(languages)
                  << " at 640x480 and 1280x720\n";
        std::cout << "text-fit check: mutator details fit in " << joined_languages(languages)
                  << " at 640x480, 960x540 and 1280x720\n";
        std::cout << "text-fit check: mutator-picker fits in " << joined_languages(languages)
                  << " at 640x480, 960x540 and 1280x720\n";
        std::cout << "text-fit check: brain-picker fits in " << joined_languages(languages)
                  << " at 640x480, 960x540 and 1280x720\n";
        return;
    }
    std::string report = "text-fit check: text does not fit:\n";
    for (const FitFailure& failure : failures) {
        report += failure.line;
        report += '\n';
    }
    if (extra_failures != 0)
        report += "and " + std::to_string(extra_failures) + " more\n";
    throw std::runtime_error(report);
}

} // namespace oa::app
