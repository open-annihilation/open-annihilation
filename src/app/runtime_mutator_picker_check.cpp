// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mutator-picker: the mutator picker over the main menu, on a feed of
// made-up mutators. At 640x480, 1280x720, 1920x1080 and 2560x1440 it requires
// the size class, the mended On order, every row's grip and switch, one
// download with the waiting words, switching that download off (one cancel)
// and on again, one more check when it installs, the turn-off question, a
// drag to the top of the On list, the set USE THESE hands back, CANCEL
// handing nothing, the verdict chip from the checking words to the
// incompatible ones, and the search taking the controller's Y. At 1280x720
// it also switches an installed mutator on (one check) and has USE THESE
// fetch Gatehouse Keep. At 2560x1440 switching on a mutator whose download
// is already under way keeps it on, and the runtime's own feed shows a
// mutator written into the check's Mutators folder, in English and in
// German. Each size's opening frame is written as
// mutator-picker-<w>x<h>.png in the working folder. The feed's check records
// a call and does not move the generation: the opening frame can still read
// the checking words. Publishing the canned verdicts moves the generation,
// and the next list carries them. After that, on the runtime's own feed, a
// made-up package that changes every builder is switched on at one option
// and then the other: the chip reads the checking words and then the
// checked words, and the verdict file holds one line for each option. The
// dry run is the Library worker's.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/data/registry/player_registries.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/mutator_picker/text.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace picker = oa::ui::mutator_picker;
namespace library = oa::ui::library;
namespace registry = oa::data::registry;
namespace layout = oa::ui::display_layout;
namespace dialogs = oa::ui::frontend_dialogs;

/// One window the check opens the picker in.
struct CheckWindow {
    int32_t width{};             ///< the window's width, in pixels
    int32_t height{};            ///< its height
    kit::SizeClass size_class{}; ///< the size class
    int32_t scale{};             ///< window pixels a point
};

/// The windows, in the order the check opens them.
constexpr std::array<CheckWindow, 4> kWindows{{
    {640, 480, kit::SizeClass::compact, 1},
    {1280, 720, kit::SizeClass::large, 1},
    {1920, 1080, kit::SizeClass::large, 1},
    {2560, 1440, kit::SizeClass::large, 2},
}};

/// The registry the fixture's listings name. Nothing is fetched.
constexpr std::string_view kRegistry = "core-prime";

/// One check the fixture was asked for.
struct RecordedCheck {
    std::string id{};
    std::vector<std::pair<std::string, std::string>> options{};
};

/// One package the fixture was asked to queue, to cancel or to retry.
struct RecordedEnqueue {
    std::string registry{};
    std::string key{};
};

/// One download the check stopped, so Get mutators can ask RETRY.
struct RecordedProblem {
    std::string id{};
    std::string problem{};
};

/// The feed's stand-in: six made-up mutators, and what the picker asked.
struct Fixture {
    uint64_t generation{1};
    bool published{false};
    bool check_enabled{true};
    std::vector<std::string> installed{};
    std::vector<RecordedCheck> checks{};
    std::vector<RecordedEnqueue> enqueues{};
    std::vector<RecordedEnqueue> cancels{};
    std::vector<RecordedEnqueue> retries{};
    std::vector<RecordedProblem> failures{};
    uint64_t next_id{1};
    /// Beacon joins the list only for the step that switches it on.
    bool include_beacon{false};
    /// Packages a download the picker did not queue is fetching: their rows
    /// read 40%, and queueing one again is refused, as the queue refuses it.
    std::vector<std::string> fetching{};
};

/// The one fixture. The feed's functions and the check's methods share it.
///
/// @return the fixture
Fixture& fixture() {
    static Fixture held;
    return held;
}

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("mutator picker check: " + std::string(what));
}

/// Writes a file whole.
///
/// @param file the file
/// @param bytes its bytes
void write_text(const fs::path& file, std::string_view bytes) {
    if (!file.parent_path().empty())
        fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

/// Writes a frame as a PNG file.
///
/// @param file the file
/// @param frame the frame
void write_png(const fs::path& file, const renderer::Surface& frame) {
    std::vector<uint8_t> bytes;
    const oa::formats::png::Header header{
        frame.width, frame.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    require(
        oa::formats::png::write(oa::formats::png::Image{header, {}, frame.rgb}, &bytes),
        "could not encode " + path_to_utf8(file)
    );
    write_text(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

/// Writes Lantern's icon: a 2 by 2 square the own feed can decode.
///
/// @param file the PNG
void write_lantern_icon(const fs::path& file) {
    const oa::formats::png::Header header{2, 2, 8, oa::formats::png::ColorType::rgb, {}};
    const std::array<uint8_t, 12> rows{
        0xE0, 0x90, 0x20, 0xE0, 0x90, 0x20, 0xC0, 0x70, 0x10, 0xC0, 0x70, 0x10
    };
    std::vector<uint8_t> bytes;
    require(
        oa::formats::png::write(oa::formats::png::Image{header, {}, rows}, &bytes),
        "could not encode the lantern icon"
    );
    write_text(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

/// Writes Lantern into a Mutators folder: a choice, a German name, an icon,
/// and a works-with line for a mod that is not played.
///
/// @param folder the mutator's folder
void write_lantern(const fs::path& folder) {
    oa::formats::oamut::Manifest manifest;
    manifest.format = oa::formats::oamut::format_version;
    manifest.id = "lantern";
    manifest.name = "Lantern";
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ridge";
    manifest.description = "A light on the ridge.";
    manifest.icon = "icon.png";
    manifest.tags = {"economy"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"ridge", {}, {}});
    manifest.layers = {oa::formats::oamut::Layer::files};
    oa::formats::oamut::Option glow;
    glow.id = "glow";
    glow.type = oa::formats::oamut::OptionType::choice;
    glow.label = "Glow";
    glow.values.push_back(oa::formats::oamut::ChoiceValue{false, {}, "dim"});
    glow.values.push_back(oa::formats::oamut::ChoiceValue{false, {}, "bright"});
    glow.value_labels = {"Dim", "Bright"};
    glow.default_index = 0;
    manifest.options.push_back(std::move(glow));
    oa::formats::oamut::Translation german;
    german.language = "de";
    german.name = "Laterne";
    german.options.push_back(oa::formats::oamut::OptionLabel{"glow", "Leuchten"});
    manifest.strings.push_back(std::move(german));
    write_text(folder / "oamut.yaml", oa::formats::oamut::write_manifest(manifest));
    write_text(folder / "mutator.lua", "return {}\n");
    write_lantern_icon(folder / "icon.png");
}

/// Writes Ridge Check Pace: a choice of two paces, and a rule that changes
/// every builder. The dry run is the same for either pace.
///
/// @param folder the mutator's folder
void write_pace(const fs::path& folder) {
    oa::formats::oamut::Manifest manifest;
    manifest.format = oa::formats::oamut::format_version;
    manifest.id = "ridge-check-pace";
    manifest.name = "Ridge Check Pace";
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "Ridge Maker";
    manifest.description = "Builders take their time.";
    manifest.tags = {"gameplay"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    manifest.layers = {oa::formats::oamut::Layer::units};
    oa::formats::oamut::Option pace;
    pace.id = "pace";
    pace.type = oa::formats::oamut::OptionType::choice;
    pace.label = "Pace";
    pace.values.push_back(oa::formats::oamut::ChoiceValue{false, {}, "steady"});
    pace.values.push_back(oa::formats::oamut::ChoiceValue{false, {}, "quick"});
    pace.default_index = 0;
    manifest.options.push_back(std::move(pace));
    write_text(folder / "oamut.yaml", oa::formats::oamut::write_manifest(manifest));
    write_text(
        folder / "mutator.lua",
        "function mutate(ctx)\n"
        "  return {\n"
        "    rules = {\n"
        "      where { builder = 1 }:change { workertime = scale(2) }\n"
        "    }\n"
        "  }\n"
        "end\n"
    );
}

/// Writes a made-up mutator whose rule scales one field of every builder.
///
/// @param folder the mutator's folder
/// @param id its id
/// @param name its name
/// @param field the builders' field the rule doubles
void write_builder_rule(
    const fs::path& folder, std::string_view id, std::string_view name, std::string_view field
) {
    oa::formats::oamut::Manifest manifest;
    manifest.format = oa::formats::oamut::format_version;
    manifest.id = std::string(id);
    manifest.name = std::string(name);
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "Ridge Maker";
    manifest.description = "A made-up mutator.";
    manifest.tags = {"gameplay"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    manifest.layers = {oa::formats::oamut::Layer::units};
    write_text(folder / "oamut.yaml", oa::formats::oamut::write_manifest(manifest));
    write_text(
        folder / "mutator.lua",
        "function mutate(ctx)\n"
        "  return {\n"
        "    rules = {\n"
        "      where { builder = 1 }:change { " +
            std::string(field) +
            " = scale(2) }\n"
            "    }\n"
            "  }\n"
            "end\n"
    );
}

/// Looks an interface word up in the language the game shows.
///
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_interface(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// Tells whether the fixture has marked a mutator installed.
///
/// @param id the mutator's id
/// @return true when it has
bool marked_installed(std::string_view id) {
    for (const std::string& extra : fixture().installed)
        if (extra == id)
            return true;
    return false;
}

/// Tells whether a download the picker did not queue is fetching a package.
///
/// @param key the package key
/// @return true when one is
bool fetching_elsewhere(std::string_view key) {
    for (const std::string& held : fixture().fetching)
        if (held == key)
            return true;
    return false;
}

/// One option's value on a recorded check.
///
/// @param record the check
/// @param key the option's id
/// @return the value; empty when the check did not name it
std::string option_of(const RecordedCheck& record, std::string_view key) {
    for (const auto& named : record.options)
        if (named.first == key)
            return named.second;
    return {};
}

/// How many times a key was queued.
///
/// @param key the package key
/// @return the count
std::size_t enqueued_key(std::string_view key) {
    std::size_t count = 0;
    for (const RecordedEnqueue& item : fixture().enqueues)
        if (item.key == key)
            ++count;
    return count;
}

/// Clears the fixture's record and moves its generation on, so the next
/// opening starts from the request's set.
void reset_fixture() {
    Fixture& held = fixture();
    held.checks.clear();
    held.enqueues.clear();
    held.cancels.clear();
    held.retries.clear();
    held.failures.clear();
    held.installed.clear();
    held.published = false;
    held.check_enabled = true;
    held.include_beacon = false;
    held.fetching.clear();
    ++held.generation;
}

/// Fast Builders, installed, with a build-speed choice.
///
/// @return the mutator
picker::Mutator fast_builders() {
    picker::Mutator mutator;
    mutator.id = "fast-builders";
    mutator.name = "Fast Builders";
    mutator.version = "1";
    mutator.summary = "Builders work faster.";
    mutator.installed = true;
    picker::OptionSpec option;
    option.id = "build_speed";
    option.type = picker::OptionSpec::Type::choice;
    option.label = "Build speed";
    option.values.emplace_back("1");
    option.values.emplace_back("4");
    option.captions.emplace_back("1×");
    option.captions.emplace_back("4×");
    option.fallback = "1";
    mutator.options.push_back(std::move(option));
    return mutator;
}

/// Gatehouse, installed.
///
/// @return the mutator
picker::Mutator gatehouse() {
    picker::Mutator mutator;
    mutator.id = "gatehouse";
    mutator.name = "Gatehouse";
    mutator.version = "1";
    mutator.summary = "A gate on the ridge.";
    mutator.installed = true;
    return mutator;
}

/// Gatehouse Keep, listed and not installed until the check marks it, needing Gatehouse.
///
/// @return the mutator
picker::Mutator gatehouse_keep() {
    picker::Mutator mutator;
    mutator.id = "gatehouse-keep";
    mutator.name = "Gatehouse Keep";
    mutator.version = "1";
    mutator.summary = "The keep above the gate.";
    mutator.installed = marked_installed("gatehouse-keep");
    mutator.needs.push_back(library::Prerequisite{"gatehouse", {}, "Gatehouse"});
    mutator.listing = {std::string(kRegistry), library::Kind::mutator, "gatehouse-keep"};
    return mutator;
}

/// Grounded, listed and not installed until the check marks it.
///
/// @return the mutator
picker::Mutator grounded() {
    picker::Mutator mutator;
    mutator.id = "grounded";
    mutator.name = "Grounded";
    mutator.version = "1";
    mutator.summary = "Units stay on the ground.";
    mutator.installed = marked_installed("grounded");
    mutator.listing = {std::string(kRegistry), library::Kind::mutator, "grounded"};
    return mutator;
}

/// Beacon, installed, with no works-with line, and not in the opening set.
/// Its verdict stays unchecked, so switching it on is the check the feed is asked for.
///
/// @return the mutator
picker::Mutator beacon() {
    picker::Mutator mutator;
    mutator.id = "beacon";
    mutator.name = "Beacon";
    mutator.version = "1";
    mutator.summary = "A light on the ridge.";
    mutator.installed = true;
    return mutator;
}

/// Ridge Walls, installed, and not for this game. The reason is the inner
/// works-with clause, filled in the language shown.
///
/// @param hooks how texts are looked up
/// @return the mutator
picker::Mutator ridge_walls(const library::TextHooks& hooks) {
    picker::Mutator mutator;
    mutator.id = "ridge-walls";
    mutator.name = "Ridge Walls";
    mutator.version = "1";
    mutator.summary = "Walls for the ridge.";
    mutator.installed = true;
    mutator.works_with_text = "Ridge";
    mutator.unavailable =
        library::filled(hooks, picker::words::works_with_mods, {{"names", std::string("Ridge")}});
    return mutator;
}

/// Applies the canned verdicts once they have been published.
///
/// Keep is compatible too: it was On at the opening, and a verdict that
/// stayed unchecked would put the chip back on the checking words.
/// Gatehouse's reason is short so the regular list's line can show it whole.
///
/// @param[in,out] mutator the mutator
void apply_verdict(picker::Mutator& mutator) {
    if (!fixture().published)
        return;
    if (mutator.id == "fast-builders" || mutator.id == "gatehouse-keep") {
        mutator.verdict.state = library::Verdict::State::compatible;
    } else if (mutator.id == "gatehouse") {
        mutator.verdict.state = library::Verdict::State::incompatible;
        mutator.verdict.reason = "no gate";
    }
}

/// The mutators the picker can show. The screen fills the setup and the set.
///
/// @return the inputs
picker::Inputs fixture_inputs(void* /*context*/, picker::Setup /*setup*/) {
    library::TextHooks hooks;
    hooks.shown = shown_interface;
    picker::Inputs inputs;
    inputs.text = hooks;
    inputs.mutators.push_back(fast_builders());
    inputs.mutators.push_back(gatehouse());
    inputs.mutators.push_back(gatehouse_keep());
    inputs.mutators.push_back(grounded());
    inputs.mutators.push_back(ridge_walls(hooks));
    if (fixture().include_beacon)
        inputs.mutators.push_back(beacon());
    for (picker::Mutator& mutator : inputs.mutators) {
        apply_verdict(mutator);
        if (mutator.installed || !fetching_elsewhere(mutator.id))
            continue;
        library::QueueItem item;
        item.registry = std::string(kRegistry);
        item.kind = library::Kind::mutator;
        item.key = mutator.id;
        item.name = mutator.name;
        item.phase = library::QueueItem::Phase::downloading;
        item.done_bytes = 40;
        item.total_bytes = 100;
        mutator.queued = std::move(item);
    }
    // A stopped download is already on the row, so the picker's own waiting
    // line does not replace it.
    for (picker::Mutator& row : inputs.mutators) {
        if (row.installed)
            continue;
        for (const RecordedProblem& stopped : fixture().failures) {
            if (stopped.id != row.id)
                continue;
            library::QueueItem failed;
            failed.registry =
                row.listing.registry.empty() ? std::string(kRegistry) : row.listing.registry;
            failed.kind = library::Kind::mutator;
            failed.key = row.id;
            failed.name = row.name;
            failed.phase = library::QueueItem::Phase::failed;
            failed.problem = stopped.problem;
            row.queued = std::move(failed);
        }
    }
    return inputs;
}

/// The fixture's generation. It moves when the verdicts are published, when
/// a mutator is marked installed, and when the check is silenced.
///
/// @return the generation
uint64_t fixture_generation(void* /*context*/) {
    return fixture().generation;
}

/// Records a package and queues nothing. The generation stays, so the row's
/// waiting line is the picker's own until a later list arrives. A package
/// already fetching is refused with the queue's words and not recorded.
///
/// @param registry the registry id
/// @param key the package key
/// @param[out] why why it was refused; may be null
/// @return a queue id
std::optional<uint64_t> fixture_enqueue(
    void* /*context*/, std::string_view registry, std::string_view key, std::string* why
) {
    if (fetching_elsewhere(key)) {
        if (why != nullptr)
            *why = "that package is already being downloaded";
        return std::nullopt;
    }
    fixture().enqueues.push_back(RecordedEnqueue{std::string(registry), std::string(key)});
    return fixture().next_id++;
}

/// Records a cancel. The fixture never started the download.
void fixture_cancel(void* /*context*/, std::string_view registry, std::string_view key) {
    fixture().cancels.push_back(RecordedEnqueue{std::string(registry), std::string(key)});
}

/// Records a retry and clears that download's stop, so the next list no
/// longer shows it failed.
void fixture_retry(void* /*context*/, std::string_view registry, std::string_view key) {
    fixture().retries.push_back(RecordedEnqueue{std::string(registry), std::string(key)});
    std::erase_if(fixture().failures, [&](const RecordedProblem& stopped) {
        return stopped.id == key;
    });
    ++fixture().generation;
}

/// Records a check. The verdict arrives later, when the check publishes it.
/// A fixture check always counts as started, so the chip reads the checking
/// words until the canned verdict is published.
bool fixture_check(void* /*context*/, std::string_view id, const picker::Chosen& chosen) {
    if (!fixture().check_enabled)
        return true;
    RecordedCheck record;
    record.id = std::string(id);
    record.options = chosen.options;
    fixture().checks.push_back(std::move(record));
    return true;
}

} // namespace

void Runtime::prepare_mutator_picker_check() {
    // Written before the content service reads the player's folder: every
    // built-in registry beside the program is off, so the check fetches nothing.
    registry::PlayerRegistries list;
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        const fs::path folder =
            path_from_utf8(base) / path_from_utf8(content::builtin_registries_folder_name);
        std::error_code error;
        if (fs::is_directory(folder, error))
            for (const registry::Descriptor& built_in :
                 registry::read_builtin_registries(folder, nullptr))
                list.disabled.push_back(built_in.id);
    }
    write_text(
        user_folder() / std::string(registry::player_registries_file),
        registry::player_registries_text(list)
    );
    std::cout << "mutator picker check: " << list.disabled.size()
              << " built-in registries turned off\n";
}

void Runtime::install_mutator_picker_check_feed() {
    MutatorPickerFeed feed;
    feed.context = &fixture();
    feed.inputs = fixture_inputs;
    feed.generation = fixture_generation;
    feed.enqueue = fixture_enqueue;
    feed.cancel = fixture_cancel;
    feed.retry = fixture_retry;
    feed.check = fixture().check_enabled ? fixture_check : nullptr;
    set_mutator_picker_feed(feed);
}

void Runtime::publish_mutator_picker_check_verdicts() {
    fixture().published = true;
    ++fixture().generation;
}

void Runtime::mark_mutator_picker_check_installed(std::string_view id) {
    fixture().installed.emplace_back(id);
    ++fixture().generation;
}

void Runtime::fail_mutator_picker_check_download(std::string_view id, std::string_view problem) {
    fixture().failures.push_back(RecordedProblem{std::string(id), std::string(problem)});
    ++fixture().generation;
}

bool Runtime::mutator_picker_check_retried(std::string_view key) const {
    for (const RecordedEnqueue& item : fixture().retries)
        if (item.key == key)
            return true;
    return false;
}

void Runtime::silence_mutator_picker_check() {
    fixture().check_enabled = false;
    fixture().published = false;
    ++fixture().generation;
    mutator_picker_feed_.check = nullptr;
}

void Runtime::check_mutator_picker() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

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
        require(control.has_value(), std::string(why));
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
    const auto picker_screen = [this]() -> LayerScreen* {
        return oa_layer().find(picker::screen_name);
    };
    const auto list_has = [&](std::string_view needle) {
        LayerScreen* screen = picker_screen();
        require(screen != nullptr, "the mutator picker is not open");
        const kit::DisplayList list = screen->display_list();
        for (const kit::Item& item : list.items) {
            if (item.text.find(needle) != std::string::npos)
                return true;
            for (const std::string& line : item.lines)
                if (line.find(needle) != std::string::npos)
                    return true;
        }
        return false;
    };
    const auto close_covering = [&] {
        dialogs::reset_dialogs();
        if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
            oa_layer().close(notice);
        if (LayerScreen* question = oa_layer().find(QuestionScreen::screen_name))
            oa_layer().close(question);
        if (LayerScreen* welcome = oa_layer().find("first-run"))
            oa_layer().close(welcome);
        if (LayerScreen* library_screen = oa_layer().find("library"))
            oa_layer().close(library_screen);
        if (LayerScreen* picker_open = oa_layer().find(picker::screen_name))
            oa_layer().close(picker_open);
        frames(1);
    };

    bool used = false;
    bool cancelled = false;
    std::vector<picker::Chosen> handed;
    const auto open_picker = [&](std::string_view on) {
        used = false;
        cancelled = false;
        handed.clear();
        MutatorPickerRequest request;
        request.setup = picker::Setup::skirmish;
        picker::Chosen keep;
        keep.id = "gatehouse-keep";
        picker::Chosen fast;
        fast.id = "fast-builders";
        fast.options.emplace_back("build_speed", "4");
        request.set.push_back(std::move(keep));
        request.set.push_back(std::move(fast));
        request.used = [&](std::vector<picker::Chosen> set) {
            handed = std::move(set);
            used = true;
        };
        request.cancelled = [&] { cancelled = true; };
        require(
            open_mutator_picker(std::move(request)), "the picker did not open" + std::string(on)
        );
        frames(1);
        require(picker_screen() != nullptr, "the picker is not on the layer" + std::string(on));
    };
    const auto open_empty = [&](std::string_view on) {
        used = false;
        cancelled = false;
        handed.clear();
        MutatorPickerRequest request;
        request.setup = picker::Setup::skirmish;
        request.used = [&](std::vector<picker::Chosen> set) {
            handed = std::move(set);
            used = true;
        };
        request.cancelled = [&] { cancelled = true; };
        require(
            open_mutator_picker(std::move(request)), "the picker did not open" + std::string(on)
        );
        frames(1);
        require(picker_screen() != nullptr, "the picker is not on the layer" + std::string(on));
    };
    const auto package_picture = [&] {
        LayerScreen* screen = picker_screen();
        require(screen != nullptr, "the mutator picker is not open");
        const kit::DisplayList list = screen->display_list();
        for (const kit::Item& item : list.items) {
            if (item.role != kit::Role::package_badge)
                continue;
            const auto* look = std::get_if<kit::BadgeLook>(&item.look);
            if (look != nullptr && look->picture.width > 0 && !look->picture.pixels.empty())
                return true;
        }
        return false;
    };

    for (const CheckWindow& window : kWindows) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        const std::string on = " on the " + size + " window";
        const bool compact = window.size_class == kit::SizeClass::compact;
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open" + on);
        frames(2);
        close_covering();
        require(!oa_layer().modal_shown(false), "a modal screen is still up" + on);
        require(dialogs::dialog_count() == 0, "a front-end dialog is still up" + on);
        require(
            oa_layer().screen_class() == window.size_class &&
                oa_layer().screen_scale() == window.scale,
            "the layer is not " + std::string(size_class_name(window.size_class)) + " at " +
                std::to_string(window.scale) + "x" + on
        );
        std::cout << "mutator picker check: " << size << " is "
                  << size_class_name(window.size_class) << " at " << window.scale << "x\n";

        reset_fixture();
        install_mutator_picker_check_feed();
        open_picker(on);

        const LayerPlacement placed = oa_layer().placement_of(picker::screen_name);
        require(
            placed.points_width > 0 && placed.points_height > 0, "the picker has no place" + on
        );
        const LayerView seen = oa_layer().view();
        const int32_t centre_x = placed.shown.x + placed.shown.width / 2;
        const int32_t centre_y = placed.shown.y + placed.shown.height / 2;
        const int32_t room_x = seen.room.x + seen.room.width / 2;
        const int32_t room_y = seen.room.y + seen.room.height / 2;
        const auto apart = [](int32_t left, int32_t right) {
            return left > right ? left - right : right - left;
        };
        require(
            apart(centre_x, room_x) <= 2 && apart(centre_y, room_y) <= 2,
            "the picker is not centred" + on
        );

        // The set was Keep, then Fast Builders. Opening brings Gatehouse above Keep.
        const char* on_ids[] = {"gatehouse", "gatehouse-keep", "fast-builders"};
        std::vector<std::pair<int32_t, std::string>> order;
        for (const char* id : on_ids) {
            const std::string row = std::string("oa.mutators.row.") + id;
            const AutomationControl control = require_named(row, row + " is not shown" + on);
            order.emplace_back(control.window_y, id);
        }
        std::sort(order.begin(), order.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        require(
            order.size() == 3 && order[0].second == "gatehouse" &&
                order[1].second == "gatehouse-keep" && order[2].second == "fast-builders",
            "the On rows are not Gatehouse, Gatehouse Keep, Fast Builders" + on
        );
        std::cout << "mutator picker check: On order is Gatehouse, Gatehouse Keep, Fast Builders"
                  << on << "\n";

        for (const char* id :
             {"gatehouse", "gatehouse-keep", "fast-builders", "grounded", "ridge-walls"}) {
            const std::string row = std::string("oa.mutators.row.") + id;
            require_named(row + ".grip", row + ".grip is not shown" + on);
            require_named(row + ".switch", row + ".switch is not shown" + on);
        }
        const bool works = list_has("it works with") && list_has("Ridge");
        if (works) {
            std::cout << "mutator picker check: Ridge Walls reads that it works with Ridge" << on
                      << "\n";
        } else if (compact) {
            std::cout << "mutator picker check: compact cuts Ridge Walls' line" << on << "\n";
        } else {
            require(false, "Ridge Walls does not read that it works with Ridge" + on);
        }
        require(enqueued_key("ridge-walls") == 0, "Ridge Walls was queued" + on);

        require(fixture().checks.size() == 3, "opening did not check the three On mutators" + on);
        require(
            fixture().checks[0].id == "gatehouse" && fixture().checks[1].id == "gatehouse-keep" &&
                fixture().checks[2].id == "fast-builders",
            "the opening checks are not Gatehouse, Keep, Fast Builders" + on
        );
        require(
            option_of(fixture().checks[2], "build_speed") == "4",
            "Fast Builders did not open at 4" + on
        );
        std::cout << "mutator picker check: opening checked Gatehouse, Gatehouse Keep and "
                     "Fast Builders at 4"
                  << on << "\n";

        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        require(
            presented.width == static_cast<uint32_t>(window.width) &&
                presented.height == static_cast<uint32_t>(window.height),
            "the frame is not the window's size" + on
        );
        write_png(fs::path("mutator-picker-" + size + ".png"), presented);
        std::cout << "mutator picker check: wrote mutator-picker-" << size << ".png\n";

        if (compact) {
            require(
                !named("oa.mutators.verdict").has_value(), "compact shows the verdict chip" + on
            );
            std::cout << "mutator picker check: compact omits the verdict chip" << on << "\n";
        } else {
            const AutomationControl chip =
                require_named("oa.mutators.verdict", "the verdict chip is not shown" + on);
            require(
                chip.text.find("Checking") != std::string::npos,
                "the chip does not read the checking words" + on
            );
            std::cout << "mutator picker check: verdict reads \"" << chip.text << "\"" << on
                      << "\n";
        }

        publish_mutator_picker_check_verdicts();
        frames(1);
        if (!compact) {
            const AutomationControl chip =
                require_named("oa.mutators.verdict", "the verdict chip left" + on);
            require(
                chip.text.find("Not compatible") != std::string::npos &&
                    chip.text.find("Gatehouse") != std::string::npos,
                "the chip does not name Gatehouse as not compatible" + on
            );
            std::cout << "mutator picker check: verdict reads \"" << chip.text << "\"" << on
                      << "\n";
        }
        const bool reason = list_has("no gate");
        if (reason) {
            std::cout << "mutator picker check: Gatehouse's row reads its reason" << on << "\n";
        } else if (compact) {
            std::cout << "mutator picker check: compact cuts Gatehouse's reason" << on << "\n";
        } else {
            require(false, "Gatehouse's row does not read its reason" + on);
        }

        click_named("oa.mutators.row.fast-builders");
        if (compact)
            click_named("oa.mutators.details");
        const std::size_t checks_before = fixture().checks.size();
        click_named("oa.mutators.option.build-speed.1");
        if (compact)
            click_named("oa.mutators.back");
        require(
            fixture().checks.size() == checks_before + 1,
            "the option change asked for no check" + on
        );
        require(
            fixture().checks.back().id == "fast-builders" &&
                option_of(fixture().checks.back(), "build_speed") == "1",
            "the option change did not check Fast Builders at 1" + on
        );
        std::cout << "mutator picker check: Fast Builders' option 1 asked for another check" << on
                  << "\n";

        const std::size_t checks_before_grounded = fixture().checks.size();
        click_named("oa.mutators.row.grounded.switch.on");
        require(fixture().enqueues.size() == 1, "Grounded did not queue exactly one package" + on);
        require(
            fixture().enqueues.front().registry == kRegistry &&
                fixture().enqueues.front().key == "grounded",
            "the queued package is not grounded from " + std::string(kRegistry) + on
        );
        require(
            fixture().checks.size() == checks_before_grounded + 1 &&
                fixture().checks.back().id == "grounded",
            "switching Grounded on did not ask for one check" + on
        );
        frames(1);
        require(
            fixture().checks.size() == checks_before_grounded + 1,
            "switching Grounded on asked for a second check" + on
        );
        click_named("oa.mutators.row.grounded.switch.off");
        require(fixture().cancels.size() == 1, "switching Grounded off did not cancel once" + on);
        require(
            fixture().cancels.front().registry == kRegistry &&
                fixture().cancels.front().key == "grounded",
            "the cancel is not grounded from " + std::string(kRegistry) + on
        );
        std::cout << "mutator picker check: switching Grounded off cancelled its waiting download"
                  << on << "\n";
        click_named("oa.mutators.row.grounded.switch.on");
        require(fixture().enqueues.size() == 2, "Grounded did not queue again" + on);
        require(
            fixture().enqueues[1].registry == kRegistry && fixture().enqueues[1].key == "grounded",
            "the second package is not grounded from " + std::string(kRegistry) + on
        );
        require(
            fixture().checks.size() == checks_before_grounded + 1,
            "switching Grounded on again asked for another check" + on
        );
        require(enqueued_key("gatehouse-keep") == 0, "Gatehouse Keep was queued" + on);
        const AutomationControl use_waiting =
            require_named("oa.mutators.use", "USE THESE is not shown" + on);
        require(use_waiting.enabled, "USE THESE is disabled while Grounded is downloading" + on);
        const AutomationControl summary =
            require_named("oa.mutators.summary", "the summary is not shown" + on);
        require(
            summary.text.find("Getting") != std::string::npos &&
                summary.text.find("Grounded") != std::string::npos &&
                summary.text.find("0%") != std::string::npos,
            "the summary does not say Grounded is being fetched" + on
        );
        std::cout << "mutator picker check: queued grounded from " << kRegistry
                  << " and USE THESE reads \"" << summary.text << "\"" << on << "\n";

        const std::size_t checks_before_install = fixture().checks.size();
        mark_mutator_picker_check_installed("grounded");
        frames(1);
        require(
            fixture().checks.size() == checks_before_install + 1 &&
                fixture().checks.back().id == "grounded",
            "installing Grounded did not ask for one check" + on
        );
        frames(1);
        require(
            fixture().checks.size() == checks_before_install + 1,
            "installing Grounded asked for a second check" + on
        );
        std::cout << "mutator picker check: installing Grounded asked for one more check" << on
                  << "\n";
        const AutomationControl use_ready =
            require_named("oa.mutators.use", "USE THESE left after the install" + on);
        require(use_ready.enabled, "USE THESE stayed disabled after Grounded was installed" + on);
        require(
            enqueued_key("gatehouse-keep") == 0, "Gatehouse Keep was queued after the install" + on
        );
        const AutomationControl keep_row =
            require_named("oa.mutators.row.gatehouse-keep", "Gatehouse Keep left the list" + on);
        const AutomationControl fast_row =
            require_named("oa.mutators.row.fast-builders", "Fast Builders left the list" + on);
        require(
            keep_row.window_y < fast_row.window_y,
            "Gatehouse Keep did not stay above Fast Builders" + on
        );
        std::cout << "mutator picker check: Grounded installed and USE THESE is enabled" << on
                  << "\n";

        click_named("oa.mutators.row.gatehouse.switch.off");
        require_named("oa.prompt.turn-off", "TURN OFF was not asked" + on);
        std::cout << "mutator picker check: switching Gatehouse off asks TURN OFF" << on << "\n";
        click_named("oa.prompt.cancel");
        require(picker_screen() != nullptr, "CANCEL closed the picker" + on);
        require(
            named("oa.mutators.row.gatehouse").has_value() &&
                named("oa.mutators.row.gatehouse-keep").has_value(),
            "CANCEL turned Gatehouse or Gatehouse Keep off" + on
        );
        const int32_t kept_gatehouse = named("oa.mutators.row.gatehouse")->window_y;
        const int32_t kept_keep = named("oa.mutators.row.gatehouse-keep")->window_y;
        require(kept_gatehouse < kept_keep, "CANCEL reordered Gatehouse and Gatehouse Keep" + on);
        std::cout << "mutator picker check: CANCEL keeps Gatehouse and Gatehouse Keep" << on
                  << "\n";

        click_named("oa.mutators.row.gatehouse.switch.off");
        click_named("oa.prompt.turn-off");
        const AutomationControl gatehouse_on =
            require_named("oa.mutators.row.gatehouse.switch.on", "Gatehouse's switch left" + on);
        const AutomationControl keep_on = require_named(
            "oa.mutators.row.gatehouse-keep.switch.on", "Gatehouse Keep's switch left" + on
        );
        require(!gatehouse_on.checked && !keep_on.checked, "TURN OFF left a switch on" + on);
        // Both left the On list: Fast Builders is above Grounded.
        const AutomationControl fast_after =
            require_named("oa.mutators.row.fast-builders", "Fast Builders left" + on);
        const AutomationControl grounded_after =
            require_named("oa.mutators.row.grounded", "Grounded left" + on);
        require(
            fast_after.window_y < grounded_after.window_y,
            "TURN OFF did not leave Fast Builders above Grounded" + on
        );
        std::cout << "mutator picker check: TURN OFF turns Gatehouse and Gatehouse Keep off" << on
                  << "\n";

        // A drag starts after the pointer moves three points. The drop at the
        // first On row's top is the top of the On list.
        const AutomationControl grip =
            require_named("oa.mutators.row.grounded.grip", "Grounded's grip is not shown" + on);
        const AutomationControl first_on = require_named(
            "oa.mutators.row.fast-builders", "Fast Builders is not the first On row" + on
        );
        const int32_t grip_x = grip.window_x + grip.window_width / 2;
        const int32_t grip_y = grip.window_y + grip.window_height / 2;
        pointer_at({grip_x, grip_y}, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at({grip_x, grip_y}, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at({grip_x, grip_y - 48}, SDL_EVENT_MOUSE_MOTION, 0);
        const int32_t drop_y = first_on.window_y + 2;
        pointer_at({grip_x, drop_y}, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at({grip_x, drop_y}, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(1);
        const AutomationControl grounded_top =
            require_named("oa.mutators.row.grounded", "Grounded left after the drag" + on);
        const AutomationControl fast_second = require_named(
            "oa.mutators.row.fast-builders", "Fast Builders left after the drag" + on
        );
        require(
            grounded_top.window_y < fast_second.window_y, "the drag did not put Grounded first" + on
        );
        std::cout << "mutator picker check: the drag put Grounded first" << on << "\n";

        require(fixture().enqueues.size() == 2, "something else was queued before USE THESE" + on);
        require(
            fixture().cancels.size() == 1, "something else was cancelled before USE THESE" + on
        );
        click_named("oa.mutators.use");
        require(used && !cancelled, "USE THESE did not hand the set" + on);
        require(picker_screen() == nullptr, "USE THESE left the picker open" + on);
        require(handed.size() == 2, "USE THESE did not hand two mutators" + on);
        require(
            handed[0].id == "grounded" && handed[1].id == "fast-builders",
            "USE THESE did not hand Grounded then Fast Builders" + on
        );
        std::string handed_speed;
        for (const auto& named_option : handed[1].options)
            if (named_option.first == "build_speed")
                handed_speed = named_option.second;
        require(handed_speed == "1", "Fast Builders was not handed at 1" + on);
        std::cout << "mutator picker check: handed Grounded, Fast Builders build_speed=1" << on
                  << "\n";

        open_picker(on);
        click_named("oa.mutators.cancel");
        require(cancelled && !used, "CANCEL handed a set" + on);
        require(picker_screen() == nullptr, "CANCEL left the picker open" + on);
        std::cout << "mutator picker check: CANCEL hands nothing" << on << "\n";

        if (window.width == 1280) {
            fixture().include_beacon = true;
            const std::size_t checks_before_beacon = fixture().checks.size();
            open_picker(on);
            click_named("oa.mutators.row.beacon.switch.on");
            require(
                fixture().checks.size() == checks_before_beacon + 1 &&
                    fixture().checks.back().id == "beacon",
                "switching Beacon on did not ask for one check" + on
            );
            frames(1);
            require(
                fixture().checks.size() == checks_before_beacon + 1,
                "switching Beacon on asked for a second check" + on
            );
            std::cout << "mutator picker check: switching Beacon on asked for one check" << on
                      << "\n";
            click_named("oa.mutators.cancel");
            require(picker_screen() == nullptr, "CANCEL left the picker open after Beacon" + on);
            fixture().include_beacon = false;

            const std::size_t keep_before = enqueued_key("gatehouse-keep");
            open_picker(on);
            click_named("oa.mutators.use");
            require(
                enqueued_key("gatehouse-keep") == keep_before + 1,
                "USE THESE did not queue Gatehouse Keep" + on
            );
            require(
                picker_screen() != nullptr, "USE THESE closed the picker while Keep waited" + on
            );
            require(
                named("oa.prompt.cancel").has_value(),
                "Get mutators was not asked while Gatehouse Keep waited" + on
            );
            require(
                list_has("Getting") && list_has("Gatehouse Keep"),
                "the summary does not say Gatehouse Keep is being fetched" + on
            );
            bool use_enabled = false;
            {
                const kit::DisplayList waiting_list = picker_screen()->display_list();
                for (const kit::AutomationEntry& entry :
                     kit::automation_parts(waiting_list, picker_screen()->interaction()))
                    if (entry.name == picker::name_use)
                        use_enabled = entry.enabled;
            }
            require(use_enabled, "USE THESE is disabled while Gatehouse Keep is downloading" + on);
            std::cout << "mutator picker check: USE THESE queued Gatehouse Keep" << on << "\n";
            mark_mutator_picker_check_installed("gatehouse-keep");
            for (int32_t wait = 0; wait < 8 && picker_screen() != nullptr; ++wait)
                frames(1);
            require(used && !cancelled, "USE THESE did not hand the set after Keep installed" + on);
            require(picker_screen() == nullptr, "USE THESE left the picker open after Keep" + on);
            require(handed.size() == 3, "USE THESE did not hand three mutators" + on);
            require(
                handed[0].id == "gatehouse" && handed[1].id == "gatehouse-keep" &&
                    handed[2].id == "fast-builders",
                "USE THESE did not hand Gatehouse, Gatehouse Keep, Fast Builders" + on
            );
            std::string keep_speed;
            for (const auto& named_option : handed[2].options)
                if (named_option.first == "build_speed")
                    keep_speed = named_option.second;
            require(keep_speed == "4", "Fast Builders was not handed at 4" + on);
            std::cout << "mutator picker check: USE THESE handed Gatehouse, Gatehouse Keep, "
                         "Fast Builders build_speed=4"
                      << on << "\n";
        }

        silence_mutator_picker_check();
        open_picker(on);
        require(
            !named("oa.mutators.verdict").has_value(), "a silenced feed still shows a chip" + on
        );
        std::cout << "mutator picker check: with no check, no verdict chip shows" << on << "\n";
        require(
            oa_layer().take_pad_action(oa::ui::pad_controls::Action::library_search),
            "the controller's Y was not taken" + on
        );
        frames(1);
        const AutomationControl search =
            require_named("oa.mutators.search", "the search is not shown" + on);
        require(search.focused, "the controller's Y did not focus the search" + on);
        std::cout << "mutator picker check: the controller's Y focuses the search" << on << "\n";
        click_named("oa.mutators.cancel");
        require(picker_screen() == nullptr, "the picker stayed open" + on);
    }

    // A download the picker did not queue, such as the Library's: switching
    // its mutator on is not a refusal, and the row and footer follow it.
    reset_fixture();
    fixture().fetching.emplace_back("grounded");
    install_mutator_picker_check_feed();
    const std::string elsewhere = " while Grounded downloads elsewhere";
    open_empty(elsewhere);
    click_named("oa.mutators.row.grounded.switch.on");
    require(fixture().enqueues.empty(), "something was queued" + elsewhere);
    require(
        !list_has("already being downloaded"), "switching Grounded on read as refused" + elsewhere
    );
    const AutomationControl elsewhere_use =
        require_named("oa.mutators.use", "USE THESE is not shown" + elsewhere);
    require(elsewhere_use.enabled, "USE THESE is disabled" + elsewhere);
    const AutomationControl elsewhere_summary =
        require_named("oa.mutators.summary", "the summary is not shown" + elsewhere);
    require(
        elsewhere_summary.text.find("Getting Grounded · 40%") != std::string::npos,
        "Grounded is not on at 40%" + elsewhere
    );
    std::cout << "mutator picker check: switching Grounded on kept it on at 40%" << elsewhere
              << "\n";
    click_named("oa.mutators.cancel");
    require(picker_screen() == nullptr, "CANCEL left the picker open" + elsewhere);
    reset_fixture();

    const fs::path lantern_folder =
        user_folder() / std::string(oa::data::mutator_library::mutators_folder_name) / "lantern";
    write_lantern(lantern_folder);
    restore_mutator_picker_feed();
    const std::string own = " on the runtime's own feed";
    open_empty(own);
    const AutomationControl lantern =
        require_named("oa.mutators.row.lantern", "Lantern's row is not shown" + own);
    require(
        lantern.text.find("Lantern") != std::string::npos, "Lantern's row does not name it" + own
    );
    require(list_has("Glow"), "Lantern's option label is not shown" + own);
    require(package_picture(), "Lantern's icon is not shown" + own);
    require(list_has("Not compatible"), "Lantern does not read that it is not compatible" + own);
    require(list_has("it works with"), "Lantern does not read that it works with a mod" + own);
    require(list_has("ridge"), "Lantern does not name ridge" + own);
    require(!list_has("Getting"), "Lantern was queued" + own);
    {
        const std::vector<content::DownloadView> downloads = content_downloads().view();
        bool queued_lantern = false;
        for (const content::DownloadView& item : downloads)
            if (item.target.key == "lantern")
                queued_lantern = true;
        require(!queued_lantern, "a download is queued for lantern" + own);
    }
    std::cout << "mutator picker check: the own feed shows Lantern\n";
    click_named("oa.mutators.cancel");
    require(picker_screen() == nullptr, "CANCEL left the own feed's picker open");

    set_language_choice("de");
    close_covering();
    open_empty(" in German");
    const AutomationControl laterne =
        require_named("oa.mutators.row.lantern", "Laterne's row is not shown");
    require(laterne.text.find("Laterne") != std::string::npos, "the own feed did not show Laterne");
    std::cout << "mutator picker check: the own feed shows Laterne in German\n";
    click_named("oa.mutators.cancel");
    set_language_choice("en");
    close_covering();

    const fs::path pace_folder = user_folder() /
                                 std::string(oa::data::mutator_library::mutators_folder_name) /
                                 "ridge-check-pace";
    write_pace(pace_folder);
    restore_mutator_picker_feed();
    {
        const std::string on = " with Ridge Check Pace";
        MutatorPickerRequest request;
        request.setup = picker::Setup::skirmish;
        picker::Chosen pace;
        pace.id = "ridge-check-pace";
        pace.options.emplace_back("pace", "quick");
        request.set.push_back(std::move(pace));
        require(open_mutator_picker(std::move(request)), "the picker did not open" + on);
        frames(1);
        require(picker_screen() != nullptr, "the picker is not on the layer" + on);
        const AutomationControl checking =
            require_named("oa.mutators.verdict", "the verdict chip is not shown" + on);
        require(
            checking.text.find("Checking") != std::string::npos,
            "the chip does not read the checking words: " + checking.text
        );
        std::cout << "mutator picker check: real verdict reads \"" << checking.text << "\"\n";

        const auto chip_text = [&] {
            const std::optional<AutomationControl> shown = named("oa.mutators.verdict");
            return shown ? shown->text : std::string{};
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        std::string checked;
        while (std::chrono::steady_clock::now() < deadline) {
            checked = chip_text();
            if (checked.find("Checked with No Mod") != std::string::npos)
                break;
            frames(1);
        }
        require(
            checked.find("Checked with No Mod") != std::string::npos,
            "the chip did not read the checked words: " + checked
        );
        std::cout << "mutator picker check: real verdict reads \"" << checked << "\"\n";

        click_named("oa.mutators.row.ridge-check-pace");
        click_named("oa.mutators.option.pace.steady");
        const AutomationControl again =
            require_named("oa.mutators.verdict", "the verdict chip left after the option");
        require(
            again.text.find("Checking") != std::string::npos,
            "the option change did not pass through checking: " + again.text
        );
        checked.clear();
        const auto again_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (std::chrono::steady_clock::now() < again_deadline) {
            checked = chip_text();
            if (checked.find("Checked with No Mod") != std::string::npos)
                break;
            frames(1);
        }
        require(
            checked.find("Checked with No Mod") != std::string::npos,
            "the option change did not finish checked: " + checked
        );
        std::cout << "mutator picker check: real verdict reads \"" << checked
                  << "\" after the option\n";

        const fs::path verdicts = user_folder() / std::string(cache_folder_name) /
                                  std::string(oa::data::mutators::verdicts_file_name);
        std::ifstream file(verdicts);
        require(static_cast<bool>(file), "the verdict file is not there");
        std::size_t lines = 0;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            require(
                line.find("\tcompatible\t") != std::string::npos,
                "a verdict line is not compatible: " + line
            );
            ++lines;
        }
        require(lines == 2, "the verdict file does not hold one line for each option");
        std::cout << "mutator picker check: the verdict file holds both option values\n";
        click_named("oa.mutators.cancel");
        require(picker_screen() == nullptr, "CANCEL left the real check's picker open");
    }

    // Over SKIRMISH, where MUTATORS… opens it, with three On mutators that
    // have no verdict yet: each is checked in turn on the Library's worker,
    // and the chip reads the checked words once the last is kept.
    {
        const std::string on = " over SKIRMISH";
        const fs::path mutators_folder =
            user_folder() / std::string(oa::data::mutator_library::mutators_folder_name);
        const std::array<std::array<std::string_view, 3>, 3> rules{{
            {"ridge-check-reach", "Ridge Check Reach", "builddistance"},
            {"ridge-check-pick", "Ridge Check Pick", "workertime"},
            {"ridge-check-store", "Ridge Check Store", "energystorage"},
        }};
        MutatorPickerRequest request;
        request.setup = picker::Setup::skirmish;
        for (const auto& rule : rules) {
            write_builder_rule(mutators_folder / std::string(rule[0]), rule[0], rule[1], rule[2]);
            picker::Chosen chosen;
            chosen.id = std::string(rule[0]);
            request.set.push_back(std::move(chosen));
        }
        load(Screen::main_menu);
        frames(2);
        close_covering();
        exercise_click(menu::resource_name(menu::Button::single_player));
        frames(1);
        exercise_click(entry::resource_name(entry::Button::skirmish));
        frames(2);
        close_covering();
        require(screen_ == Screen::skirmish, "SKIRMISH did not open" + on);
        require(open_mutator_picker(std::move(request)), "the picker did not open" + on);
        frames(1);
        const auto chip_text = [&] {
            const std::optional<AutomationControl> shown = named("oa.mutators.verdict");
            return shown ? shown->text : std::string{};
        };
        require(
            chip_text().find("Checking") != std::string::npos,
            "the chip does not read the checking words" + on + ": " + chip_text()
        );
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        std::string checked;
        while (std::chrono::steady_clock::now() < deadline) {
            checked = chip_text();
            if (checked.find("Checked with No Mod") != std::string::npos)
                break;
            frames(1);
        }
        require(
            checked.find("Checked with No Mod") != std::string::npos,
            "the chip did not read the checked words" + on + ": " + checked
        );
        std::cout << "mutator picker check: three new mutators read \"" << checked << "\"" << on
                  << "\n";
        click_named("oa.mutators.cancel");
        require(picker_screen() == nullptr, "CANCEL left the picker open" + on);
        load(Screen::main_menu);
        frames(1);
    }

    // A mutator that already has a verdict, and an On mutator whose list
    // cannot be resolved. The chip keeps the verdict. It does not read the
    // checking words for a check that never starts.
    {
        const std::string on = " with a check that cannot start";
        const fs::path mutators_folder =
            user_folder() / std::string(oa::data::mutator_library::mutators_folder_name);
        const fs::path stop_folder = mutators_folder / "ridge-check-stop";
        write_text(
            stop_folder / "oamut.yaml",
            "oamut: 1\n"
            "id: ridge-check-stop\n"
            "name: \"Ridge Check Stop\"\n"
            "version: \"1.0\"\n"
            "author:\n"
            "  name: \"Ridge Maker\"\n"
            "description: \"A made-up mutator.\"\n"
            "tags: [\"gameplay\"]\n"
            "works-with: [\"*\"]\n"
            "layers: [\"units\"]\n"
            "entry: \"mutator.lua\"\n"
        );
        write_text(
            stop_folder / "mutator.lua",
            "function mutate(ctx)\n"
            "  return {\n"
            "end\n"
        );
        restore_mutator_picker_feed();
        load(Screen::main_menu);
        frames(2);
        close_covering();
        MutatorPickerRequest stopped;
        stopped.setup = picker::Setup::skirmish;
        picker::Chosen stop_chosen;
        stop_chosen.id = "ridge-check-stop";
        stopped.set.push_back(std::move(stop_chosen));
        require(open_mutator_picker(std::move(stopped)), "the picker did not open" + on);
        frames(1);
        const auto chip_text = [&] {
            const std::optional<AutomationControl> shown = named("oa.mutators.verdict");
            return shown ? shown->text : std::string{};
        };
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        std::string failed;
        while (std::chrono::steady_clock::now() < deadline) {
            failed = chip_text();
            if (failed.find("stopped") != std::string::npos)
                break;
            frames(1);
        }
        require(
            failed.find("stopped") != std::string::npos,
            "the chip did not read the stopped words" + on + ": " + failed
        );
        click_named("oa.mutators.cancel");
        require(picker_screen() == nullptr, "CANCEL left the picker open" + on);

        const fs::path ghost_folder = mutators_folder / "ridge-check-ghost";
        write_text(
            ghost_folder / "oamut.yaml",
            "oamut: 1\n"
            "id: ridge-check-ghost\n"
            "name: \"Ridge Check Ghost\"\n"
            "version: \"1.0\"\n"
            "author:\n"
            "  name: \"Ridge Maker\"\n"
            "description: \"A made-up mutator.\"\n"
            "tags: [\"gameplay\"]\n"
            "works-with: [\"*\"]\n"
            "layers: [\"units\"]\n"
            "entry: \"mutator.lua\"\n"
        );
        write_text(ghost_folder / "mutator.lua", "return {}\n");
        write_text(
            ghost_folder / ".oa-origin.yaml",
            "oa-origin: 1\n"
            "origin: file\n"
            "sha256: \"not-a-digest\"\n"
            "installed: \"2026-01-01\"\n"
        );
        MutatorPickerRequest both;
        both.setup = picker::Setup::skirmish;
        picker::Chosen stop_again;
        stop_again.id = "ridge-check-stop";
        both.set.push_back(std::move(stop_again));
        picker::Chosen ghost;
        ghost.id = "ridge-check-ghost";
        both.set.push_back(std::move(ghost));
        require(open_mutator_picker(std::move(both)), "the picker did not open again" + on);
        frames(1);
        const std::string chip = chip_text();
        require(
            chip.find("stopped") != std::string::npos && chip.find("Checking") == std::string::npos,
            "the chip left the stopped verdict for a check that did not start: " + chip
        );
        std::cout << "mutator picker check: a check that cannot start reads \"" << chip << "\"\n";
        click_named("oa.mutators.cancel");
        require(picker_screen() == nullptr, "CANCEL left the picker open" + on);
        load(Screen::main_menu);
        frames(1);
    }

    std::cout << "mutator picker check: the picker held at 640x480, 1280x720, 1920x1080 and "
                 "2560x1440\n";
}

} // namespace oa::app
