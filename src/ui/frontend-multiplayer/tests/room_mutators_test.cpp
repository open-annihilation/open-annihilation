// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's mutator gate, the host's list parts and the unit-sync restart.
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;

namespace {

std::atomic<int> g_allocations{0};
std::atomic<bool> g_count_allocations{false};

void note_allocation() noexcept {
    if (g_count_allocations.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

// GCC 11 and later inline these replacements into the standard allocator,
// then see free() taking a pointer that came from operator new, and call it a
// mismatch. Each operator here takes its block from malloc or posix_memalign
// and gives it back to free, so the pair matches.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
    note_allocation();
    if (size == 0)
        size = 1;
    if (void* block = std::malloc(size))
        return block;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    note_allocation();
    if (size == 0)
        size = 1;
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    if (void* block = _aligned_malloc(size, align == 0 ? 1 : align))
        return block;
#else
    void* block = nullptr;
    const auto used = align < sizeof(void*) ? sizeof(void*) : align;
    if (posix_memalign(&block, used, size) == 0)
        return block;
#endif
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept {
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    std::free(block);
}

void operator delete(void* block, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(block);
#else
    std::free(block);
#endif
}

void operator delete(void* block, std::size_t, std::align_val_t) noexcept {
    ::operator delete(block, std::align_val_t{0});
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete[](void* block) noexcept {
    ::operator delete(block);
}

void operator delete[](void* block, std::size_t) noexcept {
    ::operator delete(block);
}

void operator delete[](void* block, std::align_val_t alignment) noexcept {
    ::operator delete(block, alignment);
}

void operator delete[](void* block, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(block, alignment);
}

// The nothrow forms take their blocks the same way, so a nothrow new (the
// buffer std::stable_sort borrows) and the delete that returns it match.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    note_allocation();
    if (size == 0)
        size = 1;
    return std::malloc(size);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ::operator new(size, alignment, std::nothrow);
}

void operator delete(void* block, const std::nothrow_t&) noexcept {
    ::operator delete(block);
}

void operator delete(void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(block, alignment);
}

void operator delete[](void* block, const std::nothrow_t&) noexcept {
    ::operator delete(block);
}

void operator delete[](void* block, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(block, alignment);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (condition)
        return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++g_failures;
}

bool map_selected(void*) {
    return true;
}

const char* map_name(void*) {
    return "Ridge";
}

/// A battle room: the local player in slot 0, another machine in slot 1.
struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::Panel panel{};

    /// @param hosting true seats the local player as the host
    explicit Room(bool hosting) {
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        lobby.maps.selected = map_selected;
        lobby.maps.name = map_name;
        const uint32_t local_id = hosting ? 0x100u : 0x200u;
        const uint32_t other_id = hosting ? 0x200u : 0x100u;
        game->players[0].player_id = local_id;
        mp::lobby_seat_local(lobby, 0, hosting, hosting ? "Host" : "Guest");
        seat(1, other_id, true, !hosting);
    }

    /// Seats a remote player.
    ///
    /// @param player_slot the slot
    /// @param id the player id
    /// @param oa whether the setup block carries the engine signature
    /// @param host_role whether the block says host
    void seat(int32_t player_slot, uint32_t id, bool oa, bool host_role) {
        auto& player = mp::slot_player(lobby, player_slot);
        player.in_use = 1;
        player.player_id = id;
        player.index = static_cast<uint8_t>(player_slot);
        std::snprintf(player.name, sizeof player.name, "P%u", id);
        mp::slot_set_status(lobby, player, mp::kSlotRemote);
        auto* info = mp::slot_info(lobby, player_slot);
        info->state = mp::kInfoStatePlaying;
        info->net_id = id;
        if (host_role)
            info->role = static_cast<uint8_t>(info->role | mp::kRoleHost);
        if (oa) {
            info->engine_signature[0] = ng::engine_signature_first;
            info->engine_signature[1] = ng::engine_signature_second;
        } else {
            info->engine_signature[0] = 0;
            info->engine_signature[1] = 0;
        }
        lobby.presence_records.block_player[player_slot] = id;
    }

    /// Seats a computer player of this machine.
    ///
    /// @param player_slot the slot
    void seat_computer(int32_t player_slot) {
        auto& player = mp::slot_player(lobby, player_slot);
        player.in_use = 1;
        player.player_id = 0x201;
        player.index = static_cast<uint8_t>(player_slot);
        mp::slot_set_status(lobby, player, mp::kSlotComputer);
    }
};

/// A mutator field with the room's digest and layer, and no items yet.
///
/// @param built the built flag
/// @param failed the failed flag
/// @param index the mutator those flags name
/// @param total how many mutators the list names
/// @param held how many the sender holds
/// @return the field
ng::PresenceMutators field_of(bool built, bool failed, uint8_t index, uint8_t total, uint8_t held) {
    ng::PresenceMutators field;
    field.built = built;
    field.failed = failed;
    field.index = index;
    field.total = total;
    field.held = held;
    field.digest.fill(0x11);
    if (built)
        field.layer_digest.fill(0x22);
    return field;
}

/// Encodes a presence record that carries a mutator field and, optionally, a sim hash.
///
/// @param field the mutator field
/// @param sim the sim hash; null omits it
/// @param mark the byte the sim hash is filled with
/// @param capacity the most bytes the record may take
/// @return the record; empty when it does not encode
std::vector<uint8_t>
encode_field(const ng::PresenceMutators& field, bool sim, uint8_t mark, std::size_t capacity) {
    ng::PresenceRecord record;
    record.mutators = field;
    if (sim) {
        record.sim_hash = ng::PresenceDigest{};
        record.sim_hash->fill(mark);
    }
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    if (ng::encode_presence(record, bytes.data(), capacity, &written) != ng::WireError::ok)
        return {};
    bytes.resize(written);
    return bytes;
}

/// Stores a record as the one this machine sends.
void put_local(mp::Lobby& lobby, const std::vector<uint8_t>& bytes) {
    std::memcpy(lobby.presence_records.local, bytes.data(), bytes.size());
    lobby.presence_records.local_size = static_cast<uint16_t>(bytes.size());
}

/// Stores a record as the one a remote slot sent.
void put_peer(
    mp::Lobby& lobby, int32_t player_slot, uint32_t id, const std::vector<uint8_t>& bytes
) {
    std::memcpy(lobby.presence_records.peer[player_slot], bytes.data(), bytes.size());
    lobby.presence_records.peer_size[player_slot] = static_cast<uint16_t>(bytes.size());
    lobby.presence_records.peer_id[player_slot] = id;
}

/// The host's built list of two mutators, every position installed.
ng::PresenceMutators host_ready() {
    auto field = field_of(true, false, ng::presence_mutator_index_none, 2, 2);
    return field;
}

/// Installs the host's record on the local player and returns the room.
Room hosted(const ng::PresenceMutators& field, bool sim = false, uint8_t mark = 0x5a) {
    Room room(true);
    put_local(room.lobby, encode_field(field, sim, mark, ng::presence_record_max_bytes));
    return room;
}

void expect_hold(
    const mp::MutatorsBlock& block, mp::MutatorsHold hold, int32_t slot, int position
) {
    expect(block.hold == hold, "the hold");
    expect(block.slot == slot, "the slot the hold names");
    expect(block.position == static_cast<uint8_t>(position), "the position the hold names");
}

void test_no_field_allows_start() {
    Room room(true);
    const auto block = mp::lobby_mutators_block(room.lobby);
    expect_hold(block, mp::MutatorsHold::none, -1, ng::presence_mutator_index_none);
    expect(mp::lobby_mutators_allow_start(room.lobby), "no mutator field allows START");
}

void test_each_hold() {
    {
        auto field = field_of(false, false, ng::presence_mutator_index_none, 1, 1);
        Room room = hosted(field);
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::host_not_built, 0, ng::presence_mutator_index_none);
    }
    {
        auto field = field_of(false, true, 1, 2, 2);
        Room room = hosted(field);
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::host_failed, 0, 1);
    }
    {
        Room room = hosted(host_ready());
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::no_support, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        peer.held = 1;
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::receiving, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        ng::presence_mutator_set_state(peer.states, 1, 1);
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::lacking, 1, 1);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        peer.failed = true;
        peer.index = 0;
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::failed, 1, 0);
    }
    {
        Room room = hosted(host_ready());
        auto peer = host_ready();
        peer.built = false;
        peer.layer_digest = {};
        put_peer(room.lobby, 1, 0x200, encode_field(peer, false, 0, ng::presence_record_max_bytes));
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::building, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready(), true, 0x5a);
        auto peer = host_ready();
        peer.layer_digest.fill(0x33);
        put_peer(
            room.lobby, 1, 0x200, encode_field(peer, true, 0x5a, ng::presence_record_max_bytes)
        );
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::other_result, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready(), true, 0x5a);
        auto peer = host_ready();
        put_peer(
            room.lobby, 1, 0x200, encode_field(peer, true, 0x6b, ng::presence_record_max_bytes)
        );
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::other_sim_hash, 1, ng::presence_mutator_index_none);
    }
    {
        Room room = hosted(host_ready(), true, 0x5a);
        put_peer(
            room.lobby,
            1,
            0x200,
            encode_field(host_ready(), true, 0x5a, ng::presence_record_max_bytes)
        );
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::none, -1, ng::presence_mutator_index_none);
        expect(mp::lobby_mutators_allow_start(room.lobby), "a matching room allows START");
    }
    {
        Room room = hosted(host_ready(), true, 0x5a);
        auto peer = host_ready();
        peer.index = 1;
        peer.layer_digest.fill(0x33);
        put_peer(
            room.lobby, 1, 0x200, encode_field(peer, true, 0x5a, ng::presence_record_max_bytes)
        );
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::other_result, 1, 1);
    }
    {
        auto host = field_of(true, false, 0, 2, 2);
        Room room = hosted(host, true, 0x5a);
        auto peer = host;
        peer.index = ng::presence_mutator_index_none;
        peer.layer_digest.fill(0x33);
        put_peer(
            room.lobby, 1, 0x200, encode_field(peer, true, 0x5a, ng::presence_record_max_bytes)
        );
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::other_result, 1, 0);
    }
    {
        auto host = field_of(true, false, 1, 2, 2);
        Room room = hosted(host, true, 0x5a);
        put_peer(
            room.lobby, 1, 0x200, encode_field(host, true, 0x5a, ng::presence_record_max_bytes)
        );
        const auto block = mp::lobby_mutators_block(room.lobby);
        expect_hold(block, mp::MutatorsHold::none, -1, ng::presence_mutator_index_none);
        expect(
            mp::lobby_mutators_allow_start(room.lobby),
            "a mutator dropped on both machines does not hold START"
        );
    }
}

void test_slot_order_and_who_is_skipped() {
    Room room = hosted(host_ready());
    room.seat(3, 0x300, true, false);
    auto lacking = host_ready();
    ng::presence_mutator_set_state(lacking.states, 0, 1);
    auto failed = host_ready();
    failed.failed = true;
    failed.index = 1;
    put_peer(room.lobby, 1, 0x200, encode_field(lacking, false, 0, ng::presence_record_max_bytes));
    put_peer(room.lobby, 3, 0x300, encode_field(failed, false, 0, ng::presence_record_max_bytes));
    expect_hold(mp::lobby_mutators_block(room.lobby), mp::MutatorsHold::lacking, 1, 0);
    put_peer(room.lobby, 1, 0x200, encode_field(failed, false, 0, ng::presence_record_max_bytes));
    put_peer(room.lobby, 3, 0x300, encode_field(lacking, false, 0, ng::presence_record_max_bytes));
    expect_hold(mp::lobby_mutators_block(room.lobby), mp::MutatorsHold::failed, 1, 1);

    // A seated 3.1c player holds START while mutators are on. A computer does not.
    Room plain = hosted(host_ready());
    plain.seat(1, 0x200, false, false);
    plain.seat_computer(2);
    auto missing = host_ready();
    ng::presence_mutator_set_state(missing.states, 0, 1);
    put_peer(plain.lobby, 1, 0x200, encode_field(missing, false, 0, ng::presence_record_max_bytes));
    expect_hold(
        mp::lobby_mutators_block(plain.lobby),
        mp::MutatorsHold::classic_player,
        1,
        ng::presence_mutator_index_none
    );
    expect(!mp::lobby_mutators_differ_from_host(plain.lobby, 1), "a 3.1c row has no mutator dot");
    expect(
        !mp::lobby_mutators_differ_from_host(plain.lobby, 2), "a computer row has no mutator dot"
    );
    expect(!mp::lobby_mutators_differ_from_host(plain.lobby, 0), "the host row has no mutator dot");

    Room dotted = hosted(host_ready());
    dotted.seat(3, 0x300, true, false);
    put_peer(
        dotted.lobby, 1, 0x200, encode_field(host_ready(), false, 0, ng::presence_record_max_bytes)
    );
    put_peer(
        dotted.lobby, 3, 0x300, encode_field(missing, false, 0, ng::presence_record_max_bytes)
    );
    expect(
        mp::lobby_rules_differ_from_host(dotted.lobby, 3),
        "the dot follows the player who lacks one"
    );
    expect(!mp::lobby_rules_differ_from_host(dotted.lobby, 1), "a player who matches has no dot");
    expect(!mp::lobby_rules_differ_from_host(dotted.lobby, 0), "the host's own row has no dot");
    expect(
        mp::lobby_mutators_differ_from_host(dotted.lobby, 3), "the later slot is named on its own"
    );
}

void test_have_codes() {
    Room room = hosted(host_ready());
    auto field = field_of(true, false, ng::presence_mutator_index_none, 4, 4);
    ng::presence_mutator_set_state(field.states, 1, 1);
    ng::presence_mutator_set_state(field.states, 2, 2);
    ng::presence_mutator_set_state(field.states, 3, 3);
    put_peer(room.lobby, 1, 0x200, encode_field(field, false, 0, ng::presence_record_max_bytes));
    expect(mp::lobby_mutator_have(room.lobby, 1, 0) == mp::MutatorHave::has, "has");
    expect(mp::lobby_mutator_have(room.lobby, 1, 1) == mp::MutatorHave::missing, "missing");
    expect(
        mp::lobby_mutator_have(room.lobby, 1, 2) == mp::MutatorHave::other_release, "other release"
    );
    expect(mp::lobby_mutator_have(room.lobby, 1, 3) == mp::MutatorHave::unknown, "unknown");
    expect(mp::lobby_mutator_have(room.lobby, 1, 4) == mp::MutatorHave::unknown, "past the list");
    expect(mp::lobby_mutator_have(room.lobby, 3, 0) == mp::MutatorHave::unknown, "no record");
    room.seat_computer(2);
    put_local(room.lobby, encode_field(field, false, 0, ng::presence_record_max_bytes));
    expect(
        mp::lobby_mutator_have(room.lobby, 2, 1) == mp::MutatorHave::missing,
        "this machine's computer reads its own record"
    );
}

void test_empty_host_holds_a_guest_still_sending() {
    Room room(true);
    put_peer(
        room.lobby, 1, 0x200, encode_field(host_ready(), false, 0, ng::presence_record_max_bytes)
    );
    const auto block = mp::lobby_mutators_block(room.lobby);
    expect_hold(block, mp::MutatorsHold::receiving, 1, ng::presence_mutator_index_none);
    expect(!mp::lobby_mutators_allow_start(room.lobby), "a lingering mutator field holds START");

    Room plain(true);
    plain.seat(1, 0x200, false, false);
    put_peer(
        plain.lobby, 1, 0x200, encode_field(host_ready(), false, 0, ng::presence_record_max_bytes)
    );
    expect(mp::lobby_mutators_allow_start(plain.lobby), "a 3.1c field does not hold a plain room");
}

void test_block_allocates_nothing() {
    Room room = hosted(host_ready());
    put_peer(
        room.lobby, 1, 0x200, encode_field(host_ready(), true, 0x5a, ng::presence_record_max_bytes)
    );
    g_allocations.store(0);
    g_count_allocations.store(true);
    const auto block = mp::lobby_mutators_block(room.lobby);
    const auto again = mp::lobby_mutators_allow_start(room.lobby);
    g_count_allocations.store(false);
    expect(g_allocations.load() == 0, "the gate allocates nothing");
    expect(block.hold == mp::MutatorsHold::none && again, "the counted calls still answer");
}

/// One wide mutator, 40 bytes on the wire.
ng::PresenceMutator wide_mutator(int number) {
    ng::PresenceMutator item;
    char digits[3] = {};
    std::snprintf(digits, sizeof digits, "%02d", number);
    item.id = std::string("mutator-sample") + digits;
    item.version = "1234567890123";
    item.sha256_prefix.fill(static_cast<uint8_t>(number));
    return item;
}

/// The 32-mutator list that does not fit in the host's 533-byte source.
ng::PresenceMutators wide_loadout() {
    ng::PresenceMutators field;
    field.built = true;
    field.total = 32;
    field.held = 32;
    field.digest.fill(0x44);
    field.layer_digest.fill(0x55);
    field.mutators.reserve(32);
    for (int number = 0; number < 32; ++number)
        field.mutators.push_back(wide_mutator(number));
    return field;
}

/// A guest's report of how much of that list it holds, with no items.
std::vector<uint8_t> held_record(uint8_t held) {
    auto field = wide_loadout();
    field.mutators.clear();
    field.built = false;
    field.layer_digest = {};
    field.held = held;
    return encode_field(field, false, 0, ng::presence_record_max_bytes);
}

/// The map pack a refresh asks for. `bytes` is the encoded value's length.
struct PackAsk {
    std::size_t bytes = 0;
};

/// Grows one string toward `most`, spending `extra` bytes.
void grow_pack_text(std::string& field, std::size_t most, char mark, std::size_t& extra) {
    if (extra == 0 || field.size() >= most)
        return;
    const std::size_t add = std::min(extra, most - field.size());
    field.append(add, mark);
    extra -= add;
}

/// Returns a pack whose encoded value is PackAsk::bytes.
bool answer_pack(void* context, ng::PresenceMapPack* out) {
    const auto* ask = static_cast<const PackAsk*>(context);
    ng::PresenceMapPack pack;
    pack.key = "k";
    pack.registry = "r";
    // One-character key and registry, and empty url, name and version: 43 bytes.
    std::size_t extra = ask->bytes > 43 ? ask->bytes - 43 : 0;
    grow_pack_text(pack.descriptor_url, 255, 'u', extra);
    grow_pack_text(pack.name, 64, 'n', extra);
    grow_pack_text(pack.key, 64, 'k', extra);
    grow_pack_text(pack.version, 32, 'v', extra);
    grow_pack_text(pack.registry, 32, 'r', extra);
    *out = std::move(pack);
    return extra == 0;
}

/// Binds answer_pack and asks again on the next refresh.
void bind_pack(Room& room, PackAsk& ask) {
    room.lobby.maps.pack_context = &ask;
    room.lobby.maps.pack = answer_pack;
    room.lobby.presence_records.pack_rebound = true;
}

/// Binds the wide list as the host's source, cut at 533 bytes, and the full list for parts.
void bind_wide_source(Room& room) {
    const auto full = wide_loadout();
    room.lobby.presence_records.part_source = full;
    auto bytes = encode_field(full, false, 0, 533);
    std::memcpy(room.lobby.presence_records.source, bytes.data(), bytes.size());
    room.lobby.presence_records.source_size = static_cast<uint16_t>(bytes.size());
}

/// The part on the record the host would send, if it appended one.
std::optional<ng::PresenceMutatorPart> sent_part(Room& room) {
    mp::presence_refresh(room.lobby);
    ng::PresenceRecord decoded;
    if (ng::decode_presence(
            room.lobby.presence_records.local, room.lobby.presence_records.local_size, &decoded
        ) != ng::WireError::ok)
        return std::nullopt;
    return decoded.mutator_part;
}

void test_parts() {
    expect(wide_mutator(0).id.size() == 16, "a wide mutator's id is 16 bytes");
    Room room(true);
    bind_wide_source(room);
    expect(!sent_part(room), "a peer with no mutator field gets no part");

    put_peer(room.lobby, 1, 0x200, held_record(32));
    expect(!sent_part(room), "a peer that holds the whole list gets no part");

    put_peer(room.lobby, 1, 0x200, held_record(11));
    room.seat(3, 0x300, true, false);
    put_peer(room.lobby, 3, 0x300, held_record(4));
    const auto earliest = sent_part(room);
    expect(earliest && earliest->first == 4, "the part starts at the smallest held");

    room.lobby.presence_records.peer_size[3] = 0;
    put_peer(room.lobby, 1, 0x200, held_record(11));
    PackAsk ask;
    ask.bytes = 150;
    bind_pack(room, ask);
    const auto beside = sent_part(room);
    expect(room.lobby.presence_records.pack_size == 150, "the map pack value is 150 bytes");
    expect(
        beside && beside->first == 11 && beside->mutators.size() == 7,
        "eleven already held, then seven, beside a 150-byte map pack"
    );

    // The guest keeps each record the host refreshes. Three of them finish the list.
    Room guest(false);
    const auto take = [&](const Room& host) {
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = 0x100;
        event.size = host.lobby.presence_records.local_size;
        std::memcpy(event.data, host.lobby.presence_records.local, event.size);
        expect(mp::lobby_apply_event(guest.lobby, event), "the guest keeps the host's record");
    };
    take(room);
    expect(
        guest.lobby.presence_records.host_loadout.mutators.size() == 18,
        "the head and the first part"
    );
    put_peer(room.lobby, 1, 0x200, held_record(18));
    expect(sent_part(room) && sent_part(room)->mutators.size() == 7, "the second part is seven");
    take(room);
    expect(
        guest.lobby.presence_records.host_loadout.mutators.size() == 25,
        "twenty-five after the second part"
    );
    put_peer(room.lobby, 1, 0x200, held_record(25));
    expect(sent_part(room) && sent_part(room)->mutators.size() == 7, "the third part is seven");
    take(room);
    expect(
        ng::presence_assembly_whole(guest.lobby.presence_records.host_loadout) &&
            guest.lobby.presence_records.host_loadout.mutators.size() == 32,
        "three refreshes carry the whole 32-mutator list"
    );

    // A full map pack leaves no room until the head is shortened.
    ask.bytes = 488;
    bind_pack(room, ask);
    put_peer(room.lobby, 1, 0x200, held_record(11));
    const auto packed = sent_part(room);
    expect(room.lobby.presence_records.pack_size == 488, "the map pack value is 488 bytes");
    expect(
        packed && packed->first == 11 && !packed->mutators.empty(),
        "a full map pack still carries a part"
    );

    // A record that arrives before the host slot is known is folded once it is.
    Room late(false);
    auto* host_info = mp::slot_info(late.lobby, 1);
    host_info->role = static_cast<uint8_t>(host_info->role & ~mp::kRoleHost);
    const auto head = encode_field(host_ready(), false, 0, ng::presence_record_max_bytes);
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = 0x100;
    event.size = static_cast<uint16_t>(head.size());
    std::memcpy(event.data, head.data(), head.size());
    expect(mp::lobby_apply_event(late.lobby, event), "the record is kept before the host is known");
    expect(late.lobby.presence_records.host_loadout.total == 0, "an unknown host is not assembled");
    const uint32_t unfolded = late.lobby.presence_records.host_loadout_generation;
    host_info->role = static_cast<uint8_t>(host_info->role | mp::kRoleHost);
    mp::presence_refresh(late.lobby);
    expect(
        late.lobby.presence_records.host_loadout.total == 2 &&
            late.lobby.presence_records.host_loadout_generation == unfolded + 1,
        "the stored host record is folded when the host slot is known"
    );
}

void test_part_refused_once() {
    Room room(true);
    auto field = wide_loadout();
    auto bytes = encode_field(field, false, 0, 533);
    std::memcpy(room.lobby.presence_records.source, bytes.data(), bytes.size());
    room.lobby.presence_records.source_size = static_cast<uint16_t>(bytes.size());
    field.mutators.clear();
    room.lobby.presence_records.part_source = field;
    put_peer(room.lobby, 1, 0x200, held_record(0));
    mp::presence_refresh(room.lobby);
    expect(
        room.lobby.presence_records.part_refused_digest &&
            *room.lobby.presence_records.part_refused_digest == field.digest,
        "a part that cannot be drawn is remembered"
    );
    mp::presence_refresh(room.lobby);
    expect(
        room.lobby.presence_records.part_refused_digest &&
            *room.lobby.presence_records.part_refused_digest == field.digest,
        "the same digest is not logged again"
    );
}

/// Adds a READY0 button the battle room can grey and click.
void add_ready(mp::Panel& panel) {
    panel.count = 2;
    auto& control = panel.controls[1];
    control = {};
    std::snprintf(control.name.data(), control.name.size(), "READY0");
    control.type = mp::ControlType::button;
    control.active = 1;
    control.hot = true;
    control.attributes = mp::kAttributeCheckbox;
    control.value = 1;
}

void test_ready() {
    Room guest(false);
    put_peer(
        guest.lobby, 1, 0x100, encode_field(host_ready(), false, 0, ng::presence_record_max_bytes)
    );
    auto own = host_ready();
    own.built = false;
    own.layer_digest = {};
    own.mutators.clear();
    put_local(guest.lobby, encode_field(own, false, 0, ng::presence_record_max_bytes));
    mp::local_info(guest.lobby).options =
        static_cast<uint16_t>(mp::local_info(guest.lobby).options | mp::option::ready);
    add_ready(guest.panel);
    mp::lobby_update_status(guest.lobby, guest.panel);
    expect((mp::local_info(guest.lobby).options & mp::option::ready) == 0, "READY is cleared");
    expect(guest.panel.controls[1].grayed, "READY stays greyed");
    expect(guest.panel.controls[1].value == 0, "the button is off");

    guest.panel.controls[1].grayed = false;
    guest.panel.controls[1].value = 1;
    guest.panel.selected = 1;
    (void)mp::lobby_handle_event(guest.lobby, guest.panel);
    expect(guest.panel.controls[1].value == 0, "a click does not turn READY on");
    expect(
        (mp::local_info(guest.lobby).options & mp::option::ready) == 0, "the ready bit stays off"
    );

    Room host(true);
    auto unbuilt = host_ready();
    unbuilt.built = false;
    unbuilt.layer_digest = {};
    put_local(host.lobby, encode_field(unbuilt, false, 0, ng::presence_record_max_bytes));
    mp::local_info(host.lobby).options =
        static_cast<uint16_t>(mp::local_info(host.lobby).options | mp::option::ready);
    add_ready(host.panel);
    mp::lobby_update_status(host.lobby, host.panel);
    expect((mp::local_info(host.lobby).options & mp::option::ready) != 0, "the host's READY stays");
    expect(!host.panel.controls[1].grayed, "the host's READY is not greyed");

    Room open(false);
    ng::PresenceRecord bare;
    bare.engine = ng::PresenceEngine{"0.8.0", "macOS", "arm64"};
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    expect(
        ng::encode_presence(bare, bytes.data(), bytes.size(), &written) == ng::WireError::ok,
        "a record with no mutators encodes"
    );
    bytes.resize(written);
    put_peer(open.lobby, 1, 0x100, bytes);
    put_local(open.lobby, encode_field(own, false, 0, ng::presence_record_max_bytes));
    mp::local_info(open.lobby).options =
        static_cast<uint16_t>(mp::local_info(open.lobby).options | mp::option::ready);
    add_ready(open.panel);
    mp::lobby_update_status(open.lobby, open.panel);
    expect(
        (mp::local_info(open.lobby).options & mp::option::ready) != 0,
        "no mutator field leaves READY as it was"
    );
}

/// The units a sync table is seeded from. Their checksums are already known.
mp::LobbyUnit test_units[3] = {
    {},
    {"A", "ARM", 0, 0, 0, 11, "A", 0x1111, 0},
    {"B", "CORE", 0, 0, 0, 22, "B", 0x2222, 0},
};

void use_units(Room& room) {
    room.lobby.units = test_units;
    room.lobby.unit_count = 3;
}

/// The last handshake the loopback sent, or nothing.
bool last_handshake(const Room& room, uint8_t* subtype, uint32_t* key, uint32_t* value) {
    for (int32_t index = room.loopback.sent_count - 1; index >= 0; --index) {
        if (room.loopback.sent[index][0] != 0x1a)
            continue;
        *subtype = room.loopback.sent[index][1];
        std::memcpy(key, room.loopback.sent[index] + 6, 4);
        std::memcpy(value, room.loopback.sent[index] + 10, 4);
        return true;
    }
    return false;
}

void test_greeting_restarts_the_guest() {
    Room guest(false);
    use_units(guest);
    mp::unit_sync_create(guest.lobby, false);
    guest.lobby.sync.records_handled = 4;
    guest.lobby.sync.next_unit = 2;
    uint8_t greeting[14] = {0x1a, 0};
    mp::unit_sync_receive(guest.lobby, greeting, 1);
    expect(guest.lobby.sync.next_unit == 0, "a greeting starts the report again");
    guest.loopback.sent_count = 0;
    mp::unit_sync_tick(guest.lobby);
    uint8_t subtype = 0xff;
    uint32_t key = 0;
    uint32_t value = 0;
    expect(
        last_handshake(guest, &subtype, &key, &value) && subtype == 1 && value == 2 &&
            guest.lobby.sync.next_unit == 1,
        "the next tick sends the unit count"
    );
    const auto from = guest.loopback.sent_count;
    mp::unit_sync_tick(guest.lobby);
    expect(from < guest.loopback.sent_count, "checksums follow the count");
    if (from < guest.loopback.sent_count) {
        std::memcpy(&key, guest.loopback.sent[from] + 6, 4);
        std::memcpy(&value, guest.loopback.sent[from] + 10, 4);
        expect(
            guest.loopback.sent[from][1] == 2 && key == 11 && value == 0x1111,
            "the first checksum is the first unit"
        );
    }
}

/// Feeds one peer's count and checksums. The acknowledgement waits until every
/// peer has reported, because one peer's checksums make the host send the
/// other more verdicts.
void report_units(Room& room, int32_t player_slot) {
    uint8_t count[14] = {0x1a, 1};
    const uint32_t expected = static_cast<uint32_t>(room.lobby.unit_count - 1);
    std::memcpy(count + 10, &expected, 4);
    mp::unit_sync_receive(room.lobby, count, static_cast<uint8_t>(player_slot));
    for (int32_t type = 1; type < room.lobby.unit_count; ++type) {
        uint8_t checksum[14] = {0x1a, 2};
        const auto unit_key = room.lobby.units[type].fbi_hash;
        const auto sum = room.lobby.units[type].content_checksum;
        std::memcpy(checksum + 6, &unit_key, 4);
        std::memcpy(checksum + 10, &sum, 4);
        mp::unit_sync_receive(room.lobby, checksum, static_cast<uint8_t>(player_slot));
    }
}

/// Acknowledges each peer's current sent count, as its next tick would.
void acknowledge_sent(Room& room) {
    for (int32_t index = 0; index < room.lobby.sync.peer_count; ++index) {
        const auto& peer = room.lobby.sync.peers[index];
        int32_t player_slot = -1;
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
            if (mp::slot_player(room.lobby, slot).player_id == peer.player_id)
                player_slot = slot;
        }
        if (player_slot < 0)
            continue;
        uint8_t ack[14] = {0x1a, 4};
        const uint32_t sent = peer.sent;
        std::memcpy(ack + 10, &sent, 4);
        mp::unit_sync_receive(room.lobby, ack, static_cast<uint8_t>(player_slot));
    }
}

bool both_complete(Room& room) {
    acknowledge_sent(room);
    return mp::unit_sync_peer_complete(room.lobby, 0x200) &&
           mp::unit_sync_peer_complete(room.lobby, 0x300);
}

/// How many greetings the loopback has sent to one player.
int greetings_to(const Room& room, uint32_t id) {
    int count = 0;
    for (int32_t index = 0; index < room.loopback.sent_count; ++index)
        if (room.loopback.sent[index][0] == 0x1a && room.loopback.sent[index][1] == 0 &&
            room.loopback.sent_to[index] == id)
            ++count;
    return count;
}

void test_rebuild_keeps_a_finished_legacy_peer() {
    Room host(true);
    host.seat(1, 0x200, false, false);
    host.seat(3, 0x300, true, false);
    use_units(host);
    mp::unit_sync_create(host.lobby, true);
    mp::unit_sync_tick(host.lobby);
    report_units(host, 1);
    report_units(host, 3);
    expect(both_complete(host), "both peers finish before the list changes");

    const int greeted_before = greetings_to(host, 0x300);
    mp::unit_sync_rebuild(host.lobby);
    acknowledge_sent(host);
    expect(
        host.lobby.sync.peer_count == 2 && mp::unit_sync_peer_complete(host.lobby, 0x200),
        "a finished 3.1c peer stays complete"
    );
    expect(!mp::unit_sync_peer_complete(host.lobby, 0x300), "the greeted OA peer has not reported");
    expect(
        greetings_to(host, 0x300) == greeted_before + 1, "the OA peer is greeted at the rebuild"
    );
    bool waiting = host.lobby.sync.record_count == 2;
    for (int32_t index = 0; index < host.lobby.sync.record_count; ++index)
        waiting = waiting && host.lobby.sync.records[index].remote == 0;
    expect(waiting, "units stay unagreed until the greeted peer reports");

    report_units(host, 3);
    expect(both_complete(host), "both peers finish with the new units");

    const int greeted = greetings_to(host, 0x300);
    mp::unit_sync_regreet(host.lobby, 0x300);
    expect(!mp::unit_sync_peer_complete(host.lobby, 0x300), "regreet drops that peer");
    expect(greetings_to(host, 0x300) == greeted + 1, "regreet greets that peer at once");
    report_units(host, 3);
    expect(both_complete(host), "the greeted peer finishes again");

    // Mutators off: the same rebuild greets the OA peer at once.
    mp::unit_sync_rebuild(host.lobby);
    acknowledge_sent(host);
    expect(
        mp::unit_sync_peer_complete(host.lobby, 0x200) &&
            !mp::unit_sync_peer_complete(host.lobby, 0x300) &&
            greetings_to(host, 0x300) == greeted + 2,
        "turning mutators off keeps the 3.1c peer and greets the OA peer"
    );
    report_units(host, 3);
    expect(both_complete(host), "after mutators on and off every peer is complete");
}

/// A 1.2 peer is compared by checksum. A peer that reports no checksums is not.
void test_legacy_checksum_is_compared() {
    Room host(true);
    host.seat(1, 0x200, false, false);
    auto* info = mp::slot_info(host.lobby, 1);
    info->version_major = 1;
    info->version_minor = 2;
    use_units(host);
    mp::unit_sync_create(host.lobby, true);
    mp::unit_sync_tick(host.lobby);
    report_units(host, 1);
    acknowledge_sent(host);
    expect(mp::unit_sync_peer_complete(host.lobby, 0x200), "the 1.2 peer finishes");

    mp::UnitSyncPeer* peer = nullptr;
    for (int32_t index = 0; index < host.lobby.sync.peer_count; ++index)
        if (host.lobby.sync.peers[index].player_id == 0x200)
            peer = &host.lobby.sync.peers[index];
    expect(peer != nullptr && peer->received >= 1, "the 1.2 peer stored a checksum");
    if (peer != nullptr && peer->received >= 1)
        peer->checksums[0] = 0x9999;
    mp::unit_sync_rebuild(host.lobby);
    bool first_disagrees = false;
    bool second_agrees = false;
    for (int32_t index = 0; index < host.lobby.sync.record_count; ++index) {
        const auto& record = host.lobby.sync.records[index];
        if (record.key == 11)
            first_disagrees = record.remote == 0;
        if (record.key == 22)
            second_agrees = record.remote == 1;
    }
    expect(first_disagrees, "a stored checksum that differs disagrees");
    expect(second_agrees, "a stored checksum that matches agrees");

    Room plain(true);
    plain.seat(1, 0x200, false, false);
    use_units(plain);
    mp::unit_sync_create(plain.lobby, true);
    mp::unit_sync_tick(plain.lobby);
    report_units(plain, 1);
    peer = nullptr;
    for (int32_t index = 0; index < plain.lobby.sync.peer_count; ++index)
        if (plain.lobby.sync.peers[index].player_id == 0x200)
            peer = &plain.lobby.sync.peers[index];
    if (peer != nullptr && peer->received >= 1)
        peer->checksums[0] = 0x9999;
    mp::unit_sync_rebuild(plain.lobby);
    bool presence = plain.lobby.sync.record_count == 2;
    for (int32_t index = 0; index < plain.lobby.sync.record_count; ++index)
        presence = presence && plain.lobby.sync.records[index].remote == 1;
    expect(presence, "a peer without checksums still agrees by presence");
}

/// The field a guest publishes follows the phase, including a return to an earlier list.
void test_field_follows_the_phase() {
    ng::PresenceDigest x{};
    x.fill(0x11);
    ng::PresenceDigest y{};
    y.fill(0x44);
    ng::PresenceDigest folded{};
    folded.fill(0x22);
    ng::PresenceMutators field;
    expect(
        mp::lobby_room_mutator_field(
            mp::RoomMutatorPhase::built,
            x,
            folded,
            ng::presence_mutator_index_none,
            1,
            1,
            {},
            &field
        ) && field.built &&
            field.digest == x && field.layer_digest == folded,
        "a built list publishes its mutator digest"
    );
    expect(
        mp::lobby_room_mutator_field(
            mp::RoomMutatorPhase::receiving,
            y,
            folded,
            ng::presence_mutator_index_none,
            2,
            1,
            {},
            &field
        ) && !field.built &&
            field.digest == y && field.layer_digest == ng::PresenceDigest{} &&
            field.failed == false,
        "a list that is not whole publishes receiving"
    );
    expect(
        mp::lobby_room_mutator_field(mp::RoomMutatorPhase::built, x, folded, 1, 2, 2, {}, &field) &&
            field.built && field.digest == x && field.layer_digest == folded && field.index == 1,
        "returning to the first list publishes that list again"
    );
}

/// Hands the guest a record from the host in slot 1.
///
/// @param guest the guest's room
/// @param bytes the host's record
/// @return true when the guest kept it
bool host_record_to(Room& guest, const std::vector<uint8_t>& bytes) {
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = 0x100;
    event.size = static_cast<uint16_t>(bytes.size());
    std::memcpy(event.data, bytes.data(), bytes.size());
    return mp::lobby_apply_event(guest.lobby, event);
}

/// Resets the guest's lobby and seats it in the next host's room, as a join does.
///
/// @param guest the guest's room
void join_again(Room& guest) {
    mp::lobby_reset(guest.lobby, *guest.game);
    guest.game->players[0].player_id = 0x200u;
    mp::lobby_seat_local(guest.lobby, 0, false, "Guest");
    guest.seat(1, 0x100u, true, true);
}

/// The chat lines the room has posted, oldest first.
std::vector<std::string> room_lines(mp::Lobby& lobby) {
    std::vector<std::string> lines;
    auto& game = *lobby.game;
    for (auto at = static_cast<uint32_t>(mp::lobby_chat_tail(game));
         at != mp::lobby_chat_head(game);
         at = (at + 1U) % static_cast<uint32_t>(mp::kChatLines))
        lines.emplace_back(mp::lobby_chat_line(game, at));
    return lines;
}

/// The ring lines joined with one space, which is the space a split drops.
std::string joined_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        if (!text.empty())
            text += ' ';
        text += line;
    }
    return text;
}

/// True when every part fits in one chat line.
bool parts_fit(const std::vector<std::string>& lines) {
    for (const auto& line : lines)
        if (line.size() > mp::kChatLineBytes - 1)
            return false;
    return true;
}

/// Binds the record this machine sends and composes it, as a change of list does.
void bind_source(mp::Lobby& lobby, const std::vector<uint8_t>& bytes) {
    std::memcpy(lobby.presence_records.source, bytes.data(), bytes.size());
    lobby.presence_records.source_size = static_cast<uint16_t>(bytes.size());
    mp::presence_refresh(lobby);
}

/// Applies one remote player's setup block.
///
/// @param room the room
/// @param id the player, already seated
/// @param oa whether the block carries the engine signature
/// @param watcher whether the block says the player is watching
/// @return true when the record applied
bool apply_block(Room& room, uint32_t id, bool oa, bool watcher) {
    mp::PlayerSetupInfo info{};
    info.state = mp::kInfoStatePlaying;
    info.version_major = 3;
    info.version_minor = 1;
    if (watcher)
        info.options = mp::option::watcher;
    if (oa) {
        info.engine_signature[0] = ng::engine_signature_first;
        info.engine_signature[1] = ng::engine_signature_second;
    }
    ng::PlayerInfoRecord record{};
    std::memcpy(record.info_head, &info, sizeof record.info_head);
    record.player_id = id;
    std::memcpy(
        record.info_tail,
        reinterpret_cast<const uint8_t*>(&info) + ng::player_info_tail_offset,
        sizeof record.info_tail
    );
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = id;
    std::size_t written = 0;
    if (ng::encode_record(record, event.data, sizeof event.data, &written) != ng::WireError::ok)
        return false;
    event.size = static_cast<uint16_t>(written);
    return mp::lobby_apply_event(room.lobby, event);
}

/// Sends one seated player's block with a chosen side and, when asked, the watcher bit.
///
/// @param room the room
/// @param id the player, already seated
/// @param side the block's side
/// @param watcher whether the block says the player is watching
/// @return true when the record applied
bool apply_side_block(Room& room, uint32_t id, uint8_t side, bool watcher) {
    mp::PlayerSetupInfo info{};
    info.state = mp::kInfoStatePlaying;
    info.version_major = 3;
    info.version_minor = 1;
    info.side = side;
    if (watcher)
        info.options = mp::option::watcher;
    info.engine_signature[0] = ng::engine_signature_first;
    info.engine_signature[1] = ng::engine_signature_second;
    ng::PlayerInfoRecord record{};
    std::memcpy(record.info_head, &info, sizeof record.info_head);
    record.player_id = id;
    std::memcpy(
        record.info_tail,
        reinterpret_cast<const uint8_t*>(&info) + ng::player_info_tail_offset,
        sizeof record.info_tail
    );
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = id;
    std::size_t written = 0;
    if (ng::encode_record(record, event.data, sizeof event.data, &written) != ng::WireError::ok)
        return false;
    event.size = static_cast<uint16_t>(written);
    return mp::lobby_apply_event(room.lobby, event);
}

/// Tells whether a reject of this player and reason was sent.
bool sent_reject(const Room& room, uint32_t id, uint8_t reason) {
    for (int32_t index = 0; index < room.loopback.sent_count; ++index) {
        ng::RejectRecord record{};
        if (ng::decode_record(room.loopback.sent[index], room.loopback.sent_size[index], &record) ==
                ng::WireError::ok &&
            record.player_id == id && record.reason == reason)
            return true;
    }
    return false;
}

/// A room with mutators refuses a 3.1c block, and a room without one seats it.
void test_classic_joiner() {
    const std::string join =
        "*** Bo plays Total Annihilation 3.1c and cannot join a game with mutators.";
    {
        Room room(true);
        room.seat(1, 0x200, false, false);
        // as a join does (presence_forget)
        room.lobby.presence_records.block_player[1] = 0;
        std::snprintf(
            mp::slot_player(room.lobby, 1).name, sizeof mp::slot_player(room.lobby, 1).name, "Bo"
        );
        expect(apply_block(room, 0x200, false, false), "a 3.1c block applies");
        const auto& player = mp::slot_player(room.lobby, 1);
        expect(
            player.status == mp::kSlotRemote && player.reject_reason == 0,
            "a room without mutators seats a 3.1c player"
        );
        expect(room_lines(room.lobby).empty(), "a room without mutators posts no line");
        expect(!sent_reject(room, 0x200, 8), "a room without mutators sends no refusal");
    }
    {
        Room room = hosted(host_ready());
        room.seat(1, 0x200, false, false);
        // as a join does (presence_forget)
        room.lobby.presence_records.block_player[1] = 0;
        std::snprintf(
            mp::slot_player(room.lobby, 1).name, sizeof mp::slot_player(room.lobby, 1).name, "Bo"
        );
        expect(apply_block(room, 0x200, false, false), "a 3.1c block applies under mutators");
        const auto& player = mp::slot_player(room.lobby, 1);
        expect(
            player.reject_reason == 8 && player.status == mp::kSlotOpen,
            "mutators refuse a 3.1c player with reason 8"
        );
        expect(sent_reject(room, 0x200, 8), "the refusal is reason 8");
        const auto lines = room_lines(room.lobby);
        expect(
            parts_fit(lines) && joined_lines(lines) == join,
            "the room names the 3.1c player who cannot join"
        );
        int32_t chats = 0;
        for (int32_t index = 0; index < room.loopback.sent_count; ++index)
            if (room.loopback.sent[index][0] == static_cast<uint8_t>(ng::RecordType::chat))
                ++chats;
        expect(chats == 0, "the line is not sent");
    }
    {
        Room room = hosted(host_ready());
        room.seat(2, 0x300, false, false);
        // as a join does (presence_forget)
        room.lobby.presence_records.block_player[2] = 0;
        expect(apply_block(room, 0x300, false, true), "a 3.1c watcher block applies");
        const auto& watcher = mp::slot_player(room.lobby, 2);
        expect(
            watcher.reject_reason == 8 && watcher.status == mp::kSlotOpen,
            "mutators refuse a 3.1c watcher with reason 8"
        );
    }
    {
        Room room = hosted(host_ready());
        room.seat(1, 0x200, true, false);
        // as a join does (presence_forget)
        room.lobby.presence_records.block_player[1] = 0;
        expect(apply_block(room, 0x200, true, false), "an OA block applies");
        expect(
            mp::slot_player(room.lobby, 1).status == mp::kSlotRemote &&
                mp::slot_player(room.lobby, 1).reject_reason == 0,
            "an OA block is seated while mutators are on"
        );
        put_peer(
            room.lobby,
            1,
            0x200,
            encode_field(host_ready(), false, 0, ng::presence_record_max_bytes)
        );
        expect(
            mp::slot_player(room.lobby, 1).status == mp::kSlotRemote,
            "an OA block with a mutator field stays seated"
        );
        std::vector<uint8_t> plain(ng::presence_record_max_bytes);
        std::size_t written = 0;
        expect(
            ng::encode_presence(ng::PresenceRecord{}, plain.data(), plain.size(), &written) ==
                ng::WireError::ok,
            "a record without mutators encodes"
        );
        plain.resize(written);
        put_peer(room.lobby, 1, 0x200, plain);
        expect(
            mp::slot_player(room.lobby, 1).status == mp::kSlotRemote &&
                mp::slot_player(room.lobby, 1).reject_reason == 0,
            "an OA block without a mutator field stays seated"
        );
    }
}

/// A 3.1c player already seated holds START until mutators go, and the line posts once.
void test_classic_seated() {
    const auto listed = encode_field(host_ready(), false, 0, ng::presence_record_max_bytes);
    std::vector<uint8_t> plain(ng::presence_record_max_bytes);
    std::size_t written = 0;
    expect(
        ng::encode_presence(ng::PresenceRecord{}, plain.data(), plain.size(), &written) ==
            ng::WireError::ok,
        "a record without mutators encodes"
    );
    plain.resize(written);
    const std::string seated =
        "*** Bo plays Total Annihilation 3.1c, which cannot play with mutators.";

    Room room(true);
    room.seat(1, 0x200, false, false);
    std::snprintf(
        mp::slot_player(room.lobby, 1).name, sizeof mp::slot_player(room.lobby, 1).name, "Bo"
    );
    room.seat_computer(2);
    bind_source(room.lobby, plain);
    expect(
        mp::lobby_classic_players(room.lobby).size() == 1 &&
            mp::lobby_classic_players(room.lobby)[0] == 1,
        "the picker names a 3.1c player before mutators are on"
    );
    expect(room_lines(room.lobby).empty(), "turning no list on posts nothing");

    bind_source(room.lobby, listed);
    const auto block = mp::lobby_mutators_block(room.lobby);
    expect_hold(block, mp::MutatorsHold::classic_player, 1, ng::presence_mutator_index_none);
    expect(!mp::lobby_mutators_allow_start(room.lobby), "a seated 3.1c player holds START");
    expect(!mp::lobby_mutators_differ_from_host(room.lobby, 1), "a 3.1c row is not a rules dot");
    const auto named = mp::lobby_classic_players(room.lobby);
    expect(named.size() == 1 && named[0] == 1, "the gate names the 3.1c slot");
    expect(
        mp::slot_player(room.lobby, 1).status == mp::kSlotRemote &&
            mp::slot_player(room.lobby, 1).reject_reason == 0,
        "turning mutators on leaves the 3.1c player seated"
    );
    expect(
        parts_fit(room_lines(room.lobby)) && joined_lines(room_lines(room.lobby)) == seated,
        "the room says why the seated player holds the game"
    );
    const auto posted = room_lines(room.lobby).size();
    bind_source(room.lobby, listed);
    expect(room_lines(room.lobby).size() == posted, "the same list posts the line once");

    bind_source(room.lobby, plain);
    expect(mp::lobby_mutators_allow_start(room.lobby), "START waits only while mutators are on");
    expect(
        mp::lobby_classic_players(room.lobby).size() == 1,
        "mutators off still names the seated 3.1c player"
    );
    bind_source(room.lobby, listed);
    expect(
        joined_lines(room_lines(room.lobby)) == seated + " " + seated,
        "the line posts once more when mutators return"
    );

    Room guest(false);
    guest.seat(2, 0x300, false, false);
    std::snprintf(
        mp::slot_player(guest.lobby, 2).name, sizeof mp::slot_player(guest.lobby, 2).name, "Bo"
    );
    expect(host_record_to(guest, plain), "the guest keeps a host record without mutators");
    expect(room_lines(guest.lobby).empty(), "a guest posts nothing before mutators");
    expect(host_record_to(guest, listed), "the guest keeps the host's mutators");
    expect(host_record_to(guest, listed), "the guest keeps the same list again");
    expect(
        parts_fit(room_lines(guest.lobby)) && joined_lines(room_lines(guest.lobby)) == seated,
        "a guest posts the seated line once"
    );
    expect(host_record_to(guest, plain), "the guest keeps the list going away");
    expect(host_record_to(guest, listed), "the guest keeps the list coming back");
    expect(
        joined_lines(room_lines(guest.lobby)) == seated + " " + seated,
        "a guest posts once per change"
    );
}

/// True when some posted part contains these bytes.
bool lines_contain(const std::vector<std::string>& lines, const char* text) {
    for (const auto& line : lines)
        if (line.find(text) != std::string::npos)
            return true;
    return false;
}

/// The catalogue's refusal in one language. The lobby's Translate hook is
/// left unset, so a line that still goes through that hook stays in English.
bool use_refusal(const char* tag) {
    static oa::data::languages::InterfaceText catalogue;
    static bool ready = false;
    if (!ready) {
        ready = catalogue.add(
            "[*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.]\n"
            "{\nde=*** {player} spielt Total Annihilation 3.1c und kann einem Spiel mit Mutatoren "
            "nicht beitreten.;\n"
            "fr=*** {player} joue à Total Annihilation 3.1c et ne peut pas rejoindre une partie "
            "avec des mutateurs.;\n}\n"
        );
    }
    const oa::data::languages::Language* language = oa::data::languages::find_by_tag(tag);
    if (!ready || language == nullptr)
        return false;
    oa::data::languages::set_interface_language(&catalogue, *language);
    return true;
}

/// A later block does not refuse a seated 3.1c player, and leaving lifts START.
void test_classic_stays_and_leaves() {
    const auto listed = encode_field(host_ready(), false, 0, ng::presence_record_max_bytes);
    std::vector<uint8_t> plain(ng::presence_record_max_bytes);
    std::size_t written = 0;
    expect(
        ng::encode_presence(ng::PresenceRecord{}, plain.data(), plain.size(), &written) ==
            ng::WireError::ok,
        "a plain record encodes"
    );
    plain.resize(written);

    {
        Room room(true);
        room.seat(1, 0x200, false, false);
        room.lobby.presence_records.block_player[1] = 0;
        expect(apply_block(room, 0x200, false, false), "the 3.1c block arrives with no list");
        bind_source(room.lobby, listed);
        expect(apply_block(room, 0x200, false, false), "the seated player's ready block applies");
        const auto& player = mp::slot_player(room.lobby, 1);
        expect(
            player.status == mp::kSlotRemote && player.reject_reason == 0,
            "a returning 3.1c block stays seated"
        );
        expect(!sent_reject(room, 0x200, 8), "a returning block sends no refusal");
        expect(
            !lines_contain(room_lines(room.lobby), "cannot join"),
            "a returning block posts no join line"
        );
        expect_hold(
            mp::lobby_mutators_block(room.lobby),
            mp::MutatorsHold::classic_player,
            1,
            ng::presence_mutator_index_none
        );
    }
    {
        Room room(true);
        room.seat(1, 0x200, false, false);
        room.lobby.presence_records.block_player[1] = 0;
        expect(apply_block(room, 0x200, false, true), "the watcher block arrives with no list");
        bind_source(room.lobby, listed);
        expect(apply_block(room, 0x200, false, true), "the watcher's later block applies");
        const auto& player = mp::slot_player(room.lobby, 1);
        expect(
            player.status == mp::kSlotRemote && player.reject_reason == 0,
            "a returning watcher block stays seated"
        );
        expect(!sent_reject(room, 0x200, 8), "a returning watcher block sends no refusal");
        expect(
            !lines_contain(room_lines(room.lobby), "cannot join"),
            "a returning watcher block posts no join line"
        );
        expect_hold(
            mp::lobby_mutators_block(room.lobby),
            mp::MutatorsHold::classic_player,
            1,
            ng::presence_mutator_index_none
        );
    }
    {
        Room room = hosted(host_ready());
        room.seat(1, 0x200, true, false);
        mp::slot_player(room.lobby, 1).machine_group = 2;
        room.seat(3, 0x400, true, false);
        mp::slot_player(room.lobby, 3).machine_group = 0;
        room.seat(2, 0x300, false, false);
        room.lobby.presence_records.block_player[2] = 0;
        expect(apply_block(room, 0x300, false, false), "the 3.1c joiner's block arrives");
        const auto& refused = mp::slot_player(room.lobby, 2);
        expect(
            refused.status == mp::kSlotOpen && refused.reject_reason == 8,
            "the arriving 3.1c player is refused"
        );
        expect(
            mp::slot_player(room.lobby, 1).status == mp::kSlotRemote &&
                mp::slot_player(room.lobby, 1).reject_reason == 0 &&
                mp::slot_player(room.lobby, 3).status == mp::kSlotRemote &&
                mp::slot_player(room.lobby, 3).reject_reason == 0,
            "other joiners stay seated"
        );
        expect(!sent_reject(room, 0x400, 8), "no refusal names the other joiner");
    }
    {
        Room room(true);
        room.seat(1, 0x200, false, false);
        bind_source(room.lobby, listed);
        expect(!mp::lobby_mutators_allow_start(room.lobby), "the seated player holds START");
        mp::LobbyEvent left{};
        left.kind = mp::LobbyEventKind::player_left;
        left.player_id = 0x200;
        expect(mp::lobby_apply_event(room.lobby, left), "the 3.1c player leaves");
        expect(mp::lobby_mutators_allow_start(room.lobby), "START is free once that player leaves");
        expect(mp::lobby_classic_players(room.lobby).empty(), "a player who left is not named");
    }
}

/// Hands a guest the host's rejection of a player.
///
/// @param guest the guest's room
/// @param id the player rejected
/// @param reason the reason
/// @return true when the record applied
bool host_reject_to(Room& guest, uint32_t id, uint8_t reason) {
    ng::RejectRecord record{};
    record.player_id = id;
    record.reason = reason;
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = 0x100;
    std::size_t written = 0;
    if (ng::encode_record(record, event.data, sizeof event.data, &written) != ng::WireError::ok)
        return false;
    event.size = static_cast<uint16_t>(written);
    return mp::lobby_apply_event(guest.lobby, event);
}

/// A guest names a 3.1c joiner when the host refuses it, and refuses no one.
void test_guest_posts_the_refusal() {
    const std::string join =
        "*** Bo plays Total Annihilation 3.1c and cannot join a game with mutators.";
    const auto listed = encode_field(host_ready(), false, 0, ng::presence_record_max_bytes);
    {
        Room guest(false);
        expect(host_record_to(guest, listed), "the guest keeps the host's mutators");
        guest.seat(2, 0x300, false, false);
        guest.lobby.presence_records.block_player[2] = 0;
        std::snprintf(
            mp::slot_player(guest.lobby, 2).name, sizeof mp::slot_player(guest.lobby, 2).name, "Bo"
        );
        expect(apply_block(guest, 0x300, false, false), "the guest applies the joiner's block");
        expect(room_lines(guest.lobby).empty(), "a block alone posts nothing on a guest");
        expect(!sent_reject(guest, 0x300, 8), "a guest refuses no one");
        expect(
            mp::slot_player(guest.lobby, 2).status == mp::kSlotRemote,
            "the guest leaves the joiner seated until the host refuses it"
        );
        expect(host_reject_to(guest, 0x300, 8), "the host's refusal reaches the guest");
        const auto once = room_lines(guest.lobby);
        expect(parts_fit(once) && joined_lines(once) == join, "the guest names the refused joiner");
        expect(
            mp::slot_player(guest.lobby, 2).status == mp::kSlotOpen,
            "the refused joiner leaves the guest's room"
        );
        expect(!host_reject_to(guest, 0x300, 8), "a passed-on copy of the refusal is not applied");
        expect(room_lines(guest.lobby).size() == once.size(), "the guest names the joiner once");
    }
    {
        // A new guest can hear the host's list before the greeting of a 3.1c
        // player seated before the list went on. That player is not refused.
        Room guest(false);
        expect(host_record_to(guest, listed), "the new guest keeps the host's mutators");
        guest.seat(2, 0x300, false, false);
        guest.lobby.presence_records.block_player[2] = 0;
        expect(apply_block(guest, 0x300, false, false), "the seated player's greeting applies");
        expect(
            !lines_contain(room_lines(guest.lobby), "cannot join"),
            "a seated 3.1c player is not said to be unable to join"
        );
    }
    {
        Room guest(false);
        guest.seat(2, 0x300, false, false);
        guest.lobby.presence_records.block_player[2] = 0;
        expect(apply_block(guest, 0x300, false, false), "a 3.1c block applies without a list");
        expect(host_reject_to(guest, 0x300, 8), "a refusal applies without a list");
        expect(room_lines(guest.lobby).empty(), "a room without mutators posts no line");
    }
    {
        Room guest(false);
        expect(host_record_to(guest, listed), "the guest keeps the host's mutators");
        guest.seat(2, 0x300, false, false);
        guest.lobby.presence_records.block_player[2] = 0;
        expect(apply_block(guest, 0x300, false, false), "a 3.1c block applies");
        expect(host_reject_to(guest, 0x300, 1), "another reason applies");
        expect(room_lines(guest.lobby).empty(), "only the mutator refusal posts the line");
    }
}

/// A long name and the German sentence are posted whole, in parts.
void test_classic_lines_whole() {
    const char* long_name = "ABCDEFGHIJ0123456789";
    const std::string join = std::string("*** ") + long_name +
                             " plays Total Annihilation 3.1c and cannot join a game with mutators.";
    const std::string seated = std::string("*** ") + long_name +
                               " plays Total Annihilation 3.1c, which cannot play with mutators.";
    const std::string german = std::string("*** ") + long_name +
                               " spielt Total Annihilation 3.1c und kann einem Spiel mit Mutatoren "
                               "nicht beitreten.";
    {
        Room room = hosted(host_ready());
        room.seat(1, 0x200, false, false);
        room.lobby.presence_records.block_player[1] = 0;
        std::snprintf(
            mp::slot_player(room.lobby, 1).name,
            sizeof mp::slot_player(room.lobby, 1).name,
            "%s",
            long_name
        );
        expect(apply_block(room, 0x200, false, false), "a long-named 3.1c block arrives");
        const auto lines = room_lines(room.lobby);
        expect(parts_fit(lines) && joined_lines(lines) == join, "the join line is posted whole");
        expect(!lines.empty() && lines.back().size() > 0, "the join line has a last part");
    }
    {
        Room room(true);
        room.seat(1, 0x200, false, false);
        std::snprintf(
            mp::slot_player(room.lobby, 1).name,
            sizeof mp::slot_player(room.lobby, 1).name,
            "%s",
            long_name
        );
        const auto listed = encode_field(host_ready(), false, 0, ng::presence_record_max_bytes);
        bind_source(room.lobby, listed);
        const auto lines = room_lines(room.lobby);
        expect(
            parts_fit(lines) && joined_lines(lines) == seated, "the seated line is posted whole"
        );
    }
    {
        Room room = hosted(host_ready());
        room.seat(1, 0x200, false, false);
        room.lobby.presence_records.block_player[1] = 0;
        std::snprintf(
            mp::slot_player(room.lobby, 1).name,
            sizeof mp::slot_player(room.lobby, 1).name,
            "%s",
            long_name
        );
        expect(use_refusal("de"), "the German refusal is a catalogue word");
        expect(apply_block(room, 0x200, false, false), "a translated 3.1c block arrives");
        oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
        const auto lines = room_lines(room.lobby);
        expect(
            parts_fit(lines) && joined_lines(lines) == german, "the German line is posted whole"
        );
        expect(
            !lines.empty() && lines.back().find("nicht beitreten.") != std::string::npos,
            "the last German part says the player cannot join"
        );
    }
    {
        const std::string french_utf8 =
            "*** Bo joue à Total Annihilation 3.1c et ne peut pas rejoindre une partie avec des "
            "mutateurs.";
        Room room = hosted(host_ready());
        room.seat(1, 0x200, false, false);
        room.lobby.presence_records.block_player[1] = 0;
        std::snprintf(
            mp::slot_player(room.lobby, 1).name, sizeof mp::slot_player(room.lobby, 1).name, "Bo"
        );
        expect(use_refusal("fr"), "the French refusal is a catalogue word");
        expect(apply_block(room, 0x200, false, false), "a French 3.1c block arrives");
        oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
        const auto lines = room_lines(room.lobby);
        const std::string page = ng::chat_code_page(french_utf8);
        expect(
            parts_fit(lines) && joined_lines(lines) == page,
            "the French line is the catalogue's, in the code page"
        );
        expect(
            page.find('\xE0') != std::string::npos && page.find("\xC3\xA0") == std::string::npos,
            "à is one code-page byte"
        );
    }
    {
        // With the chat list's measure bound, the line goes on in "*** "
        // lines that each fit the list as the code page draws them: 6 a
        // byte in a room of 300.
        const std::string french_utf8 =
            "*** Bo joue à Total Annihilation 3.1c et ne peut pas rejoindre une partie avec des "
            "mutateurs.";
        Room room = hosted(host_ready());
        room.lobby.chat_measure = {
            nullptr,
            [](void*) -> int32_t { return 300; },
            [](void*, std::string_view line) -> int32_t {
                return static_cast<int32_t>(ng::chat_code_page(line).size()) * 6;
            }
        };
        room.seat(1, 0x200, false, false);
        room.lobby.presence_records.block_player[1] = 0;
        std::snprintf(
            mp::slot_player(room.lobby, 1).name, sizeof mp::slot_player(room.lobby, 1).name, "Bo"
        );
        expect(use_refusal("fr"), "the French refusal is a catalogue word");
        expect(apply_block(room, 0x200, false, false), "a measured French 3.1c block arrives");
        oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
        const auto lines = room_lines(room.lobby);
        expect(lines.size() >= 2, "the measured French line goes on in a further line");
        std::string joined;
        for (const std::string& line : lines) {
            expect(line.size() * 6 <= 300, "each measured line fits the list");
            expect(line.starts_with("*** "), "each measured line starts with ***");
            if (!joined.empty())
                joined += ' ';
            joined += joined.empty() ? line : line.substr(4);
        }
        expect(
            joined == ng::chat_code_page(french_utf8),
            "the measured French line keeps every word, in the code page"
        );
    }
    {
        Room room(true);
        room.seat(1, 0x200, false, false);
        room.seat_computer(2);
        room.seat(3, 0x300, true, false);
        const auto before = mp::lobby_classic_players(room.lobby);
        expect(before.size() == 1 && before[0] == 1, "no list still names only the 3.1c slot");
        bind_source(
            room.lobby, encode_field(host_ready(), false, 0, ng::presence_record_max_bytes)
        );
        const auto after = mp::lobby_classic_players(room.lobby);
        expect(after.size() == 1 && after[0] == 1, "a list names only the 3.1c slot");
    }
}

/// A new room forgets the previous host's list, under a host that sends none
/// and under one whose records are the same bytes as before.
void test_new_room_forgets_the_host_list() {
    const auto listed = encode_field(host_ready(), false, 0, ng::presence_record_max_bytes);
    std::vector<uint8_t> plain(ng::presence_record_max_bytes);
    std::size_t written = 0;
    expect(
        ng::encode_presence(ng::PresenceRecord{}, plain.data(), plain.size(), &written) ==
            ng::WireError::ok,
        "a record without a mutator field encodes"
    );
    plain.resize(written);

    Room guest(false);
    expect(host_record_to(guest, listed), "the guest keeps the first host's record");
    const auto& records = guest.lobby.presence_records;
    expect(records.host_loadout.total == 2, "the first host's list is held");
    const uint32_t first_room = records.host_loadout_generation;

    join_again(guest);
    expect(records.host_loadout_generation != first_room, "a new room moves the list's generation");
    expect(
        records.host_loadout.total == 0 && records.host_loadout.mutators.empty(),
        "a new room empties the list"
    );
    expect(host_record_to(guest, plain), "the guest keeps a host record without a list");
    expect(
        records.host_loadout_generation != first_room && records.host_loadout.total == 0 &&
            records.host_loadout.mutators.empty(),
        "a host that sends no list leaves the guest none"
    );

    Room again(false);
    expect(host_record_to(again, listed), "the guest keeps the host's record");
    join_again(again);
    expect(host_record_to(again, listed), "the guest keeps the same bytes after joining again");
    expect(
        again.lobby.presence_records.host_loadout.total == 2,
        "the same bytes are folded again in the new room"
    );
}

/// A side past the room's side count holds START and names the player once.
void test_side_past_table() {
    constexpr const char* kLine = "*** P512 chose a side this game does not have.";
    Room room(true);
    room.game->side_count = 2;
    expect(apply_side_block(room, 0x200, 2, false), "the guest's side 2 block applies");
    expect(mp::lobby_first_side_past_table(room.lobby) == 1, "the guest on side 2 is the first");
    expect(!mp::lobby_sides_allow_start(room.lobby), "a guest on side 2 holds START");
    const auto before = room_lines(room.lobby).size();
    mp::lobby_note_sides_past_table(room.lobby);
    mp::lobby_note_sides_past_table(room.lobby);
    auto lines = room_lines(room.lobby);
    expect(lines.size() == before + 1, "the side line posts once for the same side");
    expect(!lines.empty() && lines.back() == kLine, "the side line names P512");

    expect(apply_side_block(room, 0x200, 1, false), "the guest's side 1 block applies");
    expect(mp::lobby_sides_allow_start(room.lobby), "side 1 allows START");
    const auto inside = room_lines(room.lobby).size();
    mp::lobby_note_sides_past_table(room.lobby);
    expect(room_lines(room.lobby).size() == inside, "a side inside the table posts nothing");

    expect(apply_side_block(room, 0x200, 3, false), "the guest's side 3 block applies");
    mp::lobby_note_sides_past_table(room.lobby);
    lines = room_lines(room.lobby);
    expect(lines.size() == inside + 1, "a different side posts once more");
    expect(!lines.empty() && lines.back() == kLine, "the second side line names P512");

    expect(apply_side_block(room, 0x200, 4, true), "the watcher block applies");
    expect(mp::lobby_sides_allow_start(room.lobby), "a watcher on side 4 allows START");

    mp::slot_info(room.lobby, 0)->side = 2;
    expect(mp::lobby_first_side_past_table(room.lobby) == 0, "the local seat on side 2 is first");
    expect(!mp::lobby_sides_allow_start(room.lobby), "the local seat on side 2 holds START");
    mp::slot_info(room.lobby, 0)->side = 0;

    room.seat_computer(2);
    mp::slot_info(room.lobby, 2)->side = 2;
    expect(mp::lobby_first_side_past_table(room.lobby) == 2, "the computer on side 2 is first");
    expect(!mp::lobby_sides_allow_start(room.lobby), "the computer on side 2 holds START");

    room.game->side_count = 0;
    expect(mp::lobby_sides_allow_start(room.lobby), "an unknown side count allows START");

    room.game->side_count = 2;
    mp::slot_player(room.lobby, 2).in_use = 0;
    expect(apply_side_block(room, 0x200, 2, false), "the guest's side 2 block applies again");
    room.lobby.presence_records.block_player[1] = 0;
    expect(
        mp::lobby_sides_allow_start(room.lobby),
        "a remote seat whose block has not arrived allows START"
    );

    Room guest(false);
    guest.game->side_count = 2;
    expect(apply_side_block(guest, 0x100, 2, false), "the host's side block applies on the guest");
    const auto guest_lines = room_lines(guest.lobby).size();
    expect(mp::lobby_sides_allow_start(guest.lobby), "a guest's machine allows START");
    mp::lobby_note_sides_past_table(guest.lobby);
    expect(room_lines(guest.lobby).size() == guest_lines, "a guest's machine posts nothing");
}

void test_shown_name() {
    expect(
        mp::mutator_shown_name("Ridge Tough", "Other", "ridge-tough") == "Ridge Tough",
        "an installed name is shown"
    );
    expect(
        mp::mutator_shown_name("", "Ridge Tough", "ridge-tough") == "Ridge Tough",
        "a catalogue name is shown when nothing is installed"
    );
    expect(
        mp::mutator_shown_name("", "", "ridge-tough") == "ridge-tough",
        "the id is shown only when no name is known"
    );
}

} // namespace

int main() {
    test_no_field_allows_start();
    test_each_hold();
    test_slot_order_and_who_is_skipped();
    test_have_codes();
    test_shown_name();
    test_empty_host_holds_a_guest_still_sending();
    test_block_allocates_nothing();
    test_parts();
    test_part_refused_once();
    test_ready();
    test_greeting_restarts_the_guest();
    test_rebuild_keeps_a_finished_legacy_peer();
    test_legacy_checksum_is_compared();
    test_field_follows_the_phase();
    test_new_room_forgets_the_host_list();
    test_classic_joiner();
    test_classic_seated();
    test_classic_stays_and_leaves();
    test_classic_lines_whole();
    test_guest_posts_the_refusal();
    test_side_past_table();
    if (g_failures != 0)
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
