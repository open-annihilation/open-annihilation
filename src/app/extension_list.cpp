// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The combined extension table (extension_list.hpp): each hook of it calls
// the extensions' own hooks by the rules extension.hpp gives. It is part of
// the table, so it calls them directly: what one throws passes through the
// combined hook to the engine's guarded call of it (hook_call.hpp), and the
// extensions after it are not called for that call.
#include "oa/app/extension_list.hpp"

#include "oa/app/command_line.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/hud/team_panels.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace oa::app {

// One extension of the list.
struct ListedExtension {
    const char* name{}; // RegisteredExtension::name
    Extension table{};  // what its init filled
};

// The owners of a host's entries: for each pointer-sized entry, the name of
// the extension that set it, or null while none has.
template <typename Host>
using EntryOwners = std::array<const char*, sizeof(Host) / sizeof(void*)>;

struct ExtensionListState {
    std::vector<ListedExtension> extensions; // in list order
    Extension combined{};
    // The switch handlers the extensions returned, in list order, and the
    // handler that offers each letter to them.
    std::vector<const command_line::SwitchHandler*> switch_handlers;
    command_line::SwitchHandler switch_handler{};
    // The joined usage texts, by ExtensionText.
    std::array<std::string, 5> texts;
    // The frontend's launch values, copied from the answers the combined
    // frontend_entry gives.
    std::string entry_game_name;
    std::string entry_nickname;
    // Which extension set each entry of the hosts message_hooks,
    // console_host and team_panel_host fill.
    EntryOwners<sim::messages::Hooks> message_hook_owners{};
    EntryOwners<ui::console::ConsoleHost> console_host_owners{};
    EntryOwners<ui::hud::TeamPanelHost> team_panel_host_owners{};
};

namespace {

// A host entry's name, by its offset in the host.
struct EntryName {
    std::size_t offset{};
    const char* name{};
};

constexpr EntryName kMessageHookNames[] = {
    {offsetof(sim::messages::Hooks, context), "context"},
    {offsetof(sim::messages::Hooks, play_sound), "play_sound"},
    {offsetof(sim::messages::Hooks, refresh_panel), "refresh_panel"},
    {offsetof(sim::messages::Hooks, translate), "translate"},
    {offsetof(sim::messages::Hooks, random), "random"},
    {offsetof(sim::messages::Hooks, share_chat), "share_chat"},
    {offsetof(sim::messages::Hooks, record_chat), "record_chat"},
    {offsetof(sim::messages::Hooks, game_kind), "game_kind"},
    {offsetof(sim::messages::Hooks, center_camera), "center_camera"},
    {offsetof(sim::messages::Hooks, shared_chat_line), "shared_chat_line"},
    {offsetof(sim::messages::Hooks, shared_chat_line_bytes), "shared_chat_line_bytes"},
    {offsetof(sim::messages::Hooks, share_seat_chat), "share_seat_chat"},
};

constexpr EntryName kConsoleHostNames[] = {
    {offsetof(ui::console::ConsoleHost, context), "context"},
    {offsetof(ui::console::ConsoleHost, post_message), "post_message"},
    {offsetof(ui::console::ConsoleHost, player_info_changed), "player_info_changed"},
    {offsetof(ui::console::ConsoleHost, extension_context), "extension_context"},
    {offsetof(ui::console::ConsoleHost, extend), "extend"},
};

constexpr EntryName kTeamPanelHostNames[] = {
    {offsetof(ui::hud::TeamPanelHost, context), "context"},
    {offsetof(ui::hud::TeamPanelHost, alliance_changed), "alliance_changed"},
    {offsetof(ui::hud::TeamPanelHost, setup_changed), "setup_changed"},
    {offsetof(ui::hud::TeamPanelHost, remove_player), "remove_player"},
    {offsetof(ui::hud::TeamPanelHost, game_changed), "game_changed"},
    {offsetof(ui::hud::TeamPanelHost, resources_given), "resources_given"},
    {offsetof(ui::hud::TeamPanelHost, sight_shared), "sight_shared"},
    {offsetof(ui::hud::TeamPanelHost, tournament_game), "tournament_game"},
};

/// Returns the list state a combined hook is given as its context.
///
/// @param context Extension::context of the combined table
/// @return the state
ExtensionListState& state_of(void* context) {
    return *static_cast<ExtensionListState*>(context);
}

/// Tells whether a text is null or empty.
///
/// @param text the text
/// @return true for null or ""
bool no_text(const char* text) {
    return text == nullptr || text[0] == '\0';
}

/// Calls a hook of every extension that fills it, in list order.
///
/// @param context Extension::context of the combined table
/// @param hook the hook
/// @param arguments what follows the context
template <typename Hook, typename... Arguments>
void call_every(void* context, Hook Extension::* hook, Arguments&... arguments) {
    for (const auto& extension : state_of(context).extensions)
        if (extension.table.*hook != nullptr)
            (extension.table.*hook)(extension.table.context, arguments...);
}

/// Calls a hook of every extension that fills it, in reverse list order.
///
/// @param context Extension::context of the combined table
/// @param hook the hook
/// @param arguments what follows the context
template <typename Hook, typename... Arguments>
void call_every_reversed(void* context, Hook Extension::* hook, Arguments&... arguments) {
    const auto& extensions = state_of(context).extensions;
    for (auto extension = extensions.rbegin(); extension != extensions.rend(); ++extension)
        if (extension->table.*hook != nullptr)
            (extension->table.*hook)(extension->table.context, arguments...);
}

/// Asks the extensions that fill a hook, the last in the list first, until
/// one answers true.
///
/// @param context Extension::context of the combined table
/// @param hook the hook
/// @param arguments what follows the context
/// @return true when an extension answered true
template <typename Hook, typename... Arguments>
bool first_to_take(void* context, Hook Extension::* hook, Arguments&... arguments) {
    const auto& extensions = state_of(context).extensions;
    for (auto extension = extensions.rbegin(); extension != extensions.rend(); ++extension)
        if (extension->table.*hook != nullptr &&
            (extension->table.*hook)(extension->table.context, arguments...))
            return true;
    return false;
}

/// Asks the extensions that fill a hook, the last in the list first, for
/// a text, until one gives one that `none` does not reject.
///
/// @param context Extension::context of the combined table
/// @param hook the hook
/// @param none tells the answers that count as none
/// @param arguments what follows the context
/// @return the first text taken, or null
template <typename Hook, typename... Arguments>
const char* first_text(
    void* context, Hook Extension::* hook, bool (*none)(const char*), Arguments&... arguments
) {
    const auto& extensions = state_of(context).extensions;
    for (auto extension = extensions.rbegin(); extension != extensions.rend(); ++extension) {
        if (extension->table.*hook == nullptr)
            continue;
        const char* text = (extension->table.*hook)(extension->table.context, arguments...);
        if (!none(text))
            return text;
    }
    return nullptr;
}

/// Tells whether a text is null.
///
/// @param text the text
/// @return true for null
bool null_text(const char* text) {
    return text == nullptr;
}

/// Calls a hook that fills a host, in list order, holding each entry of the
/// host to one extension.
///
/// Throws std::runtime_error naming both extensions when an extension
/// changes an entry that another has set, in this call or an earlier one.
///
/// @param context Extension::context of the combined table
/// @param hook the hook
/// @param owners which extension set each entry
/// @param names the names of the entries a message names
/// @param what the host, as a message names it
/// @param[in,out] runtime the running app
/// @param[in,out] host the host being filled
template <typename Host, std::size_t NameCount>
void fill_host(
    void* context,
    void (*Extension::*hook)(void*, Runtime&, Host&),
    EntryOwners<Host>& owners,
    const EntryName (&names)[NameCount],
    const char* what,
    Runtime& runtime,
    Host& host
) {
    static_assert(std::is_trivially_copyable_v<Host> && sizeof(Host) % sizeof(void*) == 0);
    constexpr std::size_t kEntries = sizeof(Host) / sizeof(void*);
    for (const auto& extension : state_of(context).extensions) {
        if (extension.table.*hook == nullptr)
            continue;
        std::array<uintptr_t, kEntries> before{};
        std::memcpy(before.data(), &host, sizeof host);
        (extension.table.*hook)(extension.table.context, runtime, host);
        std::array<uintptr_t, kEntries> after{};
        std::memcpy(after.data(), &host, sizeof host);
        for (std::size_t entry = 0; entry < kEntries; ++entry) {
            if (before[entry] == after[entry])
                continue;
            const char* owner = owners[entry];
            if (owner != nullptr && std::strcmp(owner, extension.name) != 0) {
                std::string name = "an entry";
                for (const auto& known : names)
                    if (known.offset == entry * sizeof(void*))
                        name = known.name;
                throw std::runtime_error(
                    std::string("the extensions ") + owner + " and " + extension.name +
                    " both set " + name + " of " + what + ", which one extension at most may set"
                );
            }
            owners[entry] = extension.name;
        }
    }
}

/// Offers a switch letter to the extensions' handlers, the last in the list
/// first, until one takes it (command_line::SwitchHandler::take).
///
/// @param context the list state
/// @param letter the switch letter, in lower case
/// @param arguments what follows the letter
/// @param[out] effects switch effects of the handler that took it
/// @return nonzero when a handler took the letter
int take_switch(
    void* context, char letter, const command_line::SwitchArguments* arguments, uint32_t* effects
) {
    const auto& handlers = state_of(context).switch_handlers;
    for (auto handler = handlers.rbegin(); handler != handlers.rend(); ++handler)
        if ((*handler)->take != nullptr &&
            (*handler)->take((*handler)->context, letter, arguments, effects) != 0)
            return 1;
    return 0;
}

/// Resets every extension's handler, in list order (command_line::SwitchHandler::reset).
///
/// @param context the list state
void reset_switches(void* context) {
    for (const auto* handler : state_of(context).switch_handlers)
        if (handler->reset != nullptr)
            handler->reset(handler->context);
}

/// Sets a hook of the combined table when an extension fills it.
///
/// @param[in,out] state the list state
/// @param hook the hook
/// @param combined the combined hook
template <typename Hook>
void combine(
    ExtensionListState& state, Hook Extension::* hook, std::type_identity_t<Hook> combined
) {
    for (const auto& extension : state.extensions) {
        if (extension.table.*hook != nullptr) {
            state.combined.*hook = combined;
            return;
        }
    }
}

/// Returns the one extension that fills a hook only one may fill.
///
/// Throws std::runtime_error naming both when two fill it.
///
/// @param state the list state
/// @param hook the hook
/// @param what the hook's name
/// @return the extension's index in the list, or the list's size when none fills it
template <typename Hook>
std::size_t only_owner(const ExtensionListState& state, Hook Extension::* hook, const char* what) {
    std::size_t owner = state.extensions.size();
    for (std::size_t index = 0; index < state.extensions.size(); ++index) {
        if (state.extensions[index].table.*hook == nullptr)
            continue;
        if (owner != state.extensions.size())
            throw std::runtime_error(
                std::string("the extensions ") + state.extensions[owner].name + " and " +
                state.extensions[index].name + " both fill " + what +
                ", which one extension at most may fill"
            );
        owner = index;
    }
    return owner;
}

/// Fills the combined table's hooks by the rules extension.hpp gives.
///
/// @param[in,out] state the list state, its extensions filled
void combine_hooks(ExtensionListState& state) {
    state.combined.context = &state;
    combine(
        state,
        &Extension::take_option,
        [](void* context, const char* name, const OptionValues& values, uint32_t& effects) {
            const auto& extensions = state_of(context).extensions;
            for (auto extension = extensions.rbegin(); extension != extensions.rend();
                 ++extension) {
                if (extension->table.take_option == nullptr)
                    continue;
                uint32_t taken_effects = 0;
                if (extension->table.take_option(
                        extension->table.context, name, values, taken_effects
                    )) {
                    effects = taken_effects;
                    return true;
                }
            }
            return false;
        }
    );
    combine(state, &Extension::check_options, [](void* context) {
        call_every(context, &Extension::check_options);
    });
    combine(
        state, &Extension::switch_handler, [](void* context) -> const command_line::SwitchHandler* {
            auto& list = state_of(context);
            list.switch_handlers.clear();
            for (const auto& extension : list.extensions) {
                if (extension.table.switch_handler == nullptr)
                    continue;
                if (const auto* handler = extension.table.switch_handler(extension.table.context))
                    list.switch_handlers.push_back(handler);
            }
            if (list.switch_handlers.empty())
                return nullptr;
            list.switch_handler = {&list, take_switch, reset_switches};
            return &list.switch_handler;
        }
    );
    combine(state, &Extension::text, [](void* context, ExtensionText which) -> const char* {
        if (which == ExtensionText::usage_note || which == ExtensionText::register_switch)
            return first_text(context, &Extension::text, null_text, which);
        auto& list = state_of(context);
        auto& joined = list.texts.at(static_cast<std::size_t>(which));
        joined.clear();
        bool any = false;
        for (const auto& extension : list.extensions) {
            if (extension.table.text == nullptr)
                continue;
            if (const char* text = extension.table.text(extension.table.context, which)) {
                joined += text;
                any = true;
            }
        }
        return any ? joined.c_str() : nullptr;
    });
    combine(state, &Extension::startup, [](void* context, Runtime& runtime) {
        call_every(context, &Extension::startup, runtime);
    });
    combine(state, &Extension::register_screens, [](void* context, ScreenRegistry* registry) {
        call_every(context, &Extension::register_screens, registry);
    });
    combine(
        state, &Extension::register_settings_pages, [](void* context, SettingsRegistry* registry) {
            call_every(context, &Extension::register_settings_pages, registry);
        }
    );
    combine(state, &Extension::ready, [](void* context, Runtime& runtime) {
        call_every(context, &Extension::ready, runtime);
    });
    combine(state, &Extension::frontend_entry, [](void* context, FrontendEntry& entry) {
        auto& list = state_of(context);
        const char* game_name = nullptr;
        const char* nickname = nullptr;
        for (auto extension = list.extensions.rbegin(); extension != list.extensions.rend();
             ++extension) {
            if (extension->table.frontend_entry == nullptr)
                continue;
            FrontendEntry given{};
            extension->table.frontend_entry(extension->table.context, given);
            if (game_name == nullptr && !no_text(given.game_name)) {
                list.entry_game_name = given.game_name;
                game_name = list.entry_game_name.c_str();
            }
            if (nickname == nullptr && !no_text(given.nickname)) {
                list.entry_nickname = given.nickname;
                nickname = list.entry_nickname.c_str();
            }
        }
        entry.game_name = game_name;
        entry.nickname = nickname;
    });
    if (const auto owner = only_owner(state, &Extension::frontend_states, "frontend_states");
        owner < state.extensions.size())
        state.combined.frontend_states = [](void* context,
                                            ui::frontend_state::StateHandler& handler) {
            call_every(context, &Extension::frontend_states, handler);
        };
    combine(
        state,
        &Extension::run_mode,
        [](void* context, Runtime& runtime, RunPhase phase, int& exit_code) {
            return first_to_take(context, &Extension::run_mode, runtime, phase, exit_code);
        }
    );
    combine(state, &Extension::start_scene, [](void* context, Runtime& runtime) {
        return first_to_take(context, &Extension::start_scene, runtime);
    });
    combine(state, &Extension::shutdown, [](void* context, Runtime& runtime) {
        call_every_reversed(context, &Extension::shutdown, runtime);
    });
    combine(state, &Extension::select_multiplayer, [](void* context, Runtime& runtime) {
        const auto& extensions = state_of(context).extensions;
        for (auto extension = extensions.rbegin(); extension != extensions.rend(); ++extension) {
            if (extension->table.select_multiplayer == nullptr)
                continue;
            const auto answer =
                extension->table.select_multiplayer(extension->table.context, runtime);
            if (answer == MultiplayerSelection::frontend || answer == MultiplayerSelection::taken)
                return answer;
        }
        return MultiplayerSelection::unavailable;
    });
    if (const auto owner = only_owner(state, &Extension::frontend_game, "frontend_game");
        owner < state.extensions.size())
        state.combined.frontend_game = [](void* context) -> oa::Game* {
            for (const auto& extension : state_of(context).extensions)
                if (extension.table.frontend_game != nullptr)
                    return extension.table.frontend_game(extension.table.context);
            return nullptr;
        };
    combine(state, &Extension::keep_stored_password, [](void* context) {
        bool keep = false;
        for (const auto& extension : state_of(context).extensions)
            if (extension.table.keep_stored_password != nullptr &&
                extension.table.keep_stored_password(extension.table.context))
                keep = true;
        return keep;
    });
    combine(state, &Extension::check_multiplayer_menu, [](void* context, Runtime& runtime) {
        call_every(context, &Extension::check_multiplayer_menu, runtime);
    });
    combine(state, &Extension::state, [](void* context, const Runtime& runtime) {
        uint32_t bits = 0;
        for (const auto& extension : state_of(context).extensions)
            if (extension.table.state != nullptr)
                bits |= extension.table.state(extension.table.context, runtime);
        return bits;
    });
    combine(state, &Extension::frame, [](void* context, Runtime& runtime, FrameStage stage) {
        call_every(context, &Extension::frame, runtime, stage);
    });
    combine(state, &Extension::simulation_step, [](void* context, Runtime& runtime) {
        return first_to_take(context, &Extension::simulation_step, runtime);
    });
    combine(state, &Extension::outcome_ready, [](void* context, Runtime& runtime) {
        bool ready = true;
        for (const auto& extension : state_of(context).extensions)
            if (extension.table.outcome_ready != nullptr &&
                !extension.table.outcome_ready(extension.table.context, runtime))
                ready = false;
        return ready;
    });
    combine(state, &Extension::match_game, [](void* context, oa::Game& game) {
        call_every(context, &Extension::match_game, game);
    });
    combine(state, &Extension::match_event, [](void* context, Runtime& runtime, MatchEvent event) {
        call_every(context, &Extension::match_event, runtime, event);
    });
    combine(state, &Extension::disconnect_text, [](void* context, uint8_t reason) {
        return first_text(context, &Extension::disconnect_text, null_text, reason);
    });
    combine(
        state,
        &Extension::give_resources,
        [](void* context, Runtime& runtime, uint8_t from, uint8_t to, float amount, bool metal) {
            return first_to_take(
                context, &Extension::give_resources, runtime, from, to, amount, metal
            );
        }
    );
    combine(
        state,
        &Extension::message_hooks,
        [](void* context, Runtime& runtime, sim::messages::Hooks& hooks) {
            fill_host(
                context,
                &Extension::message_hooks,
                state_of(context).message_hook_owners,
                kMessageHookNames,
                "the message log's hooks",
                runtime,
                hooks
            );
        }
    );
    combine(
        state,
        &Extension::player_gone,
        [](void* context, Runtime& runtime, oa::World& world, const oa::Player& player) {
            return first_to_take(context, &Extension::player_gone, runtime, world, player);
        }
    );
    combine(
        state,
        &Extension::console_host,
        [](void* context, Runtime& runtime, ui::console::ConsoleHost& host) {
            fill_host(
                context,
                &Extension::console_host,
                state_of(context).console_host_owners,
                kConsoleHostNames,
                "the console's host",
                runtime,
                host
            );
        }
    );
    combine(
        state,
        &Extension::check_console,
        [](void* context,
           Runtime& runtime,
           void (*enter_line)(void* user, const char* line),
           void* user) {
            call_every(context, &Extension::check_console, runtime, enter_line, user);
        }
    );
    combine(
        state,
        &Extension::draw_loading,
        [](void* context,
           Runtime& runtime,
           oa::Surface& target,
           const oa::present::GafSprites* font) {
            call_every(context, &Extension::draw_loading, runtime, target, font);
        }
    );
    combine(state, &Extension::draw_match_hud, [](void* context, Runtime& runtime) {
        call_every(context, &Extension::draw_match_hud, runtime);
    });
    combine(
        state,
        &Extension::draw_match_overlay,
        [](void* context, Runtime& runtime, const MatchOverlay& overlay) {
            call_every(context, &Extension::draw_match_overlay, runtime, overlay);
        }
    );
    combine(state, &Extension::pause_changed, [](void* context, Runtime& runtime, bool paused) {
        call_every(context, &Extension::pause_changed, runtime, paused);
    });
    combine(
        state,
        &Extension::load_progress,
        [](void* context, Runtime& runtime, const uint8_t* rows, size_t row_count) {
            call_every(context, &Extension::load_progress, runtime, rows, row_count);
        }
    );
    combine(
        state,
        &Extension::team_panel_host,
        [](void* context, Runtime& runtime, ui::hud::TeamPanelHost& host) {
            fill_host(
                context,
                &Extension::team_panel_host,
                state_of(context).team_panel_host_owners,
                kTeamPanelHostNames,
                "the team panels' host",
                runtime,
                host
            );
        }
    );
    combine(state, &Extension::close_requested, [](void* context, Runtime& runtime) {
        return first_to_take(context, &Extension::close_requested, runtime);
    });
    combine(state, &Extension::return_label, [](void* context) {
        return first_text(context, &Extension::return_label, no_text);
    });
    combine(state, &Extension::speed_changed, [](void* context, Runtime& runtime, uint16_t speed) {
        call_every(context, &Extension::speed_changed, runtime, speed);
    });
    combine(state, &Extension::app_mode_set, [](void* context, Runtime& runtime, int32_t mode) {
        call_every(context, &Extension::app_mode_set, runtime, mode);
    });
    combine(
        state,
        &Extension::open_recording,
        [](void* context,
           Runtime& runtime,
           const RecordingInput& input,
           ReplayHooks& replay,
           RecordingInfo& info) {
            const auto& extensions = state_of(context).extensions;
            for (auto extension = extensions.rbegin(); extension != extensions.rend();
                 ++extension) {
                if (extension->table.open_recording == nullptr)
                    continue;
                // Each extension starts from zero, whatever one that
                // declined wrote; the caller's are filled only on a take.
                ReplayHooks opened{};
                RecordingInfo opened_info{};
                if (extension->table.open_recording(
                        extension->table.context, runtime, input, opened, opened_info
                    )) {
                    replay = opened;
                    info = opened_info;
                    return true;
                }
            }
            return false;
        }
    );
    combine(state, &Extension::release_runtime, [](void* context, Runtime& runtime) {
        call_every_reversed(context, &Extension::release_runtime, runtime);
    });
}

} // namespace

ExtensionList::ExtensionList(std::span<const RegisteredExtension> extensions)
    : state_(std::make_unique<ExtensionListState>()) {
    state_->extensions.reserve(extensions.size());
    for (const auto& extension : extensions) {
        ListedExtension listed{extension.name, {}};
        extension.init(&listed.table);
        state_->extensions.push_back(listed);
    }
    combine_hooks(*state_);
}

ExtensionList::~ExtensionList() = default;

const Extension& ExtensionList::combined() const noexcept {
    return state_->combined;
}

} // namespace oa::app
