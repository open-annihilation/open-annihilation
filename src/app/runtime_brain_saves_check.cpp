// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-saved-brains: a scratch folder holds steady-commander. A skirmish
// seats it on place 1, runs, and is saved. The save names the brain and
// holds its state. Loading seats it with that state, and a second save's
// state says the brain started twice. A save with no brain has no account.
// A newer manifest leaves the state out. Removing the brain refuses the
// load, and the Load screen asks to load with the built-in player.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/persist/hapibank.hpp"
#include "oa/data/persist/save_brains.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/data/script_host/plain_data.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include "lua.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace brain = oa::data::brain_library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace persist = oa::data::persist;
namespace host = oa::data::script_host;
namespace brains = oa::sim::brains;
namespace dialogs = oa::ui::frontend_dialogs;
namespace first_run = oa::ui::first_run;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;

/// The computer seat the skirmish gives the brain.
constexpr uint8_t kPlayer = 1;

/// Ticks the first skirmish runs before it is saved.
constexpr int32_t kTicks = 30;

/// The brain's script. start counts its calls into the state save returns.
constexpr std::string_view kScript =
    "local brain = {}\n"
    "local starts = 0\n"
    "function brain.start(game, me, saved)\n"
    "  if type(saved) == \"table\" and type(saved.starts) == \"number\" then\n"
    "    starts = saved.starts\n"
    "  end\n"
    "  starts = starts + 1\n"
    "end\n"
    "function brain.save()\n"
    "  return { starts = starts }\n"
    "end\n"
    "return brain\n";

/// The line a load writes when a newer brain leaves a state out.
constexpr std::string_view kLeftOut =
    "open-annihilation: brains: player 1: the saved state was left out: it was saved by 1.2";

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("saved brains check: " + std::string(what));
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

/// Writes steady-commander at the version `minor` names (1.`minor`).
///
/// @param folder the brain's folder
/// @param minor the second part of the version
void write_commander(const fs::path& folder, uint16_t minor) {
    oabrain::Manifest manifest;
    manifest.format = oabrain::format_version;
    manifest.id = "steady-commander";
    manifest.name = "Steady Commander";
    manifest.version.major = 1;
    manifest.version.minor = minor;
    manifest.version.parts = 2;
    manifest.author.name = "Ridge Testers";
    manifest.description = "Counts its starts.";
    manifest.tags = {"teaching"};
    manifest.works_with.push_back(oamut::Condition{"*", {}, {}});
    oabrain::Level level;
    level.id = "firm";
    level.name = "Firm";
    level.difficulty = oabrain::Difficulty::medium;
    manifest.levels.push_back(std::move(level));
    write_text(folder / std::string(oabrain::manifest_file), oabrain::write_manifest(manifest));
    write_text(folder / std::string(oabrain::default_entry), kScript);
}

/// One save file's bank, destroyed with the guard.
struct BankGuard {
    persist::Bank bank{};

    BankGuard() { persist::bank_init(&bank); }

    ~BankGuard() { persist::bank_destroy(&bank); }

    BankGuard(const BankGuard&) = delete;
    BankGuard& operator=(const BankGuard&) = delete;
};

/// Reads a save's Brains account.
///
/// @param file the save
/// @return the account, or nothing when the save has none
std::optional<persist::SavedBrains> read_account(const fs::path& file) {
    BankGuard guard;
    persist::BankError error{};
    const persist::FileSource source = persist::stdio_file_source();
    require(
        persist::bank_read_file(
            &guard.bank,
            path_to_utf8(file).c_str(),
            persist::savegame_description,
            nullptr,
            &source,
            &error
        ),
        std::string("could not read ") + path_to_utf8(file) + ": " + error.message
    );
    return persist::save_read_brains(&guard.bank);
}

/// The question showing now, or null.
///
/// @param layer the layer
/// @return the question
const kit::Question* question_of(OaLayer& layer) {
    LayerScreen* screen = layer.find(QuestionScreen::screen_name);
    if (screen == nullptr)
        return nullptr;
    return &static_cast<const QuestionScreen*>(screen)->question();
}

/// Joins a question's paragraphs.
///
/// @param question the question
/// @return its text
std::string body_of(const kit::Question& question) {
    std::string body;
    for (const kit::Paragraph& paragraph : question.paragraphs) {
        if (!body.empty())
            body += ' ';
        body += paragraph.text;
    }
    return body;
}

/// Where a state's decoded `starts` lands. The object stays outside Lua.
struct StartsRead {
    const uint8_t* bytes{};
    std::size_t count{};
    int64_t starts{-1};
    int ok{};
};

/// Pushes a saved state and reads its starts field.
///
/// @param context a StartsRead
/// @param state the sandbox
void read_starts(void* context, lua_State* state) {
    auto* box = static_cast<StartsRead*>(context);
    host::push_plain_data(state, std::span<const uint8_t>(box->bytes, box->count));
    lua_getfield(state, -1, "starts");
    if (lua_isinteger(state, -1) != 0) {
        box->starts = static_cast<int64_t>(lua_tointeger(state, -1));
        box->ok = 1;
    }
    lua_pop(state, 2);
}

/// The state saved for one player, or null.
///
/// @param saved the account
/// @param player the player
/// @return the state, or null
const persist::SavedBrainState* state_for(const persist::SavedBrains& saved, uint8_t player) {
    for (const persist::SavedBrainState& state : saved.states)
        if (state.player == player)
            return &state;
    return nullptr;
}

/// Holds standard error for one step, then puts it back.
struct CaughtErrors {
    std::stringstream text{};
    std::streambuf* previous{};

    CaughtErrors() : previous(std::cerr.rdbuf(text.rdbuf())) {}

    ~CaughtErrors() { std::cerr.rdbuf(previous); }

    CaughtErrors(const CaughtErrors&) = delete;
    CaughtErrors& operator=(const CaughtErrors&) = delete;
};

} // namespace

void Runtime::check_saved_brains() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(static_cast<bool>(options_.preferences_file), "needs --preferences-file");
    require(!preference_path_.empty(), "the run has no preferences file");

    const fs::path brain_folder =
        user_folder_ / std::string(brain::brains_folder_name) / "steady-commander";
    write_commander(brain_folder, 2);
    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();
    const brain::InstalledBrain* installed = brain::find(installed_brains(), "steady-commander");
    require(installed != nullptr, "steady-commander is not installed");
    require(
        oamut::version_text(installed->manifest.version) == "1.2", "the installed brain is not 1.2"
    );
    require(installed->manifest.name == "Steady Commander", "the brain's name was not read");

    require(
        SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
        "the window did not take 1280x720"
    );
    apply_output_mode();

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
    const auto wait_named = [&](std::string_view name, std::string_view why) {
        for (int32_t frame = 0; frame < 30 && !named(name).has_value(); ++frame)
            frames(1);
        return require_named(name, why);
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
        frames(1);
    };
    const auto open_menu = [&] {
        load(Screen::main_menu);
        frames(2);
        close_covering();
        require(screen_ == Screen::main_menu, "the main menu did not open");
    };
    const auto play_skirmish = [&] {
        if (match_)
            leave_match();
        open_menu();
        start_benchmark_skirmish();
        require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
        for (int32_t tick = 0; tick < kTicks; ++tick)
            step_match_simulation();
    };

    brain::Seat remembered;
    remembered.place = kPlayer;
    remembered.brain = "steady-commander";
    remembered.level = "firm";
    brain::Seats memory;
    memory.seats.push_back(std::move(remembered));
    remember_brain_seats(brain::Setup::skirmish, memory);

    const fs::path saves = saves_folder();
    fs::create_directories(saves);
    const fs::path first_save = saves / "brains.sav";
    play_skirmish();
    require(
        save_match_game(
            first_save, persist::command_line_description, persist::command_line_game_id
        ),
        "the first save failed (" + status_ + ")"
    );
    const std::optional<persist::SavedBrains> first = read_account(first_save);
    require(first.has_value(), "the save has no Brains account");
    const std::optional<brain::Seats> seats = brain::parse_seats(first->seats_text);
    require(seats.has_value() && seats->seats.size() == 1, "the account's seats did not read");
    const brain::Seat& named_seat = seats->seats.front();
    require(
        named_seat.place == kPlayer && named_seat.brain == "steady-commander" &&
            named_seat.version == "1.2" && named_seat.level == "firm" &&
            named_seat.name == "Steady Commander",
        "the account does not name steady-commander 1.2 at firm"
    );
    const persist::SavedBrainState* held_state = state_for(*first, kPlayer);
    require(held_state != nullptr && !held_state->bytes.empty(), "the account holds no state");
    const std::vector<uint8_t> first_state = held_state->bytes;
    std::cout << "saved brains check: the save names steady-commander 1.2 at firm\n";

    require(load_saved_game(first_save), "the save did not load (" + status_ + ")");
    require(screen_ == Screen::match && match_ != nullptr, "the load did not enter a match");
    const BrainSeat* seated = nullptr;
    for (const BrainSeat& one : seated_brains())
        if (one.player == kPlayer)
            seated = &one;
    require(
        seated != nullptr && seated->id == "steady-commander" && seated->saved.has_value() &&
            *seated->saved == first_state,
        "the load did not seat Steady Commander with its state"
    );
    std::cout << "saved brains check: the load seated Steady Commander with its state\n";

    const fs::path second_save = saves / "brains-again.sav";
    require(
        save_match_game(
            second_save, persist::command_line_description, persist::command_line_game_id
        ),
        "the second save failed (" + status_ + ")"
    );
    const std::optional<persist::SavedBrains> second = read_account(second_save);
    require(second.has_value(), "the second save has no Brains account");
    const persist::SavedBrainState* again = state_for(*second, kPlayer);
    require(again != nullptr, "the second save holds no state");
    const std::vector<uint8_t> second_state = again->bytes;
    host::PlainDataProblem problem;
    require(
        host::check_plain_data(second_state, problem),
        "the second state is not plain data (" + host::describe(problem) + ")"
    );
    host::Sandbox* sandbox = brains::seat_sandbox(*match_, kPlayer);
    require(sandbox != nullptr, "the seated brain has no sandbox");
    StartsRead decoded;
    decoded.bytes = second_state.data();
    decoded.count = second_state.size();
    host::ScriptError error;
    require(
        sandbox->run_host("read", &decoded, read_starts, error),
        "the second state did not decode (" + host::describe(error) + ")"
    );
    require(decoded.ok == 1 && decoded.starts == 2, "the second state does not hold starts 2");
    std::cout << "saved brains check: the second save holds starts 2\n";

    remember_brain_seats(brain::Setup::skirmish, {});
    const fs::path empty_save = saves / "brains-empty.sav";
    play_skirmish();
    require(seated_brains().empty(), "a skirmish with no brain seated one");
    require(
        save_match_game(
            empty_save, persist::command_line_description, persist::command_line_game_id
        ),
        "the save with no brain failed (" + status_ + ")"
    );
    require(!read_account(empty_save).has_value(), "a save with no brain holds a Brains account");
    std::cout << "saved brains check: a save with no brain has no account\n";

    write_commander(brain_folder, 3);
    reload_installed_brains();
    const brain::InstalledBrain* newer = brain::find(installed_brains(), "steady-commander");
    require(
        newer != nullptr && oamut::version_text(newer->manifest.version) == "1.3",
        "the brain did not become 1.3"
    );
    bool newer_loaded = false;
    std::string logged;
    {
        CaughtErrors caught;
        newer_loaded = load_saved_game(first_save);
        logged = caught.text.str();
    }
    require(newer_loaded, "the newer brain did not load (" + status_ + ")");
    const BrainSeat* reseated = nullptr;
    for (const BrainSeat& one : seated_brains())
        if (one.player == kPlayer)
            reseated = &one;
    require(
        reseated != nullptr && reseated->id == "steady-commander" && !reseated->saved.has_value(),
        "the newer brain was seated with the saved state"
    );
    require(
        logged.find(kLeftOut) != std::string::npos,
        "the left-out line was not logged (" + logged + ")"
    );
    std::cout << "saved brains check: a newer brain left the saved state out\n";

    leave_match();
    require(match_ == nullptr, "leaving the match left it running");
    std::error_code remove_error;
    fs::remove_all(brain_folder, remove_error);
    require(!remove_error, "the brain folder could not be removed");
    reload_installed_brains();
    require(
        brain::find(installed_brains(), "steady-commander") == nullptr,
        "the removed brain is still installed"
    );
    require(!load_saved_game(first_save), "a missing brain loaded");
    require(match_ == nullptr, "a missing brain started a match");
    require(
        status_.find("Steady Commander") != std::string::npos &&
            status_.find("1.2") != std::string::npos,
        "the refusal does not name Steady Commander 1.2 (" + status_ + ")"
    );
    std::cout << "saved brains check: a missing brain was refused\n";

    open_menu();
    exercise_click(menu::resource_name(menu::Button::single_player));
    frames(1);
    exercise_click(entry::resource_name(entry::Button::load_game));
    frames(2);
    require(screen_ == Screen::load_game, "LOAD GAME did not open");
    const std::string dialog_path = "SAVEGAME\\brains.sav";
    const fs::path dialog_file = game_file_path(dialog_path, ui::frontend::SavePathUse::read);
    std::error_code file_error;
    require(
        fs::is_regular_file(dialog_file, file_error),
        "the load dialog's path is not the save (" + path_to_utf8(dialog_file) + ")"
    );
    require(
        !start_saved_game(dialog_path), "the missing brain loaded without asking (" + status_ + ")"
    );
    wait_named("oa.prompt.load-built-in", "the load did not ask for the built-in player");
    wait_named("oa.prompt.body", "the question has no body");
    const kit::Question* asked = question_of(oa_layer());
    require(asked != nullptr, "the question is not on the layer");
    const std::string body = body_of(*asked);
    require(
        body.find("Steady Commander") != std::string::npos,
        "the question does not name Steady Commander (" + body + ")"
    );
    const LayerScreen* question_screen = oa_layer().find(QuestionScreen::screen_name);
    require(
        question_screen != nullptr &&
            static_cast<const QuestionScreen*>(question_screen)->over() == Screen::load_game,
        "the question is not over the Load screen"
    );
    click_named("oa.prompt.back");
    require(
        match_ == nullptr && screen_ == Screen::load_game &&
            oa_layer().find(QuestionScreen::screen_name) == nullptr,
        "BACK loaded a game or left the Load screen"
    );
    std::cout << "saved brains check: BACK left no match\n";

    require(!start_saved_game(dialog_path), "the second ask loaded without a choice");
    wait_named("oa.prompt.load-built-in", "the question did not come back");
    click_named("oa.prompt.load-built-in");
    for (int32_t frame = 0; frame < 8 && screen_ != Screen::match; ++frame)
        frames(1);
    require(
        screen_ == Screen::match && match_ != nullptr,
        "LOAD WITH THE BUILT-IN PLAYER did not load the game (" + status_ + ")"
    );
    require(seated_brains().empty(), "the built-in load seated a brain");
    std::cout << "saved brains check: LOAD WITH THE BUILT-IN PLAYER seated nobody\n";
}

} // namespace oa::app
