// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Seats one brain: the package's bytes are copied, every network's digest is
// checked, each network is built, then the entry, fits and start run. A
// failure removes the seat and installs no hooks.

#include "internal.hpp"

#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/data/neural/network.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

namespace oa::sim::brains {

Seat::Seat() = default;

Seat::~Seat() {
    delete[] incoming.items;
    delete[] delivering.items;
}

void write_reason(std::array<char, 256>& reason, std::string_view text) {
    const std::size_t count = text.size() < reason_limit ? text.size() : reason_limit;
    for (std::size_t index = 0; index < count; ++index)
        reason[index] = text[index];
    reason[count] = '\0';
}

namespace {

constexpr const char* hook_names[] = {
    "fits",
    "start",
    "onTick",
    "onUnitCreated",
    "onUnitFinished",
    "onUnitDamaged",
    "onUnitDestroyed",
    "onUnitGiven",
    "onUnitSeen",
    "onUnitLost",
    "onRadarContact",
    "onRadarLost",
    "onUnitIdle",
    "onCommandDone",
    "onUnitLoaded",
    "onUnitUnloaded",
    "onPathFailed",
    "onAllianceChanged",
    "onPlayerDefeated",
    "onChat",
    "save",
    "report",
};

void write_parts(
    std::array<char, 256>& reason,
    std::string_view first,
    std::string_view second,
    std::string_view third
) {
    char buffer[256]{};
    std::size_t used = 0;
    const std::string_view pieces[] = {first, second, third};
    for (const std::string_view piece : pieces) {
        for (std::size_t index = 0; index < piece.size() && used < reason_limit; ++index)
            buffer[used++] = piece[index];
    }
    write_reason(reason, std::string_view(buffer, used));
}

bool refuse(SeatOutcome& outcome, std::string_view text) {
    outcome.result = SeatResult::refused;
    write_reason(outcome.reason, text);
    return false;
}

bool refuse_network(SeatOutcome& outcome, std::string_view id, std::string_view detail) {
    char buffer[256];
    const int id_n = id.size() > 80 ? 80 : static_cast<int>(id.size());
    const int detail_n = detail.size() > 160 ? 160 : static_cast<int>(detail.size());
    std::snprintf(
        buffer, sizeof buffer, "network %.*s: %.*s", id_n, id.data(), detail_n, detail.data()
    );
    outcome.result = SeatResult::refused;
    write_reason(outcome.reason, buffer);
    return false;
}

void copy_fit(char* dest, std::string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index)
        dest[index] = text[index];
    dest[text.size()] = '\0';
}

bool same_text(const char* text, const char* literal) {
    if (text == nullptr || literal == nullptr)
        return false;
    while (*text != '\0' && *text == *literal) {
        ++text;
        ++literal;
    }
    return *text == *literal;
}

const char* difficulty_text(int32_t difficulty) noexcept {
    if (difficulty == OA_DIFFICULTY_EASY)
        return "easy";
    if (difficulty == OA_DIFFICULTY_HARD)
        return "hard";
    return "medium";
}

const PackageFile* find_file(const SeatSpec& spec, std::string_view path) {
    for (const PackageFile& file : spec.files) {
        if (file.path == path)
            return &file;
    }
    return nullptr;
}

bool player_ready(match_runtime::Match& match, const SeatSpec& spec, SeatOutcome& outcome) {
    if (spec.player >= OA_PLAYER_COUNT)
        return refuse(outcome, "the player is outside 0 to 9");
    const oa::Player& record = match.state().game.players[spec.player];
    if (record.in_use == 0)
        return refuse(outcome, "the player is not in the game");
    if (record.status != OA_PLAYER_STATUS_COMPUTER)
        return refuse(outcome, "the player is not a computer");
    if (match.seat_hooks(spec.player).play != nullptr)
        return refuse(outcome, "the seat already has a brain");
    return true;
}

bool texts_fit(const SeatSpec& spec, SeatOutcome& outcome) {
    if (spec.id.size() > id_bytes)
        return refuse(outcome, "the brain id is longer than 64 bytes");
    if (spec.name.size() > name_bytes)
        return refuse(outcome, "the name is longer than 128 bytes");
    if (spec.version.size() > version_bytes)
        return refuse(outcome, "the version is longer than 32 bytes");
    if (spec.level.size() > level_bytes)
        return refuse(outcome, "the level is longer than 64 bytes");
    if (spec.entry.size() > path_bytes)
        return refuse(outcome, "the entry path is longer than 256 bytes");
    if (spec.options.size() > most_options)
        return refuse(outcome, "the seat has more than 16 options");
    for (const OptionValue& value : spec.options) {
        if (value.id.empty())
            return refuse(outcome, "an option has no id");
        if (value.id.size() > option_id_bytes)
            return refuse(outcome, "an option id is longer than 64 bytes");
        if (value.text.size() > option_text_bytes)
            return refuse(outcome, "an option value is longer than 64 bytes");
    }
    return true;
}

bool entry_present(const SeatSpec& spec, SeatOutcome& outcome) {
    if (find_file(spec, spec.entry) != nullptr)
        return true;
    outcome.result = SeatResult::refused;
    write_parts(outcome.reason, spec.entry, " is not among the package files", {});
    return false;
}

bool networks_match(const SeatSpec& spec, SeatOutcome& outcome) {
    std::size_t total = 0;
    for (const NetworkFile& network : spec.networks) {
        const PackageFile* file = find_file(spec, network.path);
        if (file == nullptr) {
            outcome.result = SeatResult::refused;
            write_parts(outcome.reason, network.path, " is not among the package files", {});
            return false;
        }
        if (file->bytes.size() > network_bytes_limit ||
            total > network_bytes_limit - file->bytes.size())
            return refuse(outcome, "the networks are larger than 64 MiB");
        total += file->bytes.size();
        if (base::sha256::digest_of(file->bytes) != network.sha256) {
            outcome.result = SeatResult::refused;
            write_parts(
                outcome.reason, network.path, " does not match its sha256 in oabrain.yaml", {}
            );
            return false;
        }
    }
    return true;
}

void close_seat(Brains& brains, uint8_t player) {
    brains.seats[player].reset();
}

void fail_host(
    SeatOutcome& outcome, Brains& brains, uint8_t player, const script_host::ScriptError& error
) {
    outcome.result = SeatResult::failed;
    outcome.error = error;
    if (Seat* sitting = brains.seats[player].get())
        note_prints(*sitting);
    close_seat(brains, player);
}

void fail_sandbox_host(SeatOutcome& outcome, Seat& seat, const script_host::ScriptError& error) {
    outcome.result = SeatResult::failed;
    outcome.error = error;
    note_prints(seat);
    seat.sandbox.reset();
}

void fail_sandbox_text(
    SeatOutcome& outcome,
    Seat& seat,
    std::string_view phase,
    std::string_view chunk,
    std::string_view message
) {
    outcome.result = SeatResult::failed;
    outcome.error.kind = script_host::ErrorKind::runtime;
    outcome.error.phase = phase;
    outcome.error.chunk = chunk;
    outcome.error.message = message;
    outcome.error.package_id = seat.id;
    write_reason(outcome.reason, message);
    note_prints(seat);
    seat.sandbox.reset();
}

bool copy_seat(Seat& seat, const SeatSpec& spec) {
    seat.player = spec.player;
    copy_fit(seat.id, spec.id);
    copy_fit(seat.level, spec.level);
    copy_fit(seat.difficulty, difficulty_text(spec.difficulty));
    seat.full_awareness = spec.full_awareness;
    seat.option_count = static_cast<uint8_t>(spec.options.size());
    for (std::size_t index = 0; index < spec.options.size(); ++index) {
        const OptionValue& value = spec.options[index];
        StoredOption& stored = seat.options[index];
        copy_fit(stored.id, value.id);
        stored.kind = value.kind;
        stored.toggle = value.toggle;
        stored.number = value.number;
        copy_fit(stored.text, value.text);
    }
    for (const PackageFile& file : spec.files) {
        if (!seat.sources.add(file.path, file.bytes))
            return false;
    }
    return true;
}

Brains* brains_from(match_runtime::Match& match, SeatOutcome& outcome) {
    auto& state = match.seat_state();
    if (!state) {
        Brains* created = nullptr;
        try {
            created = new Brains;
        } catch (const std::bad_alloc&) {
            outcome.result = SeatResult::failed;
            write_reason(outcome.reason, "the seat could not be stored");
            return nullptr;
        }
        state.reset(created, [](void* pointer) { delete static_cast<Brains*>(pointer); });
    }
    return static_cast<Brains*>(state.get());
}

struct LoadBox {
    Seat* seat{};
    int status{};
    const char* hook{};
};

void read_entry(void* context, lua_State* state, int results) {
    auto* box = static_cast<LoadBox*>(context);
    if (results < 1) {
        box->status = 1;
        return;
    }
    const int table = lua_gettop(state) - results + 1;
    if (lua_type(state, table) != LUA_TTABLE) {
        box->status = 1;
        return;
    }
    for (const char* name : hook_names) {
        lua_getfield(state, table, name);
        const int type = lua_type(state, -1);
        if (type != LUA_TNIL && type != LUA_TFUNCTION) {
            box->status = 2;
            box->hook = name;
            lua_pop(state, 1);
            return;
        }
        if (type == LUA_TFUNCTION && same_text(name, "fits"))
            box->seat->has_fits = true;
        if (type == LUA_TFUNCTION && same_text(name, "start"))
            box->seat->has_start = true;
        lua_pop(state, 1);
    }
    lua_pushvalue(state, table);
    box->seat->hooks_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    box->status = 0;
}

struct FitsBox {
    Seat* seat{};
    int status{};
    char reason[256]{};
};

int push_fits(void* context, lua_State* state) {
    auto* box = static_cast<FitsBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, "fits");
    lua_remove(state, -2);
    lua_getglobal(state, "game");
    return 1;
}

void read_fits(void* context, lua_State* state, int results) {
    auto* box = static_cast<FitsBox*>(context);
    if (results < 1) {
        box->status = 2;
        return;
    }
    const int value = lua_gettop(state) - results + 1;
    if (lua_type(state, value) == LUA_TBOOLEAN && lua_toboolean(state, value) != 0) {
        box->status = 0;
        return;
    }
    if (lua_type(state, value) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, value, &length);
        const std::size_t count = length < reason_limit ? length : reason_limit;
        if (text != nullptr) {
            for (std::size_t index = 0; index < count; ++index)
                box->reason[index] = text[index];
        }
        box->reason[count] = '\0';
        box->status = 1;
        return;
    }
    box->status = 2;
}

struct StartBox {
    Seat* seat{};
};

int push_start(void* context, lua_State* state) {
    auto* box = static_cast<StartBox*>(context);
    lua_rawgeti(state, LUA_REGISTRYINDEX, box->seat->hooks_ref);
    lua_getfield(state, -1, "start");
    lua_remove(state, -2);
    lua_getglobal(state, "game");
    lua_getglobal(state, "me");
    lua_pushnil(state);
    return 3;
}

bool play_turn(void* context, match_runtime::Match& match, uint8_t player) {
    return deliver_turn(context, match, player);
}

} // namespace

bool build_networks(Seat& seat, const SeatSpec& spec, SeatOutcome& outcome) {
    if (spec.networks.empty())
        return true;
    namespace neural = oa::data::neural;
    namespace oann = oa::formats::oann;
    try {
        auto held = std::make_unique<neural::NetworkSet>(neural::max_brain_network_bytes);
        std::size_t planned = 0;
        for (const NetworkFile& network : spec.networks) {
            const PackageFile* file = find_file(spec, network.path);
            if (file == nullptr) {
                outcome.result = SeatResult::refused;
                write_parts(outcome.reason, network.path, " is not among the package files", {});
                return false;
            }
            oann::Problem problem;
            const std::optional<oann::Model> model = oann::read(file->bytes, problem);
            if (!model)
                return refuse_network(outcome, network.id, problem.message);
            const std::size_t bytes = neural::Network::memory_bytes_of(*model);
            if (bytes > neural::max_brain_network_bytes ||
                planned > neural::max_brain_network_bytes - bytes) {
                uint64_t would = static_cast<uint64_t>(planned);
                const uint64_t more = static_cast<uint64_t>(bytes);
                if (would > ~uint64_t{0} - more)
                    would = ~uint64_t{0};
                else
                    would += more;
                char detail[160];
                std::snprintf(
                    detail,
                    sizeof detail,
                    "networks would take %llu bytes; a brain may hold %llu",
                    static_cast<unsigned long long>(would),
                    static_cast<unsigned long long>(neural::max_brain_network_bytes)
                );
                return refuse_network(outcome, network.id, detail);
            }
            planned += bytes;
            std::optional<neural::Network> built = neural::Network::build(*model, problem);
            if (!built)
                return refuse_network(outcome, network.id, problem.message);
            if (!held->add(network.id, std::move(*built), problem))
                return refuse_network(outcome, network.id, problem.message);
        }
        seat.networks = std::move(held);
    } catch (const std::exception&) {
        write_reason(outcome.reason, "the seat could not be stored");
        outcome.result = SeatResult::failed;
        return false;
    }
    return true;
}

bool package_acceptable(const SeatSpec& spec, SeatOutcome& outcome) {
    return texts_fit(spec, outcome) && entry_present(spec, outcome) &&
           networks_match(spec, outcome);
}

bool copy_spec_into(Seat& seat, const SeatSpec& spec) {
    return copy_seat(seat, spec);
}

bool open_and_fit(Seat& seat, const SeatSpec& spec, SeatOutcome& outcome) {
    try {
        script_host::OpenOptions options;
        options.package_id = spec.id;
        options.profile = brain_profile(seat, spec.bank);
        options.sources = seat.sources.hooks();
        uint32_t match_seed = 0;
        if (seat.match != nullptr)
            match_seed = seat.match->random_seed();
        options.random_seed = brain_random_seed(match_seed, seat.player, spec.id, spec.version);
        note_classic(seat, spec);
        script_host::ScriptError error;
        auto opened = script_host::Sandbox::open(options, error);
        if (!opened) {
            fail_sandbox_host(outcome, seat, error);
            return false;
        }
        seat.sandbox = std::move(*opened);

        LoadBox loaded;
        loaded.seat = &seat;
        loaded.status = 1;
        script_host::CallHooks load_hooks;
        load_hooks.context = &loaded;
        load_hooks.read = read_entry;
        if (!seat.sandbox->run_file(spec.entry, "load", &load_hooks, error)) {
            fail_sandbox_host(outcome, seat, error);
            return false;
        }
        note_prints(seat);
        if (loaded.status == 1) {
            std::array<char, 256> message{};
            write_parts(message, spec.entry, " must return a table of hooks", {});
            fail_sandbox_text(outcome, seat, "load", spec.entry, message.data());
            return false;
        }
        if (loaded.status == 2) {
            const char* hook = loaded.hook != nullptr ? loaded.hook : "hook";
            std::array<char, 256> prefix{};
            write_parts(prefix, spec.entry, ": ", hook);
            std::array<char, 256> message{};
            write_parts(message, prefix.data(), " must be a function", {});
            fail_sandbox_text(outcome, seat, "load", spec.entry, message.data());
            return false;
        }

        if (seat.has_fits) {
            FitsBox fits;
            fits.seat = &seat;
            fits.status = 2;
            script_host::CallHooks fit_hooks;
            fit_hooks.context = &fits;
            fit_hooks.push = push_fits;
            fit_hooks.read = read_fits;
            if (!seat.sandbox->call("fits", fit_hooks, error)) {
                fail_sandbox_host(outcome, seat, error);
                return false;
            }
            if (fits.status == 1) {
                note_prints(seat);
                outcome.result = SeatResult::does_not_fit;
                write_reason(outcome.reason, fits.reason);
                seat.sandbox.reset();
                return false;
            }
            if (fits.status != 0) {
                fail_sandbox_text(
                    outcome, seat, "fits", spec.entry, "fits must return true or a reason"
                );
                return false;
            }
            note_prints(seat);
        }
    } catch (const std::exception&) {
        write_reason(outcome.reason, "the seat could not be stored");
        outcome.result = SeatResult::failed;
        note_prints(seat);
        seat.sandbox.reset();
        return false;
    }
    return true;
}

bool brain_seated(const match_runtime::Match& match, uint8_t player) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return false;
    return match.seat_hooks(player).play == play_turn;
}

script_host::Sandbox* seat_sandbox(match_runtime::Match& match, uint8_t player) noexcept {
    if (player >= OA_PLAYER_COUNT)
        return nullptr;
    auto& state = match.seat_state();
    if (!state)
        return nullptr;
    auto* brains = static_cast<Brains*>(state.get());
    Seat* seat = brains->seats[player].get();
    if (seat == nullptr || !seat->sandbox.has_value())
        return nullptr;
    return &seat->sandbox.value();
}

SeatOutcome seat_brain(match_runtime::Match& match, const SeatSpec& spec) {
    SeatOutcome outcome;
    outcome.result = SeatResult::failed;
    if (!player_ready(match, spec, outcome) || !package_acceptable(spec, outcome))
        return outcome;

    Brains* brains = brains_from(match, outcome);
    if (brains == nullptr)
        return outcome;

    std::unique_ptr<Seat> created;
    try {
        created = std::make_unique<Seat>();
    } catch (const std::bad_alloc&) {
        write_reason(outcome.reason, "the seat could not be stored");
        return outcome;
    }
    if (!copy_spec_into(*created, spec)) {
        write_reason(outcome.reason, "the seat could not be stored");
        return outcome;
    }
    const uint8_t player = spec.player;
    created->match = &match;
    created->facts = &brains->facts;
    brains->seats[player] = std::move(created);
    Seat& seat = *brains->seats[player];
    seat.match = &match;
    const auto& computer_slot = match.computer_player_state();
    if (computer_slot) {
        const auto* computer_players =
            static_cast<const sim::ai::ComputerPlayers*>(computer_slot.get());
        seat.handed_over = computer_players->initialized != 0;
    }

    if (!build_networks(seat, spec, outcome)) {
        close_seat(*brains, player);
        return outcome;
    }

    if (!open_and_fit(seat, spec, outcome)) {
        close_seat(*brains, player);
        return outcome;
    }

    try {
        script_host::ScriptError error;
        if (seat.has_start) {
            StartBox starting;
            starting.seat = &seat;
            script_host::CallHooks start_hooks;
            start_hooks.context = &starting;
            start_hooks.push = push_start;
            if (!seat.sandbox->call("start", start_hooks, error)) {
                fail_host(outcome, *brains, player, error);
                return outcome;
            }
            note_prints(seat);
        }

        sim::ai::computer_players_set_difficulty(
            sim::ai::match_computer_players(match), player, spec.difficulty
        );
    } catch (const std::exception&) {
        write_reason(outcome.reason, "the seat could not be stored");
        outcome.result = SeatResult::failed;
        note_prints(seat);
        close_seat(*brains, player);
        return outcome;
    }

    match_runtime::SeatHooks hooks;
    hooks.context = brains->seats[player].get();
    hooks.play = play_turn;
    match.set_seat_hooks(player, hooks);
    ensure_listener(*brains, match);
    outcome.result = SeatResult::seated;
    return outcome;
}

void set_match_game_facts(match_runtime::Match& match, oa::data::brain_facts::GameFacts facts) {
    SeatOutcome outcome;
    outcome.result = SeatResult::failed;
    Brains* brains = brains_from(match, outcome);
    if (brains == nullptr)
        return;
    brains->facts = std::move(facts);
}

} // namespace oa::sim::brains
