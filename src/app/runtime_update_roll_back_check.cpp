// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-update-roll-back: a state file written before the start, then
// confirming it, the roll-back prompt, or ROLL BACK. The confirm case also
// leaves a receipt of a failed install and checks it is shown once and
// holds its release. The guard in main does not count this run, because the
// option is a check.

#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "user_folder_state.hpp"

#include "oa/app/update/build.hpp"
#include "oa/app/update/choices.hpp"
#include "oa/app/update/plan.hpp"
#include "oa/app/update/receipt.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/kit/components_more.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace settings = oa::ui::engine_settings;
namespace dialogs = oa::ui::frontend_dialogs;

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

/// Throws when a step of the check fails.
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("update-roll-back: " + std::string(what));
}

/// Reads a state file. Nothing when it is missing or refused.
std::optional<update::State> read_state_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    std::string error;
    return update::read_state(bytes, &error);
}

/// Prints one line the native check looks for.
void say(std::string_view line) {
    std::cout << "update-roll-back: " << line << '\n';
    std::cout.flush();
}

} // namespace

int Runtime::check_update_roll_back() {
    require(options_.data_dir.has_value(), "the run has no --data-dir");
    const fs::path state_path = *options_.data_dir / "updates" / "state.json";
    const std::string version = update::this_build({}).version_text;
    const std::string& part = options_.roll_back_case;

    if (part == "guard") {
        const std::optional<update::State> state = read_state_file(state_path);
        require(state.has_value(), "the guard's state was not read");
        require(!state->confirmed, "the guard's state was already confirmed");
        require(state->failed_starts == 2, "the guard's count is not still 2");
        require(state->installed_version == version, "the guard's version is not this build");
        say("guard left 2");
        return 0;
    }

    preference_values_[std::string(settings::key::language)] = "en";
    preference_values_[std::string(oa::ui::first_run::preference_key)] =
        std::string(oa::ui::first_run::preference_done);
    preferences_dirty_ = true;
    flush_preferences();
    if (shown_language().tag != "en")
        set_language_choice("en");

    bool running = true;
    const auto deadline_from = [](std::chrono::seconds limit) {
        return std::chrono::steady_clock::now() + limit;
    };
    const auto clear_blockers = [this]() {
        dialogs::reset_dialogs();
        OaLayer& layer = oa_layer();
        for (int pass = 0; pass < 8; ++pass) {
            LayerScreen* top = layer.top();
            if (top == nullptr || top->name() == "roll-back" || top->name() == "update-previous" ||
                top->name() == "settings" || top->name() == "ready-update" ||
                top->name() == "updated-notice" || top->name() == "update-receipt")
                return;
            layer.close(top);
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
    const auto click_named = [&](std::string_view name) {
        const auto deadline = deadline_from(std::chrono::seconds{15});
        std::optional<AutomationControl> control = named(name);
        while (!control) {
            require(
                std::chrono::steady_clock::now() < deadline, std::string(name) + " is not shown"
            );
            clear_blockers();
            idle_tick();
            control = named(name);
        }
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        const auto pointer = [&](SDL_EventType type, uint8_t button) {
            SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
            dispatch_event(event, running);
            require(running, "a pointer event ended the run");
        };
        pointer(SDL_EVENT_MOUSE_MOTION, 0);
        pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        idle_tick();
    };
    const auto settle_menu = [&]() {
        const auto deadline = deadline_from(std::chrono::seconds{60});
        while (!update_menu_settled()) {
            require(std::chrono::steady_clock::now() < deadline, "the main menu did not settle");
            clear_blockers();
            if (screen_ != Screen::main_menu)
                load(Screen::main_menu);
            idle_tick();
        }
    };

    if (part == "confirm") {
        // A receipt of an install that failed and was put back, left by the helper.
        constexpr int64_t failed_release = 7;
        const fs::path receipt_path =
            *options_.data_dir / "updates" / std::string(update::receipt_file_name);
        {
            update::Receipt receipt;
            receipt.action = update::Action::install;
            receipt.outcome = update::Outcome::failed_restored;
            receipt.from_version = version;
            receipt.to_version = "9.9.9";
            receipt.release = failed_release;
            receipt.started_at = 1;
            receipt.finished_at = 2;
            receipt.steps.push_back({update::Step::verify, update::StepResult::done, {}});
            receipt.steps.push_back(
                {update::Step::swap, update::StepResult::failed, "another program had it open"}
            );
            receipt.steps.push_back({update::Step::undo, update::StepResult::done, {}});
            std::string why;
            require(update::write_receipt(receipt_path, receipt, &why), "the receipt: " + why);
        }
        update::write_scheduled(preference_values_, failed_release);
        settle_menu();
        const auto deadline = deadline_from(std::chrono::seconds{20});
        std::optional<update::State> state;
        while (true) {
            idle_tick();
            state = read_state_file(state_path);
            if (state && state->confirmed && state->failed_starts == 0 &&
                state->installed_version == version && !state->kept_version.empty())
                break;
            require(std::chrono::steady_clock::now() < deadline, "the version was not confirmed");
        }
        say("confirmed");
        {
            const auto shown = deadline_from(std::chrono::seconds{20});
            LayerScreen* screen = nullptr;
            while (screen == nullptr) {
                require(std::chrono::steady_clock::now() < shown, "the receipt was not shown");
                idle_tick();
                screen = oa_layer().find("update-receipt");
                if (screen == nullptr)
                    screen = oa_layer().waiting("update-receipt");
            }
            const kit::Question& asked = static_cast<const QuestionScreen*>(screen)->question();
            require(
                asked.title == "The update failed. " + version + " was put back.",
                "the receipt's title was " + asked.title
            );
            bool listed = false;
            for (const kit::Paragraph& paragraph : asked.paragraphs)
                if (paragraph.text.find("another program had it open") != std::string::npos)
                    listed = true;
            require(listed, "the receipt did not list the failed step");
            require(
                update::read_held(preference_values_) == failed_release,
                "the failed release was not held"
            );
            require(
                !update::read_scheduled(preference_values_).has_value(),
                "the failed release stayed scheduled"
            );
            const std::optional<update::Receipt> marked = update::load_receipt(receipt_path);
            require(marked.has_value() && marked->shown, "the receipt was not marked shown");
            click_named("oa.update-receipt.ok");
            for (int frame = 0; frame < 8; ++frame) {
                idle_tick();
                require(
                    oa_layer().find("update-receipt") == nullptr &&
                        oa_layer().waiting("update-receipt") == nullptr,
                    "the receipt showed again"
                );
            }
            say("receipt shown once");
        }
        return 0;
    }

    if (part == "prompt") {
        settle_menu();
        const auto deadline = deadline_from(std::chrono::seconds{20});
        LayerScreen* screen = nullptr;
        while (screen == nullptr) {
            require(std::chrono::steady_clock::now() < deadline, "Back on did not show");
            clear_blockers();
            idle_tick();
            screen = oa_layer().find("roll-back");
            if (screen == nullptr)
                screen = oa_layer().waiting("roll-back");
        }
        const kit::Question& asked = static_cast<const QuestionScreen*>(screen)->question();
        require(asked.title.find("Back on") != std::string::npos, "the title is not Back on");
        say("back on");
        click_named("oa.roll-back.show-the-log");
        const fs::path logs = *options_.data_dir / "logs";
        bool saw_logs = false;
        for (const fs::path& opened : user_folder_state().opened) {
            if (opened == logs)
                saw_logs = true;
        }
        require(saw_logs, "SHOW THE LOG did not open the logs folder");
        say("log shown");
        click_named("oa.roll-back.ok");
        return 0;
    }

    if (part == "settings") {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
        say("this build does not replace itself");
        return 1;
#else
        settle_menu();
        dialogs::reset_dialogs();
        {
            const auto deadline = deadline_from(std::chrono::seconds{15});
            while (engine_settings_dialog() == nullptr) {
                require(std::chrono::steady_clock::now() < deadline, "settings did not open");
                if (screen_ != Screen::main_menu)
                    load(Screen::main_menu);
                open_engine_settings_from_menu();
                if (engine_settings_dialog() == nullptr)
                    idle_tick();
            }
        }
        engine_settings_dialog()->page = settings::Page::updates;
        apply_update_download(*engine_settings_dialog());
        idle_tick();
        click_named("oa.settings.update-previous.roll-back");
        click_named("oa.update-previous.roll-back");
        require(roll_back_requested_, "ROLL BACK did not ask to leave");
        return 0;
#endif
    }

    require(false, "the roll-back case is not known");
    return 1;
}

} // namespace oa::app
