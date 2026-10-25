// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A room past 3.1c's weapon types or sides: who is refused, who holds START,
// and the lines written to the log.
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"
#include "oa/ui/frontend_multiplayer/room_limits.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <malloc.h>
#else
#include <unistd.h>
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
    /// @param watcher whether the block says the player is watching
    void seat(int32_t player_slot, uint32_t id, bool oa, bool host_role, bool watcher = false) {
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
        if (watcher)
            info->options = static_cast<uint16_t>(info->options | mp::option::watcher);
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
        std::snprintf(player.name, sizeof player.name, "Cpu");
        mp::slot_set_status(lobby, player, mp::kSlotComputer);
    }
};

constexpr const char* kWeaponLine = "*** {player} plays Total Annihilation 3.1c, which cannot play "
                                    "a mod with more than 256 weapon types.";
constexpr const char* kSidesLine = "*** {player} plays Total Annihilation 3.1c, which cannot play "
                                   "a mod with more than five sides.";
constexpr const char* kNewerLine = "*** {player} needs a newer Open Annihilation for this mod.";
constexpr const char* kLeaveLine =
    "This mod needs every player on Open Annihilation. The host plays Total Annihilation 3.1c.";
constexpr const char* kMutatorLine =
    "*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.";

void set_rules(mp::Lobby& lobby, uint32_t weapons, uint8_t sides) {
    lobby.wire_rules.weapon_types = weapons;
    lobby.wire_rules.sides = sides;
}

void name_slot(Room& room, int32_t player_slot, const char* name) {
    auto& player = mp::slot_player(room.lobby, player_slot);
    std::snprintf(player.name, sizeof player.name, "%s", name);
}

/// Applies one remote player's setup block.
///
/// @param room the room
/// @param id the player, already seated
/// @param oa whether the block carries the engine signature
/// @param watcher whether the block says the player is watching
/// @return true when the record applied
bool apply_block(Room& room, uint32_t id, bool oa, bool watcher, bool host_role = false) {
    mp::PlayerSetupInfo info{};
    info.state = mp::kInfoStatePlaying;
    info.version_major = 3;
    info.version_minor = 1;
    if (host_role)
        info.role = mp::kRoleHost;
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

/// Applies one presence record from a seated player.
bool apply_presence(Room& room, uint32_t id, const std::vector<uint8_t>& bytes) {
    if (bytes.size() > mp::kLobbyEventBytes)
        return false;
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = id;
    event.size = static_cast<uint16_t>(bytes.size());
    std::memcpy(event.data, bytes.data(), bytes.size());
    return mp::lobby_apply_event(room.lobby, event);
}

/// Hands a guest the host's rejection of a player.
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

int chat_sent(const Room& room) {
    int chats = 0;
    for (int32_t index = 0; index < room.loopback.sent_count; ++index) {
        if (room.loopback.sent_size[index] > 0 &&
            room.loopback.sent[index][0] == static_cast<uint8_t>(ng::RecordType::chat))
            ++chats;
    }
    return chats;
}

bool sent_presence(const Room& room) {
    for (int32_t index = 0; index < room.loopback.sent_count; ++index) {
        if (room.loopback.sent_size[index] > 0 &&
            room.loopback.sent[index][0] == ng::presence_record_type)
            return true;
    }
    return false;
}

std::vector<std::string> room_lines(mp::Lobby& lobby) {
    std::vector<std::string> lines;
    auto& game = *lobby.game;
    for (auto at = static_cast<uint32_t>(mp::lobby_chat_tail(game));
         at != mp::lobby_chat_head(game);
         at = (at + 1U) % static_cast<uint32_t>(mp::kChatLines))
        lines.emplace_back(mp::lobby_chat_line(game, at));
    return lines;
}

std::string joined_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) {
        if (!text.empty())
            text += ' ';
        text += line;
    }
    return text;
}

std::string filled(const char* words, const char* player) {
    std::string line(words);
    const auto mark = line.find("{player}");
    if (mark != std::string::npos)
        line.replace(mark, std::string_view("{player}").size(), player);
    return line;
}

std::vector<uint8_t> encode_record(const ng::PresenceRecord& record) {
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    if (ng::encode_presence(record, bytes.data(), bytes.size(), &written) != ng::WireError::ok)
        return {};
    bytes.resize(written);
    return bytes;
}

/// A limits tag naming 255 weapon types. The encoder refuses a value that small.
std::vector<uint8_t> low_weapon_record() {
    return {0xf0, 12, 0, 1, 0x24, 5, 0, 0xff, 0x00, 0x00, 0x00, 5};
}

/// Stores a record as the one this machine sends, so the room has mutators on.
void put_local_mutators(mp::Lobby& lobby) {
    ng::PresenceMutators field;
    field.built = true;
    field.total = 2;
    field.held = 2;
    field.index = ng::presence_mutator_index_none;
    field.digest.fill(0x11);
    field.layer_digest.fill(0x22);
    ng::PresenceRecord record;
    record.mutators = field;
    const auto bytes = encode_record(record);
    if (bytes.empty())
        return;
    std::memcpy(lobby.presence_records.local, bytes.data(), bytes.size());
    lobby.presence_records.local_size = static_cast<uint16_t>(bytes.size());
}

/// Captures what write_line sends to stdout. Restores stdout before reading.
class CapturedOut {
  public:

    CapturedOut() {
        std::fflush(stdout);
#if defined(_WIN32)
        if (_pipe(ends_, 8192, _O_BINARY) != 0)
            return;
        saved_ = _dup(_fileno(stdout));
        if (saved_ < 0)
            return;
        _dup2(ends_[1], _fileno(stdout));
#else
        if (pipe(ends_) != 0)
            return;
        saved_ = dup(fileno(stdout));
        if (saved_ < 0)
            return;
        dup2(ends_[1], fileno(stdout));
#endif
        open_ = true;
    }

    std::string take() {
        if (!open_)
            return {};
        std::fflush(stdout);
        std::string text;
        char buffer[512];
#if defined(_WIN32)
        _dup2(saved_, _fileno(stdout));
        _close(saved_);
        _close(ends_[1]);
        int count = 0;
        while ((count = _read(ends_[0], buffer, sizeof buffer)) > 0)
            text.append(buffer, static_cast<std::size_t>(count));
        _close(ends_[0]);
#else
        dup2(saved_, fileno(stdout));
        close(saved_);
        close(ends_[1]);
        ssize_t count = 0;
        while ((count = ::read(ends_[0], buffer, sizeof buffer)) > 0)
            text.append(buffer, static_cast<std::size_t>(count));
        close(ends_[0]);
#endif
        open_ = false;
        return text;
    }

    ~CapturedOut() {
        if (open_)
            (void)take();
    }

    CapturedOut(const CapturedOut&) = delete;
    CapturedOut& operator=(const CapturedOut&) = delete;

  private:

    int ends_[2]{-1, -1};
    int saved_{-1};
    bool open_{false};
};

int count_line(const std::string& log, const std::string& line) {
    const std::string needle = line + '\n';
    int found = 0;
    for (std::size_t at = 0; (at = log.find(needle, at)) != std::string::npos; at += needle.size())
        ++found;
    return found;
}

/// With 3.1c's rules a 3.1c block and an OA block without a record are seated.
void stock_room_seats_everyone() {
    Room room(true);
    set_rules(room.lobby, 256, 5);
    room.lobby.presence_records.block_player[1] = 0;
    name_slot(room, 1, "Bo");
    expect(apply_block(room, 0x200, false, false), "a 3.1c block applies in a stock room");
    const auto& classic = mp::slot_player(room.lobby, 1);
    expect(
        classic.status == mp::kSlotRemote && classic.reject_reason == 0,
        "a stock room seats a 3.1c player"
    );
    room.seat(2, 0x300, true, false);
    room.lobby.presence_records.block_player[2] = 0;
    expect(apply_block(room, 0x300, true, false), "an OA block applies in a stock room");
    const auto& older = mp::slot_player(room.lobby, 2);
    expect(
        older.status == mp::kSlotRemote && older.reject_reason == 0,
        "a stock room seats an OA player who has sent no record"
    );
    expect(room_lines(room.lobby).empty(), "a stock room posts no limits line");
    expect(
        !sent_reject(room, 0x200, 8) && !sent_reject(room, 0x300, 8),
        "a stock room sends no refusal"
    );
    expect(mp::lobby_limits_allow_start(room.lobby), "a stock room does not hold START for limits");
    expect(
        mp::lobby_limits_block(room.lobby).hold == mp::LimitsHold::none,
        "a stock room has no limits hold"
    );
    expect(room.lobby.leave_line == nullptr, "a stock room sets no leave line");
}

/// Weapon types 1024 refuse a 3.1c player and watcher. Sides 7 use the sides line.
/// Mutators on as well post only the mutators' line.
void classic_joiner_refused() {
    const std::string weapon = filled(kWeaponLine, "Bo");
    const std::string sides = filled(kSidesLine, "Bo");
    const std::string watcher = filled(kWeaponLine, "Cam");
    const std::string mutator = filled(kMutatorLine, "Bo");
    {
        Room room(true);
        set_rules(room.lobby, 1024, 5);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(
            apply_block(room, 0x200, false, false), "a 3.1c block applies under 1024 weapon types"
        );
        const auto& player = mp::slot_player(room.lobby, 1);
        expect(
            player.reject_reason == 8 && player.status == mp::kSlotOpen,
            "1024 weapon types refuse a 3.1c player with reason 8"
        );
        expect(sent_reject(room, 0x200, 8), "the weapon-types refusal is reason 8");
        expect(
            joined_lines(room_lines(room.lobby)) == weapon, "the room posts the weapon-types line"
        );
        expect(chat_sent(room) == 0, "the weapon-types line is not sent as chat");
    }
    {
        Room room(true);
        set_rules(room.lobby, 1024, 5);
        room.seat(2, 0x300, false, false);
        room.lobby.presence_records.block_player[2] = 0;
        name_slot(room, 2, "Cam");
        expect(apply_block(room, 0x300, false, true), "a 3.1c watcher block applies");
        const auto& watching = mp::slot_player(room.lobby, 2);
        expect(
            watching.reject_reason == 8 && watching.status == mp::kSlotOpen,
            "1024 weapon types refuse a 3.1c watcher with reason 8"
        );
        expect(
            joined_lines(room_lines(room.lobby)) == watcher,
            "the room posts the weapon-types line for a watcher"
        );
        expect(chat_sent(room) == 0, "the watcher line is not sent as chat");
    }
    {
        Room room(true);
        set_rules(room.lobby, 256, 7);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(apply_block(room, 0x200, false, false), "a 3.1c block applies under seven sides");
        expect(
            mp::slot_player(room.lobby, 1).reject_reason == 8,
            "seven sides refuse a 3.1c player with reason 8"
        );
        expect(
            joined_lines(room_lines(room.lobby)) == sides, "seven sides post the five-sides line"
        );
        expect(chat_sent(room) == 0, "the five-sides line is not sent as chat");
    }
    {
        Room room(true);
        set_rules(room.lobby, 1024, 7);
        put_local_mutators(room.lobby);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(
            apply_block(room, 0x200, false, false),
            "a 3.1c block applies with mutators and wide limits"
        );
        expect(
            mp::slot_player(room.lobby, 1).reject_reason == 8, "mutators still refuse with reason 8"
        );
        expect(
            joined_lines(room_lines(room.lobby)) == mutator,
            "mutators on post only the mutators' line"
        );
        expect(chat_sent(room) == 0, "the mutator line is not sent as chat");
    }
    {
        Room guest(false);
        set_rules(guest.lobby, 1024, 5);
        guest.seat(2, 0x300, false, false);
        guest.lobby.presence_records.block_player[2] = 0;
        name_slot(guest, 2, "Bo");
        expect(apply_block(guest, 0x300, false, false), "a guest applies a 3.1c block");
        expect(!sent_reject(guest, 0x300, 8), "a guest refuses no 3.1c player");
        expect(room_lines(guest.lobby).empty(), "a guest posts nothing until the host refuses");
        expect(host_reject_to(guest, 0x300, 8), "the host's reason 8 reaches the guest");
        expect(joined_lines(room_lines(guest.lobby)) == weapon, "the guest names the 3.1c player");
    }
}

/// An OA block is seated until its record arrives. A record that does not cover is refused once.
void older_build_refused() {
    const std::string newer = filled(kNewerLine, "Bo");
    const auto empty = encode_record(ng::PresenceRecord{});
    ng::PresenceRecord covered_fields;
    covered_fields.limits = ng::PresenceLimits{65536, 32};
    const auto covered = encode_record(covered_fields);
    expect(!empty.empty() && !covered.empty(), "the presence records encode");
    {
        Room room(true);
        set_rules(room.lobby, 1024, 5);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(
            apply_block(room, 0x200, true, false), "an OA block applies under 1024 weapon types"
        );
        const auto& seated = mp::slot_player(room.lobby, 1);
        expect(
            seated.status == mp::kSlotRemote && seated.reject_reason == 0,
            "an OA block is seated before its record arrives"
        );
        const auto held = mp::lobby_limits_block(room.lobby);
        expect(
            held.hold == mp::LimitsHold::older_build && held.slot == 1,
            "START is held as older_build until a record arrives"
        );
        expect(!mp::lobby_limits_allow_start(room.lobby), "START waits with no record");
        expect(room_lines(room.lobby).empty(), "no record posts no newer-build line");
        expect(apply_presence(room, 0x200, empty), "a record without the tag arrives");
        expect(
            mp::slot_player(room.lobby, 1).reject_reason == 8 &&
                mp::slot_player(room.lobby, 1).status == mp::kSlotOpen,
            "a record without the tag is refused with reason 8"
        );
        expect(sent_reject(room, 0x200, 8), "the newer-build refusal is reason 8");
        expect(
            joined_lines(room_lines(room.lobby)) == newer, "the room posts the newer-build line"
        );
        expect(chat_sent(room) == 0, "the newer-build line is not sent as chat");
        const auto posted = room_lines(room.lobby).size();
        expect(!apply_presence(room, 0x200, empty), "a later record finds the player gone");
        expect(room_lines(room.lobby).size() == posted, "the newer-build line is posted once");
    }
    {
        Room room(true);
        set_rules(room.lobby, 1024, 5);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(apply_block(room, 0x200, true, false), "an OA block applies before a low tag");
        expect(
            apply_presence(room, 0x200, low_weapon_record()),
            "a record with 255 weapon types arrives"
        );
        expect(
            mp::slot_player(room.lobby, 1).reject_reason == 8,
            "255 weapon types are refused with reason 8"
        );
        expect(
            joined_lines(room_lines(room.lobby)) == newer,
            "255 weapon types post the newer-build line"
        );
    }
    {
        Room room(true);
        set_rules(room.lobby, 1024, 5);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(apply_block(room, 0x200, true, false), "an OA block applies before a covering tag");
        expect(!mp::lobby_limits_allow_start(room.lobby), "START waits before the covering tag");
        expect(
            apply_presence(room, 0x200, covered),
            "a record with 65536 weapon types and 32 sides arrives"
        );
        const auto& player = mp::slot_player(room.lobby, 1);
        expect(
            player.status == mp::kSlotRemote && player.reject_reason == 0,
            "a covering tag leaves the player seated"
        );
        expect(mp::lobby_limits_allow_start(room.lobby), "a covering tag lifts the hold");
        expect(mp::lobby_limits_block(room.lobby).hold == mp::LimitsHold::none, "the hold is none");
        expect(room_lines(room.lobby).empty(), "a covering tag posts no line");
        expect(!sent_reject(room, 0x200, 8), "a covering tag sends no refusal");
    }
    {
        Room guest(false);
        set_rules(guest.lobby, 1024, 5);
        guest.seat(2, 0x300, true, false);
        guest.lobby.presence_records.block_player[2] = 0;
        name_slot(guest, 2, "Bo");
        expect(apply_block(guest, 0x300, true, false), "a guest applies an OA block");
        expect(apply_presence(guest, 0x300, empty), "a guest keeps a record without the tag");
        expect(!sent_reject(guest, 0x300, 8), "a guest refuses no older build");
        expect(
            mp::slot_player(guest.lobby, 2).status == mp::kSlotRemote,
            "a guest leaves the older build seated"
        );
        expect(room_lines(guest.lobby).empty(), "a guest posts nothing before the host refuses");
        expect(host_reject_to(guest, 0x300, 8), "the host's reason 8 reaches the guest");
        expect(joined_lines(room_lines(guest.lobby)) == newer, "the guest names the older build");
        expect(
            mp::slot_player(guest.lobby, 0).reject_reason == 0, "the guest does not refuse itself"
        );
    }
    {
        Room guest(false);
        set_rules(guest.lobby, 1024, 5);
        guest.seat(2, 0x300, true, false);
        name_slot(guest, 2, "Bo");
        expect(apply_presence(guest, 0x300, covered), "a guest keeps a covering record");
        expect(host_reject_to(guest, 0x300, 8), "a reason 8 arrives for a covering record");
        expect(room_lines(guest.lobby).empty(), "a covering record posts no newer-build line");
    }
}

/// A guest whose mod is OA-only leaves a 3.1c host. Stock rules stay.
void classic_host_left() {
    {
        Room guest(false);
        set_rules(guest.lobby, 1024, 5);
        guest.seat_computer(2);
        guest.lobby.presence_records.block_player[1] = 0;
        guest.loopback.sent_count = 0;
        mp::slot_info(guest.lobby, 1)->role = 0;
        expect(apply_block(guest, 0x100, false, false, true), "the host's 3.1c block applies");
        expect(
            guest.lobby.leave_line != nullptr &&
                std::strcmp(guest.lobby.leave_line, kLeaveLine) == 0,
            "the guest sets the leave line"
        );
        expect(
            mp::slot_player(guest.lobby, 0).reject_reason == 2,
            "the local player leaves with reason 2"
        );
        expect(
            mp::slot_player(guest.lobby, 2).reject_reason == 2,
            "the computer player leaves with reason 2"
        );
        expect(sent_reject(guest, 0x200, 2), "the local refusal is reason 2");
        expect(sent_reject(guest, 0x201, 2), "the computer refusal is reason 2");
        expect(
            !sent_reject(guest, 0x200, 8) && !sent_reject(guest, 0x100, 8),
            "the guest sends no reason 8"
        );
        expect(!sent_presence(guest), "the guest sends no presence record to the 3.1c host");
    }
    {
        Room guest(false);
        set_rules(guest.lobby, 256, 5);
        guest.seat_computer(2);
        guest.lobby.presence_records.block_player[1] = 0;
        guest.loopback.sent_count = 0;
        mp::slot_info(guest.lobby, 1)->role = 0;
        expect(
            apply_block(guest, 0x100, false, false, true),
            "a stock guest applies the host's 3.1c block"
        );
        expect(guest.lobby.leave_line == nullptr, "stock rules set no leave line");
        expect(
            mp::slot_player(guest.lobby, 0).reject_reason == 0, "stock rules keep the local player"
        );
        expect(
            mp::slot_player(guest.lobby, 2).reject_reason == 0,
            "stock rules keep the computer player"
        );
        expect(!sent_reject(guest, 0x200, 2), "stock rules send no leave");
    }
}

/// Each refusal line and the leave line reach the log once, whole, with the name filled.
void lines_reach_the_log() {
    const std::string weapon = filled(kWeaponLine, "Bo");
    const std::string sides = filled(kSidesLine, "Bo");
    const std::string newer = filled(kNewerLine, "Bo");
    const auto empty = encode_record(ng::PresenceRecord{});
    CapturedOut captured;
    {
        Room room(true);
        set_rules(room.lobby, 1024, 5);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(apply_block(room, 0x200, false, false), "the weapon-types refusal logs");
    }
    {
        Room room(true);
        set_rules(room.lobby, 256, 7);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(apply_block(room, 0x200, false, false), "the five-sides refusal logs");
    }
    {
        Room room(true);
        set_rules(room.lobby, 1024, 5);
        room.lobby.presence_records.block_player[1] = 0;
        name_slot(room, 1, "Bo");
        expect(apply_block(room, 0x200, true, false), "the OA block is seated before the log line");
        expect(apply_presence(room, 0x200, empty), "the newer-build refusal logs");
    }
    {
        Room guest(false);
        set_rules(guest.lobby, 1024, 5);
        guest.lobby.presence_records.block_player[1] = 0;
        mp::slot_info(guest.lobby, 1)->role = 0;
        expect(apply_block(guest, 0x100, false, false, true), "the leave line logs");
    }
    const std::string log = captured.take();
    expect(count_line(log, weapon) == 1, "the weapon-types line is one whole log line");
    expect(count_line(log, sides) == 1, "the five-sides line is one whole log line");
    expect(count_line(log, newer) == 1, "the newer-build line is one whole log line");
    expect(count_line(log, kLeaveLine) == 1, "the leave line is one whole log line");
    expect(log.find("{player}") == std::string::npos, "the log fills the player's name");
}

/// lobby_limits_allow_start and lobby_limits_block allocate nothing.
void start_gate_allocates_nothing() {
    Room room(true);
    set_rules(room.lobby, 1024, 5);
    room.seat_computer(2);
    room.seat(3, 0x300, true, false);
    room.seat(4, 0x400, true, false, true);
    ng::PresenceRecord covered_fields;
    covered_fields.limits = ng::PresenceLimits{65536, 32};
    const auto covered = encode_record(covered_fields);
    expect(
        covered.size() <= sizeof room.lobby.presence_records.peer[1], "the covering record fits"
    );
    const auto keep = [&](int32_t player_slot, uint32_t id) {
        std::memcpy(room.lobby.presence_records.peer[player_slot], covered.data(), covered.size());
        room.lobby.presence_records.peer_size[player_slot] = static_cast<uint16_t>(covered.size());
        room.lobby.presence_records.peer_id[player_slot] = id;
    };
    keep(1, 0x200);
    keep(3, 0x300);
    room.seat(9, 0x900, false, false);
    name_slot(room, 9, "Bo");

    g_allocations.store(0, std::memory_order_relaxed);
    g_count_allocations.store(true, std::memory_order_relaxed);
    const auto classic = mp::lobby_limits_block(room.lobby);
    const auto classic_allows = mp::lobby_limits_allow_start(room.lobby);
    mp::slot_player(room.lobby, 9).in_use = 0;
    const auto older = mp::lobby_limits_block(room.lobby);
    const auto older_allows = mp::lobby_limits_allow_start(room.lobby);
    const bool covered_seat = mp::lobby_limits_covered(room.lobby, 3);
    const int used = g_allocations.load(std::memory_order_relaxed);
    g_count_allocations.store(false, std::memory_order_relaxed);

    expect(
        classic.hold == mp::LimitsHold::classic_player && classic.slot == 9,
        "a 3.1c player in the last slot is the first hold"
    );
    expect(!classic_allows, "a seated 3.1c player holds START");
    expect(
        older.hold == mp::LimitsHold::older_build && older.slot == 4,
        "with no 3.1c player the first uncovered OA player holds START"
    );
    expect(!older_allows, "an OA watcher without a record holds START");
    expect(covered_seat, "the tagged record covers the room");
    expect(used == 0, "the start gate allocates nothing");
}

} // namespace

int main() {
    stock_room_seats_everyone();
    classic_joiner_refused();
    older_build_refused();
    classic_host_left();
    lines_reach_the_log();
    start_gate_allocates_nothing();
    if (g_failures != 0)
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
