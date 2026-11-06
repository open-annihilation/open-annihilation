// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutator layer. Each machine builds the host's list on
// its own mod data, without the map, and says in its presence record what
// it built. The match mounts its own layer when it starts.

#include "oa/app/runtime.hpp"

#include "mutator_state.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/clashes.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace mutators = oa::data::mutators;
namespace mp = oa::ui::frontend_multiplayer;
namespace oamut = oa::formats::oamut;

namespace {

/// Tells whether this machine is hosting the battle room.
///
/// The role outlives the screen: the start handler runs after the battle
/// room has stopped counting as showing.
///
/// @return true when the local player hosts
bool room_hosting() {
    return mp::lobby_hosts(mp::multiplayer_lobby());
}

/// The first position whose code is not has.
///
/// @param codes one code per position
/// @return that position, or none
uint8_t first_not_has(const std::vector<uint8_t>& codes) noexcept {
    const auto has = static_cast<uint8_t>(mp::MutatorHave::has);
    for (std::size_t at = 0; at < codes.size() && at < netgame::presence_mutators_max; ++at) {
        if (codes[at] != has)
            return static_cast<uint8_t>(at);
    }
    return netgame::presence_mutator_index_none;
}

/// Tells whether an option key fits the wire.
///
/// @param key the key
/// @return true when it is 1 to 32 ascii bytes
bool wire_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > netgame::presence_mutator_options_max * 4)
        return false;
    if (key.size() > 32)
        return false;
    for (const unsigned char byte : key) {
        if (byte < 0x21 || byte > 0x7e)
            return false;
    }
    return true;
}

/// Tells whether an option value fits the wire.
///
/// @param value the value
/// @return true when it is at most 32 text bytes
bool wire_value(std::string_view value) noexcept {
    if (value.size() > 32)
        return false;
    for (const unsigned char byte : value) {
        if (byte < 0x20 || byte == 0x7f)
            return false;
    }
    return true;
}

/// The manifest option of an id, or null.
///
/// @param manifest the manifest
/// @param id the option id
/// @return the option, or null
const oamut::Option*
manifest_option(const oamut::Manifest& manifest, std::string_view id) noexcept {
    for (const oamut::Option& option : manifest.options) {
        if (option.id == id)
            return &option;
    }
    return nullptr;
}

/// Why the host's options do not fit the wire, or empty when they do.
///
/// Only a value that differs from the manifest's default is sent. More than
/// eight of those, or a key or value the wire refuses, names that mutator.
///
/// @param chosen the mutators, top first
/// @param[out] position the mutator, when the reason is not empty
/// @return the reason, or empty
std::string
options_refuse(const std::vector<mutators::ResolvedMutator>& chosen, uint8_t* position) {
    for (std::size_t at = 0; at < chosen.size() && at < netgame::presence_mutators_max; ++at) {
        const mutators::ResolvedMutator& one = chosen[at];
        std::size_t differ = 0;
        for (const mutators::LoadoutOption& option : one.options) {
            const oamut::Option* defined = manifest_option(one.manifest, option.id);
            if (defined == nullptr)
                continue;
            if (mutators::option_text(option) ==
                mutators::option_text(mutators::default_option(*defined)))
                continue;
            ++differ;
            if (differ > netgame::presence_mutator_options_max) {
                *position = static_cast<uint8_t>(at);
                return one.manifest.id + ": more than 8 options differ from their defaults";
            }
            if (!wire_key(option.id) || !wire_value(mutators::option_text(option))) {
                *position = static_cast<uint8_t>(at);
                return one.manifest.id + ": an option does not fit on the wire";
            }
        }
    }
    return {};
}

/// Writes the options that differ from the manifest into a presence field.
///
/// @param[out] field the field
/// @param chosen the mutators, top first
void fill_host_items(
    netgame::PresenceMutators& field, const std::vector<mutators::ResolvedMutator>& chosen
) {
    field.mutators.reserve(chosen.size());
    for (const mutators::ResolvedMutator& one : chosen) {
        if (field.mutators.size() >= netgame::presence_mutators_max)
            break;
        netgame::PresenceMutator item;
        item.id = one.manifest.id;
        item.version = oamut::version_text(one.manifest.version);
        std::memcpy(item.sha256_prefix.data(), one.package.data(), item.sha256_prefix.size());
        for (const mutators::LoadoutOption& option : one.options) {
            if (item.options.size() >= netgame::presence_mutator_options_max)
                break;
            const oamut::Option* defined = manifest_option(one.manifest, option.id);
            if (defined == nullptr)
                continue;
            const std::string value = mutators::option_text(option);
            if (value == mutators::option_text(mutators::default_option(*defined)))
                continue;
            if (!wire_key(option.id) || !wire_value(value))
                continue;
            netgame::PresenceMutatorOption wire;
            wire.key = option.id;
            wire.value = value;
            item.options.push_back(std::move(wire));
        }
        field.mutators.push_back(std::move(item));
    }
}

/// Reads the unit headers again.
///
/// @return the error, or empty when they were read
std::string reload_units() {
    try {
        mp::multiplayer_units_changed();
        return {};
    } catch (const std::exception& error) {
        return error.what() != nullptr ? error.what() : "the unit headers could not be read again";
    } catch (...) {
        return "the unit headers could not be read again";
    }
}

/// The latest last-write time of the Mutators folder and each named package.
///
/// @param folder the Mutators folder
/// @param assembly the host's list
/// @return that time, or the epoch when none of them could be read
std::filesystem::file_time_type mutators_stamp(
    const std::filesystem::path& folder, const netgame::PresenceLoadoutAssembly& assembly
) {
    std::filesystem::file_time_type latest{};
    std::error_code error;
    const auto root = std::filesystem::last_write_time(folder, error);
    if (!error)
        latest = root;
    for (const netgame::PresenceMutator& item : assembly.mutators) {
        error.clear();
        const auto when = std::filesystem::last_write_time(folder / item.id, error);
        if (!error && when > latest)
            latest = when;
    }
    return latest;
}

/// The clash mode a mutator plays when the player and its manifest name none.
///
/// The same preference the match start reads, so the room's digest is the
/// one that start makes.
///
/// @param values the preferences
/// @return the mode
oamut::ClashMode clash_default_of(const std::map<std::string, std::string>& values) {
    const auto stored = values.find(std::string{mutators::clash_default_preference});
    return stored == values.end() ? mutators::clash_setting_default
                                  : mutators::clash_from_setting(stored->second);
}

/// One bit per mutator: set means Add, clear means Replace.
///
/// Bits at or past the wire's list length stay clear.
///
/// @param loadout the loadout, modes already in force
/// @return the mask
uint32_t clash_mask(const mutators::Loadout& loadout) noexcept {
    uint32_t mask = 0;
    const std::size_t count = loadout.entries.size() < netgame::presence_mutators_max
                                  ? loadout.entries.size()
                                  : netgame::presence_mutators_max;
    for (std::size_t at = 0; at < count; ++at) {
        if (loadout.entries[at].clashes != oamut::ClashMode::add)
            continue;
        mask |= uint32_t{1} << static_cast<unsigned>(at);
    }
    return mask;
}

/// Writes the host's clash mode onto each resolved mutator.
///
/// A mask that names the list sets Add where its bit is set and Replace
/// where it is clear. No mask, or one the wire refuses, sets every mode to
/// Replace. The player's choice then wins over the manifest and over this
/// machine's default.
///
/// @param[in,out] resolved the mutators, top first
/// @param modes the host's mask, or nothing when the record has none
void apply_host_modes(
    std::vector<mutators::ResolvedMutator>& resolved, std::optional<uint32_t> modes
) noexcept {
    std::array<netgame::PresenceClashMode, netgame::presence_mutators_max> decoded{};
    const auto count = static_cast<uint8_t>(
        resolved.size() < netgame::presence_mutators_max ? resolved.size()
                                                         : netgame::presence_mutators_max
    );
    const bool named =
        modes.has_value() && count >= 1 &&
        netgame::presence_clash_modes(*modes, count, decoded.data()) == netgame::WireError::ok;
    for (std::size_t at = 0; at < resolved.size() && at < netgame::presence_mutators_max; ++at) {
        const bool add = named && decoded[at] == netgame::PresenceClashMode::add;
        resolved[at].clashes = add ? oamut::ClashMode::add : oamut::ClashMode::replace;
    }
}

/// The clash-mode mask on the host's current presence record.
///
/// Nothing when the record was not sent, could not be read, or carries no
/// mask. An older host leaves the mask out.
///
/// @return the mask, or nothing
std::optional<uint32_t> host_clash_modes() {
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return std::nullopt;
    const auto host = mp::lobby_host_slot(lobby);
    if (host == mp::kNoSlot)
        return std::nullopt;
    const auto bytes = mp::lobby_presence_record(lobby, static_cast<int32_t>(host));
    if (bytes.empty())
        return std::nullopt;
    try {
        netgame::PresenceRecord decoded;
        if (netgame::decode_presence(bytes.data(), bytes.size(), &decoded) !=
            netgame::WireError::ok)
            return std::nullopt;
        return decoded.mutator_modes;
    } catch (...) {
        return std::nullopt;
    }
}

/// The mutators of one run whose outcome is applied, in that order.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
/// @return those mutators
std::vector<mutators::ResolvedMutator> mutators_that_applied(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    std::vector<mutators::ResolvedMutator> kept;
    const std::size_t count =
        chosen.size() < result.mutators.size() ? chosen.size() : result.mutators.size();
    kept.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (result.mutators[index].outcome != mutators::Outcome::applied)
            continue;
        kept.push_back(chosen[index]);
    }
    return kept;
}

/// The ids a run left out, in list order.
///
/// @param chosen the mutators, in the order they were run
/// @param result the pipeline's result
/// @return those ids, separated by commas
std::string mutators_left_out(
    const std::vector<mutators::ResolvedMutator>& chosen, const mutators::PipelineResult& result
) {
    std::string omitted;
    const std::size_t count =
        chosen.size() < result.mutators.size() ? chosen.size() : result.mutators.size();
    for (std::size_t index = 0; index < count; ++index) {
        if (result.mutators[index].outcome == mutators::Outcome::applied)
            continue;
        if (!omitted.empty())
            omitted += ", ";
        omitted += result.mutators[index].id;
    }
    for (std::size_t index = count; index < chosen.size(); ++index) {
        if (!omitted.empty())
            omitted += ", ";
        omitted += chosen[index].manifest.id;
    }
    if (omitted.empty())
        omitted = "a mutator";
    return omitted;
}

/// The position in `resolved` of one mutator of the shorter list.
///
/// @param resolved the list the room was given
/// @param played the mutators the second run was given
/// @param slot the slot in `played`
/// @return that position, or none when it is not in the presence field
uint8_t played_position(
    const std::vector<mutators::ResolvedMutator>& resolved,
    const std::vector<mutators::ResolvedMutator>& played,
    std::size_t slot
) {
    if (slot >= played.size())
        return netgame::presence_mutator_index_none;
    const std::string& id = played[slot].manifest.id;
    for (std::size_t index = 0; index < resolved.size() && index < netgame::presence_mutators_max;
         ++index) {
        if (resolved[index].manifest.id == id)
            return static_cast<uint8_t>(index);
    }
    return netgame::presence_mutator_index_none;
}

/// The first position whose outcome is not applied.
///
/// @param result the pipeline's result
/// @return that position, or none when every mutator applied
uint8_t dropped_position(const mutators::PipelineResult& result) noexcept {
    for (std::size_t at = 0; at < result.mutators.size() && at < netgame::presence_mutators_max;
         ++at) {
        if (result.mutators[at].outcome != mutators::Outcome::applied)
            return static_cast<uint8_t>(at);
    }
    return netgame::presence_mutator_index_none;
}

} // namespace

/// One Open Annihilation peer and the list this machine last greeted it for.
struct GreetMark {
    uint32_t player_id = 0;
    bool saw = false;           ///< its record has carried a mutator field
    bool greeted_none = false;  ///< greeted after that field went away
    bool greeted_built = false; ///< greeted for built_for
    oa::base::sha256::Digest built_for{};
};

/// A guest resolve, kept until the digest, the folder or a second changes.
struct ResolveCache {
    oa::base::sha256::Digest digest{};
    bool valid = false;
    std::filesystem::file_time_type stamp{};
    std::chrono::steady_clock::time_point at{};
    bool ok = false;
    std::optional<uint32_t> modes{};
    std::vector<mutators::ResolvedMutator> list{};
    std::vector<uint8_t> codes{};
    oa::base::sha256::Digest wanted{};
};

struct Runtime::RoomMutatorState {
    mp::RoomMutatorPhase phase = mp::RoomMutatorPhase::none;
    bool seeded = false;
    bool mounted = false;
    bool send = false;
    bool wanted_set = false;
    bool have_modes = false;
    bool have_folded = false;
    bool assembly_known = false;
    bool over_logged = false;
    bool sync_logged = false;
    oa::base::sha256::Digest for_digest{};
    oa::base::sha256::Digest folded{};
    oa::base::sha256::Digest wanted{};
    uint32_t modes = 0;
    std::optional<uint32_t> outgoing_modes{};
    std::string built_sim{};
    std::string wanted_sim{};
    uint8_t index = netgame::presence_mutator_index_none;
    uint8_t total = 0;
    uint8_t held = 0;
    uint32_t chosen_stamp = 1;
    uint32_t wanted_stamp = 0;
    uint32_t outgoing_generation = 0;
    uint32_t seen_generation = 0;
    std::vector<mutators::ResolvedMutator> chosen{};
    std::vector<mutators::ResolvedMutator> resolved{};
    std::vector<uint8_t> codes{};
    netgame::PresenceMutators outgoing{};
    netgame::PresenceLoadoutAssembly assembly{};
    std::vector<GreetMark> greets{};
    ResolveCache resolve{};
};

void Runtime::destroy_room_mutator_state(RoomMutatorState* state) noexcept {
    delete state;
}

Runtime::RoomMutatorState& Runtime::ensure_room_mutators() {
    if (room_mutator_state_ == nullptr) {
        std::unique_ptr<RoomMutatorState, void (*)(RoomMutatorState*) noexcept> made{
            new RoomMutatorState, destroy_room_mutator_state
        };
        room_mutator_state_ = std::move(made);
    }
    return *room_mutator_state_;
}

void Runtime::set_room_mutators(std::vector<mutators::ResolvedMutator> chosen) {
    RoomMutatorState& state = ensure_room_mutators();
    state.chosen = std::move(chosen);
    state.seeded = true;
    ++state.chosen_stamp;
    state.over_logged = false;
}

std::span<const mutators::ResolvedMutator> Runtime::room_mutators() const noexcept {
    if (room_mutator_state_ == nullptr)
        return {};
    if (room_hosting())
        return room_mutator_state_->chosen;
    return room_mutator_state_->resolved;
}

std::span<const mutators::ResolvedMutator> Runtime::room_match_mutators() const {
    // The battle room clears its showing flag before the start handler runs,
    // so this answer cannot depend on that flag. Leaving the screens releases
    // the layer, and a phase other than built answers nothing.
    if (room_mutator_state_ == nullptr)
        return {};
    const RoomMutatorState& state = *room_mutator_state_;
    if (state.phase != mp::RoomMutatorPhase::built)
        return {};
    const std::string sim = simulation_hash(*this);
    if (state.built_sim != sim)
        return {};
    if (room_hosting()) {
        if (!state.wanted_set || state.for_digest != state.wanted)
            return {};
        return state.chosen;
    }
    if (state.assembly.total == 0 || state.for_digest != state.assembly.digest)
        return {};
    // A guest plays the list only while the host's current record names it,
    // so a copy kept from an earlier record or room plays nothing.
    mp::Lobby& lobby = mp::multiplayer_lobby();
    const auto host = lobby.game != nullptr ? mp::lobby_host_slot(lobby) : mp::kNoSlot;
    if (host == mp::kNoSlot)
        return {};
    const auto record = mp::lobby_presence_record(lobby, static_cast<int32_t>(host));
    const auto named = netgame::presence_mutators_state(record.data(), record.size());
    if (!named || named->digest != state.for_digest)
        return {};
    return state.resolved;
}

bool Runtime::room_followed() const noexcept {
    return room_mutator_state_ != nullptr;
}

bool Runtime::room_match_unbuilt() const {
    // See room_match_mutators: the start asks after the room has stopped showing.
    if (room_hosting()) {
        if (room_mutator_state_ == nullptr || room_mutator_state_->chosen.empty())
            return false;
        return room_match_mutators().empty();
    }
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return false;
    const auto host = mp::lobby_host_slot(lobby);
    if (host == mp::kNoSlot)
        return false;
    const auto record = mp::lobby_presence_record(lobby, static_cast<int32_t>(host));
    if (!netgame::presence_mutators_state(record.data(), record.size()))
        return false;
    return room_match_mutators().empty();
}

const netgame::PresenceMutators* Runtime::room_mutators_presence() const noexcept {
    if (room_mutator_state_ == nullptr || !room_mutator_state_->send)
        return nullptr;
    return &room_mutator_state_->outgoing;
}

std::optional<uint32_t> Runtime::room_mutator_modes() const noexcept {
    if (room_mutator_state_ == nullptr || !room_mutator_state_->send)
        return std::nullopt;
    return room_mutator_state_->outgoing_modes;
}

uint32_t Runtime::room_mutators_generation() const noexcept {
    if (room_mutator_state_ == nullptr)
        return 0;
    return room_mutator_state_->outgoing_generation;
}

void Runtime::release_room_mutators() noexcept {
    RoomMutatorState* state = room_mutator_state_.get();
    if (state == nullptr)
        return;
    // The host's list and its resolve belong to the room left; the next
    // follow copies the lobby's list again, whatever its generation.
    state->assembly = {};
    state->assembly_known = false;
    state->resolve.valid = false;
    if (state->mounted && !drop_room_layer())
        return;
    state->phase = mp::RoomMutatorPhase::none;
    state->for_digest = {};
    state->folded = {};
    state->have_folded = false;
    state->index = netgame::presence_mutator_index_none;
    state->codes.clear();
    state->total = 0;
    state->held = 0;
    state->built_sim.clear();
    state->greets.clear();
    state->sync_logged = false;
    state->resolved.clear();
    if (!state->send && !state->outgoing_modes)
        return;
    state->send = false;
    state->outgoing = {};
    state->outgoing_modes.reset();
    ++state->outgoing_generation;
}

bool Runtime::drop_room_layer() noexcept {
    RoomMutatorState* held = room_mutator_state_.get();
    if (held == nullptr || !held->mounted)
        return true;
    RoomMutatorState& state = *held;
    try {
        if (!assets_.mutator_layer_mounted()) {
            state.mounted = false;
            return true;
        }
        if (!assets_.unmount_mutator_layer())
            return false;
        state.mounted = false;
        std::cerr << "open-annihilation: mutators: room released\n";
        std::cerr.flush();
        return true;
    } catch (...) {
        return false;
    }
}

void Runtime::release_empty_room() {
    RoomMutatorState& state = ensure_room_mutators();
    if (state.phase == mp::RoomMutatorPhase::none && !state.mounted && !state.send)
        return;
    const bool reload = state.phase == mp::RoomMutatorPhase::built || state.mounted;
    if (state.mounted && !drop_room_layer())
        return;
    state.phase = mp::RoomMutatorPhase::none;
    state.for_digest = {};
    state.folded = {};
    state.have_folded = false;
    state.index = netgame::presence_mutator_index_none;
    state.codes.clear();
    state.total = 0;
    state.held = 0;
    state.built_sim.clear();
    state.resolved.clear();
    state.sync_logged = false;
    if (state.send || state.outgoing_modes) {
        state.send = false;
        state.outgoing = {};
        state.outgoing_modes.reset();
        ++state.outgoing_generation;
    }
    if (!reload)
        return;
    const std::string error = reload_units();
    if (error.empty())
        return;
    std::cerr << "open-annihilation: mutators: room failed: the unit headers could not be read "
                 "again: "
              << one_line(error) << '\n';
    std::cerr.flush();
}

Runtime::SharedMutatorLayer
Runtime::build_shared_mutator_layer(const std::vector<mutators::ResolvedMutator>& resolved) {
    SharedMutatorLayer built;
    const auto release = [this] {
        try {
            if (assets_.mutator_layer_mounted())
                (void)assets_.unmount_mutator_layer();
        } catch (...) {
        }
    };
    try {
        uint8_t option_at = netgame::presence_mutator_index_none;
        const std::string options = options_refuse(resolved, &option_at);
        if (!options.empty()) {
            built.failure = options;
            built.position = option_at;
            release();
            return built;
        }
        mutators::PipelineInput input = mutator_pipeline_input(resolved);
        // A networked room keeps the same unit-sync limit the match start
        // uses when MatchBootstrap::multiplayer is set.
        input.limits.shared = true;
        bind_planned_hack_check(input);
        mutators::PipelineResult result = mutators::run_pipeline(assets_, input);
        const std::optional<std::size_t> stopper = mutators::first_blocking(result, input.mutators);
        if (!result.refusal.empty() || stopper) {
            uint8_t position = netgame::presence_mutator_index_none;
            if (result.refusal.empty() && stopper && *stopper < netgame::presence_mutators_max)
                position = static_cast<uint8_t>(*stopper);
            built.failure = mutator_failure_text(resolved, result);
            built.position = position;
            release();
            return built;
        }
        // The same rule as the match start. The scripts' seed is the list
        // they run, and the match runs again when a mutator was left out, so
        // the room's layer and digest are that second run. The dropped
        // position stays the first run's: the field still names the mutator
        // this machine did not apply. A list that applied in full is built
        // once.
        const uint8_t dropped = dropped_position(result);
        const std::vector<mutators::ResolvedMutator> played =
            mutators_that_applied(resolved, result);
        const mutators::Loadout applied =
            mutators::applied_loadout(mutators::loadout_of(resolved, input.clash_default), result);
        if (played.size() < resolved.size()) {
            const std::string skipped = mutators_left_out(resolved, result);
            if (assets_.mutator_layer_mounted() && !assets_.unmount_mutator_layer()) {
                built.failure = "the mutator layer could not be released";
                built.position = netgame::presence_mutator_index_none;
                return built;
            }
            input = mutator_pipeline_input(played);
            input.limits.shared = true;
            bind_planned_hack_check(input);
            result = mutators::run_pipeline(assets_, input);
            if (!result.playable()) {
                uint8_t position = netgame::presence_mutator_index_none;
                const std::optional<std::size_t> again =
                    mutators::first_blocking(result, input.mutators);
                if (result.refusal.empty() && again)
                    position = played_position(resolved, played, *again);
                built.failure = mutator_failure_text(played, result);
                built.position = position;
                release();
                return built;
            }
            std::cerr << "open-annihilation: mutators: seeded again from the mutators that "
                         "applied, because "
                      << skipped << " did not apply\n";
            std::cerr.flush();
        }
        // The mode in force is the player's choice, already the host's on a
        // guest, else the manifest, else this machine's clash default. The
        // digest is the one the match start makes.
        built.layer = mutators::layer_digest(result.layer, [this](std::string_view path) {
            return assets_.read(path).bytes;
        });
        built.folded = mutators::mutator_digest(applied, built.layer);
        built.dropped = dropped;
        built.applied = true;
        return built;
    } catch (const std::exception& error) {
        built.applied = false;
        built.failure = error.what();
        built.position = netgame::presence_mutator_index_none;
        release();
        return built;
    } catch (...) {
        built.applied = false;
        built.failure = "the mutators did not apply";
        built.position = netgame::presence_mutator_index_none;
        release();
        return built;
    }
}

bool Runtime::build_room_layer(
    const std::vector<mutators::ResolvedMutator>& resolved, const oa::base::sha256::Digest& wanted
) {
    RoomMutatorState& state = ensure_room_mutators();
    // The match's layer shares the store. Leave it, and do not record a failure.
    if (!state.mounted && assets_.mutator_layer_mounted())
        return false;
    if (!drop_room_layer())
        return false;
    const auto started = std::chrono::steady_clock::now();
    const auto fail = [&](std::string reason, uint8_t position) {
        try {
            if (assets_.mutator_layer_mounted()) {
                if (!assets_.unmount_mutator_layer())
                    state.mounted = true;
                else
                    state.mounted = false;
            } else {
                state.mounted = false;
            }
        } catch (...) {
            state.mounted = true;
        }
        state.phase = mp::RoomMutatorPhase::failed;
        state.for_digest = wanted;
        state.folded = {};
        state.have_folded = false;
        state.index = position;
        state.built_sim = simulation_hash(*this);
        state.sync_logged = false;
        state.total = resolved.empty() ? static_cast<uint8_t>(1)
                                       : static_cast<uint8_t>(
                                             resolved.size() < netgame::presence_mutators_max
                                                 ? resolved.size()
                                                 : netgame::presence_mutators_max
                                         );
        state.held = state.total;
        state.codes.assign(state.total, static_cast<uint8_t>(mp::MutatorHave::has));
        std::cerr << "open-annihilation: mutators: room failed: " << one_line(std::move(reason))
                  << '\n';
        std::cerr.flush();
    };
    const SharedMutatorLayer built = build_shared_mutator_layer(resolved);
    if (!built.applied) {
        fail(built.failure, built.position);
        return true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    std::cerr << "open-annihilation: mutators: room built " << resolved.size() << " in "
              << elapsed.count() << " ms loadout " << hex_digits(wanted, 16) << " layer "
              << hex_digits(built.layer, 16) << " mutators " << hex_digits(built.folded, 16)
              << " dropped ";
    if (built.dropped == netgame::presence_mutator_index_none)
        std::cerr << "none";
    else
        std::cerr << static_cast<unsigned>(built.dropped);
    std::cerr << '\n';
    std::cerr.flush();
    state.mounted = assets_.mutator_layer_mounted();
    state.phase = mp::RoomMutatorPhase::built;
    state.for_digest = wanted;
    state.folded = built.folded;
    state.have_folded = true;
    state.index = built.dropped;
    state.built_sim = simulation_hash(*this);
    state.sync_logged = false;
    state.total = static_cast<uint8_t>(
        resolved.size() < netgame::presence_mutators_max ? resolved.size()
                                                         : netgame::presence_mutators_max
    );
    state.held = state.total;
    state.codes.assign(state.total, static_cast<uint8_t>(mp::MutatorHave::has));
    const std::string reloaded = reload_units();
    if (!reloaded.empty()) {
        fail(
            "the unit headers could not be read again: " + reloaded,
            netgame::presence_mutator_index_none
        );
        return true;
    }
    return true;
}

bool Runtime::mount_chosen_mutator_layer(std::string& failure) {
    // The match start plays nothing under --no-mutators, so there is no layer to match.
    if (options_.no_mutators || chosen_match_mutators().empty())
        return false;
    if (assets_.mutator_layer_mounted()) {
        failure = "a mutator layer is already mounted";
        return false;
    }
    const std::span<const mutators::ResolvedMutator> chosen = chosen_match_mutators();
    const SharedMutatorLayer built = build_shared_mutator_layer(
        std::vector<mutators::ResolvedMutator>(chosen.begin(), chosen.end())
    );
    if (!built.applied) {
        failure = built.failure.empty() ? std::string("the mutators did not apply") : built.failure;
        return false;
    }
    return true;
}

bool Runtime::release_chosen_mutator_layer() noexcept {
    try {
        if (!assets_.mutator_layer_mounted())
            return true;
        // The battle room's layer and a running match's are theirs to take off.
        const bool room_holds = room_mutator_state_ != nullptr && room_mutator_state_->mounted;
        if (room_holds || match_mutators() != nullptr)
            return false;
        return assets_.unmount_mutator_layer();
    } catch (...) {
        return false;
    }
}

bool Runtime::resolve_guest_loadout(
    const netgame::PresenceLoadoutAssembly& assembly,
    std::optional<uint32_t> modes,
    std::vector<mutators::ResolvedMutator>& resolved,
    std::vector<uint8_t>& codes,
    oa::base::sha256::Digest& wanted
) {
    // One listing of the Mutators folder serves every mutator of the room's list.
    const oa::data::mutator_library::Listing listing = oa::data::mutator_library::list_installed(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
    );
    resolved.clear();
    codes.assign(assembly.mutators.size(), static_cast<uint8_t>(mp::MutatorHave::has));
    bool all_has = true;
    for (std::size_t at = 0; at < assembly.mutators.size(); ++at) {
        const netgame::PresenceMutator& item = assembly.mutators[at];
        mutators::MutatorChoice choice;
        choice.target = item.id;
        choice.by_path = false;
        choice.options.reserve(item.options.size());
        for (const netgame::PresenceMutatorOption& option : item.options)
            choice.options.emplace_back(option.key, option.value);
        const std::optional<mutators::ResolvedMutator> found =
            mutators::resolve_choice(choice, listing, nullptr);
        if (!found) {
            codes[at] = static_cast<uint8_t>(mp::MutatorHave::missing);
            all_has = false;
            continue;
        }
        const bool version_differs = oamut::version_text(found->manifest.version) != item.version;
        const bool prefix_differs =
            std::memcmp(
                found->package.data(), item.sha256_prefix.data(), item.sha256_prefix.size()
            ) != 0;
        if (version_differs || prefix_differs) {
            codes[at] = static_cast<uint8_t>(mp::MutatorHave::other_release);
            all_has = false;
            continue;
        }
        resolved.push_back(*found);
    }
    if (!all_has)
        return false;
    apply_host_modes(resolved, modes);
    wanted = mutators::loadout_digest(mutators::loadout_of(resolved));
    if (wanted == assembly.digest)
        return true;
    if (!codes.empty())
        codes[0] = static_cast<uint8_t>(mp::MutatorHave::other_release);
    return false;
}

void Runtime::publish_current(bool include_items) {
    RoomMutatorState& state = ensure_room_mutators();
    if (state.phase == mp::RoomMutatorPhase::none) {
        if (!state.send && !state.outgoing_modes)
            return;
        state.send = false;
        state.outgoing = {};
        state.outgoing_modes.reset();
        ++state.outgoing_generation;
        return;
    }
    netgame::PresenceMutators field;
    if (!mp::lobby_room_mutator_field(
            state.phase,
            state.for_digest,
            state.folded,
            state.index,
            state.total,
            state.held,
            state.codes,
            &field
        )) {
        if (!state.send && !state.outgoing_modes)
            return;
        state.send = false;
        state.outgoing = {};
        state.outgoing_modes.reset();
        ++state.outgoing_generation;
        return;
    }
    if (include_items)
        fill_host_items(field, state.chosen);
    const std::optional<uint32_t> modes =
        room_hosting() && state.have_modes ? std::optional<uint32_t>{state.modes} : std::nullopt;
    if (state.send && state.outgoing == field && state.outgoing_modes == modes)
        return;
    state.outgoing = std::move(field);
    state.outgoing_modes = modes;
    state.send = true;
    ++state.outgoing_generation;
}

void Runtime::greet_room_peers() {
    if (!room_hosting())
        return;
    RoomMutatorState& state = ensure_room_mutators();
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return;
    std::vector<uint32_t> live;
    live.reserve(static_cast<std::size_t>(mp::kSlotCount));
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        if (!mp::lobby_remote_oa_player(lobby, slot))
            continue;
        const Player& player = mp::slot_player(lobby, slot);
        if (player.player_id == 0)
            continue;
        live.push_back(player.player_id);
        GreetMark* mark = nullptr;
        for (GreetMark& one : state.greets) {
            if (one.player_id == player.player_id) {
                mark = &one;
                break;
            }
        }
        if (mark == nullptr) {
            state.greets.push_back(GreetMark{});
            mark = &state.greets.back();
            mark->player_id = player.player_id;
        }
        const mp::PresenceRecords& records = lobby.presence_records;
        std::optional<netgame::PresenceMutatorsState> row;
        if (records.peer_size[slot] != 0 && records.peer_id[slot] == player.player_id)
            row = netgame::presence_mutators_state(records.peer[slot], records.peer_size[slot]);
        if (row) {
            mark->saw = true;
            mark->greeted_none = false;
            if (state.phase == mp::RoomMutatorPhase::built && row->built &&
                row->digest == state.for_digest) {
                if (!mark->greeted_built || mark->built_for != state.for_digest) {
                    mp::multiplayer_regreet(player.player_id);
                    mark->greeted_built = true;
                    mark->built_for = state.for_digest;
                }
            } else {
                mark->greeted_built = false;
            }
        } else {
            mark->greeted_built = false;
            if (state.phase == mp::RoomMutatorPhase::none && mark->saw && !mark->greeted_none) {
                mp::multiplayer_regreet(player.player_id);
                mark->greeted_none = true;
            }
        }
    }
    std::vector<GreetMark> still;
    still.reserve(live.size());
    for (const GreetMark& one : state.greets) {
        for (const uint32_t id : live) {
            if (id != one.player_id)
                continue;
            still.push_back(one);
            break;
        }
    }
    state.greets = std::move(still);
}

void Runtime::remember_wanted() {
    RoomMutatorState& state = ensure_room_mutators();
    const std::string sim = simulation_hash(*this);
    const mutators::Loadout loadout =
        mutators::loadout_of(state.chosen, clash_default_of(preference_values_));
    const oa::base::sha256::Digest digest = mutators::loadout_digest(loadout);
    const uint32_t modes = clash_mask(loadout);
    if (state.wanted_set && state.wanted_stamp == state.chosen_stamp && state.wanted_sim == sim &&
        state.wanted == digest && state.have_modes && state.modes == modes)
        return;
    state.wanted = digest;
    state.modes = modes;
    state.have_modes = true;
    state.wanted_sim = std::move(sim);
    state.wanted_stamp = state.chosen_stamp;
    state.wanted_set = true;
}

void Runtime::refresh_room_assembly() {
    RoomMutatorState& state = ensure_room_mutators();
    const uint32_t generation = mp::multiplayer_room_mutators_generation();
    if (state.assembly_known && state.seen_generation == generation)
        return;
    state.assembly_known = true;
    state.seen_generation = generation;
    netgame::PresenceLoadoutAssembly fresh;
    if (mp::multiplayer_room_mutators(&fresh))
        state.assembly = std::move(fresh);
    else
        state.assembly = {};
}

void Runtime::note_unit_sync() {
    RoomMutatorState& state = ensure_room_mutators();
    if (state.sync_logged || state.phase != mp::RoomMutatorPhase::built || !room_hosting())
        return;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.sync.peer_count <= 0 || !mp::unit_sync_complete(lobby))
        return;
    char detail[96];
    const char* diagnostic = mp::unit_sync_diagnostic(lobby, detail, sizeof detail);
    if (diagnostic == nullptr || std::string_view(diagnostic) != "OK")
        return;
    state.sync_logged = true;
    std::cerr << "multiplayer: unit sync OK\n";
    std::cerr.flush();
}

void Runtime::follow_room_mutators() {
    if (match_running())
        return;
    if (!mp::multiplayer_showing()) {
        RoomMutatorState* held = room_mutator_state_.get();
        if (held == nullptr)
            return;
        const bool reload = held->mounted || held->phase == mp::RoomMutatorPhase::built;
        held->seeded = false;
        held->chosen.clear();
        held->resolved.clear();
        ++held->chosen_stamp;
        release_room_mutators();
        if (!reload || held->mounted)
            return;
        const std::string error = reload_units();
        if (error.empty())
            return;
        std::cerr << "open-annihilation: mutators: room failed: the unit headers could not be "
                     "read again: "
                  << one_line(error) << '\n';
        std::cerr.flush();
        return;
    }

    RoomMutatorState& state = ensure_room_mutators();
    if (!room_hosting()) {
        state.seeded = false;
        if (!state.chosen.empty()) {
            state.chosen.clear();
            ++state.chosen_stamp;
        }
        refresh_room_assembly();
        if (state.assembly.total == 0) {
            release_empty_room();
            return;
        }
        // A launch that bars mutators does not build a list the host named.
        // Holding the start is safer than playing a different list.
        if (mp::lobby_mutators_barred(mp::multiplayer_lobby()) &&
            state.assembly.total <= netgame::presence_mutators_max) {
            if (state.mounted && !drop_room_layer())
                return;
            const bool already = state.phase == mp::RoomMutatorPhase::failed &&
                                 state.for_digest == state.assembly.digest;
            if (!already) {
                std::cerr << "open-annihilation: mutators: room failed: this battle plays "
                             "with none\n";
                std::cerr.flush();
                state.phase = mp::RoomMutatorPhase::failed;
                state.for_digest = state.assembly.digest;
                state.index = netgame::presence_mutator_index_none;
                state.total = static_cast<uint8_t>(state.assembly.total);
                state.held = state.total;
                state.codes.assign(
                    static_cast<std::size_t>(state.total),
                    static_cast<uint8_t>(mp::MutatorHave::has)
                );
                state.folded = {};
                state.have_folded = false;
                state.built_sim.clear();
                state.resolved.clear();
            }
            publish_current(false);
            return;
        }
        if (state.assembly.total > netgame::presence_mutators_max ||
            state.assembly.mutators.size() > state.assembly.total) {
            if (state.phase != mp::RoomMutatorPhase::failed ||
                state.for_digest != state.assembly.digest ||
                state.index != netgame::presence_mutator_index_none) {
                std::cerr << "open-annihilation: mutators: room failed: the list names more than "
                          << netgame::presence_mutators_max << " mutators\n";
                std::cerr.flush();
                state.phase = mp::RoomMutatorPhase::failed;
                state.for_digest = state.assembly.digest;
                state.index = netgame::presence_mutator_index_none;
                state.total = static_cast<uint8_t>(netgame::presence_mutators_max);
                state.held = state.total;
                state.codes.clear();
                state.folded = {};
                state.have_folded = false;
                state.built_sim.clear();
                state.resolved.clear();
            }
            publish_current(false);
            return;
        }
        if (!netgame::presence_assembly_whole(state.assembly)) {
            state.phase = mp::RoomMutatorPhase::receiving;
            state.for_digest = state.assembly.digest;
            state.index = netgame::presence_mutator_index_none;
            state.total = state.assembly.total;
            state.held = static_cast<uint8_t>(state.assembly.mutators.size());
            state.codes.clear();
            state.folded = {};
            state.have_folded = false;
            publish_current(false);
            return;
        }
        const std::string sim = simulation_hash(*this);
        const bool same = state.for_digest == state.assembly.digest && state.built_sim == sim;
        if (state.phase == mp::RoomMutatorPhase::built && same) {
            publish_current(false);
            return;
        }
        if (state.phase == mp::RoomMutatorPhase::failed && same) {
            drop_room_layer();
            publish_current(false);
            return;
        }
        const std::filesystem::path folder =
            user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
        const auto stamp = mutators_stamp(folder, state.assembly);
        const auto now = std::chrono::steady_clock::now();
        const std::optional<uint32_t> modes = host_clash_modes();
        const bool cached = state.resolve.valid && state.resolve.digest == state.assembly.digest &&
                            state.resolve.stamp == stamp && state.resolve.modes == modes &&
                            now - state.resolve.at < std::chrono::seconds(1);
        if (!cached) {
            std::cerr << "open-annihilation: mutators: room resolve "
                      << hex_digits(state.assembly.digest, 16) << '\n';
            std::cerr.flush();
            state.resolve.digest = state.assembly.digest;
            state.resolve.stamp = stamp;
            state.resolve.modes = modes;
            state.resolve.at = now;
            state.resolve.valid = true;
            state.resolve.ok = resolve_guest_loadout(
                state.assembly, modes, state.resolve.list, state.resolve.codes, state.resolve.wanted
            );
        }
        if (!state.resolve.ok) {
            if (state.mounted && state.for_digest != state.assembly.digest) {
                if (!drop_room_layer())
                    return;
                const std::string error = reload_units();
                if (!error.empty()) {
                    std::cerr << "open-annihilation: mutators: room failed: the unit headers could "
                                 "not be read again: "
                              << one_line(error) << '\n';
                    std::cerr.flush();
                }
            }
            state.phase = mp::RoomMutatorPhase::lacking;
            state.for_digest = state.assembly.digest;
            state.index = first_not_has(state.resolve.codes);
            state.total = state.assembly.total;
            state.held = state.assembly.total;
            state.codes = state.resolve.codes;
            state.folded = {};
            state.have_folded = false;
            state.resolved.clear();
            publish_current(false);
            return;
        }
        state.resolved = state.resolve.list;
        if (!(state.phase == mp::RoomMutatorPhase::built &&
              state.for_digest == state.resolve.wanted && state.built_sim == sim)) {
            if (!build_room_layer(state.resolved, state.resolve.wanted))
                return;
        }
        publish_current(false);
        return;
    }

    // A launch that bars mutators starts from an empty list. A remembered
    // set is not applied, and a later seed cannot refill it.
    if (mp::lobby_mutators_barred(mp::multiplayer_lobby())) {
        if (!state.seeded || !state.chosen.empty()) {
            state.chosen.clear();
            state.seeded = true;
            ++state.chosen_stamp;
        }
    } else if (!state.seeded) {
        const auto listed = chosen_match_mutators();
        state.chosen.assign(listed.begin(), listed.end());
        state.seeded = true;
        ++state.chosen_stamp;
    }
    if (state.chosen.empty()) {
        release_empty_room();
        greet_room_peers();
        return;
    }
    remember_wanted();
    if (state.chosen.size() > netgame::presence_mutators_max) {
        if (!state.over_logged || state.phase != mp::RoomMutatorPhase::failed ||
            state.for_digest != state.wanted) {
            state.over_logged = true;
            std::cerr << "open-annihilation: mutators: room failed: the list names more than "
                      << netgame::presence_mutators_max << " mutators\n";
            std::cerr.flush();
            if (state.mounted)
                drop_room_layer();
            state.phase = mp::RoomMutatorPhase::failed;
            state.for_digest = state.wanted;
            state.index = netgame::presence_mutator_index_none;
            state.total = static_cast<uint8_t>(netgame::presence_mutators_max);
            state.held = state.total;
            state.codes.clear();
            state.folded = {};
            state.have_folded = false;
            state.built_sim = state.wanted_sim;
        }
        publish_current(false);
        greet_room_peers();
        return;
    }
    state.over_logged = false;
    const bool same = state.for_digest == state.wanted && state.built_sim == state.wanted_sim;
    if (state.phase == mp::RoomMutatorPhase::built && same) {
        publish_current(true);
        greet_room_peers();
        note_unit_sync();
        return;
    }
    if (state.phase == mp::RoomMutatorPhase::failed && same) {
        drop_room_layer();
        publish_current(true);
        greet_room_peers();
        return;
    }
    if (!build_room_layer(state.chosen, state.wanted))
        return;
    publish_current(true);
    greet_room_peers();
    note_unit_sync();
}

} // namespace oa::app
